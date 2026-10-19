/*
 * Defuse to Wake - OLED text, hints, morning summary and briefing/coach text (pure logic: no
 * Arduino headers, no heap, no globals). Compiled by the sketch and by the host tests
 * (tools/test_text.cpp, C++11).
 *
 * Everything the display and the briefing generator need as plain functions of their inputs:
 *   - word wrap + marquee for the 21-column size-1 OLED text,
 *   - the plain hints a strike reveals (dial digits, one wrong wire),
 *   - the morning history summary, the result line and the deterministic briefing/coach templates
 *     (the fallback whenever the LLM text is missing or invalid),
 *   - validation of the LLM's {"briefing","coach"} JSON.
 */
#ifndef TEXT_LOGIC_H
#define TEXT_LOGIC_H

#include <stddef.h>
#include <stdint.h>

#include "history_ring.h"
#include "schedule.h"
#include "../shared/protocol.h"

/* ---- OLED text geometry (size-1 font: 6x8 px glyphs on 128x64 = 21 columns x 8 rows) ---- */
#define TEXT_COLS               21
#define TEXT_MAX_LINES          8
#define MARQUEE_GAP             3       /* blank columns between the end of the text and its restart */
#define MARQUEE_CPS             5       /* marquee speed, characters per second */

/* ---- Hints (CLAUDE.md "Puzzles": one strike reveals a plain hint) ---- */
#define HINT_BUF                24
#define DIAL_HINT_BASE_DIGITS   2       /* digits shown after the first strike... */
#define DIAL_HINT_MAX_DIGITS    3       /* ...one more per further strike, but never the whole code */

/* ---- DEFUSED screen pages ---- */
#define DEFUSED_PAGE_MS         4000u
#define DEFUSED_PAGES           3       /* result line, briefing, coach */

/* ---- Briefing / coach ---- */
#define BRIEF_BRIEFING_MAX      120
#define BRIEF_COACH_MAX         80
#define BRIEF_MIN_CHARS         8
#define BRIEF_CLASSES_MAX       6
#define BRIEF_OK_BRIEFING       1
#define BRIEF_OK_COACH          2
#define HIST_SUMMARY_WINDOW     7       /* newest mornings the summary and the coach look at */
#define HIST_SUMMARY_TEXT_MAX   120
#define RESULT_LINE_MAX         48

/* ================= wrap + marquee ================= */

/* Word-wrap text into at most max_lines lines of at most cols characters. out is max_lines x stride
 * bytes (stride > cols); every written line is NUL-terminated, lines not written are untouched.
 * Words longer than cols are split. Returns the number of lines written. *rest_pos (optional) is
 * the index of the first character that did not fit (strlen(text) if everything fit), spaces
 * skipped. */
int wrap_text(const char *text, int cols, int max_lines, char *out, size_t stride, size_t *rest_pos);

/* A cols-wide window onto text scrolling right to left: offset 0 shows the start, each +1 moves one
 * character, with MARQUEE_GAP blanks before the text restarts. Text that fits is returned as is.
 * out holds cols + 1 bytes. */
void marquee_window(const char *text, uint32_t offset, int cols, char *out);

/* Marquee offset after now_ms at chars_per_s characters per second. */
uint32_t marquee_offset(uint32_t now_ms, uint32_t chars_per_s);

/* The OLED text block: text wrapped into max_lines lines; when it needs more, the first
 * max_lines - 1 lines stay still and the last one scrolls the rest (marquee at `offset`).
 * All max_lines lines of out are written ("" for unused ones). Returns the lines in use. */
int fit_text(const char *text, int cols, int max_lines, uint32_t offset, char *out, size_t stride);

/* ================= hints ================= */

/* "Hint: 4 2 _ _" once the current bomb has at least one strike, else "". 2 digits after the first
 * strike, 3 after the second; the last digit is never given away. Returns the length. */
size_t dial_hint(const uint8_t code[PROTO_DIAL_DIGITS], int strikes, char *out, size_t size);

/* The wire the wires hint marks: the lowest-numbered wrong wire (not correct_k, 1-based) that is
 * not cut yet (bit i of cut_mask = wire i + 1 is cut). 0-based index, -1 = no hint (no strike yet,
 * or nothing left to point at). */
int wire_hint_index(int correct_k, int n, int strikes, uint8_t cut_mask);

/* "Hint: not 1" for that wire, else "". Returns the length. */
size_t wire_hint(int correct_k, int n, int strikes, uint8_t cut_mask, char *out, size_t size);

/* ================= morning summary + result line ================= */
struct HistSummary {
    uint8_t count;                  /* mornings summarised (<= HIST_SUMMARY_WINDOW) */
    int streak;                     /* consecutive days with a defuse */
    uint8_t snoozes;                /* snoozes over those mornings, saturating at 255 */
    uint16_t avg_duration_s;        /* average defuse time */
    uint16_t avg_strikes_x10;       /* average strikes per morning, times 10 */
};

/* recent: newest first. Only the first HIST_SUMMARY_WINDOW records count. */
HistSummary history_summary(const HistRecord *recent, int n, int streak);

/* "streak 3 days; 2 snoozes in the last 7 mornings; average defuse time 2:31; average strikes 1.3
 * per morning" (or "no mornings recorded yet"): the history part of the LLM prompt. */
size_t history_summary_text(const HistSummary &s, char *out, size_t size);

/* "Defused 2:31, 1 strike, 0 snoozes" */
size_t result_line(uint32_t duration_s, int strikes, int snoozes, char *out, size_t size);

/* ================= briefing + coach ================= */
struct BriefClass {
    char module[SCHED_MODULE_LEN];
    char type[SCHED_ABBREV_LEN];
    uint16_t start_min;             /* minutes after local midnight */
    char venue[SCHED_VENUE_LEN];
};

/* The classes taught on one local date (weekday and teaching-week mask honoured, recess and
 * out-of-semester dates give none), earliest first, at most max. Returns how many. */
int classes_on_day(const Lesson *lessons, int count, int year, int month, int day, BriefClass *out, int max);

/* One line: the day's first class with its venue, then the other classes ("First: CG2271 LEC 14:00
 * @LT19. Then CS2040 16:00"). At most min(size - 1, BRIEF_BRIEFING_MAX) characters; a class that
 * does not fit is left out whole. */
size_t briefing_template(const BriefClass *cls, int n, char *out, size_t size);

/* Deterministic coach line from the history (at most BRIEF_COACH_MAX characters). */
size_t coach_template(const HistSummary &s, char *out, size_t size);

/* The user message of the briefing request: alarm time, classes, history, difficulty. */
size_t brief_user_prompt(const char *alarm_text, const BriefClass *cls, int n, const char *summary_text,
                         const char *difficulty, char *out, size_t size);

/* Printable ASCII (0x20..0x7E), no leading or trailing space, length min_len..max_len. */
bool brief_text_ok(const char *s, size_t min_len, size_t max_len);

/* The text shown for one day. for_day = local day number (see local_day), 0 = nothing. */
struct BriefText {
    uint32_t for_day;
    char briefing[BRIEF_BRIEFING_MAX + 1];
    char coach[BRIEF_COACH_MAX + 1];
};

/* Local (UTC+8) day number of a unix time. */
uint32_t local_day(uint32_t unix_s);

/* Is a BriefText read back from flash usable? */
bool brief_valid(const BriefText &b);

/* An LLM reply -> out->briefing / out->coach, each only when it is valid (briefing 8..120 chars,
 * coach 8..80, printable ASCII). Fields that are not valid are left empty. Returns a mask of
 * BRIEF_OK_BRIEFING | BRIEF_OK_COACH. json_scratch/scratch_size: room for the extracted JSON object. */
int brief_parse(const char *reply, char *json_scratch, size_t scratch_size, BriefText *out);

/* What to show for today_day: the stored text if it is for that day, else the template built from
 * today's timetable if that is for that day, else NULL. */
const BriefText *brief_pick(const BriefText &stored, const BriefText &today_tmpl, uint32_t today_day);

/* DEFUSED screen page for the time spent on it. */
int defused_page(uint32_t defused_ms);

#endif /* TEXT_LOGIC_H */
