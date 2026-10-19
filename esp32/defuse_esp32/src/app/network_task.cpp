/* Defuse to Wake - ESP32 network task. See network_task.h. */
#include "network_task.h"

#include <WiFi.h>
#include <WiFiMulti.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../../secrets.h"
#include "app_state.h"
#include "config.h"
#include "flash_store.h"
#include "history.h"
#include "link_task.h"
#include "mirror.h"
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
    bool force_send;            /* a new alarm exists that the MCX has not been told about */
    /* alarm scheduling */
    bool resched_pending;       /* recompute once the clock is valid */
    bool alarm_consumed;        /* the alarm rang and the bomb was defused: replace it now */
    bool first_schedule_done;   /* false until the first reschedule after boot */
    uint32_t cur_alarm_unix;    /* the alarm we last scheduled, 0 = none */
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
 * and the 30 s DEFUSED hold), so nothing is sent in those states and force_send stays set until
 * the MCX is back in IDLE. ARMED is fine: a new lead time or timetable must be able to move a
 * pending alarm. */
static bool game_ringing(const AppState &st)
{
    return st.game_valid && st.game.state != PROTO_STATE_IDLE && st.game.state != PROTO_STATE_ARMED;
}

/* Queue TIME_SYNC, then SET_ALARM if one is stored. Both or neither: the MCX needs them
 * in this order (a TIME_SYNC moves its clock, and an armed deadline is not moved with it). */
static bool send_time_and_alarm(uint32_t unix_now, uint32_t alarm_unix)
{
    proto_time_sync_t ts;
    proto_set_alarm_t sa;
    uint8_t pl[PROTO_SIZE_TIME_SYNC];
    uint8_t pl2[PROTO_SIZE_SET_ALARM];

    if (link_tx_space() < 2u) {
        return false; /* retry on the next loop */
    }
    ts.unix_time = unix_now;
    if (proto_pack_time_sync(&ts, pl, sizeof(pl)) == 0 ||
        !link_send(PROTO_MSG_TIME_SYNC, pl, PROTO_SIZE_TIME_SYNC)) {
        return false;
    }
    Serial.printf("[net] TIME_SYNC %u queued\n", (unsigned)unix_now);
    if (alarm_unix != 0u) {
        sa.alarm_unix = alarm_unix;
        if (proto_pack_set_alarm(&sa, pl2, sizeof(pl2)) != 0 &&
            link_send(PROTO_MSG_SET_ALARM, pl2, PROTO_SIZE_SET_ALARM)) {
            Serial.printf("[net] SET_ALARM %u queued\n", (unsigned)alarm_unix);
        }
    }
    return true;
}

/* ================= alarm scheduling ================= */
/* May the current alarm be replaced right now? Not while it is imminent or has just rung and
 * nobody defused yet: a recompute in the last minute before it rings would skip it (next_alarm
 * only returns alarms more than SCHED_MIN_LEAD_S away) and the MCX would be re-armed for the
 * NEXT class. */
static bool alarm_replaceable(const NetCtx &c, uint32_t unix_now)
{
    if (c.cur_alarm_unix == 0u || c.alarm_consumed) {
        return true;
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
                   settings_get_lead_min());
    c.first_schedule_done = true;
    new_alarm = a.found ? a.alarm_unix : 0u;
    c.alarm_consumed = false;
    if (new_alarm == c.cur_alarm_unix) {
        return;
    }
    remember_fired_alarm(c, unix_now);
    c.cur_alarm_unix = new_alarm;
    memset(c.cur_module, 0, sizeof(c.cur_module));
    if (!a.found) {
        app_state_set_schedule(0u, 0u, NULL);
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
    bool defused;

    while (link_result_pop(&r)) {
        defused = (r.outcome == PROTO_OUTCOME_DEFUSED);
        if (history_on_result(defused, r.duration_s, r.strikes, r.snoozes, unix_now,
                              defused ? module_for_result(c, unix_now) : "", &rec)) {
            mirror_push(rec);
        }
        if (defused) {
            c.alarm_consumed = true;               /* the next class becomes the alarm */
            c.resched_pending = true;
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
static void sync_with_mcx(NetCtx &c, uint32_t now_ms, uint32_t unix_now)
{
    AppState st;
    bool new_link;
    bool periodic;
    uint32_t alarm;

    if (!app_state_read(&st)) {
        return;
    }
    if (c.resched_pending) {
        reschedule(c, unix_now);
        c.resched_pending = false;
    }
    if (!st.link_up) {
        return;
    }
    new_link = (st.link_up_count != c.seen_link_count);
    periodic = (c.last_sync_ms != 0u) && ((uint32_t)(now_ms - c.last_sync_ms) >= APP_TIME_SYNC_PERIOD_MS);
    if (!(new_link || c.last_sync_ms == 0u || periodic || c.force_send)) {
        return;
    }
    reschedule(c, unix_now);       /* after every TIME_SYNC the alarm is recomputed, then re-sent */
    if (game_ringing(st)) {
        return;                    /* never touch the MCX under a running bomb; force_send stays set */
    }
    alarm = (c.cur_alarm_unix > unix_now) ? c.cur_alarm_unix : 0u;   /* never send an alarm that is already past */
    if (send_time_and_alarm(unix_now, alarm)) {
        c.seen_link_count = st.link_up_count;
        c.last_sync_ms = (now_ms == 0u) ? 1u : now_ms; /* 0 means "never" */
        c.force_send = false;
    }
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
    } else {
        Serial.println("[cmd] commands: lead <15..180> | link <url> | refresh | resched | hist");
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
    if (!flash_store_init() || !timetable_init() || !history_init()) {
        Serial.println("[net] could not create the storage objects");
        return false;
    }
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
