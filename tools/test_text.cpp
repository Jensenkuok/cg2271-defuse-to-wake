/*
 * Host-side tests for esp32/defuse_esp32/src/app/text_logic.*. Plain checks, no framework.
 * Covers the OLED wrap/marquee, the dial/wire hints, the history summary, the result line, the
 * briefing/coach templates and the validation of the LLM's briefing JSON.
 */
#include <stdio.h>
#include <string.h>

#include "text_logic.h"

static int g_checks;
static int g_failed;

#define CHECK(cond)                                                              \
    do {                                                                         \
        g_checks++;                                                              \
        if (!(cond)) {                                                           \
            g_failed++;                                                          \
            if (g_failed < 40) {                                                 \
                printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);          \
            }                                                                    \
        }                                                                        \
    } while (0)

#define STRIDE (TEXT_COLS + 1)

/* ================= wrap ================= */
static void test_wrap(void)
{
    char out[TEXT_MAX_LINES][STRIDE];
    size_t rest = 99;
    int n;

    n = wrap_text("1st=3; 2nd=1st+4; 3rd=2nd-5; 4th=1st*3", TEXT_COLS, 4, out[0], STRIDE, &rest);
    CHECK(n == 2 && strcmp(out[0], "1st=3; 2nd=1st+4;") == 0 && strcmp(out[1], "3rd=2nd-5; 4th=1st*3") == 0);
    CHECK(rest == strlen("1st=3; 2nd=1st+4; 3rd=2nd-5; 4th=1st*3"));

    n = wrap_text("short", TEXT_COLS, 3, out[0], STRIDE, &rest);
    CHECK(n == 1 && strcmp(out[0], "short") == 0 && rest == 5);

    n = wrap_text("exactly twenty-one chr", TEXT_COLS, 3, out[0], STRIDE, &rest);   /* 22 chars: breaks before "chr" */
    CHECK(n == 2 && strcmp(out[0], "exactly twenty-one") == 0 && strcmp(out[1], "chr") == 0);
    n = wrap_text("exactly twenty-one ch", TEXT_COLS, 3, out[0], STRIDE, &rest);    /* exactly 21 */
    CHECK(n == 1 && strcmp(out[0], "exactly twenty-one ch") == 0);

    /* spaces: leading, trailing and doubled are collapsed at the breaks, never printed at a line edge */
    n = wrap_text("   a   b   ", TEXT_COLS, 3, out[0], STRIDE, &rest);
    CHECK(n == 1 && strcmp(out[0], "a b") == 0 && rest == strlen("   a   b   "));

    /* a word wider than the screen is split at the column limit */
    n = wrap_text("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789", TEXT_COLS, 4, out[0], STRIDE, &rest);
    CHECK(n == 2 && strcmp(out[0], "ABCDEFGHIJKLMNOPQRSTU") == 0 && strcmp(out[1], "VWXYZ0123456789") == 0);
    n = wrap_text("hi ABCDEFGHIJKLMNOPQRSTUVWXYZ", TEXT_COLS, 4, out[0], STRIDE, &rest);
    CHECK(n == 3 && strcmp(out[0], "hi") == 0 && strcmp(out[1], "ABCDEFGHIJKLMNOPQRSTU") == 0 && strcmp(out[2], "VWXYZ") == 0);

    /* not enough lines: rest_pos points at what did not fit */
    {
        char narrow[2][10];                                    /* 9 columns + NUL */

        n = wrap_text("aaaa bbbb cccc dddd eeee ffff gggg", 9, 2, narrow[0], 10, &rest);
        CHECK(n == 2 && strcmp(narrow[0], "aaaa bbbb") == 0 && strcmp(narrow[1], "cccc dddd") == 0);
        CHECK(rest == strlen("aaaa bbbb cccc dddd ") &&
              strcmp("aaaa bbbb cccc dddd eeee ffff gggg" + rest, "eeee ffff gggg") == 0);
        n = wrap_text("abc", 9, 0, narrow[0], 10, &rest);
        CHECK(n == 0 && rest == 0);
    }

    /* every output line is at most `cols` characters, for a spread of widths and texts */
    {
        static const char *const texts[] = {
            "Hello world this is a test of the wrapping code",
            "a b c d e f g h i j k l m n o p q r s t u v w x y z",
            "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx yy zzzzzzzzzzzzzz",
            "One wire only, and 3 wires mean the 2nd one from the left"
        };
        char o[16][STRIDE];
        size_t t;
        int cols;
        int i;
        bool ok = true;

        for (t = 0; t < sizeof(texts) / sizeof(texts[0]); t++) {
            for (cols = 4; cols <= TEXT_COLS; cols++) {
                n = wrap_text(texts[t], cols, 16, o[0], STRIDE, &rest);
                for (i = 0; i < n; i++) {
                    if (strlen(o[i]) > (size_t)cols || o[i][0] == ' ' || o[i][0] == '\0') {
                        ok = false;
                    }
                }
                if (rest != strlen(texts[t]) && n != 16) {
                    ok = false;
                }
            }
        }
        CHECK(ok);
    }

    /* invalid arguments */
    CHECK(wrap_text(NULL, 21, 2, out[0], STRIDE, &rest) == 0);
    CHECK(wrap_text("x", 0, 2, out[0], STRIDE, &rest) == 0);
    CHECK(wrap_text("x", 21, 2, out[0], 21, &rest) == 0);                 /* stride too small for 21 + NUL */
    CHECK(wrap_text("x", 21, 2, NULL, STRIDE, NULL) == 0);
    CHECK(wrap_text("", 21, 2, out[0], STRIDE, &rest) == 0 && rest == 0);
}

/* ================= marquee ================= */
static void test_marquee(void)
{
    char w[TEXT_COLS + 1];
    const char *t = "0123456789ABCDEFGHIJKLMNOPQRSTUV";    /* 32 chars */

    marquee_window(t, 0, 10, w);
    CHECK(strcmp(w, "0123456789") == 0);
    marquee_window(t, 5, 10, w);
    CHECK(strcmp(w, "56789ABCDE") == 0);
    marquee_window(t, 25, 10, w);                              /* runs into the gap */
    CHECK(strcmp(w, "PQRSTUV   ") == 0);
    marquee_window(t, 30, 10, w);
    CHECK(strcmp(w, "UV   01234") == 0);
    marquee_window(t, 35, 10, w);                              /* period = 32 + 3: back to the start */
    CHECK(strcmp(w, "0123456789") == 0);
    marquee_window(t, 35u * 1000u + 5u, 10, w);                /* large offsets wrap */
    CHECK(strcmp(w, "56789ABCDE") == 0);
    marquee_window("short", 7, 10, w);                         /* fits: shown as is */
    CHECK(strcmp(w, "short") == 0);
    marquee_window("exactly10!", 3, 10, w);
    CHECK(strcmp(w, "exactly10!") == 0);
    marquee_window(NULL, 0, 10, w);
    CHECK(w[0] == '\0');
    marquee_window(t, 0, 0, w);                                /* cols 0: untouched, no crash */

    CHECK(marquee_offset(0, MARQUEE_CPS) == 0);
    CHECK(marquee_offset(200, MARQUEE_CPS) == 1);              /* 5 chars/s: one step per 200 ms */
    CHECK(marquee_offset(199, MARQUEE_CPS) == 0);
    CHECK(marquee_offset(1000, MARQUEE_CPS) == 5);
    CHECK(marquee_offset(2500, MARQUEE_CPS) == 12);
    CHECK(marquee_offset(4294967295u, MARQUEE_CPS) == 21474836u);  /* no 32-bit overflow */
}

static void test_fit_text(void)
{
    char out[TEXT_MAX_LINES][STRIDE];
    const char *shortt = "Fits on one line";
    const char *two = "1st=3; 2nd=1st+4; 3rd=2nd-5; 4th=1st*3";
    const char *longt = "1st=3; 2nd=1st+4; 3rd=2nd-5; 4th=1st*3; and then keep going with more words here";
    int n;

    n = fit_text(shortt, TEXT_COLS, 3, 0, out[0], STRIDE);
    CHECK(n == 1 && strcmp(out[0], shortt) == 0 && out[1][0] == '\0' && out[2][0] == '\0');

    n = fit_text(two, TEXT_COLS, 2, 7, out[0], STRIDE);          /* exactly fills 2 lines: no marquee */
    CHECK(n == 2 && strcmp(out[1], "3rd=2nd-5; 4th=1st*3") == 0);

    /* too long for 3 lines: lines 0-1 are still, line 2 scrolls the rest */
    n = fit_text(longt, TEXT_COLS, 3, 0, out[0], STRIDE);
    CHECK(n == 3 && strcmp(out[0], "1st=3; 2nd=1st+4;") == 0 && strcmp(out[1], "3rd=2nd-5; 4th=1st*3;") == 0);
    CHECK(strcmp(out[2], "and then keep going w") == 0);
    n = fit_text(longt, TEXT_COLS, 3, 4, out[0], STRIDE);
    CHECK(n == 3 && strcmp(out[0], "1st=3; 2nd=1st+4;") == 0 && strcmp(out[2], "then keep going with ") == 0);
    n = fit_text(longt, TEXT_COLS, 3, 10000, out[0], STRIDE);
    CHECK(n == 3 && strlen(out[2]) <= (size_t)TEXT_COLS);

    n = fit_text(longt, TEXT_COLS, 1, 0, out[0], STRIDE);        /* one line: all marquee */
    CHECK(n == 1 && strcmp(out[0], "1st=3; 2nd=1st+4; 3rd") == 0);
    CHECK(fit_text("", TEXT_COLS, 2, 0, out[0], STRIDE) == 0 && out[0][0] == '\0' && out[1][0] == '\0');
    CHECK(fit_text(NULL, TEXT_COLS, 2, 0, out[0], STRIDE) == 0);
    CHECK(fit_text("x", TEXT_COLS, 0, 0, out[0], STRIDE) == 0);
}

/* ================= hints ================= */
static void test_dial_hint(void)
{
    const uint8_t code[4] = { 4, 2, 7, 1 };
    char h[HINT_BUF];

    CHECK(dial_hint(code, 0, h, sizeof(h)) == 0 && h[0] == '\0');
    CHECK(dial_hint(code, -1, h, sizeof(h)) == 0 && h[0] == '\0');
    CHECK(dial_hint(code, 1, h, sizeof(h)) == 13 && strcmp(h, "Hint: 4 2 _ _") == 0);
    CHECK(dial_hint(code, 2, h, sizeof(h)) == 13 && strcmp(h, "Hint: 4 2 7 _") == 0);
    CHECK(dial_hint(code, 3, h, sizeof(h)) == 13 && strcmp(h, "Hint: 4 2 7 _") == 0);     /* never the last digit */
    CHECK(dial_hint(code, 99, h, sizeof(h)) == 13 && strcmp(h, "Hint: 4 2 7 _") == 0);
    CHECK(strlen(h) <= (size_t)TEXT_COLS);
    {
        const uint8_t zeros[4] = { 0, 0, 9, 9 };

        CHECK(dial_hint(zeros, 1, h, sizeof(h)) == 13 && strcmp(h, "Hint: 0 0 _ _") == 0);
    }
    CHECK(dial_hint(code, 1, h, 8) == 0 && h[0] == '\0');                                  /* does not fit */
    CHECK(dial_hint(NULL, 1, h, sizeof(h)) == 0);
    CHECK(dial_hint(code, 1, NULL, 0) == 0);
}

static void test_wire_hint(void)
{
    char h[HINT_BUF];

    CHECK(wire_hint_index(3, 5, 0, 0) == -1);                          /* no strike yet */
    CHECK(wire_hint_index(1, 4, 1, 0) == 1);                           /* wire 1 is correct: lowest wrong is 2 */
    CHECK(wire_hint_index(3, 4, 1, 0) == 0);                           /* lowest wrong, 0-based */
    CHECK(wire_hint_index(3, 4, 2, 0x01) == 1);                        /* wire 1 already cut: next one */
    CHECK(wire_hint_index(3, 4, 1, 0x03) == 3);                        /* 1, 2 cut, 3 correct: 4 */
    CHECK(wire_hint_index(2, 3, 1, 0x05) == -1);                       /* only the correct one is left */
    CHECK(wire_hint_index(0, 4, 1, 0) == -1);                          /* correct_k out of range */
    CHECK(wire_hint_index(5, 4, 1, 0) == -1);
    CHECK(wire_hint_index(1, 0, 1, 0) == -1);
    CHECK(wire_hint_index(1, 9, 1, 0) == -1);

    CHECK(wire_hint(3, 4, 0, 0, h, sizeof(h)) == 0 && h[0] == '\0');
    CHECK(wire_hint(3, 4, 1, 0, h, sizeof(h)) == 11 && strcmp(h, "Hint: not 1") == 0);
    CHECK(wire_hint(1, 6, 1, 0, h, sizeof(h)) == 11 && strcmp(h, "Hint: not 2") == 0);
    CHECK(wire_hint(2, 3, 1, 0x05, h, sizeof(h)) == 0 && h[0] == '\0');
    CHECK(wire_hint(3, 4, 1, 0, h, 6) == 0 && h[0] == '\0');
    CHECK(wire_hint(3, 4, 1, 0, NULL, 0) == 0);
}

/* ================= history summary, result line ================= */
static HistRecord rec(uint16_t dur, uint8_t strikes, uint8_t snoozes)
{
    HistRecord r;

    memset(&r, 0, sizeof(r));
    r.duration_s = dur;
    r.strikes = strikes;
    r.snoozes = snoozes;
    return r;
}

static void test_summary(void)
{
    HistRecord h[10];
    HistSummary s;
    char t[HIST_SUMMARY_TEXT_MAX + 1];
    int i;

    s = history_summary(NULL, 0, 4);
    CHECK(s.count == 0 && s.streak == 4 && s.snoozes == 0 && s.avg_duration_s == 0 && s.avg_strikes_x10 == 0);
    CHECK(history_summary(h, -3, -2).streak == 0);

    h[0] = rec(151, 1, 0);
    h[1] = rec(90, 0, 2);
    h[2] = rec(120, 2, 1);
    s = history_summary(h, 3, 3);
    CHECK(s.count == 3 && s.streak == 3 && s.snoozes == 3);
    CHECK(s.avg_duration_s == 120);                                /* (151+90+120)/3 = 120.33 */
    CHECK(s.avg_strikes_x10 == 10);                                /* 3 strikes / 3 mornings = 1.0 */

    h[0] = rec(100, 1, 0);
    h[1] = rec(101, 1, 0);
    s = history_summary(h, 2, 1);
    CHECK(s.avg_duration_s == 101);                                /* rounds half up: 100.5 */
    CHECK(s.avg_strikes_x10 == 10);

    /* only the newest 7 count, even when 10 are passed */
    for (i = 0; i < 10; i++) {
        h[i] = rec(60, 0, i < 7 ? 1 : 5);
    }
    s = history_summary(h, 10, 0);
    CHECK(s.count == HIST_SUMMARY_WINDOW && s.snoozes == 7);

    for (i = 0; i < 10; i++) {
        h[i] = rec(10, 250, 250);
    }
    CHECK(history_summary(h, 10, 0).snoozes == 255);              /* saturates */

    s = history_summary(NULL, 0, 0);
    history_summary_text(s, t, sizeof(t));
    CHECK(strcmp(t, "no mornings recorded yet") == 0);

    h[0] = rec(151, 1, 0);
    h[1] = rec(151, 2, 2);
    s = history_summary(h, 2, 3);
    CHECK(history_summary_text(s, t, sizeof(t)) == strlen(t));
    CHECK(strcmp(t, "streak 3 days; 2 snoozes in the last 2 mornings; average defuse time 2:31; "
                    "average strikes 1.5 per morning") == 0);
    s.streak = 1;
    s.snoozes = 1;
    history_summary_text(s, t, sizeof(t));
    CHECK(strstr(t, "streak 1 day;") != NULL && strstr(t, "1 snooze in") != NULL);
    history_summary_text(s, t, 20);                                /* truncates, stays terminated */
    CHECK(strlen(t) == 19);
}

static void test_result_line(void)
{
    char t[RESULT_LINE_MAX];

    CHECK(result_line(151, 1, 0, t, sizeof(t)) == strlen(t) && strcmp(t, "Defused 2:31, 1 strike, 0 snoozes") == 0);
    CHECK(strcmp((result_line(5, 0, 1, t, sizeof(t)), t), "Defused 0:05, 0 strikes, 1 snooze") == 0);
    CHECK(strcmp((result_line(3600, 3, 2, t, sizeof(t)), t), "Defused 60:00, 3 strikes, 2 snoozes") == 0);
    CHECK(strcmp((result_line(65535, 255, 255, t, sizeof(t)), t), "Defused 1092:15, 255 strikes, 255 snoozes") == 0);
    CHECK(result_line(65535, 255, 255, t, sizeof(t)) < sizeof(t));
    CHECK(strcmp((result_line(60, -4, -1, t, sizeof(t)), t), "Defused 1:00, 0 strikes, 0 snoozes") == 0);
    CHECK(result_line(151, 1, 0, t, 10) == 9 && strcmp(t, "Defused 2") == 0);                 /* truncated */
    CHECK(result_line(151, 1, 0, NULL, 0) == 0);
}

/* ================= briefing + coach ================= */
static Lesson lesson(const char *mod, const char *type, uint8_t weekday, uint16_t start_min, uint16_t weeks,
                     const char *venue)
{
    Lesson l;

    memset(&l, 0, sizeof(l));
    snprintf(l.module, sizeof(l.module), "%s", mod);
    snprintf(l.type, sizeof(l.type), "%s", type);
    l.weekday = weekday;
    l.start_min = start_min;
    l.end_min = (uint16_t)(start_min + 60u);
    l.weeks = weeks;
    snprintf(l.venue, sizeof(l.venue), "%s", venue);
    return l;
}

#define ALL_WEEKS 0x1FFFu

static void test_classes_on_day(void)
{
    /* 2026-08-12 is a Wednesday in teaching week 1 */
    Lesson ls[8];
    BriefClass c[BRIEF_CLASSES_MAX];
    int n;

    ls[0] = lesson("CS2040", "TUT", 3, 16 * 60, ALL_WEEKS, "COM1-0210");
    ls[1] = lesson("CG2271", "LEC", 3, 9 * 60 + 30, ALL_WEEKS, "LT19");
    ls[2] = lesson("GEA1000", "LEC", 4, 8 * 60, ALL_WEEKS, "LT1");           /* Thursday: not on Wednesday */
    ls[3] = lesson("QF2104", "TUT", 3, 12 * 60, 0x0002u, "");                  /* week 2 only */
    ls[4] = lesson("EE2026", "LAB", 3, 9 * 60 + 30, ALL_WEEKS, "E4-02");       /* same start as CG2271 */

    n = classes_on_day(ls, 5, 2026, 8, 12, c, BRIEF_CLASSES_MAX);
    CHECK(n == 3);
    CHECK(strcmp(c[0].module, "CG2271") == 0 && c[0].start_min == 9 * 60 + 30);     /* earliest first */
    CHECK(strcmp(c[1].module, "EE2026") == 0);                                       /* tie: timetable order */
    CHECK(strcmp(c[2].module, "CS2040") == 0 && strcmp(c[2].venue, "COM1-0210") == 0);

    n = classes_on_day(ls, 5, 2026, 8, 19, c, BRIEF_CLASSES_MAX);                    /* week 2: QF2104 joins */
    CHECK(n == 4 && strcmp(c[2].module, "QF2104") == 0);
    CHECK(classes_on_day(ls, 5, 2026, 9, 23, c, BRIEF_CLASSES_MAX) == 0);            /* recess Wednesday */
    CHECK(classes_on_day(ls, 5, 2026, 8, 15, c, BRIEF_CLASSES_MAX) == 0);            /* Saturday, nothing */
    CHECK(classes_on_day(ls, 5, 2026, 8, 5, c, BRIEF_CLASSES_MAX) == 0);             /* before week 1 */
    CHECK(classes_on_day(NULL, 5, 2026, 8, 12, c, BRIEF_CLASSES_MAX) == 0);

    /* more classes than fit: the earliest ones are kept, in order */
    {
        Lesson many[8];
        BriefClass m[3];
        int i;

        for (i = 0; i < 8; i++) {
            many[i] = lesson("M", "LEC", 3, (uint16_t)((8 - i) * 60), ALL_WEEKS, "V");     /* latest first in the table */
        }
        n = classes_on_day(many, 8, 2026, 8, 12, m, 3);
        CHECK(n == 3 && m[0].start_min == 60 && m[1].start_min == 120 && m[2].start_min == 180);
    }
}

static void test_briefing_template(void)
{
    BriefClass c[BRIEF_CLASSES_MAX];
    char t[BRIEF_BRIEFING_MAX + 1];
    int i;

    memset(c, 0, sizeof(c));
    snprintf(c[0].module, sizeof(c[0].module), "CG2271");
    snprintf(c[0].type, sizeof(c[0].type), "LEC");
    snprintf(c[0].venue, sizeof(c[0].venue), "LT19");
    c[0].start_min = 14 * 60;
    snprintf(c[1].module, sizeof(c[1].module), "CS2040");
    snprintf(c[1].type, sizeof(c[1].type), "TUT");
    c[1].start_min = 16 * 60;
    snprintf(c[2].module, sizeof(c[2].module), "GEA1000");
    snprintf(c[2].type, sizeof(c[2].type), "LEC");
    c[2].start_min = 18 * 60 + 5;

    CHECK(briefing_template(c, 1, t, sizeof(t)) == strlen(t) && strcmp(t, "First: CG2271 LEC 14:00 @LT19.") == 0);
    briefing_template(c, 3, t, sizeof(t));
    CHECK(strcmp(t, "First: CG2271 LEC 14:00 @LT19. Then CS2040 16:00, GEA1000 18:05") == 0);
    snprintf(c[0].venue, sizeof(c[0].venue), "%s", "");
    briefing_template(c, 2, t, sizeof(t));
    CHECK(strcmp(t, "First: CG2271 LEC 14:00. Then CS2040 16:00") == 0);
    CHECK(strcmp((briefing_template(NULL, 0, t, sizeof(t)), t), "No classes today. Enjoy the quiet.") == 0);
    CHECK(strcmp((briefing_template(c, 0, t, sizeof(t)), t), "No classes today. Enjoy the quiet.") == 0);

    /* six long entries: never more than 120 characters, never a half class */
    for (i = 0; i < BRIEF_CLASSES_MAX; i++) {
        snprintf(c[i].module, sizeof(c[i].module), "ABCD%04dXYZ", 1000 + i);
        snprintf(c[i].type, sizeof(c[i].type), "SEM");
        snprintf(c[i].venue, sizeof(c[i].venue), "VENUE-12345-67");
        c[i].start_min = (uint16_t)(8 * 60 + i * 90);
    }
    briefing_template(c, BRIEF_CLASSES_MAX, t, sizeof(t));
    CHECK(strlen(t) <= BRIEF_BRIEFING_MAX && strncmp(t, "First: ABCD1000XYZ SEM 08:00 @VENUE-12345-67. Then ", 51) == 0);
    CHECK(strstr(t, "ABCD1001XYZ 09:30") != NULL);
    CHECK(brief_text_ok(t, BRIEF_MIN_CHARS, BRIEF_BRIEFING_MAX));
    {
        char small[40];

        briefing_template(c, BRIEF_CLASSES_MAX, small, sizeof(small));
        CHECK(strlen(small) < sizeof(small) && brief_text_ok(small, 1, sizeof(small) - 1));
    }
    CHECK(briefing_template(c, 2, NULL, 0) == 0);
}

static void test_coach_template(void)
{
    HistSummary s;
    char t[BRIEF_COACH_MAX + 1];

    memset(&s, 0, sizeof(s));
    coach_template(s, t, sizeof(t));
    CHECK(strcmp(t, "First morning on record. Make it a clean one.") == 0);

    s.count = 7;
    s.snoozes = 4;
    coach_template(s, t, sizeof(t));
    CHECK(strcmp(t, "4 snoozes this week. Snooze is not a plan.") == 0);

    s.snoozes = 0;
    s.avg_strikes_x10 = 25;
    coach_template(s, t, sizeof(t));
    CHECK(strcmp(t, "Avg 2.5 strikes. Read the clue, then cut.") == 0);

    s.avg_strikes_x10 = 5;
    s.streak = 5;
    coach_template(s, t, sizeof(t));
    CHECK(strcmp(t, "5-day streak. Don't you dare break it.") == 0);

    s.streak = 1;
    coach_template(s, t, sizeof(t));
    CHECK(strcmp(t, "No snoozes lately. Keep it quick and boring.") == 0);

    s.snoozes = 2;
    coach_template(s, t, sizeof(t));
    CHECK(strcmp(t, "Steady so far. Defuse faster, own the day.") == 0);

    /* whatever the numbers: valid and at most 80 characters */
    s.count = 7;
    s.snoozes = 255;
    s.streak = 100000;
    s.avg_strikes_x10 = 65535;
    coach_template(s, t, sizeof(t));
    CHECK(brief_text_ok(t, BRIEF_MIN_CHARS, BRIEF_COACH_MAX));
    s.snoozes = 0;
    coach_template(s, t, sizeof(t));
    CHECK(brief_text_ok(t, BRIEF_MIN_CHARS, BRIEF_COACH_MAX));
    s.avg_strikes_x10 = 0;
    coach_template(s, t, sizeof(t));
    CHECK(brief_text_ok(t, BRIEF_MIN_CHARS, BRIEF_COACH_MAX) && strstr(t, "999-day streak") != NULL);
    CHECK(coach_template(s, t, 10) == 9);
}

static void test_user_prompt(void)
{
    BriefClass c[2];
    char p[400];
    char tiny[30];

    memset(c, 0, sizeof(c));
    snprintf(c[0].module, sizeof(c[0].module), "CG2271");
    snprintf(c[0].type, sizeof(c[0].type), "LEC");
    snprintf(c[0].venue, sizeof(c[0].venue), "LT19");
    c[0].start_min = 8 * 60 + 30;
    snprintf(c[1].module, sizeof(c[1].module), "CS2040");
    snprintf(c[1].type, sizeof(c[1].type), "TUT");
    c[1].start_min = 10 * 60;

    CHECK(brief_user_prompt("Fri 07:30", c, 2, "streak 2 days", "No history yet: standard bomb", p, sizeof(p)) == strlen(p));
    CHECK(strcmp(p, "Next alarm: Fri 07:30\nClasses that day: CG2271 LEC 08:30 LT19; CS2040 TUT 10:00 -\n"
                    "History: streak 2 days\nDifficulty: No history yet: standard bomb") == 0);
    brief_user_prompt("Fri 07:30", NULL, 0, "none", "x", p, sizeof(p));
    CHECK(strstr(p, "Classes that day: none") != NULL);
    CHECK(brief_user_prompt("Fri 07:30", c, 2, "streak 2 days", "d", tiny, sizeof(tiny)) == strlen(tiny));   /* cut, terminated */
    CHECK(strlen(tiny) < sizeof(tiny));
    CHECK(brief_user_prompt("x", c, 2, "s", "d", NULL, 0) == 0);
}

/* ================= LLM reply validation ================= */
static void test_text_ok(void)
{
    CHECK(brief_text_ok("CG2271 at LT19, 08:30", 8, 120));
    CHECK(!brief_text_ok("short", 8, 120));                            /* too short */
    CHECK(!brief_text_ok("0123456789", 1, 9));                         /* too long */
    CHECK(brief_text_ok("0123456789", 1, 10));
    CHECK(!brief_text_ok(" leading space here", 1, 80));
    CHECK(!brief_text_ok("trailing space here ", 1, 80));
    CHECK(!brief_text_ok("tab\there is bad", 1, 80));
    CHECK(!brief_text_ok("newline\nhere ok no", 1, 80));
    CHECK(!brief_text_ok("caf\xC3\xA9 is not ASCII", 1, 80));           /* UTF-8 */
    CHECK(!brief_text_ok("smart \x93quotes\x94 here", 1, 80));
    CHECK(!brief_text_ok("del\x7F char here ok", 1, 80));
    CHECK(brief_text_ok("~ tilde and ! bang are fine", 1, 80));
    CHECK(!brief_text_ok("", 0, 80));
    CHECK(!brief_text_ok(NULL, 1, 80));
}

static void test_brief_parse(void)
{
    char scratch[512];
    BriefText b;
    int ok;

    ok = brief_parse("{\"briefing\":\"CG2271 LEC 14:00 at LT19, then CS2040 at 16:00.\",\"coach\":\"Two snoozes? Bold strategy.\"}",
                     scratch, sizeof(scratch), &b);
    CHECK(ok == (BRIEF_OK_BRIEFING | BRIEF_OK_COACH));
    CHECK(strcmp(b.briefing, "CG2271 LEC 14:00 at LT19, then CS2040 at 16:00.") == 0 && strcmp(b.coach, "Two snoozes? Bold strategy.") == 0);

    /* reasoning text and a fence around the JSON */
    ok = brief_parse("<think>hmm {\"x\":1}</think>```json\n{\"coach\":\"Up and at them, champ!\",\"briefing\":\"Lecture at 9, lab at 2.\"}\n```",
                     scratch, sizeof(scratch), &b);
    CHECK(ok == 3 && strcmp(b.coach, "Up and at them, champ!") == 0);

    /* one field too long: only the other one survives */
    {
        char reply[400];
        char longs[100];

        memset(longs, 'a', sizeof(longs));
        longs[sizeof(longs) - 1] = '\0';                                       /* 99 chars, coach limit is 80 */
        snprintf(reply, sizeof(reply), "{\"briefing\":\"Lecture at 9 in LT19.\",\"coach\":\"%s\"}", longs);
        ok = brief_parse(reply, scratch, sizeof(scratch), &b);
        CHECK(ok == BRIEF_OK_BRIEFING && b.coach[0] == '\0' && strcmp(b.briefing, "Lecture at 9 in LT19.") == 0);
    }
    ok = brief_parse("{\"briefing\":\"x\",\"coach\":\"Fine coach line here\"}", scratch, sizeof(scratch), &b);   /* briefing too short */
    CHECK(ok == BRIEF_OK_COACH && b.briefing[0] == '\0');
    /* a non-ASCII \u escape makes json_get_string give up on the whole object: reply rejected, the caller
     * moves on to the next model or the template. An ASCII escape is decoded. */
    ok = brief_parse("{\"briefing\":\"caf\\u00e9 at nine, room 2\",\"coach\":\"All good, go\"}", scratch, sizeof(scratch), &b);
    CHECK(ok == 0 && b.briefing[0] == '\0' && b.coach[0] == '\0');
    ok = brief_parse("{\"briefing\":\"Room \\u0041 at nine\",\"coach\":\"All good, go\"}", scratch, sizeof(scratch), &b);
    CHECK(ok == 3 && strcmp(b.briefing, "Room A at nine") == 0);

    /* missing keys, no JSON, wrong types */
    CHECK(brief_parse("{\"other\":\"value long enough\"}", scratch, sizeof(scratch), &b) == 0);
    CHECK(brief_parse("Sorry, I cannot help.", scratch, sizeof(scratch), &b) == 0);
    CHECK(brief_parse("{\"briefing\":\"truncated reply that never ends", scratch, sizeof(scratch), &b) == 0);
    CHECK(brief_parse("", scratch, sizeof(scratch), &b) == 0 && b.briefing[0] == '\0' && b.coach[0] == '\0');
    CHECK(brief_parse(NULL, scratch, sizeof(scratch), &b) == 0);
    CHECK(brief_parse("{}", NULL, 0, &b) == 0);
    CHECK(brief_parse("{}", scratch, sizeof(scratch), NULL) == 0);
    CHECK(brief_parse("{\"briefing\":\"abcdefghij\",\"coach\":\"abcdefghij\"}", scratch, 10, &b) == 0);   /* scratch too small */
}

static void test_pick_and_pages(void)
{
    BriefText stored;
    BriefText tmpl;
    const uint32_t today = 20000u;

    memset(&stored, 0, sizeof(stored));
    memset(&tmpl, 0, sizeof(tmpl));
    CHECK(brief_pick(stored, tmpl, today) == NULL);                            /* nothing at all */

    stored.for_day = today;
    snprintf(stored.briefing, sizeof(stored.briefing), "%s", "Stored briefing for today");
    snprintf(stored.coach, sizeof(stored.coach), "%s", "Stored coach for today");
    CHECK(brief_valid(stored));
    CHECK(brief_pick(stored, tmpl, today) == &stored);

    stored.for_day = today - 1u;                                               /* stale: yesterday's text */
    CHECK(brief_pick(stored, tmpl, today) == NULL);
    tmpl.for_day = today;
    snprintf(tmpl.briefing, sizeof(tmpl.briefing), "%s", "Template briefing today");
    snprintf(tmpl.coach, sizeof(tmpl.coach), "%s", "Template coach today");
    CHECK(brief_pick(stored, tmpl, today) == &tmpl);
    stored.for_day = today;
    CHECK(brief_pick(stored, tmpl, today) == &stored);                         /* stored wins when current */
    stored.briefing[0] = '\0';                                                 /* stored but invalid */
    CHECK(!brief_valid(stored) && brief_pick(stored, tmpl, today) == &tmpl);
    CHECK(brief_pick(stored, tmpl, 0u) == NULL);                               /* clock unknown */
    tmpl.for_day = today + 1u;
    CHECK(brief_pick(stored, tmpl, today) == NULL);

    CHECK(local_day(0u) == 0u);                                                /* 1970-01-01 08:00 local */
    CHECK(local_day(16u * 3600u - 1u) == 0u && local_day(16u * 3600u) == 1u);  /* local midnight = 16:00 UTC */
    CHECK(local_day(sched_local_to_unix(2026, 8, 12, 23, 59)) == local_day(sched_local_to_unix(2026, 8, 12, 0, 1)));
    CHECK(local_day(sched_local_to_unix(2026, 8, 13, 0, 0)) == local_day(sched_local_to_unix(2026, 8, 12, 0, 0)) + 1u);

    CHECK(defused_page(0) == 0 && defused_page(3999) == 0);
    CHECK(defused_page(4000) == 1 && defused_page(7999) == 1);
    CHECK(defused_page(8000) == 2 && defused_page(11999) == 2);
    CHECK(defused_page(12000) == 0);                                           /* wraps over the 30 s hold */
    CHECK(defused_page(30000) == 1);
}

int main(void)
{
    test_wrap();
    test_marquee();
    test_fit_text();
    test_dial_hint();
    test_wire_hint();
    test_summary();
    test_result_line();
    test_classes_on_day();
    test_briefing_template();
    test_coach_template();
    test_user_prompt();
    test_text_ok();
    test_brief_parse();
    test_pick_and_pages();
    printf("test_text: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
