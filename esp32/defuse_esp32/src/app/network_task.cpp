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
#include "briefing.h"
#include "config.h"
#include "fault.h"
#if APP_USE_GYRO
#include "gyro_task.h"
#endif
#include "flash_store.h"
#include "history.h"
#include "i2c_bus.h"
#include "link_task.h"
#include "mem_big.h"
#include "mirror.h"
#include "puzzle_gen.h"
#include "schedule.h"
#include "settings.h"
#include "status.h"
#include "timetable.h"
#include "web_logic.h"
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
    uint32_t cur_class_start_unix;      /* start of the class that alarm is for, 0 for a demo alarm */
    /* Demo morning alarm: a SET_ALARM for "now + N min" that goes through exactly the same path as a
     * real one (TIME_SYNC, BOMB_CONFIG, SET_ALARM, ARMED countdown, ring, defuse). RAM only: a reset
     * forgets it and the first sync sends the real alarm again. */
    uint32_t demo_alarm_unix;           /* 0 = no demo requested */
    bool cur_is_demo;                   /* cur_alarm_unix is the demo (and cur_class_start_unix is 0) */
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
static portMUX_TYPE s_req_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_regen_requested;      /* guarded by s_req_mux: set by the web task, taken by this task */
static bool s_demo_requested;       /* guarded by s_req_mux: a demo alarm request from the web task... */
static int s_demo_minutes;          /* ...with its minutes (0 = cancel) */
/* The pseudo-lesson a demo alarm shows on the OLED / status ("DEMO", no type, no venue). Not a class:
 * the scheduler never consumes anything for it (cur_class_start_unix stays 0). */
static const Lesson k_demo_lesson = { "DEMO", "", 0u, 0u, 0u, 0u, "" };
static WiFiMulti s_multi;
static NetCtx s_ctx;
static AppState *s_puzzle_state;               /* service_llm_jobs only (PSRAM) */
static Lesson *s_lessons;                      /* scheduler input (PSRAM, SCHED_MAX_LESSONS), network task only */
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

/* The demo alarm is used up (it rang and its session ended, it never got a session, or it was
 * cancelled while scheduled): forget it AND the cur_alarm_* it left behind. Clearing cur_alarm_unix
 * matters: alarm_replaceable() treats a past alarm with no class as "just rang, keep it" for
 * ALARM_GRACE_S, which would hold the real alarm back for 10 minutes after the demo. The OLED/status
 * schedule is cleared too, because reschedule() returns early when nothing changes (no real class). */
static void drop_demo(NetCtx &c)
{
    c.demo_alarm_unix = 0u;
    if (c.cur_is_demo) {
        c.cur_is_demo = false;
        c.cur_alarm_unix = 0u;
        c.cur_class_start_unix = 0u;
        memset(c.cur_module, 0, sizeof(c.cur_module));
        app_state_set_schedule(0u, 0u, NULL);
    }
    c.resched_pending = true;                          /* the real alarm comes back at once */
    c.force_send = true;
}

static void reschedule(NetCtx &c, uint32_t unix_now)
{
    NextAlarm a;
    int n;
    uint32_t new_alarm;
    bool use_demo;
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
    if (!c.first_schedule_done) {
        /* Only a request flag here: service_llm_jobs consumes it after this pass has returned, by which
         * time app_state_set_schedule() below has written the alarm that begin_run() reads. */
        briefing_request_boot();
    }
    c.first_schedule_done = true;
    /* A pending demo replaces the real alarm only while it is EARLIER (sched_pick_alarm). If the real
     * alarm now comes at or before the demo (a bigger lead time, a timetable refresh), the demo is
     * dropped so the real one is sent: a demo must never cost the user the real alarm. */
    new_alarm = sched_pick_alarm(a.found ? a.alarm_unix : 0u, c.demo_alarm_unix, unix_now);
    if (c.demo_alarm_unix > unix_now && new_alarm != c.demo_alarm_unix) {
        char note[APP_DEMO_MSG_LEN];

        web_demo_err_message(DEMO_ERR_REAL_FIRST, new_alarm, note, sizeof(note));
        Serial.println("[demo] dropped: real alarm first");
        app_state_set_demo_msg(note);                  /* "your real alarm at HH:MM rings first" */
        c.demo_alarm_unix = 0u;
    }
    use_demo = (new_alarm != 0u && new_alarm == c.demo_alarm_unix);
    if (new_alarm == c.cur_alarm_unix && use_demo == c.cur_is_demo) {
        return;
    }
    remember_fired_alarm(c, unix_now);
    c.cur_alarm_unix = new_alarm;
    c.cur_is_demo = use_demo;
    memset(c.cur_module, 0, sizeof(c.cur_module));
    if (use_demo) {
        /* No class behind it: cur_class_start_unix 0 means track_session() never marks a real class as
         * consumed for a demo ring, and alarm_replaceable() still protects it in its last minute. The
         * module name stays "" so the morning is logged like "Ring now" (module_for_result). */
        c.cur_class_start_unix = 0u;
        app_state_set_schedule(new_alarm, 0u, &k_demo_lesson, true);
        c.force_send = true;
        fmt_local(when, sizeof(when), new_alarm);
        Serial.printf("[sched] DEMO alarm %s (unix %u), real alarm held back until it is used up\n", when,
                      (unsigned)new_alarm);
        return;
    }
    c.cur_class_start_unix = a.found ? a.class_start_unix : 0u;
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
 * class that rang as consumed and reschedule (-> the next day's first class).
 *
 * A demo morning alarm rides on the same session: it rings like a real one, but it has no class
 * (cur_class_start_unix 0), so a demo session never consumes a real class. When the session ends the
 * demo is used up (drop_demo) and the real alarm is scheduled again straight away. */
static void track_session(NetCtx &c, const AppState &st, uint32_t unix_now)
{
    if (st.session_starts != c.seen_session_starts) {
        c.seen_session_starts = st.session_starts;
        /* Only a bomb started by the alarm consumes it; a demo RING_NOW must not eat tomorrow's.
         * A demo ALARM has cur_class_start_unix 0, so it consumes nothing either. */
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
        if (c.demo_alarm_unix != 0u && c.demo_alarm_unix <= unix_now + SCHED_MIN_LEAD_S) {
            drop_demo(c);                              /* it rang (or was due) during this session: used up */
            Serial.println("[sched] demo alarm used up: the real alarm is scheduled again");
        }
        Serial.println("[sched] session ended: alarm consumed, rescheduling");
    }
    /* A demo whose ring never produced a session (the MCX link was down, say) is dropped after the same
     * grace a real alarm gets, so it cannot hold the real alarm back for ever. */
    if (c.demo_alarm_unix != 0u && !st.session_active && unix_now >= c.demo_alarm_unix + ALARM_GRACE_S) {
        drop_demo(c);
        Serial.println("[sched] demo alarm expired without a session: dropped");
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

/* O10: the puzzle set follows the effective difficulty level (mode setting, or the score band in Auto).
 * Runs every pass, before the MCX sync so a BOMB_CONFIG sent this pass already finds the new set; the work
 * itself is cheap (one history copy) and puzzle_gen_apply_level only acts when the level differs from the
 * cached set's. It never acts inside a bomb session (CLAUDE.md session guard): the refill waits for it. */
static void service_difficulty_level(void)
{
    AppState *const st = s_puzzle_state;
    Difficulty d;

    if (!app_state_read(st) || !bomb_config_difficulty(&d)) {
        return;                                    /* state or history busy: try again next pass */
    }
    (void)puzzle_gen_apply_level(d.level, st->session_active || game_ringing(*st));
}

/* LLM jobs: the puzzle run and the briefing. Together they do at most ONE LLM call per pass: the
 * briefing waits while a puzzle run is in progress, so the loop (timetable, results, alarm sync)
 * is back within one call's timeout. */
static void service_llm_jobs(const NetCtx &c, uint32_t now_ms, uint32_t unix_now)
{
    AppState *const st = s_puzzle_state;           /* PSRAM: ~1.3 KB that would stay on the stack during the TLS call */
    const bool gen_ok = app_state_read(st) && puzzle_calls_allowed(c, *st, unix_now);
    const bool net_ok = c.wifi_up && c.time_valid;
    const uint32_t unix_valid = c.time_valid ? unix_now : 0u;

    puzzle_gen_service(now_ms, unix_valid, net_ok, gen_ok);
    if (!puzzle_gen_busy()) {
        briefing_service(now_ms, unix_valid, net_ok, gen_ok);
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

/* ================= demo morning alarm ================= */
/* "demo <min>" (Serial) and POST /api/demo_alarm (web task, through the mailbox) end here. The same
 * checks as the web handler run again, on this task's own view of the schedule, because the state
 * can change between the click and this pass. Session guard: nothing is accepted while a bomb or a
 * session is running, so a demo never overwrites the alarm a snooze left on the MCX; the SET_ALARM
 * itself is sent by sync_with_mcx through the normal TIME_SYNC -> BOMB_CONFIG -> SET_ALARM path.
 * minutes 0 = cancel (the real alarm comes back, or a CANCEL is sent when there is none). */
/* The real timetable alarm as reschedule() would compute it now (0 = none). Read-only: it changes no state. */
static uint32_t real_alarm_now(const NetCtx &c, uint32_t unix_now)
{
    const int n = timetable_copy(s_lessons, SCHED_MAX_LESSONS);
    const NextAlarm a = next_alarm(s_lessons, n, unix_now, settings_get_lead_min(), c.consumed_class_start_unix);

    return a.found ? a.alarm_unix : 0u;
}

static void apply_demo_request(NetCtx &c, int minutes, uint32_t unix_now)
{
    AppState st;
    DemoCtx ctx;
    DemoErr err;
    char when[48];
    char note[APP_DEMO_MSG_LEN];

    if (minutes < 0 || minutes > APP_DEMO_ALARM_MAX_MIN || !app_state_read(&st)) {
        Serial.println("[demo] ignored: bad minutes or state busy");
        return;
    }
    ctx.link_up = st.link_up;
    ctx.bomb_running = game_ringing(st);
    ctx.session_active = st.session_active;
    ctx.time_valid = c.time_valid && unix_now != 0u;
    ctx.now = unix_now;
    ctx.alarm_is_demo = c.cur_is_demo || c.demo_alarm_unix != 0u;
    ctx.alarm_unix = c.demo_alarm_unix != 0u ? c.demo_alarm_unix : c.cur_alarm_unix;
    /* The authoritative real-alarm check. While a demo is current, cur_alarm_unix is the demo, so the real
     * alarm behind it is recomputed; otherwise cur_alarm_unix already is the real one. This is what stops
     * a REPLACING demo from running into the real alarm, which the web task cannot see. */
    ctx.real_alarm_unix = c.cur_is_demo ? real_alarm_now(c, unix_now) : c.cur_alarm_unix;
    err = web_demo_check(minutes, ctx);
    if (err != DEMO_OK) {
        web_demo_err_message(err, ctx.real_alarm_unix, note, sizeof(note));
        Serial.printf("[demo] refused: %s\n", note);
        app_state_set_demo_msg(note);                  /* the web reply is already sent: the page reads this */
        return;
    }
    app_state_set_demo_msg("");
    if (minutes == 0) {
        drop_demo(c);                                  /* the next reschedule restores the real alarm */
        Serial.println("[demo] cancelled");
        return;
    }
    c.demo_alarm_unix = sched_demo_alarm_at(unix_now, minutes, APP_DEMO_ALARM_SLACK_S);
    c.resched_pending = true;
    c.force_send = true;
    fmt_local(when, sizeof(when), c.demo_alarm_unix);
    Serial.printf("[demo] alarm requested for %s (unix %u)\n", when, (unsigned)c.demo_alarm_unix);
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

/* The line did not match any command: show what actually arrived (printable text and hex), so a
 * garbled USB-serial link (wrong line ending, altered bytes) is visible instead of just "help". */
static void print_unknown_command(const char *cmd, size_t len)
{
    char text[APP_CMD_ECHO_BYTES + 1];
    char hex[APP_CMD_ECHO_BYTES * 3 + 1];
    const size_t shown = (len < APP_CMD_ECHO_BYTES) ? len : APP_CMD_ECHO_BYTES;
    size_t i;

    for (i = 0; i < shown; i++) {
        const unsigned char b = (unsigned char)cmd[i];

        text[i] = (b >= 0x20u && b <= 0x7Eu) ? (char)b : '.';
        snprintf(&hex[i * 3u], 4u, i + 1u < shown ? "%02x " : "%02x", b);
    }
    text[shown] = '\0';
    hex[shown == 0u ? 0u : shown * 3u - 1u] = '\0';
    Serial.printf("[cmd] unknown: '%s%s' hex: %s%s\n", text, len > shown ? "..." : "", hex, len > shown ? " ..." : "");
}

static void run_command(NetCtx &c, const char *cmd, size_t len, uint32_t unix_now)
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
    } else if (strcmp(cmd, "i2c") == 0) {
        i2c_bus_report(false);
    } else if (strcmp(cmd, "mpu") == 0) {
#if APP_USE_GYRO
        gyro_task_dump_regs();
#else
        Serial.println("[cmd] gyro disabled (APP_USE_GYRO 0)");
#endif
    } else if (strcmp(cmd, "brief") == 0) {
        briefing_request();
        Serial.println("[cmd] briefing run requested");
    } else if (strncmp(cmd, "demo ", 5) == 0) {
        int minutes = 0;

        if (web_validate_demo_minutes(cmd + 5, &minutes) == DEMO_OK) {
            apply_demo_request(c, minutes, unix_now);
        } else {
            Serial.printf("[cmd] demo <1..%d> starts a demo alarm, demo 0 cancels it\n", APP_DEMO_ALARM_MAX_MIN);
        }
    } else {
        print_unknown_command(cmd, len);
        Serial.printf("[cmd] commands: lead <%d..%d> | link <url> | refresh | resched | hist | puzzles | ai | diff | brief | demo <0..%d> | i2c | mpu\n",
                      APP_LEAD_MIN_LOW, APP_LEAD_MIN_HIGH, APP_DEMO_ALARM_MAX_MIN);
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
                run_command(c, line, len, unix_now);
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

/* ================= requests from the web task, fault recovery ================= */
static bool regen_take(void)
{
    bool requested;

    taskENTER_CRITICAL(&s_req_mux);
    requested = s_regen_requested;
    s_regen_requested = false;
    taskEXIT_CRITICAL(&s_req_mux);
    return requested;
}

/* True and *minutes set when the web task left a demo-alarm request; the mailbox is emptied. */
static bool demo_take(int *minutes)
{
    bool requested;

    taskENTER_CRITICAL(&s_req_mux);
    requested = s_demo_requested;
    *minutes = s_demo_minutes;
    s_demo_requested = false;
    taskEXIT_CRITICAL(&s_req_mux);
    return requested;
}

/* "Regenerate puzzles" from the web page: the same entry as the serial command "puzzles".
 * "Demo alarm" from the web page: the same entry as the serial command "demo <min>". */
static void service_web_requests(NetCtx &c, uint32_t unix_now)
{
    int minutes = 0;

    if (regen_take()) {
        puzzle_gen_request();
        Serial.println("[web] puzzle run requested");
    }
    if (demo_take(&minutes)) {
        Serial.printf("[web] demo alarm requested: %d min\n", minutes);
        apply_demo_request(c, minutes, unix_now);
    }
}

/* The web page's simulated outage ended: forget the Firebase back-off and retry the timetable now,
 * instead of waiting out delays that were only growing because of the simulated outage. */
static void service_outage_end(NetCtx &c, uint32_t now_ms)
{
    static bool s_was_active;
    const bool active = fault_outage_active();

    if (s_was_active && !active) {
        mirror_wifi_up();
        if (c.tt_due) {
            c.tt_next_try_ms = now_ms;
        }
        Serial.println("[fault] simulated outage over: Firebase and timetable retry now");
    }
    s_was_active = active;
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
        service_web_requests(c, unix_valid);
        service_outage_end(c, now);
        handle_settings(c, now);
        handle_results(c, unix_valid);
        service_difficulty_level();
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

/* The level a new puzzle set is built for at boot: the mode, or the score of the stored mornings. */
static int initial_level(void)
{
    Difficulty d;

    (void)bomb_config_difficulty(&d);
    return d.level;
}

bool network_task_start(void)
{
    NetCtx &c = s_ctx;

    memset(&c, 0, sizeof(c));
    c.tt_due = true;                               /* fetch once at boot, as soon as Wi-Fi + time are valid */
    c.tt_last_nightly_day = -1;
    c.resched_pending = true;                      /* schedule from the flash cache as soon as the time is valid */
    s_lessons = mem_big_new<Lesson>(SCHED_MAX_LESSONS, "network lessons");
    s_puzzle_state = mem_big_new<AppState>(1u, "network app state");
    if (s_lessons == NULL || s_puzzle_state == NULL || !flash_store_init()) {
        Serial.println("[net] could not create the storage objects");
        return false;
    }
    settings_load_difficulty();                    /* RAM copy of the mode, before the first difficulty() call */
    if (!timetable_init() || !history_init() || !puzzle_gen_init(initial_level()) || !briefing_init()) {
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

void network_request_puzzles(void)
{
    taskENTER_CRITICAL(&s_req_mux);
    s_regen_requested = true;
    taskEXIT_CRITICAL(&s_req_mux);
}

void network_request_demo_alarm(int minutes)
{
    taskENTER_CRITICAL(&s_req_mux);
    s_demo_requested = true;                       /* a newer request replaces one that was not taken yet */
    s_demo_minutes = minutes;
    taskEXIT_CRITICAL(&s_req_mux);
}
