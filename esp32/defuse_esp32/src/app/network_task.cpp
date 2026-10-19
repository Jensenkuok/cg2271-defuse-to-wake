/* Defuse to Wake - ESP32 network task. See network_task.h. */
#include "network_task.h"

#include <WiFi.h>
#include <WiFiMulti.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../../secrets.h"
#include "app_state.h"
#include "bomb_config.h"
#include "config.h"
#include "flash_store.h"
#include "history.h"
#include "link_task.h"
#include "mirror.h"
#include "puzzle_gen.h"
#include "schedule.h"
#include "settings.h"
#include "status.h"
#include "timetable.h"
#include "../shared/protocol.h"

#define ALARM_GRACE_S      600u   /* a fired alarm is kept this long (or until DEFUSED) before it is replaced */
#define ALARM_BOOT_GRACE_S 120u   /* see reschedule(): covers an alarm that is due right after a reboot */
#define SEC_PER_HOUR_N     3600u
#define SEC_PER_DAY_N      86400u

struct NetCtx {
    /* connectivity */
    bool wifi_up;
    bool ntp_started;
    bool time_valid;
    uint32_t next_wifi_ms;
    /* MCX time/alarm sync */
    uint32_t last_sync_ms;
    uint32_t seen_link_count;
    bool force_send;            /* the alarm changed: run the TIME_SYNC / SET_ALARM / CANCEL decision now */
    uint32_t last_sent_alarm_unix; /* the alarm the MCX was last told, 0 = none / cancelled / consumed */
    /* alarm scheduling */
    bool resched_pending;       /* recompute once the clock is valid */
    bool first_schedule_done;   /* false until the first reschedule after boot */
    uint32_t cur_alarm_unix;    /* the alarm we last scheduled, 0 = none */
    uint32_t cur_class_start_unix;      /* start of the class that alarm is for */
    uint32_t consumed_class_start_unix; /* classes starting at or before this already had their alarm */
    uint32_t session_class_start_unix;  /* class whose alarm started the running session, 0 = demo ring */
    uint32_t seen_session_starts;
    uint32_t seen_session_ends;
    char cur_module[SCHED_MODULE_LEN];
    uint32_t ring_alarm_unix;   /* the previous alarm, remembered after it was replaced */
    char ring_module[SCHED_MODULE_LEN];
    /* timetable refresh */
    bool tt_due;
    uint32_t tt_next_try_ms;
    int32_t tt_last_nightly_day;
    /* settings / UI feedback */
    uint32_t seen_settings_ver;
    uint8_t seen_mirror_pending;
};

static TaskHandle_t s_task;
static WiFiMulti s_multi;
static NetCtx s_ctx;
static Lesson s_lessons[SCHED_MAX_LESSONS];    /* scheduler input, network task only */
static char s_link[APP_LINK_MAX_CHARS + 1];

/* ================= helpers ================= */
static void fmt_local(char *dst, size_t n, uint32_t unix_s)
{
    static const char *const k_weekday[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    time_t at = (time_t)(unix_s + (uint32_t)APP_TZ_OFFSET_S);
    struct tm t;

    gmtime_r(&at, &t);
    snprintf(dst, n, "%s %04d-%02d-%02d %02d:%02d", k_weekday[t.tm_wday % 7], t.tm_year + 1900, t.tm_mon + 1,
             t.tm_mday, t.tm_hour, t.tm_min);
}

/* The MCX only accepts SET_ALARM in IDLE or ARMED (it drops it silently in BOMB_ACTIVE, EXPLODED
 * and the 30 s DEFUSED hold: game_ringing() in app_state.h), so nothing is sent in those states
 * and force_send stays set until the MCX is back in IDLE. ARMED is fine: a new lead time or
 * timetable must be able to move a pending alarm. */

/* Each helper queues one frame for the link task; false if the queue refused it. */
static bool queue_time_sync(uint32_t unix_now)
{
    proto_time_sync_t ts;
    uint8_t pl[PROTO_SIZE_TIME_SYNC];

    ts.unix_time = unix_now;
    if (proto_pack_time_sync(&ts, pl, sizeof(pl)) == 0 || !link_send(PROTO_MSG_TIME_SYNC, pl, PROTO_SIZE_TIME_SYNC)) {
        return false;
    }
    Serial.printf("[net] TIME_SYNC %u queued\n", (unsigned)unix_now);
    return true;
}

static bool queue_set_alarm(uint32_t alarm_unix)
{
    proto_set_alarm_t sa;
    uint8_t pl[PROTO_SIZE_SET_ALARM];

    sa.alarm_unix = alarm_unix;
    if (proto_pack_set_alarm(&sa, pl, sizeof(pl)) == 0 || !link_send(PROTO_MSG_SET_ALARM, pl, PROTO_SIZE_SET_ALARM)) {
        return false;
    }
    Serial.printf("[net] SET_ALARM %u queued\n", (unsigned)alarm_unix);
    return true;
}

static bool queue_cancel(void)
{
    proto_cmd_t cmd;
    uint8_t pl[PROTO_SIZE_CMD];

    cmd.cmd = PROTO_CMD_CANCEL;
    cmd.arg = 0u;
    if (proto_pack_cmd(&cmd, pl, sizeof(pl)) == 0 || !link_send(PROTO_MSG_CMD, pl, PROTO_SIZE_CMD)) {
        return false;
    }
    Serial.println("[net] CMD_CANCEL queued (alarm cleared)");
    return true;
}

/* ================= alarm scheduling ================= */
/* May the current alarm be replaced right now? Not while it is imminent or has just rung and
 * nobody defused yet: a recompute in the last minute before it rings would skip it (next_alarm
 * only returns alarms more than SCHED_MIN_LEAD_S away) and the MCX would be re-armed for the
 * NEXT class. */
static bool alarm_replaceable(const NetCtx &c, uint32_t unix_now)
{
    if (c.cur_alarm_unix == 0u ||
        (c.cur_class_start_unix != 0u && c.cur_class_start_unix <= c.consumed_class_start_unix)) {
        return true;                                   /* nothing scheduled, or its alarm was already used */
    }
    return c.cur_alarm_unix > unix_now + SCHED_MIN_LEAD_S || unix_now >= c.cur_alarm_unix + ALARM_GRACE_S;
}

static void remember_fired_alarm(NetCtx &c, uint32_t unix_now)
{
    if (c.cur_alarm_unix != 0u && c.cur_alarm_unix <= unix_now + SCHED_MIN_LEAD_S) {
        c.ring_alarm_unix = c.cur_alarm_unix;
        memcpy(c.ring_module, c.cur_module, sizeof(c.ring_module));
    }
}

static void reschedule(NetCtx &c, uint32_t unix_now)
{
    NextAlarm a;
    int n;
    uint32_t new_alarm;
    char when[48];

    if (!alarm_replaceable(c, unix_now)) {
        return;
    }
    n = timetable_copy(s_lessons, SCHED_MAX_LESSONS);
    /* First schedule after a boot: the MCX may already be ARMED for an alarm that is less than a
     * minute away (we rebooted, it did not). Look back BOOT_GRACE so that alarm is returned again
     * instead of being skipped for the next class. A past result is never sent (see sync_with_mcx). */
    a = next_alarm(s_lessons, n, c.first_schedule_done ? unix_now : unix_now - ALARM_BOOT_GRACE_S,
                   settings_get_lead_min(), c.consumed_class_start_unix);
    c.first_schedule_done = true;
    new_alarm = a.found ? a.alarm_unix : 0u;
    if (new_alarm == c.cur_alarm_unix) {
        return;
    }
    remember_fired_alarm(c, unix_now);
    c.cur_alarm_unix = new_alarm;
    c.cur_class_start_unix = a.found ? a.class_start_unix : 0u;
    memset(c.cur_module, 0, sizeof(c.cur_module));
    if (!a.found) {
        app_state_set_schedule(0u, 0u, NULL);
        c.force_send = true;                           /* alarm_action decides whether the MCX needs a CANCEL */
        Serial.printf("[sched] no class in the next %d days: alarm cleared (%d lessons cached)\n",
                      SCHED_LOOKAHEAD_DAYS, n);
        return;
    }
    memcpy(c.cur_module, s_lessons[a.lesson_idx].module, sizeof(c.cur_module) - 1u);
    app_state_set_schedule(a.alarm_unix, a.class_start_unix, &s_lessons[a.lesson_idx]);
    c.force_send = true;
    fmt_local(when, sizeof(when), a.alarm_unix);
    Serial.printf("[sched] next alarm %s (unix %u) for %s %s %s, lead %d min\n", when, (unsigned)a.alarm_unix,
                  s_lessons[a.lesson_idx].module, s_lessons[a.lesson_idx].type, s_lessons[a.lesson_idx].venue,
                  settings_get_lead_min());
}

/* ================= RESULT frames -> history + Firebase queue ================= */
static const char *module_for_result(const NetCtx &c, uint32_t unix_now)
{
    if (unix_now == 0u) {
        return "";
    }
    if (c.cur_alarm_unix != 0u && c.cur_alarm_unix <= unix_now + SCHED_MIN_LEAD_S) {
        return c.cur_module;                       /* the alarm that is ringing now */
    }
    if (c.ring_alarm_unix != 0u && unix_now - c.ring_alarm_unix < (uint32_t)APP_RING_RECENT_S) {
        return c.ring_module;                      /* it was replaced meanwhile */
    }
    return "";                                     /* manual / demo ring */
}

static void handle_results(NetCtx &c, uint32_t unix_now)
{
    proto_result_msg_t r;
    HistRecord rec;

    /* The alarm is marked consumed when the session ends (track_session), not here: the MCX is
     * still in its 30 s DEFUSED hold and a snooze keeps the session, and the alarm, alive. */
    while (link_result_pop(&r)) {
        const bool defused = (r.outcome == PROTO_OUTCOME_DEFUSED);

        if (history_on_result(defused, r.duration_s, r.strikes, r.snoozes, unix_now,
                              defused ? module_for_result(c, unix_now) : "", &rec)) {
            mirror_push(rec);
        }
    }
}

/* ================= settings, timetable refresh ================= */
static void handle_settings(NetCtx &c, uint32_t now_ms)
{
    uint32_t v = settings_version();

    if (v != c.seen_settings_ver) {
        c.seen_settings_ver = v;
        c.tt_due = true;
        c.tt_next_try_ms = now_ms;                 /* refresh now, then reschedule */
        c.resched_pending = true;                  /* a new lead time applies even before the fetch ends */
        Serial.println("[cfg] settings changed: timetable refresh + reschedule");
    }
}

static void service_timetable(NetCtx &c, uint32_t now_ms, uint32_t unix_now)
{
    const uint32_t local = unix_now + (uint32_t)APP_TZ_OFFSET_S;
    const int32_t day = (int32_t)(local / SEC_PER_DAY_N);
    const bool night = ((local % SEC_PER_DAY_N) / SEC_PER_HOUR_N) >= (uint32_t)APP_TT_NIGHTLY_HOUR;

    if (night && day != c.tt_last_nightly_day) {
        c.tt_last_nightly_day = day;               /* one nightly refresh per evening */
        c.tt_due = true;
        c.tt_next_try_ms = now_ms;
    }
    if (!c.tt_due || (int32_t)(now_ms - c.tt_next_try_ms) < 0) {
        return;
    }
    settings_get_link(s_link, sizeof(s_link));
    if (timetable_refresh(s_link, unix_now)) {
        c.tt_due = false;
        c.resched_pending = true;
        app_state_set_timetable_info((uint8_t)timetable_count(), timetable_fetched_unix());
    } else {
        c.tt_next_try_ms = millis() + APP_TT_RETRY_MS;
        Serial.printf("[tt] will retry in %u s\n", (unsigned)(APP_TT_RETRY_MS / 1000u));
    }
}

/* ================= MCX sync ================= */
/* Session = from the first BOMB_ACTIVE until the MCX is back in IDLE (app_state, fed by
 * GAME_STATE). The MCX owns the alarm during it (a snooze leaves it ARMED), so alarm_action()
 * sends nothing. When it ends the MCX has used up its alarm: forget what we sent, mark the
 * class that rang as consumed and reschedule (-> the next day's first class). */
static void track_session(NetCtx &c, const AppState &st, uint32_t unix_now)
{
    if (st.session_starts != c.seen_session_starts) {
        c.seen_session_starts = st.session_starts;
        /* Only a bomb started by the alarm consumes it; a demo RING_NOW must not eat tomorrow's. */
        c.session_class_start_unix =
            (c.cur_alarm_unix != 0u && c.cur_alarm_unix <= unix_now + SCHED_MIN_LEAD_S) ? c.cur_class_start_unix : 0u;
        Serial.println("[sched] session started: no SET_ALARM / CANCEL until the MCX is IDLE");
    }
    if (st.session_ends != c.seen_session_ends) {
        c.seen_session_ends = st.session_ends;
        if (c.session_class_start_unix > c.consumed_class_start_unix) {
            c.consumed_class_start_unix = c.session_class_start_unix;
        }
        c.session_class_start_unix = 0u;
        c.last_sent_alarm_unix = 0u;
        c.resched_pending = true;
        c.force_send = true;
        Serial.println("[sched] session ended: alarm consumed, rescheduling");
    }
}

/* Queue what alarm_action() decided, TIME_SYNC first (the MCX needs that order). False = the
 * TX queue was full; nothing is marked as sent and the caller retries on the next loop. */
static bool send_to_mcx(NetCtx &c, uint32_t unix_now, AlarmAction action, bool send_sync, uint32_t alarm)
{
    /* a SET_ALARM travels with a BOMB_CONFIG: one more slot */
    const uint32_t need = (send_sync ? 1u : 0u) + (action != ALARM_ACTION_NONE ? 1u : 0u) +
                          (action == ALARM_ACTION_SEND_SET_ALARM ? 1u : 0u);

    if (link_tx_space() < need) {
        return false;
    }
    if (send_sync && !queue_time_sync(unix_now)) {
        return false;
    }
    if (action == ALARM_ACTION_SEND_SET_ALARM) {
        /* Outside a session (alarm_action sends nothing inside one): the bomb this alarm starts gets
         * the next puzzle pair and the current difficulty. Order on the wire: TIME_SYNC, BOMB_CONFIG,
         * SET_ALARM. A config that cannot be queued (one still waiting for its ACK) never holds the
         * alarm back. */
        (void)bomb_config_send("alarm");
        if (!queue_set_alarm(alarm)) {
            return false;
        }
        c.last_sent_alarm_unix = alarm;
    } else if (action == ALARM_ACTION_SEND_CANCEL) {
        if (!queue_cancel()) {
            return false;
        }
        c.last_sent_alarm_unix = 0u;
    }
    return true;
}

static void sync_with_mcx(NetCtx &c, uint32_t now_ms, uint32_t unix_now)
{
    AppState st;
    AlarmAction action;
    bool new_link;
    bool sync_due;
    bool send_sync;
    uint32_t desired;

    if (!app_state_read(&st)) {
        return;
    }
    track_session(c, st, unix_now);
    if (c.resched_pending) {
        reschedule(c, unix_now);
        c.resched_pending = false;
    }
    if (!st.link_up) {
        return;
    }
    new_link = (st.link_up_count != c.seen_link_count);
    sync_due = new_link || c.last_sync_ms == 0u || (uint32_t)(now_ms - c.last_sync_ms) >= APP_TIME_SYNC_PERIOD_MS;
    if (!(sync_due || c.force_send)) {
        return;
    }
    reschedule(c, unix_now);       /* after every TIME_SYNC the alarm is recomputed, then re-sent */
    if (game_ringing(st)) {
        return;                    /* the MCX drops SET_ALARM in these states; force_send stays set */
    }
    /* An alarm that is already past has rung (or is ringing now): leave the MCX alone, the session
     * or the next reschedule takes over. A CANCEL here could kill the bomb that is just starting. */
    desired = c.cur_alarm_unix;
    action = (desired != 0u && desired <= unix_now)
                 ? ALARM_ACTION_NONE
                 : alarm_action(desired, c.last_sent_alarm_unix, st.game.state, st.session_active, sync_due);
    send_sync = sync_due || action == ALARM_ACTION_SEND_SET_ALARM;   /* TIME_SYNC always precedes SET_ALARM */
    if (send_sync || action != ALARM_ACTION_NONE) {
        if (!send_to_mcx(c, unix_now, action, send_sync, desired)) {
            return;                /* retry on the next loop */
        }
        if (send_sync) {
            c.seen_link_count = st.link_up_count;
            c.last_sync_ms = (now_ms == 0u) ? 1u : now_ms; /* 0 means "never" */
        }
    }
    c.force_send = false;
}

/* ================= puzzles + BOMB_CONFIG ================= */
/* LLM calls (up to 30 s each) only when nothing time-critical is going on: no bomb session, the MCX
 * is not ringing, and the alarm is not about to fire or has just fired. */
static bool puzzle_calls_allowed(const NetCtx &c, const AppState &st, uint32_t unix_now)
{
    if (st.session_active || game_ringing(st)) {
        return false;
    }
    if (c.time_valid && c.cur_alarm_unix != 0u && unix_now + APP_PUZZLE_QUIET_S >= c.cur_alarm_unix &&
        unix_now <= c.cur_alarm_unix + ALARM_GRACE_S) {
        return false;
    }
    return true;
}

/* The BOOT demo ring and bomb_config_service() live in the LINK task now (see bomb_config.h):
 * the LLM jobs below can block this task for up to 30 s per call. */

/* LLM jobs: the puzzle run does at most ONE LLM call per pass, so the loop (timetable, results,
 * alarm sync) is back within one call's timeout. */
static void service_llm_jobs(const NetCtx &c, uint32_t now_ms, uint32_t unix_now)
{
    static AppState st;                            /* static: ~1.3 KB that would stay on the stack during the TLS call */
    const bool gen_ok = app_state_read(&st) && puzzle_calls_allowed(c, st, unix_now);
    const bool net_ok = c.wifi_up && c.time_valid;
    const uint32_t unix_valid = c.time_valid ? unix_now : 0u;

    puzzle_gen_service(now_ms, unix_valid, net_ok, gen_ok);
}

/* ================= Wi-Fi + SNTP ================= */
static void service_connectivity(NetCtx &c, uint32_t now_ms, uint32_t *unix_now)
{
    bool wifi_now = c.wifi_up;
    bool time_now;

    /* WiFiMulti.run() blocks while it scans/connects, so it runs on a slow cadence. */
    if ((int32_t)(now_ms - c.next_wifi_ms) >= 0) {
        wifi_now = (s_multi.run(APP_WIFI_CONNECT_TIMEOUT_MS) == WL_CONNECTED);
        c.next_wifi_ms = millis() + (wifi_now ? APP_WIFI_POLL_CONNECTED_MS : APP_WIFI_POLL_DOWN_MS);
    }
    if (wifi_now && !c.ntp_started) {
        configTime(APP_TZ_OFFSET_S, 0, APP_NTP_SERVER_1, APP_NTP_SERVER_2); /* SNTP keeps itself synced */
        c.ntp_started = true;
    }
    *unix_now = (uint32_t)time(nullptr);
    time_now = c.ntp_started && (*unix_now >= APP_MIN_VALID_UNIX);

    if (wifi_now != c.wifi_up) {
        c.wifi_up = wifi_now;
        app_state_set_wifi(wifi_now);
        Serial.printf("[net] Wi-Fi %s\n", wifi_now ? "connected" : "disconnected");
        if (wifi_now) {
            mirror_wifi_up();                      /* try the pending uploads now, not after the back-off */
        }
    }
    if (time_now != c.time_valid) {
        c.time_valid = time_now;
        app_state_set_time_valid(time_now);
        Serial.printf("[net] time %s\n", time_now ? "valid" : "invalid");
    }
}

/* ================= Serial test commands ================= */
static void print_history(uint32_t unix_now)
{
    HistRecord recs[HIST_LEN];
    int n = history_recent(recs, HIST_LEN);
    int i;

    Serial.printf("[hist] %d mornings, streak %d\n", n, history_current_streak(unix_now));
    for (i = 0; i < n; i++) {
        Serial.printf("[hist]  #%d end=%u dur=%us strikes=%u snoozes=%u explosions=%u module=%s\n", i,
                      (unsigned)recs[i].end_unix, (unsigned)recs[i].duration_s, recs[i].strikes, recs[i].snoozes,
                      recs[i].explosions, recs[i].module[0] != '\0' ? recs[i].module : "-");
    }
}

static void run_command(NetCtx &c, const char *cmd, uint32_t unix_now)
{
    if (strncmp(cmd, "lead ", 5) == 0) {
        Serial.printf("[cmd] lead_min = %d\n", settings_set_lead_min(atoi(cmd + 5)));
    } else if (strncmp(cmd, "link ", 5) == 0) {
        Serial.printf("[cmd] link %s\n", settings_set_link(cmd + 5) ? "saved" : "rejected");
    } else if (strcmp(cmd, "refresh") == 0) {
        c.tt_due = true;
        c.tt_next_try_ms = millis();
    } else if (strcmp(cmd, "resched") == 0) {
        c.resched_pending = true;
    } else if (strcmp(cmd, "hist") == 0) {
        print_history(unix_now);
    } else if (strcmp(cmd, "puzzles") == 0) {
        puzzle_gen_request();
        Serial.println("[cmd] puzzle run requested");
    } else if (strcmp(cmd, "ai") == 0) {
        puzzle_print_set();
    } else if (strcmp(cmd, "diff") == 0) {
        bomb_config_print_difficulty();
    } else {
        Serial.println("[cmd] commands: lead <15..180> | link <url> | refresh | resched | hist | puzzles | ai | diff");
    }
}

static void serial_commands(NetCtx &c, uint32_t unix_now)
{
    static char line[APP_CMD_MAX_CHARS + 1];
    static size_t len;
    static bool overflow;
    int ch;

    while (Serial.available() > 0) {
        ch = Serial.read();
        if (ch == '\r' || ch == '\n') {
            if (len > 0u && !overflow) {
                line[len] = '\0';
                run_command(c, line, unix_now);
            }
            len = 0;
            overflow = false;
        } else if (len < APP_CMD_MAX_CHARS) {
            line[len++] = (char)ch;
        } else {
            overflow = true;                       /* drop the over-long line */
        }
    }
}

/* ================= task ================= */
static void network_task(void *arg)
{
    NetCtx &c = s_ctx;
    uint32_t now;
    uint32_t unix_now = 0;
    uint32_t unix_valid;
    uint8_t pending;

    (void)arg;
    WiFi.mode(WIFI_STA);
    s_multi.addAP(WIFI_SSID_1, WIFI_PASS_1);
    s_multi.addAP(WIFI_SSID_2, WIFI_PASS_2);

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(APP_NET_LOOP_MS));
        now = millis();
        service_connectivity(c, now, &unix_now);
        unix_valid = c.time_valid ? unix_now : 0u;

        serial_commands(c, unix_valid);
        handle_settings(c, now);
        handle_results(c, unix_valid);
        puzzle_flush_used();                       /* the link task marks pairs used in RAM; the flash write is ours */
        if (c.time_valid) {
            if (c.wifi_up) {
                service_timetable(c, now, unix_now);
                mirror_service(now, true);
            }
            /* The two services above can block for seconds: take fresh timestamps so TIME_SYNC
             * carries the real time and the alarm decisions are not made on a stale clock. */
            now = millis();
            unix_now = (uint32_t)time(nullptr);
            sync_with_mcx(c, now, unix_now);
        }
        /* The puzzle run / briefing do at most one LLM call per pass, after the MCX work above. */
        service_llm_jobs(c, millis(), c.time_valid ? (uint32_t)time(nullptr) : 0u);
        pending = (uint8_t)mirror_pending();
        if (pending != c.seen_mirror_pending) {
            c.seen_mirror_pending = pending;
            app_state_set_mirror_pending(pending);
        }
        status_tick(now);
    }
}

bool network_task_start(void)
{
    NetCtx &c = s_ctx;

    memset(&c, 0, sizeof(c));
    c.tt_due = true;                               /* fetch once at boot, as soon as Wi-Fi + time are valid */
    c.tt_last_nightly_day = -1;
    c.resched_pending = true;                      /* schedule from the flash cache as soon as the time is valid */
    if (!flash_store_init() || !timetable_init() || !history_init() || !puzzle_gen_init()) {
        Serial.println("[net] could not create the storage objects");
        return false;
    }
    bomb_config_init();                            /* last: from now on the link task may send BOMB_CONFIG */
    (void)mirror_init();
    (void)timetable_load_cache();                  /* offline-first: the cache works with no network */
    app_state_set_timetable_info((uint8_t)timetable_count(), timetable_fetched_unix());
    app_state_set_mirror_pending((uint8_t)mirror_pending());
    c.seen_settings_ver = settings_version();
    c.seen_mirror_pending = (uint8_t)mirror_pending();
    return xTaskCreate(network_task, "network", APP_STACK_NETWORK, NULL, APP_PRIO_NETWORK, &s_task) == pdPASS;
}

TaskHandle_t network_task_handle(void)
{
    return s_task;
}
