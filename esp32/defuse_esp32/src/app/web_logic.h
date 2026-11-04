/*
 * Defuse to Wake - pure helpers of the local web page (no Arduino headers, no heap, no globals;
 * compiled by the sketch and by the host tests, tools/test_web.cpp, C++11):
 *   - validation of the settings form (NUSMods link, wake-up lead, difficulty mode),
 *   - the "simulated internet outage" expiry rule,
 *   - a small bounded JSON writer + string escaper for /api/status (fixed buffer, no String),
 *   - the /api/timetable response (weeks mask -> list, HH:MM, whole-object writer),
 *   - whether the active puzzle's answer may be shown, and the bomb's module total.
 */
#ifndef WEB_LOGIC_H
#define WEB_LOGIC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "schedule.h"

/* ================= settings form ================= */
#define WEB_LINK_PREFIX     "https://nusmods.com/timetable/"

enum SettingsErr {
    SETTINGS_OK = 0,
    SETTINGS_ERR_LINK_MISSING,
    SETTINGS_ERR_LINK_TOO_LONG,
    SETTINGS_ERR_LINK_PREFIX,
    SETTINGS_ERR_LINK_CHARS,
    SETTINGS_ERR_LINK_NO_SLOTS,
    SETTINGS_ERR_LEAD_MISSING,
    SETTINGS_ERR_LEAD_NOT_NUMBER,
    SETTINGS_ERR_LEAD_RANGE,
    SETTINGS_ERR_DIFFICULTY
};

/* link: at most APP_LINK_MAX_CHARS characters, starts with WEB_LINK_PREFIX, printable ASCII without
 * spaces, and parse_share_link() finds at least one module slot in it. lead_text: decimal digits
 * (surrounding blanks allowed) within APP_LEAD_MIN_LOW..APP_LEAD_MIN_HIGH; it is rejected, never
 * clamped. On SETTINGS_OK *lead_out holds the number (lead_out may be NULL). */
SettingsErr web_validate_settings(const char *link, const char *lead_text, int *lead_out);

/* The difficulty field: exactly one of "auto" "easy" "normal" "hard" (difficulty.h ids, lowercase, no
 * blanks); anything else is SETTINGS_ERR_DIFFICULTY. On SETTINGS_OK *mode_out is the DiffMode number
 * (mode_out may be NULL). The field is optional in the request: the caller validates it only when present. */
SettingsErr web_validate_difficulty(const char *text, int *mode_out);

/* Message for the 400 response, e.g. "lead_min must be between <LOW> and <HIGH>" (built from
 * APP_LEAD_MIN_LOW / APP_LEAD_MIN_HIGH). */
const char *web_settings_err_text(SettingsErr err);

/* ================= simulated outage ================= */
/* Set by the web page's fault button, read before every HTTPS call. All comparisons are
 * wraparound-safe (millis() wraps after 49 days). */
struct Outage {
    bool active;
    uint32_t until_ms;
};

void outage_start(Outage *o, uint32_t now_ms, uint32_t duration_ms);
bool outage_active(const Outage &o, uint32_t now_ms);
/* Whole seconds left, rounded up (0 when over or never started). */
uint32_t outage_left_s(const Outage &o, uint32_t now_ms);

/* ================= JSON writer ================= */
/* Writes into a caller buffer. Once the buffer is full or the nesting is wrong, `overflow` is set,
 * everything after it is dropped, and the buffer stays NUL-terminated. Commas are managed here. */
#define JSON_MAX_DEPTH 8

struct JsonBuf {
    char *buf;
    size_t cap;
    size_t len;
    bool overflow;
    bool after_key;
    uint8_t depth;
    char close[JSON_MAX_DEPTH];       /* '}' or ']' per open level */
    bool has_item[JSON_MAX_DEPTH];
};

void jb_init(JsonBuf *j, char *buf, size_t cap);
bool jb_ok(const JsonBuf &j);          /* no overflow and every object/array closed */

void jb_obj(JsonBuf *j);               /* { as a value (array element, after a key, or the root) */
void jb_arr(JsonBuf *j);
void jb_end(JsonBuf *j);               /* closes the innermost object / array */
void jb_key(JsonBuf *j, const char *key);

void jb_str(JsonBuf *j, const char *s);   /* escaped, quoted; NULL = "" */
void jb_uint(JsonBuf *j, uint32_t v);
void jb_int(JsonBuf *j, int32_t v);
void jb_bool(JsonBuf *j, bool v);
void jb_null(JsonBuf *j);

void jb_kv_str(JsonBuf *j, const char *key, const char *s);
void jb_kv_uint(JsonBuf *j, const char *key, uint32_t v);
void jb_kv_int(JsonBuf *j, const char *key, int32_t v);
void jb_kv_bool(JsonBuf *j, const char *key, bool v);

/* Escape s for a JSON string (no quotes): " \ and control characters become escapes, < > & become
 * < > &, bytes >= 0x80 become '?'. out is NUL-terminated. Returns false if it did
 * not fit (out then holds the truncated text). */
bool json_escape(const char *s, char *out, size_t out_size);

/* ================= timetable response (/api/timetable) ================= */
#define WEB_TT_WEEKS_MAX   13u    /* teaching weeks 1..13: bit (w - 1) of Lesson.weeks */

/* Teaching weeks set in a Lesson.weeks mask, ascending, written to out (room for WEB_TT_WEEKS_MAX).
 * Bits above week 13 are ignored. Returns how many. */
uint8_t web_weeks_list(uint16_t mask, uint8_t *out);

/* "HH:MM" from minutes after midnight (clamped to 23:59) into out (at least 6 bytes). */
void web_fmt_hhmm(uint16_t minutes, char *out, size_t out_size);

/* Writes the whole object {"ok":true,"week":N,"fetched_local":"..","lessons":[..]} into j (which must be
 * freshly initialised). Lessons whose weekday is outside 1..6 are skipped. An answer that does not fit sets
 * j->overflow: the caller must then refuse (jb_ok false), never send the cut-off text. */
void web_put_timetable(JsonBuf *j, const Lesson *lessons, int count, int week, const char *fetched_local);

/* ================= demo morning alarm (POST /api/demo_alarm) ================= */
enum DemoErr {
    DEMO_OK = 0,
    /* the value: the handler answers 400 */
    DEMO_ERR_MISSING,
    DEMO_ERR_NOT_NUMBER,
    DEMO_ERR_RANGE,
    /* the device state: the handler answers 409 */
    DEMO_ERR_LINK_DOWN,
    DEMO_ERR_BUSY,            /* a bomb is running or a session is active (the MCX owns the alarm) */
    DEMO_ERR_NO_TIME,
    DEMO_ERR_REAL_FIRST,      /* the real alarm rings before the requested demo time */
    DEMO_ERR_NO_DEMO,         /* cancel with no demo pending */
    DEMO_ERR_CANCEL_LATE,     /* cancel within SCHED_MIN_LEAD_S of ringing: the scheduler would ignore it */
    DEMO_ERR_REPLACE_LATE     /* same for replacing a demo that is about to ring */
};

/* The "minutes" form field: decimal digits (surrounding blanks allowed), 0 = cancel, otherwise
 * 1..APP_DEMO_ALARM_MAX_MIN. Rejected, never clamped. On DEMO_OK *minutes_out holds the number. */
DemoErr web_validate_demo_minutes(const char *text, int *minutes_out);

/* What the handler knows about the device (a fresh copy of AppState). */
struct DemoCtx {
    bool link_up;
    bool bomb_running;        /* game_ringing(): BOMB_ACTIVE / DEFUSED / EXPLODED hold */
    bool session_active;
    bool time_valid;
    uint32_t now;             /* unix seconds */
    uint32_t alarm_unix;      /* the alarm currently scheduled, 0 = none */
    bool alarm_is_demo;       /* ...and it is a demo alarm */
};

/* Whether a validated request may go to the network task. minutes 0 = cancel. Checks, in this order:
 * link, bomb/session, clock, then for a start: a real alarm that is not later than the demo time
 * (it would ring first), a demo about to ring (cannot be replaced); for a cancel: a pending demo that
 * is not within SCHED_MIN_LEAD_S of ringing. */
DemoErr web_demo_check(int minutes, const DemoCtx &ctx);

/* Message for the response. DEMO_ERR_REAL_FIRST has no time here: the handler writes its own text. */
const char *web_demo_err_text(DemoErr err);

/* ================= game panel helpers ================= */
/* May the web page show the ACTIVE puzzle's answer? No while the MCX is ARMED or BOMB_ACTIVE, while
 * a bomb session is running (EXPLODED / DEFUSED holds), or while the MCX state is unknown. */
bool answer_hidden(bool game_valid, uint8_t mcx_state, bool session_active);

/* Modules in the running bomb, recovered from GAME_STATE: progress_pct = cleared * 100 / total with
 * cleared == module_idx. 0 = cannot tell (module_idx 0 or an inconsistent progress). */
uint8_t modules_total(uint8_t module_idx, uint8_t progress_pct);

#endif /* WEB_LOGIC_H */
