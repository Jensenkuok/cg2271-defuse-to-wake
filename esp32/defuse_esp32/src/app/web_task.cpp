/* Defuse to Wake - local web page task. See web_task.h. */
#include "web_task.h"

#include <ESPmDNS.h>
#include <WebServer.h>
#include <WiFi.h>
#include <string.h>
#include <time.h>

#include "app_state.h"
#include "bomb_config.h"
#include "config.h"
#include "display_task.h"
#include "fault.h"
#include "frame_log.h"
#include "gyro_task.h"
#include "history.h"
#include "link_task.h"
#include "network_task.h"
#include "puzzle_gen.h"
#include "settings.h"
#include "timetable.h"
#include "web_logic.h"
#include "web_page.h"

#define TIME_TEXT_LEN       48
#define LEAD_TEXT_MAX       15u
#define STATE_COUNT         5
#define WEB_REDUCED_LOG     8       /* frame-log rows kept if the full status does not fit the buffer */
#define WEB_REDUCED_HIST    7

/* All of these are used by this task only (handlers run inside handleClient), so they are static:
 * the 8 KB task stack would not hold them, and one copy is enough. */
static TaskHandle_t s_task;
static WebServer s_server(APP_WEB_PORT);
static char s_json[APP_WEB_STATUS_MAX];     /* the response being built */
static AppState s_st;
static ScreenModel s_model;
static PuzzleSet s_set;
static Lesson s_lessons[SCHED_MAX_LESSONS];
static BriefClass s_classes[BRIEF_CLASSES_MAX];
static FrameLogEntry s_flog[FRAME_LOG_LEN];
static HistRecord s_hist[HIST_LEN];
static char s_cfg_link[APP_LINK_MAX_CHARS + 1];   /* settings cache: re-read only when settings_version() moves */
static int s_cfg_lead;
static uint32_t s_cfg_version;
static bool s_cfg_valid;
static char s_in_link[APP_LINK_MAX_CHARS + 1];    /* the form value being validated */
static bool s_reduced;                            /* build_status retry: shorter frame log and history */

static bool s_server_started;
static bool s_mdns_up;
static bool s_wifi_prev;
static uint32_t s_ip_raw;
static uint32_t s_next_wifi_check_ms;

static const char *const k_state_names[STATE_COUNT] = { "IDLE", "ARMED", "BOMB_ACTIVE", "DEFUSED", "EXPLODED" };
static const char *const k_module_names[4] = { "-", "DIAL", "WIRES", "TRIPWIRE" };
static const char *const k_weekday[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };

/* ================= small helpers ================= */
/* "Mon 2026-10-19 07:12" in UTC+8; "" for 0. */
static void fmt_local(char *dst, size_t n, uint32_t unix_s)
{
    time_t at = (time_t)(unix_s + (uint32_t)APP_TZ_OFFSET_S);
    struct tm t;

    if (unix_s == 0u) {
        dst[0] = '\0';
        return;
    }
    gmtime_r(&at, &t);
    snprintf(dst, n, "%s %04d-%02d-%02d %02d:%02d", k_weekday[t.tm_wday % 7], t.tm_year + 1900, t.tm_mon + 1,
             t.tm_mday, t.tm_hour, t.tm_min);
}

/* "Mon 10-19 07:12" (history rows); "-" for 0. */
static void fmt_short(char *dst, size_t n, uint32_t unix_s)
{
    time_t at = (time_t)(unix_s + (uint32_t)APP_TZ_OFFSET_S);
    struct tm t;

    if (unix_s == 0u) {
        snprintf(dst, n, "-");
        return;
    }
    gmtime_r(&at, &t);
    snprintf(dst, n, "%s %02d-%02d %02d:%02d", k_weekday[t.tm_wday % 7], t.tm_mon + 1, t.tm_mday, t.tm_hour,
             t.tm_min);
}

static void send_json(int code, const char *body)
{
    s_server.sendHeader("Cache-Control", "no-store");
    s_server.sendHeader("X-Content-Type-Options", "nosniff");
    s_server.send_P(code, "application/json", body, strlen(body));   /* straight from the buffer: no String copy */
}

/* {"ok":<ok>,"<key>":"<text>"} */
static void reply(int code, bool ok, const char *key, const char *text)
{
    JsonBuf j;

    jb_init(&j, s_json, sizeof(s_json));
    jb_obj(&j);
    jb_kv_bool(&j, "ok", ok);
    jb_kv_str(&j, key, text);
    jb_end(&j);
    send_json(code, jb_ok(j) ? s_json : "{\"ok\":false,\"error\":\"internal\"}");
}

static void refresh_settings_cache(void)
{
    const uint32_t v = settings_version();

    if (s_cfg_valid && v == s_cfg_version) {
        return;
    }
    settings_get_link(s_cfg_link, sizeof(s_cfg_link));
    s_cfg_lead = settings_get_lead_min();
    s_cfg_version = v;
    s_cfg_valid = true;
}

/* ================= /api/status sections ================= */
static void put_module_value(JsonBuf *j, uint8_t module_type)
{
    jb_key(j, "value");
    jb_obj(j);
    if (module_type == PROTO_MODULE_DIAL) {
        jb_kv_uint(j, "digit", s_model.dial_digit);
        jb_kv_uint(j, "value", s_model.dial_value);
    } else if (module_type == PROTO_MODULE_WIRES) {
        jb_kv_uint(j, "cursor", s_model.wire_cursor);
        jb_kv_uint(j, "cut_mask", s_model.wire_cut_mask);
        jb_kv_uint(j, "wires", s_model.wire_count);
    } else if (module_type == PROTO_MODULE_TRIPWIRE) {
        jb_kv_uint(j, "cm", s_model.trip_cm);
        jb_kv_uint(j, "hold_pct", s_model.trip_hold_pct);
        jb_kv_uint(j, "min", s_model.trip_min_cm);
        jb_kv_uint(j, "max", s_model.trip_max_cm);
    }
    jb_end(j);
}

static void put_game(JsonBuf *j)
{
    const proto_game_state_t &g = s_st.game;
    const uint8_t state = (s_st.game_valid && g.state < STATE_COUNT) ? g.state : 0u;
    const bool active = s_st.game_valid && state == PROTO_STATE_BOMB_ACTIVE;
    const uint8_t type = (s_st.game_valid && g.module_type < 4u) ? g.module_type : 0u;

    jb_key(j, "game");
    jb_obj(j);
    jb_kv_bool(j, "valid", s_st.game_valid);
    jb_kv_str(j, "state", k_state_names[state]);
    jb_kv_uint(j, "state_id", state);
    jb_kv_str(j, "module", k_module_names[type]);
    jb_kv_uint(j, "module_id", type);
    jb_kv_uint(j, "idx", g.module_idx);
    jb_kv_uint(j, "total", active ? modules_total(g.module_idx, g.progress_pct) : 0u);
    jb_kv_str(j, "timer", s_model.timer_text);
    jb_kv_uint(j, "time_left_s", g.time_left_s);
    jb_kv_uint(j, "strikes", g.strikes);
    jb_kv_uint(j, "max_strikes", s_model.max_strikes);
    jb_kv_uint(j, "progress_pct", g.progress_pct);
    jb_kv_uint(j, "ver", s_st.game_version);
    if (active) {
        put_module_value(j, type);
    } else {
        jb_key(j, "value");
        jb_obj(j);
        jb_end(j);
    }
    jb_kv_str(j, "puzzle", s_model.puzzle_text);    /* only filled by build_model while a bomb is active */
    jb_kv_str(j, "hint", s_model.hint);
    jb_end(j);
}

static void put_stats(JsonBuf *j)
{
    const proto_link_stats_t &s = s_st.link_stats;

    jb_key(j, "stats");
    jb_obj(j);
    jb_kv_uint(j, "tx", s.tx_frames);
    jb_kv_uint(j, "rx", s.rx_frames);
    jb_kv_uint(j, "retx", s.retransmits);
    jb_kv_uint(j, "fail", s.send_failures);
    jb_kv_uint(j, "crc", s.crc_errors);
    jb_kv_uint(j, "len", s.len_errors);
    jb_kv_uint(j, "dup", s.dup_drops);
    jb_kv_uint(j, "nack", s.nacks_sent);
    jb_kv_uint(j, "bp", s_st.tx_backpressure);
    jb_end(j);
}

static void put_link(JsonBuf *j, uint32_t now_ms)
{
    const int n = link_frame_log_copy(s_flog, s_reduced ? WEB_REDUCED_LOG : FRAME_LOG_LEN);
    int32_t age;
    int i;

    jb_key(j, "link");
    jb_obj(j);
    jb_kv_bool(j, "up", s_st.link_up);
    jb_kv_uint(j, "ups", s_st.link_up_count);
    jb_kv_bool(j, "corrupt_armed", fault_corrupt_armed());
    put_stats(j);
    jb_key(j, "log");
    jb_arr(j);
    for (i = 0; i < n; i++) {                       /* newest first */
        jb_obj(j);
        age = (int32_t)(now_ms - s_flog[i].ms);          /* an entry logged after now_ms was read is "just now" */
        jb_kv_uint(j, "ago", age < 0 ? 0u : (uint32_t)age);
        jb_kv_str(j, "dir", frame_dir_name(s_flog[i].dir));
        jb_kv_str(j, "type", frame_type_name(s_flog[i].type));
        jb_kv_uint(j, "seq", s_flog[i].seq);
        jb_kv_uint(j, "len", s_flog[i].len);
        jb_kv_str(j, "out", frame_outcome_name(s_flog[i].outcome));
        jb_end(j);
    }
    jb_end(j);
    jb_end(j);
}

static void put_net(JsonBuf *j, uint32_t unix_now)
{
    char when[TIME_TEXT_LEN];

    fmt_local(when, sizeof(when), unix_now);
    jb_key(j, "net");
    jb_obj(j);
    jb_kv_bool(j, "wifi", s_st.wifi_up);
    jb_kv_str(j, "ip", s_st.wifi_up ? s_st.ip : "");
    jb_kv_bool(j, "time_valid", s_st.time_valid);
    jb_kv_uint(j, "unix", unix_now);
    jb_kv_str(j, "now_local", when);
    jb_kv_uint(j, "outage_s", fault_outage_left_s());
    jb_end(j);
}

static void put_alarm(JsonBuf *j, uint32_t unix_now)
{
    char when[TIME_TEXT_LEN];
    char cls[TIME_TEXT_LEN];
    const bool valid = s_st.next_valid && s_st.alarm_unix != 0u;

    fmt_local(when, sizeof(when), valid ? s_st.alarm_unix : 0u);
    fmt_local(cls, sizeof(cls), valid ? s_st.next_start_unix : 0u);
    jb_key(j, "alarm");
    jb_obj(j);
    jb_kv_bool(j, "valid", valid);
    jb_kv_uint(j, "unix", valid ? s_st.alarm_unix : 0u);
    jb_kv_str(j, "local", when);
    jb_kv_str(j, "class_local", cls);
    jb_kv_uint(j, "in_s", (valid && unix_now != 0u && s_st.alarm_unix > unix_now) ? s_st.alarm_unix - unix_now : 0u);
    jb_kv_str(j, "module", valid ? s_st.next_module : "");
    jb_kv_str(j, "type", valid ? s_st.next_type : "");
    jb_kv_str(j, "venue", valid ? s_st.next_venue : "");
    jb_end(j);
}

static void put_today(JsonBuf *j, uint32_t unix_now)
{
    char date[TIME_TEXT_LEN];
    char start[16];
    int n = 0;
    int i;

    date[0] = '\0';
    if (unix_now != 0u) {
        time_t at = (time_t)(unix_now + (uint32_t)APP_TZ_OFFSET_S);
        struct tm t;

        gmtime_r(&at, &t);
        snprintf(date, sizeof(date), "%s %04d-%02d-%02d", k_weekday[t.tm_wday % 7], t.tm_year + 1900, t.tm_mon + 1,
                 t.tm_mday);
        n = classes_on_day(s_lessons, timetable_copy(s_lessons, SCHED_MAX_LESSONS), t.tm_year + 1900, t.tm_mon + 1,
                           t.tm_mday, s_classes, BRIEF_CLASSES_MAX);
    }
    jb_key(j, "today");
    jb_obj(j);
    jb_kv_str(j, "date", date);
    jb_key(j, "classes");
    jb_arr(j);
    for (i = 0; i < n; i++) {
        snprintf(start, sizeof(start), "%02d:%02d", s_classes[i].start_min / 60, s_classes[i].start_min % 60);
        jb_obj(j);
        jb_kv_str(j, "start", start);
        jb_kv_str(j, "module", s_classes[i].module);
        jb_kv_str(j, "type", s_classes[i].type);
        jb_kv_str(j, "venue", s_classes[i].venue);
        jb_end(j);
    }
    jb_end(j);
    jb_end(j);
}

static void put_history(JsonBuf *j, uint32_t unix_now)
{
    const int n = history_recent(s_hist, s_reduced ? WEB_REDUCED_HIST : HIST_LEN);
    char when[TIME_TEXT_LEN];
    int i;

    jb_key(j, "history");
    jb_obj(j);
    jb_kv_int(j, "streak", history_current_streak(unix_now));
    jb_kv_int(j, "count", n);
    jb_key(j, "records");
    jb_arr(j);
    for (i = 0; i < n; i++) {                       /* newest first */
        fmt_short(when, sizeof(when), s_hist[i].end_unix);
        jb_obj(j);
        jb_kv_str(j, "when", when);
        jb_kv_uint(j, "dur", s_hist[i].duration_s);
        jb_kv_uint(j, "strikes", s_hist[i].strikes);
        jb_kv_uint(j, "snoozes", s_hist[i].snoozes);
        jb_kv_uint(j, "expl", s_hist[i].explosions);
        jb_kv_str(j, "module", s_hist[i].module);
        jb_end(j);
    }
    jb_end(j);
    jb_end(j);
}

/* One cached puzzle. While answer_hidden() (MCX armed, bomb session running, or state unknown) NO
 * answer is shown: the pair the MCX holds is marked ACTIVE, and the other pairs are the ones later
 * bombs of the same session will use, so showing them would give the next answers away. In IDLE the
 * engineering view shows every answer. */
static void put_puzzle(JsonBuf *j, const Puzzle &p, char tag, unsigned idx, bool is_active_pair)
{
    const bool hide = answer_hidden(s_st.game_valid, s_st.game.state, s_st.session_active);
    char id[3] = { tag, (char)('0' + idx), '\0' };
    char ans[PUZZLE_ANSWER_LEN];

    if (!puzzle_answer_text(p, ans, sizeof(ans))) {
        snprintf(ans, sizeof(ans), "?");
    }
    jb_obj(j);
    jb_kv_str(j, "id", id);
    jb_kv_str(j, "text", p.text);
    jb_kv_str(j, "src", p.source == PUZZLE_SRC_AI ? "AI" : "BANK");
    jb_kv_str(j, "setter", p.setter);
    jb_kv_str(j, "checker", p.checker);
    jb_kv_str(j, "said", p.checker_answer);
    jb_kv_bool(j, "ok", p.verified != 0u);
    jb_kv_uint(j, "tries", p.attempts);
    jb_kv_uint(j, "s_ms", p.setter_ms);
    jb_kv_uint(j, "c_ms", p.checker_ms);
    jb_kv_uint(j, "wires", p.kind == PUZZLE_WIRE ? p.wire_count : 0u);
    jb_kv_bool(j, "active", is_active_pair);
    jb_kv_str(j, "answer", hide ? "hidden" : ans);
    jb_end(j);
}

static void put_ai_header(JsonBuf *j, uint32_t gen)
{
    PuzzleRunInfo run;
    char when[TIME_TEXT_LEN];

    puzzle_gen_info(&run);
    fmt_local(when, sizeof(when), s_set.generated_unix);
    jb_kv_uint(j, "gen", gen);
    jb_kv_str(j, "generated", when);
    jb_kv_uint(j, "ai_count", s_set.ai_count);
    jb_kv_uint(j, "total", 2u * PUZZLE_PAIRS);
    jb_kv_bool(j, "busy", run.busy);
    jb_kv_bool(j, "queued", run.queued);
    jb_kv_uint(j, "progress", run.progress);
    jb_kv_uint(j, "runs", run.runs_done);
    jb_key(j, "last");
    jb_obj(j);
    jb_kv_bool(j, "valid", run.runs_done > 0u);
    jb_kv_uint(j, "ai", run.last_ai);
    jb_kv_bool(j, "installed", run.last_installed);
    jb_end(j);
}

static void put_ai(JsonBuf *j)
{
    uint32_t gen = 0u;
    unsigned i;
    const bool have = puzzle_set_snapshot(&s_set, &gen);
    const int active_pair = (s_st.active.valid && s_st.active.set_gen == gen) ? (int)s_st.active.pair : -1;

    jb_key(j, "ai");
    if (!have) {
        jb_null(j);                                 /* set busy: the page keeps the previous picture */
        return;
    }
    jb_obj(j);
    put_ai_header(j, gen);
    jb_key(j, "puzzles");
    jb_arr(j);
    for (i = 0u; i < PUZZLE_PAIRS; i++) {
        put_puzzle(j, s_set.dial[i], 'D', i, active_pair == (int)i);
    }
    for (i = 0u; i < PUZZLE_PAIRS; i++) {
        put_puzzle(j, s_set.wire[i], 'W', i, active_pair == (int)i);
    }
    jb_end(j);
    jb_end(j);
}

static void put_diff_brief(JsonBuf *j, uint32_t unix_now)
{
    Difficulty d;
    const BriefText *bt = (unix_now != 0u) ? brief_pick(s_st.brief, s_st.brief_today, local_day(unix_now)) : NULL;

    bomb_config_difficulty(&d);
    jb_key(j, "diff");
    jb_obj(j);
    jb_kv_uint(j, "modules", d.module_count);
    jb_kv_uint(j, "time_s", d.time_limit_s);
    jb_kv_str(j, "why", d.explanation);
    jb_end(j);
    jb_key(j, "brief");
    jb_obj(j);
    jb_kv_str(j, "briefing", bt != NULL ? bt->briefing : APP_BRIEF_GENERIC);
    jb_kv_str(j, "coach", bt != NULL ? bt->coach : APP_COACH_GENERIC);
    jb_kv_str(j, "src", bt == NULL ? "generic" : (bt == &s_st.brief ? "stored" : "template"));
    jb_end(j);
}

static void put_stack(JsonBuf *j, const char *name, TaskHandle_t h)
{
    jb_kv_uint(j, name, h != NULL ? (uint32_t)uxTaskGetStackHighWaterMark(h) : 0u);
}

static void put_system(JsonBuf *j, uint32_t now_ms)
{
    jb_key(j, "fb");
    jb_obj(j);
    jb_kv_uint(j, "pending", s_st.mirror_pending);
    jb_end(j);
    jb_key(j, "sys");
    jb_obj(j);
    jb_kv_uint(j, "heap", ESP.getFreeHeap());
    jb_kv_uint(j, "heap_min", ESP.getMinFreeHeap());
    jb_kv_uint(j, "lock_to", app_state_lock_timeouts());
    jb_key(j, "stack");                             /* bytes never used so far, per task */
    jb_obj(j);
    put_stack(j, "link", link_task_handle());
    put_stack(j, "gyro", gyro_task_handle());
    put_stack(j, "display", display_task_handle());
    put_stack(j, "net", network_task_handle());
    put_stack(j, "web", s_task);
    jb_end(j);
    jb_end(j);
    jb_kv_uint(j, "up_s", now_ms / 1000u);
}

/* Only the lead time and a version go into every poll; the 512-character link is fetched with
 * GET /api/settings when the version moves (keeps the worst-case status under 8 KB). */
static void put_settings(JsonBuf *j)
{
    refresh_settings_cache();
    jb_key(j, "settings");
    jb_obj(j);
    jb_kv_int(j, "lead_min", s_cfg_lead);
    jb_kv_uint(j, "ver", s_cfg_version);
    jb_end(j);
}

/* The whole status object into j. False if a lock timed out or the buffer was too small. */
static bool build_status(JsonBuf *j)
{
    const uint32_t now_ms = millis();
    uint32_t unix_now;

    if (!app_state_read(&s_st)) {
        return false;
    }
    unix_now = s_st.time_valid ? (uint32_t)time(nullptr) : 0u;
    build_model(s_st, now_ms, unix_now, 0u, &s_model);   /* the same decoding the OLED uses */
    jb_obj(j);
    put_game(j);
    put_link(j, now_ms);
    put_net(j, unix_now);
    put_alarm(j, unix_now);
    put_today(j, unix_now);
    put_history(j, unix_now);
    put_ai(j);
    put_diff_brief(j, unix_now);
    put_system(j, now_ms);
    put_settings(j);
    jb_end(j);
    return jb_ok(*j);
}

/* ================= handlers ================= */
static void handle_root(void)
{
    s_server.sendHeader("Cache-Control", "no-store");
    s_server.send_P(200, "text/html; charset=utf-8", WEB_PAGE, sizeof(WEB_PAGE) - 1u);
}

static void handle_status(void)
{
    JsonBuf j;

    s_reduced = false;
    jb_init(&j, s_json, sizeof(s_json));
    if (!build_status(&j) && j.overflow) {
        /* Worst case (every field at its maximum) is just under the buffer; if it ever does not fit,
         * answer with a shorter log and history instead of failing the whole page. */
        s_reduced = true;
        jb_init(&j, s_json, sizeof(s_json));
        (void)build_status(&j);
    }
    if (!jb_ok(j)) {
        Serial.printf("[web] status not built (buffer %u of %u bytes used)\n", (unsigned)j.len, (unsigned)j.cap);
        send_json(503, "{\"ok\":false,\"error\":\"status unavailable\"}");
        return;
    }
    send_json(200, s_json);
}

/* {"ok":true,"nusmods_link":"..","lead_min":N}: the stored values (also the answer to a save). */
static void send_settings(void)
{
    JsonBuf j;

    refresh_settings_cache();
    jb_init(&j, s_json, sizeof(s_json));
    jb_obj(&j);
    jb_kv_bool(&j, "ok", true);
    jb_kv_str(&j, "nusmods_link", s_cfg_link);
    jb_kv_int(&j, "lead_min", s_cfg_lead);
    jb_end(&j);
    send_json(jb_ok(j) ? 200 : 500, jb_ok(j) ? s_json : "{\"ok\":false,\"error\":\"internal\"}");
}

/* Every POST must carry X-Requested-With (the page's fetch adds it). A page on another site cannot
 * send a custom header without a CORS preflight, which this server never grants, so a hostile page
 * cannot press the buttons or change the settings from the user's browser. */
static bool post_allowed(void)
{
    if (s_server.hasHeader("X-Requested-With")) {
        return true;
    }
    reply(403, false, "error", "forbidden");
    return false;
}

static void handle_settings(void)
{
    const String &link_arg = s_server.arg("nusmods_link");
    const String &lead_arg = s_server.arg("lead_min");
    char lead_text[LEAD_TEXT_MAX + 1];
    SettingsErr err;
    int lead = 0;

    if (!post_allowed()) {
        return;
    }
    if (!s_server.hasArg("nusmods_link") || !s_server.hasArg("lead_min")) {
        reply(400, false, "error", "nusmods_link and lead_min are required");
        return;
    }
    if (link_arg.length() > (unsigned)APP_LINK_MAX_CHARS) {
        err = SETTINGS_ERR_LINK_TOO_LONG;
    } else if (lead_arg.length() > LEAD_TEXT_MAX) {
        err = SETTINGS_ERR_LEAD_NOT_NUMBER;
    } else {
        snprintf(s_in_link, sizeof(s_in_link), "%s", link_arg.c_str());
        snprintf(lead_text, sizeof(lead_text), "%s", lead_arg.c_str());
        err = web_validate_settings(s_in_link, lead_text, &lead);
    }
    if (err != SETTINGS_OK) {
        reply(400, false, "error", web_settings_err_text(err));
        return;
    }
    if (!settings_set_link(s_in_link) || settings_set_lead_min(lead) != lead) {
        reply(500, false, "error", "could not store the settings in flash");
        return;
    }
    Serial.printf("[web] settings saved: lead %d min\n", lead);   /* the link is not logged: it can be 500 characters */
    send_settings();
}

static void handle_ring(void)
{
    if (!post_allowed()) {
        return;
    }
    if (!app_state_read(&s_st)) {
        reply(503, false, "error", "state busy, try again");
    } else if (!s_st.link_up) {
        reply(409, false, "error", "the MCX link is down");
    } else if (game_ringing(s_st)) {
        reply(409, false, "error", "a bomb is already running");
    } else {
        link_request_demo_ring();
        Serial.println("[web] demo ring requested");
        reply(200, true, "msg", "Ringing: the MCX arms now and the bomb starts in 3 s");
    }
}

static void handle_corrupt(void)
{
    if (!post_allowed()) {
        return;
    }
    fault_corrupt_arm();
    reply(200, true, "msg", "Armed: the next reliable frame to the MCX gets a bad CRC (press Ring now)");
}

static void handle_outage(void)
{
    if (!post_allowed()) {
        return;
    }
    fault_outage_start();
    reply(200, true, "msg", "Simulated internet outage for 60 s");
}

static void handle_puzzles(void)
{
    if (!post_allowed()) {
        return;
    }
    network_request_puzzles();
    reply(200, true, "msg", "queued");
}

static void handle_not_found(void)
{
    reply(404, false, "error", "not found");
}

static void handle_favicon(void)
{
    s_server.send(204, "text/plain", "");
}

/* ================= Wi-Fi follower: server + mDNS ================= */
static void ip_text(char *dst, size_t n, uint32_t raw)
{
    snprintf(dst, n, "%u.%u.%u.%u", (unsigned)(raw & 0xFFu), (unsigned)((raw >> 8) & 0xFFu),
             (unsigned)((raw >> 16) & 0xFFu), (unsigned)((raw >> 24) & 0xFFu));
}

/* Wi-Fi came up (or its address changed): publish the address and (re)start mDNS. The listening
 * socket is created once, on the first connection, and survives Wi-Fi drops. */
static void wifi_came_up(uint32_t ip_raw)
{
    char ip[APP_IP_TEXT_LEN];

    ip_text(ip, sizeof(ip), ip_raw);
    app_state_set_ip(ip);
    if (!s_server_started) {
        s_server.begin();
        s_server_started = true;
    }
    if (s_mdns_up) {
        MDNS.end();
        s_mdns_up = false;
    }
    if (MDNS.begin(APP_MDNS_HOSTNAME)) {
        MDNS.addService("http", "tcp", APP_WEB_PORT);
        s_mdns_up = true;
    }
    Serial.printf("[web] page at http://%s.local or http://%s (mDNS %s)\n", APP_MDNS_HOSTNAME, ip,
                  s_mdns_up ? "on" : "failed");
}

static void service_wifi(uint32_t now_ms)
{
    uint32_t raw = 0u;
    bool up;

    if ((int32_t)(now_ms - s_next_wifi_check_ms) < 0) {
        return;
    }
    s_next_wifi_check_ms = now_ms + APP_WEB_WIFI_POLL_MS;
    up = (WiFi.status() == WL_CONNECTED);
    if (up) {
        raw = (uint32_t)WiFi.localIP();
    }
    if (up && (!s_wifi_prev || raw != s_ip_raw)) {
        wifi_came_up(raw);
        s_ip_raw = raw;
    } else if (!up && s_wifi_prev) {
        app_state_set_ip("");
        s_ip_raw = 0u;
    }
    s_wifi_prev = up;
}

static void web_task(void *arg)
{
    (void)arg;
    for (;;) {
        service_wifi(millis());
        if (s_server_started) {
            s_server.handleClient();
        }
        vTaskDelay(pdMS_TO_TICKS(APP_WEB_LOOP_MS));
    }
}

bool web_task_start(void)
{
    static const char *k_headers[] = { "X-Requested-With" };   /* collectHeaders wants a non-const array */

    s_server.collectHeaders(k_headers, 1u);
    s_server.on("/", HTTP_GET, handle_root);
    s_server.on("/favicon.ico", HTTP_GET, handle_favicon);
    s_server.on("/api/status", HTTP_GET, handle_status);
    s_server.on("/api/settings", HTTP_GET, send_settings);
    s_server.on("/api/settings", HTTP_POST, handle_settings);
    s_server.on("/api/ring", HTTP_POST, handle_ring);
    s_server.on("/api/fault/corrupt", HTTP_POST, handle_corrupt);
    s_server.on("/api/fault/outage", HTTP_POST, handle_outage);
    s_server.on("/api/puzzles", HTTP_POST, handle_puzzles);
    s_server.onNotFound(handle_not_found);
    return xTaskCreate(web_task, "web", APP_STACK_WEB, NULL, APP_PRIO_WEB, &s_task) == pdPASS;
}

TaskHandle_t web_task_handle(void)
{
    return s_task;
}
