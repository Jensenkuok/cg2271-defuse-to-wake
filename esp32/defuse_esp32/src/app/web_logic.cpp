/* Defuse to Wake - pure helpers of the local web page. See web_logic.h. */
#include "web_logic.h"

#include <stdio.h>
#include <string.h>

#include "config.h"
#include "difficulty.h"
#include "schedule.h"
#include "../shared/protocol.h"

#define OUTAGE_MAX_MS     0x7FFFFFFFu   /* wraparound-safe comparisons need a duration below 2^31 */
#define MS_PER_S          1000u
#define ESC_BUF           8u
#define STR_(x)           #x
#define STR(x)            STR_(x)   /* expands the macro first, so the text follows config.h */
#define MINUTES_PER_HOUR  60u
#define LAST_MINUTE_OF_DAY 1439u

/* ================= settings form ================= */
static bool is_blank(char c)
{
    return c == ' ' || c == '\t';
}

static SettingsErr check_link(const char *link)
{
    static ShareLink parsed;          /* ~630 bytes: static keeps it off the web task's stack (single caller) */
    const size_t prefix_len = strlen(WEB_LINK_PREFIX);
    size_t len;
    size_t i;

    if (link == NULL || link[0] == '\0') {
        return SETTINGS_ERR_LINK_MISSING;
    }
    len = strlen(link);
    if (len > (size_t)APP_LINK_MAX_CHARS) {
        return SETTINGS_ERR_LINK_TOO_LONG;
    }
    if (strncmp(link, WEB_LINK_PREFIX, prefix_len) != 0) {
        return SETTINGS_ERR_LINK_PREFIX;
    }
    for (i = 0; i < len; i++) {
        if ((unsigned char)link[i] < 0x21u || (unsigned char)link[i] > 0x7Eu) {
            return SETTINGS_ERR_LINK_CHARS;       /* spaces, control characters, non-ASCII */
        }
    }
    if (!parse_share_link(link, &parsed) || parsed.count == 0u) {
        return SETTINGS_ERR_LINK_NO_SLOTS;
    }
    return SETTINGS_OK;
}

static SettingsErr check_lead(const char *text, int *lead_out)
{
    size_t start = 0u;
    size_t end;
    size_t i;
    uint32_t value = 0u;

    if (text == NULL) {
        return SETTINGS_ERR_LEAD_MISSING;
    }
    end = strlen(text);
    while (start < end && is_blank(text[start])) {
        start++;
    }
    while (end > start && is_blank(text[end - 1u])) {
        end--;
    }
    if (start == end) {
        return SETTINGS_ERR_LEAD_MISSING;
    }
    for (i = start; i < end; i++) {
        if (text[i] < '0' || text[i] > '9') {
            return SETTINGS_ERR_LEAD_NOT_NUMBER;
        }
        if (value <= (uint32_t)APP_LEAD_MIN_HIGH) {   /* once above the range the exact value no longer matters */
            value = value * 10u + (uint32_t)(text[i] - '0');
        }
    }
    if (value < (uint32_t)APP_LEAD_MIN_LOW || value > (uint32_t)APP_LEAD_MIN_HIGH) {
        return SETTINGS_ERR_LEAD_RANGE;
    }
    if (lead_out != NULL) {
        *lead_out = (int)value;
    }
    return SETTINGS_OK;
}

SettingsErr web_validate_settings(const char *link, const char *lead_text, int *lead_out)
{
    const SettingsErr link_err = check_link(link);

    if (link_err != SETTINGS_OK) {
        return link_err;
    }
    return check_lead(lead_text, lead_out);
}

SettingsErr web_validate_difficulty(const char *text, int *mode_out)
{
    return difficulty_mode_parse(text, mode_out) ? SETTINGS_OK : SETTINGS_ERR_DIFFICULTY;
}

const char *web_settings_err_text(SettingsErr err)
{
    switch (err) {
    case SETTINGS_OK:                  return "ok";
    case SETTINGS_ERR_LINK_MISSING:    return "nusmods_link is missing";
    case SETTINGS_ERR_LINK_TOO_LONG:   return "nusmods_link is longer than 512 characters";
    case SETTINGS_ERR_LINK_PREFIX:     return "nusmods_link must start with https://nusmods.com/timetable/";
    case SETTINGS_ERR_LINK_CHARS:      return "nusmods_link must not contain spaces or special characters";
    case SETTINGS_ERR_LINK_NO_SLOTS:   return "nusmods_link has no module selection (use the Share/Sync link)";
    case SETTINGS_ERR_LEAD_MISSING:    return "lead_min is missing";
    case SETTINGS_ERR_LEAD_NOT_NUMBER: return "lead_min must be a whole number";
    case SETTINGS_ERR_LEAD_RANGE:      return "lead_min must be between " STR(APP_LEAD_MIN_LOW) " and " STR(APP_LEAD_MIN_HIGH);
    case SETTINGS_ERR_DIFFICULTY:      return "difficulty must be auto, easy, normal or hard";
    }
    return "invalid settings";
}

/* ================= demo morning alarm ================= */
DemoErr web_validate_demo_minutes(const char *text, int *minutes_out)
{
    size_t start = 0u;
    size_t end;
    size_t i;
    uint32_t value = 0u;

    if (text == NULL) {
        return DEMO_ERR_MISSING;
    }
    end = strlen(text);
    while (start < end && is_blank(text[start])) {
        start++;
    }
    while (end > start && is_blank(text[end - 1u])) {
        end--;
    }
    if (start == end) {
        return DEMO_ERR_MISSING;
    }
    for (i = start; i < end; i++) {
        if (text[i] < '0' || text[i] > '9') {
            return DEMO_ERR_NOT_NUMBER;
        }
        if (value <= (uint32_t)APP_DEMO_ALARM_MAX_MIN) {   /* once above the range the exact value no longer matters */
            value = value * 10u + (uint32_t)(text[i] - '0');
        }
    }
    if (value > (uint32_t)APP_DEMO_ALARM_MAX_MIN) {
        return DEMO_ERR_RANGE;
    }
    if (minutes_out != NULL) {
        *minutes_out = (int)value;
    }
    return DEMO_OK;
}

DemoErr web_demo_check(int minutes, const DemoCtx &ctx)
{
    const bool has_alarm = ctx.alarm_unix != 0u;

    if (!ctx.link_up) {
        return DEMO_ERR_LINK_DOWN;
    }
    if (ctx.bomb_running || ctx.session_active) {
        return DEMO_ERR_BUSY;                 /* the MCX owns the alarm: SET_ALARM / CANCEL would clobber a snooze */
    }
    if (!ctx.time_valid) {
        return DEMO_ERR_NO_TIME;
    }
    if (minutes == 0) {                       /* cancel */
        if (!ctx.alarm_is_demo || !has_alarm) {
            return DEMO_ERR_NO_DEMO;
        }
        return (ctx.alarm_unix <= ctx.now + SCHED_MIN_LEAD_S) ? DEMO_ERR_CANCEL_LATE : DEMO_OK;
    }
    if (ctx.alarm_is_demo && has_alarm && ctx.alarm_unix <= ctx.now + SCHED_MIN_LEAD_S) {
        return DEMO_ERR_REPLACE_LATE;
    }
    if (ctx.real_alarm_unix != 0u) {
        const uint32_t demo_at = sched_demo_alarm_at(ctx.now, minutes, APP_DEMO_ALARM_SLACK_S);

        if (ctx.real_alarm_unix <= demo_at) {
            return DEMO_ERR_REAL_FIRST;       /* equal times count too: two alarms at once */
        }
        if (ctx.real_alarm_unix - demo_at < (uint32_t)APP_DEMO_CLEAR_S) {
            return DEMO_ERR_TOO_CLOSE;
        }
    }
    return DEMO_OK;
}

void web_demo_err_message(DemoErr err, uint32_t real_alarm_unix, char *out, size_t out_size)
{
    if (out == NULL || out_size == 0u) {
        return;
    }
    if (err == DEMO_ERR_REAL_FIRST || err == DEMO_ERR_TOO_CLOSE) {
        const uint32_t minute_of_day =
            ((real_alarm_unix + (uint32_t)SCHED_TZ_OFFSET_S) % (uint32_t)SCHED_SEC_PER_DAY) / (uint32_t)SCHED_SEC_PER_MIN;

        snprintf(out, out_size, "your real alarm at %02u:%02u %s", (unsigned)(minute_of_day / MINUTES_PER_HOUR),
                 (unsigned)(minute_of_day % MINUTES_PER_HOUR), err == DEMO_ERR_REAL_FIRST ? "rings first" : "is too close");
        return;
    }
    snprintf(out, out_size, "%s", web_demo_err_text(err));
}

const char *web_demo_err_text(DemoErr err)
{
    switch (err) {
    case DEMO_OK:               return "ok";
    case DEMO_ERR_MISSING:      return "minutes is missing";
    case DEMO_ERR_NOT_NUMBER:   return "minutes must be a whole number";
    case DEMO_ERR_RANGE:        return "minutes must be 0 (cancel) or between 1 and " STR(APP_DEMO_ALARM_MAX_MIN);
    case DEMO_ERR_LINK_DOWN:    return "the MCX link is down";
    case DEMO_ERR_BUSY:         return "a bomb is already running";
    case DEMO_ERR_NO_TIME:      return "the clock is not set yet (no Wi-Fi time)";
    case DEMO_ERR_REAL_FIRST:   return "your real alarm rings first";
    case DEMO_ERR_TOO_CLOSE:    return "your real alarm is too close";
    case DEMO_ERR_NO_DEMO:      return "no demo alarm is pending";
    case DEMO_ERR_CANCEL_LATE:  return "too late to cancel: the demo alarm rings in under a minute";
    case DEMO_ERR_REPLACE_LATE: return "too late to change: the demo alarm rings in under a minute";
    }
    return "invalid request";
}

/* ================= simulated outage ================= */
void outage_start(Outage *o, uint32_t now_ms, uint32_t duration_ms)
{
    if (o == NULL) {
        return;
    }
    if (duration_ms > OUTAGE_MAX_MS) {
        duration_ms = OUTAGE_MAX_MS;
    }
    o->active = (duration_ms > 0u);
    o->until_ms = now_ms + duration_ms;
}

bool outage_active(const Outage &o, uint32_t now_ms)
{
    return o.active && (int32_t)(o.until_ms - now_ms) > 0;
}

uint32_t outage_left_s(const Outage &o, uint32_t now_ms)
{
    uint32_t left_ms;

    if (!outage_active(o, now_ms)) {
        return 0u;
    }
    left_ms = o.until_ms - now_ms;
    return (left_ms + (MS_PER_S - 1u)) / MS_PER_S;
}

/* ================= JSON ================= */
/* The text one character becomes inside a JSON string; returns its length (tmp holds ESC_BUF bytes). */
static size_t escape_char(unsigned char c, char *tmp)
{
    static const char k_hex[] = "0123456789abcdef";

    switch (c) {
    case '"':  memcpy(tmp, "\\\"", 2u); return 2u;
    case '\\': memcpy(tmp, "\\\\", 2u); return 2u;
    case '\n': memcpy(tmp, "\\n", 2u);  return 2u;
    case '\r': memcpy(tmp, "\\r", 2u);  return 2u;
    case '\t': memcpy(tmp, "\\t", 2u);  return 2u;
    default:
        break;
    }
    if (c < 0x20u || c == 0x7Fu || c == '<' || c == '>' || c == '&') {
        memcpy(tmp, "\\u00", 4u);
        tmp[4] = k_hex[c >> 4];
        tmp[5] = k_hex[c & 0x0Fu];
        return 6u;
    }
    tmp[0] = (c >= 0x80u) ? '?' : (char)c;
    return 1u;
}

bool json_escape(const char *s, char *out, size_t out_size)
{
    char tmp[ESC_BUF];
    size_t used = 0u;
    size_t n;

    if (out == NULL || out_size == 0u) {
        return false;
    }
    out[0] = '\0';
    if (s == NULL) {
        return true;
    }
    for (; *s != '\0'; s++) {
        n = escape_char((unsigned char)*s, tmp);
        if (used + n >= out_size) {
            out[used] = '\0';
            return false;                       /* never cut an escape in half */
        }
        memcpy(out + used, tmp, n);
        used += n;
    }
    out[used] = '\0';
    return true;
}

void jb_init(JsonBuf *j, char *buf, size_t cap)
{
    memset(j, 0, sizeof(*j));
    j->buf = buf;
    j->cap = cap;
    if (buf == NULL || cap == 0u) {
        j->overflow = true;
        return;
    }
    buf[0] = '\0';
}

bool jb_ok(const JsonBuf &j)
{
    return !j.overflow && j.depth == 0u && !j.after_key;
}

static void put(JsonBuf *j, const char *s, size_t n)
{
    if (j->overflow) {
        return;
    }
    if (j->len + n + 1u > j->cap) {
        j->overflow = true;
        return;
    }
    memcpy(j->buf + j->len, s, n);
    j->len += n;
    j->buf[j->len] = '\0';
}

static void put_char(JsonBuf *j, char c)
{
    put(j, &c, 1u);
}

/* Comma handling before a value: none right after a key, a comma between array elements. */
static void value_prefix(JsonBuf *j)
{
    if (j->after_key) {
        j->after_key = false;
        return;
    }
    if (j->depth > 0u) {
        if (j->has_item[j->depth - 1u]) {
            put_char(j, ',');
        }
        j->has_item[j->depth - 1u] = true;
    }
}

static void open(JsonBuf *j, char open_c, char close_c)
{
    if (j->overflow) {
        return;
    }
    if (j->depth >= JSON_MAX_DEPTH) {
        j->overflow = true;
        return;
    }
    value_prefix(j);
    put_char(j, open_c);
    j->close[j->depth] = close_c;
    j->has_item[j->depth] = false;
    j->depth++;
}

void jb_obj(JsonBuf *j)
{
    open(j, '{', '}');
}

void jb_arr(JsonBuf *j)
{
    open(j, '[', ']');
}

void jb_end(JsonBuf *j)
{
    if (j->overflow) {
        return;
    }
    if (j->depth == 0u || j->after_key) {
        j->overflow = true;                     /* one end too many, or a key without a value */
        return;
    }
    j->depth--;
    put_char(j, j->close[j->depth]);
}

static void put_string(JsonBuf *j, const char *s)
{
    char tmp[ESC_BUF];
    size_t n;

    put_char(j, '"');
    if (s != NULL) {
        for (; *s != '\0'; s++) {
            n = escape_char((unsigned char)*s, tmp);
            put(j, tmp, n);
        }
    }
    put_char(j, '"');
}

void jb_key(JsonBuf *j, const char *key)
{
    if (j->overflow) {
        return;
    }
    if (j->depth == 0u || j->close[j->depth - 1u] != '}' || j->after_key) {
        j->overflow = true;                     /* a key outside an object, or two keys in a row */
        return;
    }
    if (j->has_item[j->depth - 1u]) {
        put_char(j, ',');
    }
    j->has_item[j->depth - 1u] = true;
    put_string(j, key);
    put_char(j, ':');
    j->after_key = true;
}

void jb_str(JsonBuf *j, const char *s)
{
    if (j->overflow) {
        return;
    }
    value_prefix(j);
    put_string(j, s);
}

void jb_uint(JsonBuf *j, uint32_t v)
{
    char tmp[12];
    size_t i = sizeof(tmp);

    if (j->overflow) {
        return;
    }
    value_prefix(j);
    do {
        tmp[--i] = (char)('0' + (v % 10u));
        v /= 10u;
    } while (v != 0u);
    put(j, tmp + i, sizeof(tmp) - i);
}

void jb_int(JsonBuf *j, int32_t v)
{
    if (v < 0) {
        char tmp[12];
        uint32_t mag = 0u - (uint32_t)v;
        size_t i = sizeof(tmp);

        if (j->overflow) {
            return;
        }
        value_prefix(j);
        do {
            tmp[--i] = (char)('0' + (mag % 10u));
            mag /= 10u;
        } while (mag != 0u);
        tmp[--i] = '-';
        put(j, tmp + i, sizeof(tmp) - i);
        return;
    }
    jb_uint(j, (uint32_t)v);
}

void jb_bool(JsonBuf *j, bool v)
{
    if (j->overflow) {
        return;
    }
    value_prefix(j);
    put(j, v ? "true" : "false", v ? 4u : 5u);
}

void jb_null(JsonBuf *j)
{
    if (j->overflow) {
        return;
    }
    value_prefix(j);
    put(j, "null", 4u);
}

void jb_kv_str(JsonBuf *j, const char *key, const char *s)
{
    jb_key(j, key);
    jb_str(j, s);
}

void jb_kv_uint(JsonBuf *j, const char *key, uint32_t v)
{
    jb_key(j, key);
    jb_uint(j, v);
}

void jb_kv_int(JsonBuf *j, const char *key, int32_t v)
{
    jb_key(j, key);
    jb_int(j, v);
}

void jb_kv_bool(JsonBuf *j, const char *key, bool v)
{
    jb_key(j, key);
    jb_bool(j, v);
}

/* ================= timetable response ================= */
uint8_t web_weeks_list(uint16_t mask, uint8_t *out)
{
    uint8_t n = 0u;
    uint8_t w;

    for (w = 1u; w <= WEB_TT_WEEKS_MAX; w++) {
        if ((mask & (1u << (w - 1u))) != 0u) {
            out[n++] = w;
        }
    }
    return n;
}

void web_fmt_hhmm(uint16_t minutes, char *out, size_t out_size)
{
    if (minutes > LAST_MINUTE_OF_DAY) {
        minutes = LAST_MINUTE_OF_DAY;
    }
    snprintf(out, out_size, "%02u:%02u", (unsigned)(minutes / MINUTES_PER_HOUR), (unsigned)(minutes % MINUTES_PER_HOUR));
}

void web_put_timetable(JsonBuf *j, const Lesson *lessons, int count, int week, const char *fetched_local)
{
    uint8_t weeks[WEB_TT_WEEKS_MAX];
    char hhmm[8];
    int i;
    uint8_t k;
    uint8_t n;

    jb_obj(j);
    jb_kv_bool(j, "ok", true);
    jb_kv_int(j, "week", week);
    jb_kv_str(j, "fetched_local", fetched_local);
    jb_key(j, "lessons");
    jb_arr(j);
    for (i = 0; i < count; i++) {
        if (lessons[i].weekday < 1u || lessons[i].weekday > SCHED_MAX_WEEKDAY) {
            continue;
        }
        jb_obj(j);
        jb_kv_str(j, "module", lessons[i].module);
        jb_kv_str(j, "type", lessons[i].type);
        jb_kv_uint(j, "day", lessons[i].weekday);
        web_fmt_hhmm(lessons[i].start_min, hhmm, sizeof(hhmm));
        jb_kv_str(j, "start", hhmm);
        web_fmt_hhmm(lessons[i].end_min, hhmm, sizeof(hhmm));
        jb_kv_str(j, "end", hhmm);
        jb_key(j, "weeks");
        jb_arr(j);
        n = web_weeks_list(lessons[i].weeks, weeks);
        for (k = 0u; k < n; k++) {
            jb_uint(j, weeks[k]);
        }
        jb_end(j);
        jb_kv_str(j, "venue", lessons[i].venue);
        jb_end(j);
    }
    jb_end(j);
    jb_end(j);
}

/* ================= game panel helpers ================= */
bool answer_hidden(bool game_valid, uint8_t mcx_state, bool session_active)
{
    return !game_valid || session_active || mcx_state == PROTO_STATE_ARMED || mcx_state == PROTO_STATE_BOMB_ACTIVE;
}

uint8_t modules_total(uint8_t module_idx, uint8_t progress_pct)
{
    unsigned total;

    if (module_idx == 0u) {
        return 0u;                              /* 0 % is true for every total */
    }
    for (total = (unsigned)module_idx + 1u; total <= PROTO_MAX_MODULES; total++) {
        if ((unsigned)module_idx * 100u / total == progress_pct) {
            return (uint8_t)total;
        }
    }
    return 0u;
}
