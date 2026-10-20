/*
 * Defuse to Wake - pure helpers of the local web page (no Arduino headers, no heap, no globals;
 * compiled by the sketch and by the host tests, tools/test_web.cpp, C++11):
 *   - validation of the settings form (NUSMods link, wake-up lead),
 *   - the "simulated internet outage" expiry rule,
 *   - a small bounded JSON writer + string escaper for /api/status (fixed buffer, no String),
 *   - whether the active puzzle's answer may be shown, and the bomb's module total.
 */
#ifndef WEB_LOGIC_H
#define WEB_LOGIC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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
    SETTINGS_ERR_LEAD_RANGE
};

/* link: at most APP_LINK_MAX_CHARS characters, starts with WEB_LINK_PREFIX, printable ASCII without
 * spaces, and parse_share_link() finds at least one module slot in it. lead_text: decimal digits
 * (surrounding blanks allowed) within APP_LEAD_MIN_LOW..APP_LEAD_MIN_HIGH; it is rejected, never
 * clamped. On SETTINGS_OK *lead_out holds the number (lead_out may be NULL). */
SettingsErr web_validate_settings(const char *link, const char *lead_text, int *lead_out);

/* Message for the 400 response, e.g. "lead_min must be between 15 and 180". */
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

/* ================= game panel helpers ================= */
/* May the web page show the ACTIVE puzzle's answer? No while the MCX is ARMED or BOMB_ACTIVE, while
 * a bomb session is running (EXPLODED / DEFUSED holds), or while the MCX state is unknown. */
bool answer_hidden(bool game_valid, uint8_t mcx_state, bool session_active);

/* Modules in the running bomb, recovered from GAME_STATE: progress_pct = cleared * 100 / total with
 * cleared == module_idx. 0 = cannot tell (module_idx 0 or an inconsistent progress). */
uint8_t modules_total(uint8_t module_idx, uint8_t progress_pct);

#endif /* WEB_LOGIC_H */
