/* Defuse to Wake - OLED text, hints, summaries and briefing text. See text_logic.h. */
#include "text_logic.h"

#include <stdio.h>
#include <string.h>

#include "puzzle_logic.h"

namespace {

#define SEC_PER_MIN_T       60u
#define MAX_WIRES_HINT      8
#define MAX_STREAK_SHOWN    999
#define MAX_COUNT_SHOWN     99u
#define COACH_SNOOZE_LIMIT  3u     /* snoozes in the window that earn a scolding */
#define COACH_STRIKES_X10   20u    /* average strikes (x10) that earn a "read the clue" */
#define COACH_STREAK_MIN    3

/* Bounded appender: never writes past size - 1, remembers overflow. */
struct Out {
    char *buf;
    size_t size;
    size_t len;
    bool full;
};

void out_init(Out *o, char *buf, size_t size, size_t cap)
{
    o->buf = buf;
    o->size = (size < cap + 1u) ? size : cap + 1u;     /* at most `cap` characters */
    o->len = 0u;
    o->full = false;
    if (o->size > 0u) {
        o->buf[0] = '\0';
    }
}

/* Append s, all or nothing. */
bool out_put(Out *o, const char *s)
{
    const size_t n = strlen(s);

    if (o->size == 0u || o->len + n + 1u > o->size) {
        o->full = true;
        return false;
    }
    memcpy(o->buf + o->len, s, n + 1u);
    o->len += n;
    return true;
}

size_t skip_spaces(const char *t, size_t p)
{
    while (t[p] == ' ') {
        p++;
    }
    return p;
}

size_t word_len(const char *t, size_t p)
{
    size_t n = 0u;

    while (t[p + n] != '\0' && t[p + n] != ' ') {
        n++;
    }
    return n;
}

unsigned umin(unsigned a, unsigned b)
{
    return a < b ? a : b;
}

}  // namespace

/* ================= wrap + marquee ================= */
int wrap_text(const char *text, int cols, int max_lines, char *out, size_t stride, size_t *rest_pos)
{
    size_t p;
    size_t n;
    size_t wl;
    char *line;
    int lines = 0;

    if (rest_pos != NULL) {
        *rest_pos = 0u;
    }
    if (text == NULL || out == NULL || cols <= 0 || (size_t)cols + 1u > stride) {
        return 0;
    }
    p = skip_spaces(text, 0u);
    while (lines < max_lines && text[p] != '\0') {
        line = out + (size_t)lines * stride;
        n = 0u;
        while (text[p] != '\0') {
            wl = word_len(text, p);
            if (n + (n > 0u ? 1u : 0u) + wl <= (size_t)cols) {
                if (n > 0u) {
                    line[n++] = ' ';
                }
                memcpy(line + n, text + p, wl);
                n += wl;
                p = skip_spaces(text, p + wl);
            } else if (n == 0u) {
                memcpy(line, text + p, (size_t)cols);       /* one word wider than the screen: split it */
                n = (size_t)cols;
                p += (size_t)cols;
                break;
            } else {
                break;                                      /* the word starts the next line */
            }
        }
        line[n] = '\0';
        lines++;
    }
    if (rest_pos != NULL) {
        *rest_pos = p;
    }
    return lines;
}

void marquee_window(const char *text, uint32_t offset, int cols, char *out)
{
    size_t len;
    size_t period;
    size_t start;
    int i;

    if (out == NULL || cols <= 0) {
        return;
    }
    if (text == NULL) {
        out[0] = '\0';
        return;
    }
    len = strlen(text);
    if (len <= (size_t)cols) {
        memcpy(out, text, len + 1u);
        return;
    }
    period = len + MARQUEE_GAP;
    start = (size_t)(offset % (uint32_t)period);
    for (i = 0; i < cols; i++) {
        const size_t idx = (start + (size_t)i) % period;

        out[i] = (idx < len) ? text[idx] : ' ';
    }
    out[cols] = '\0';
}

uint32_t marquee_offset(uint32_t now_ms, uint32_t chars_per_s)
{
    return (now_ms / 1000u) * chars_per_s + ((now_ms % 1000u) * chars_per_s) / 1000u;
}

int fit_text(const char *text, int cols, int max_lines, uint32_t offset, char *out, size_t stride)
{
    size_t rest = 0u;
    int n;
    int i;

    if (out == NULL || max_lines <= 0 || cols <= 0 || (size_t)cols + 1u > stride) {
        return 0;
    }
    for (i = 0; i < max_lines; i++) {
        out[(size_t)i * stride] = '\0';
    }
    if (text == NULL) {
        return 0;
    }
    n = wrap_text(text, cols, max_lines, out, stride, &rest);
    if (text[rest] == '\0') {
        return n;
    }
    /* Too long: keep max_lines - 1 lines still and scroll everything after them on the last line. */
    n = wrap_text(text, cols, max_lines - 1, out, stride, &rest);
    marquee_window(text + rest, offset, cols, out + (size_t)n * stride);
    return n + 1;
}

/* ================= hints ================= */
size_t dial_hint(const uint8_t code[PROTO_DIAL_DIGITS], int strikes, char *out, size_t size)
{
    char tmp[HINT_BUF];
    size_t len = 0u;
    int shown;
    int i;

    if (out == NULL || size == 0u) {
        return 0u;
    }
    out[0] = '\0';
    if (code == NULL || strikes < 1) {
        return 0u;
    }
    shown = DIAL_HINT_BASE_DIGITS + (strikes - 1);
    if (shown > DIAL_HINT_MAX_DIGITS) {
        shown = DIAL_HINT_MAX_DIGITS;
    }
    len = (size_t)snprintf(tmp, sizeof(tmp), "Hint:");
    for (i = 0; i < (int)PROTO_DIAL_DIGITS && len + 2u < sizeof(tmp); i++) {
        tmp[len++] = ' ';
        tmp[len++] = (i < shown) ? (char)('0' + (code[i] % 10u)) : '_';
        tmp[len] = '\0';
    }
    if (len + 1u > size) {
        return 0u;
    }
    memcpy(out, tmp, len + 1u);
    return len;
}

int wire_hint_index(int correct_k, int n, int strikes, uint8_t cut_mask)
{
    int i;

    if (strikes < 1 || n < 1 || n > MAX_WIRES_HINT || correct_k < 1 || correct_k > n) {
        return -1;
    }
    for (i = 0; i < n; i++) {
        if (i + 1 != correct_k && ((cut_mask >> i) & 1u) == 0u) {
            return i;
        }
    }
    return -1;
}

size_t wire_hint(int correct_k, int n, int strikes, uint8_t cut_mask, char *out, size_t size)
{
    const int idx = wire_hint_index(correct_k, n, strikes, cut_mask);
    int len;

    if (out == NULL || size == 0u) {
        return 0u;
    }
    out[0] = '\0';
    if (idx < 0) {
        return 0u;
    }
    len = snprintf(out, size, "Hint: not %d", idx + 1);
    if (len < 0 || (size_t)len >= size) {
        out[0] = '\0';
        return 0u;
    }
    return (size_t)len;
}

/* ================= morning summary + result line ================= */
HistSummary history_summary(const HistRecord *recent, int n, int streak)
{
    HistSummary s;
    uint32_t snoozes = 0u;
    uint32_t duration = 0u;
    uint32_t strikes = 0u;
    int i;

    memset(&s, 0, sizeof(s));
    s.streak = streak < 0 ? 0 : streak;
    if (recent == NULL || n <= 0) {
        return s;
    }
    if (n > HIST_SUMMARY_WINDOW) {
        n = HIST_SUMMARY_WINDOW;
    }
    for (i = 0; i < n; i++) {
        snoozes += recent[i].snoozes;
        duration += recent[i].duration_s;
        strikes += recent[i].strikes;
    }
    s.count = (uint8_t)n;
    s.snoozes = (uint8_t)(snoozes > 255u ? 255u : snoozes);
    s.avg_duration_s = (uint16_t)((duration + (uint32_t)n / 2u) / (uint32_t)n);
    s.avg_strikes_x10 = (uint16_t)((strikes * 10u + (uint32_t)n / 2u) / (uint32_t)n);
    return s;
}

size_t history_summary_text(const HistSummary &s, char *out, size_t size)
{
    int len;

    if (out == NULL || size == 0u) {
        return 0u;
    }
    if (s.count == 0u) {
        len = snprintf(out, size, "no mornings recorded yet");
    } else {
        len = snprintf(out, size,
                       "streak %d day%s; %u snooze%s in the last %u mornings; average defuse time %u:%02u; "
                       "average strikes %u.%u per morning",
                       s.streak > MAX_STREAK_SHOWN ? MAX_STREAK_SHOWN : s.streak, s.streak == 1 ? "" : "s",
                       (unsigned)s.snoozes, s.snoozes == 1u ? "" : "s", (unsigned)s.count,
                       (unsigned)(s.avg_duration_s / SEC_PER_MIN_T), (unsigned)(s.avg_duration_s % SEC_PER_MIN_T),
                       (unsigned)(s.avg_strikes_x10 / 10u), (unsigned)(s.avg_strikes_x10 % 10u));
    }
    if (len < 0 || (size_t)len >= size) {
        out[size - 1u] = '\0';
        return strlen(out);
    }
    return (size_t)len;
}

size_t result_line(uint32_t duration_s, int strikes, int snoozes, char *out, size_t size)
{
    int len;

    if (out == NULL || size == 0u) {
        return 0u;
    }
    if (strikes < 0) {
        strikes = 0;
    }
    if (snoozes < 0) {
        snoozes = 0;
    }
    len = snprintf(out, size, "Defused %u:%02u, %d strike%s, %d snooze%s", (unsigned)(duration_s / SEC_PER_MIN_T),
                   (unsigned)(duration_s % SEC_PER_MIN_T), strikes, strikes == 1 ? "" : "s", snoozes,
                   snoozes == 1 ? "" : "s");
    if (len < 0 || (size_t)len >= size) {
        out[size - 1u] = '\0';
        return strlen(out);
    }
    return (size_t)len;
}

/* ================= briefing + coach ================= */
int classes_on_day(const Lesson *lessons, int count, int year, int month, int day, BriefClass *out, int max)
{
    const int week = teaching_week(year, month, day);
    const int weekday = sched_weekday(year, month, day);
    int n = 0;
    int i;
    int pos;
    int last;

    if (lessons == NULL || out == NULL || max <= 0 || week == 0) {
        return 0;
    }
    if (count > SCHED_MAX_LESSONS) {
        count = SCHED_MAX_LESSONS;
    }
    for (i = 0; i < count; i++) {
        BriefClass c;

        if (lessons[i].weekday != weekday || ((lessons[i].weeks >> (week - 1)) & 1u) == 0u) {
            continue;
        }
        memset(&c, 0, sizeof(c));
        memcpy(c.module, lessons[i].module, sizeof(c.module) - 1u);
        memcpy(c.type, lessons[i].type, sizeof(c.type) - 1u);
        memcpy(c.venue, lessons[i].venue, sizeof(c.venue) - 1u);
        c.start_min = lessons[i].start_min;
        /* sorted insertion by start time; equal times keep the timetable order */
        pos = n;
        while (pos > 0 && out[pos - 1].start_min > c.start_min) {
            pos--;
        }
        if (pos >= max) {
            continue;                                   /* later than the `max` classes already kept */
        }
        last = (n < max) ? n : max - 1;                 /* a full list drops its latest class */
        memmove(&out[pos + 1], &out[pos], (size_t)(last - pos) * sizeof(BriefClass));
        out[pos] = c;
        if (n < max) {
            n++;
        }
    }
    return n;
}

size_t briefing_template(const BriefClass *cls, int n, char *out, size_t size)
{
    Out o;
    char piece[48];
    int i;

    if (out == NULL || size == 0u) {
        return 0u;
    }
    out_init(&o, out, size, BRIEF_BRIEFING_MAX);
    if (cls == NULL || n <= 0) {
        (void)out_put(&o, "No classes today. Enjoy the quiet.");
        return o.len;
    }
    snprintf(piece, sizeof(piece), "First: %s %s %02u:%02u", cls[0].module, cls[0].type,
             (unsigned)(cls[0].start_min / 60u), (unsigned)(cls[0].start_min % 60u));
    (void)out_put(&o, piece);
    if (cls[0].venue[0] != '\0') {
        snprintf(piece, sizeof(piece), " @%s", cls[0].venue);
        (void)out_put(&o, piece);
    }
    (void)out_put(&o, ".");
    for (i = 1; i < n; i++) {
        snprintf(piece, sizeof(piece), "%s%s %02u:%02u", i == 1 ? " Then " : ", ", cls[i].module,
                 (unsigned)(cls[i].start_min / 60u), (unsigned)(cls[i].start_min % 60u));
        if (!out_put(&o, piece)) {
            break;                                      /* a class that does not fit is left out whole */
        }
    }
    return o.len;
}

size_t coach_template(const HistSummary &s, char *out, size_t size)
{
    char tmp[BRIEF_COACH_MAX + 32];

    if (out == NULL || size == 0u) {
        return 0u;
    }
    if (s.count == 0u) {
        snprintf(tmp, sizeof(tmp), "First morning on record. Make it a clean one.");
    } else if (s.snoozes >= COACH_SNOOZE_LIMIT) {
        snprintf(tmp, sizeof(tmp), "%u snoozes this week. Snooze is not a plan.", umin(s.snoozes, MAX_COUNT_SHOWN));
    } else if (s.avg_strikes_x10 >= COACH_STRIKES_X10) {
        snprintf(tmp, sizeof(tmp), "Avg %u.%u strikes. Read the clue, then cut.", umin(s.avg_strikes_x10 / 10u, MAX_COUNT_SHOWN),
                 (unsigned)(s.avg_strikes_x10 % 10u));
    } else if (s.streak >= COACH_STREAK_MIN) {
        snprintf(tmp, sizeof(tmp), "%d-day streak. Don't you dare break it.",
                 s.streak > MAX_STREAK_SHOWN ? MAX_STREAK_SHOWN : s.streak);
    } else if (s.snoozes == 0u) {
        snprintf(tmp, sizeof(tmp), "No snoozes lately. Keep it quick and boring.");
    } else {
        snprintf(tmp, sizeof(tmp), "Steady so far. Defuse faster, own the day.");
    }
    tmp[BRIEF_COACH_MAX] = '\0';
    snprintf(out, size, "%s", tmp);
    return strlen(out);
}

size_t brief_user_prompt(const char *alarm_text, const BriefClass *cls, int n, const char *summary_text,
                         const char *difficulty, char *out, size_t size)
{
    Out o;
    char piece[64];
    int i;

    if (out == NULL || size == 0u) {
        return 0u;
    }
    out_init(&o, out, size, size - 1u);
    snprintf(piece, sizeof(piece), "Next alarm: %s\n", alarm_text != NULL ? alarm_text : "unknown");
    (void)out_put(&o, piece);
    (void)out_put(&o, "Classes that day:");
    if (cls == NULL || n <= 0) {
        (void)out_put(&o, " none");
    }
    for (i = 0; cls != NULL && i < n && i < BRIEF_CLASSES_MAX; i++) {
        snprintf(piece, sizeof(piece), "%s %s %s %02u:%02u %s", i == 0 ? "" : ";", cls[i].module, cls[i].type,
                 (unsigned)(cls[i].start_min / 60u), (unsigned)(cls[i].start_min % 60u),
                 cls[i].venue[0] != '\0' ? cls[i].venue : "-");
        (void)out_put(&o, piece);
    }
    (void)out_put(&o, "\nHistory: ");
    (void)out_put(&o, summary_text != NULL ? summary_text : "none");
    (void)out_put(&o, "\nDifficulty: ");
    (void)out_put(&o, difficulty != NULL ? difficulty : "standard");
    return o.len;
}

bool brief_text_ok(const char *s, size_t min_len, size_t max_len)
{
    size_t len;
    size_t i;

    if (s == NULL) {
        return false;
    }
    len = strlen(s);
    if (len < min_len || len > max_len || len == 0u || s[0] == ' ' || s[len - 1u] == ' ') {
        return false;
    }
    for (i = 0u; i < len; i++) {
        if ((unsigned char)s[i] < 0x20u || (unsigned char)s[i] > 0x7Eu) {
            return false;
        }
    }
    return true;
}

uint32_t local_day(uint32_t unix_s)
{
    return (unix_s + (uint32_t)SCHED_TZ_OFFSET_S) / (uint32_t)SCHED_SEC_PER_DAY;
}

bool brief_valid(const BriefText &b)
{
    return b.for_day != 0u && brief_text_ok(b.briefing, BRIEF_MIN_CHARS, BRIEF_BRIEFING_MAX) &&
           brief_text_ok(b.coach, BRIEF_MIN_CHARS, BRIEF_COACH_MAX);
}

int brief_parse(const char *reply, char *json_scratch, size_t scratch_size, BriefText *out)
{
    char tmp_b[BRIEF_BRIEFING_MAX + 1];
    char tmp_c[BRIEF_COACH_MAX + 1];
    int ok = 0;

    if (out == NULL) {
        return 0;
    }
    out->briefing[0] = '\0';
    out->coach[0] = '\0';
    if (reply == NULL || json_scratch == NULL || !extract_json_object(reply, json_scratch, scratch_size)) {
        return 0;
    }
    /* json_get_string fails when the value does not fit the buffer: too long counts as invalid. */
    if (json_get_string(json_scratch, "briefing", tmp_b, sizeof(tmp_b)) &&
        brief_text_ok(tmp_b, BRIEF_MIN_CHARS, BRIEF_BRIEFING_MAX)) {
        memcpy(out->briefing, tmp_b, sizeof(tmp_b));
        ok |= BRIEF_OK_BRIEFING;
    }
    if (json_get_string(json_scratch, "coach", tmp_c, sizeof(tmp_c)) &&
        brief_text_ok(tmp_c, BRIEF_MIN_CHARS, BRIEF_COACH_MAX)) {
        memcpy(out->coach, tmp_c, sizeof(tmp_c));
        ok |= BRIEF_OK_COACH;
    }
    return ok;
}

const BriefText *brief_pick(const BriefText &stored, const BriefText &today_tmpl, uint32_t today_day)
{
    if (today_day == 0u) {
        return NULL;
    }
    if (stored.for_day == today_day && brief_valid(stored)) {
        return &stored;
    }
    if (today_tmpl.for_day == today_day && brief_valid(today_tmpl)) {
        return &today_tmpl;
    }
    return NULL;
}

int defused_page(uint32_t defused_ms)
{
    return (int)((defused_ms / DEFUSED_PAGE_MS) % DEFUSED_PAGES);
}
