/* Host-side tests for esp32/defuse_esp32/src/app/schedule.{h,cpp}. Plain checks, no framework.
 * Fixtures are typed from the verified NUSMods facts in CLAUDE.md (AY2026/27 Sem 1). */
#include <stdio.h>
#include <string.h>

#include "schedule.h"

static int g_checks;
static int g_failed;

#define CHECK(cond)                                                              \
    do {                                                                         \
        g_checks++;                                                              \
        if (!(cond)) {                                                           \
            g_failed++;                                                          \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);              \
        }                                                                        \
    } while (0)

static const uint16_t kWeeks1to13 = 0x1FFF;
static const uint16_t kWeeks3to13 = 0x1FFC;
static const uint16_t kWeek9Only = 0x0100;

enum { L_CG2271_LEC = 0, L_QF_LEC_WED, L_QF_LEC_FRI, L_QF_TUT, L_GEN_TUT, L_COUNT };
static Lesson g_lessons[L_COUNT];

static Lesson make_lesson(const char *module, const char *type, int weekday, int start_hhmm, int end_hhmm,
                          uint16_t weeks, const char *venue)
{
    Lesson l;

    memset(&l, 0, sizeof(l));
    strcpy(l.module, module);
    strcpy(l.type, type);
    l.weekday = (uint8_t)weekday;
    l.start_min = (uint16_t)((start_hhmm / 100) * 60 + start_hhmm % 100);
    l.end_min = (uint16_t)((end_hhmm / 100) * 60 + end_hhmm % 100);
    l.weeks = weeks;
    strcpy(l.venue, venue);
    return l;
}

static void build_fixtures(void)
{
    g_lessons[L_CG2271_LEC] = make_lesson("CG2271", "LEC", 5, 1600, 1800, kWeeks1to13, "LT19");
    g_lessons[L_QF_LEC_WED] = make_lesson("QF2104", "LEC", 3, 800, 1000, kWeeks1to13, "LT33");
    g_lessons[L_QF_LEC_FRI] = make_lesson("QF2104", "LEC", 5, 1200, 1400, kWeeks1to13, "LT33");
    g_lessons[L_QF_TUT] = make_lesson("QF2104", "TUT", 4, 1000, 1100, kWeeks3to13, "COM1-0210");
    g_lessons[L_GEN_TUT] = make_lesson("GEN2061Y", "TUT", 1, 800, 1000, kWeek9Only, "AS1-0201");
}

static NextAlarm alarm_at(int y, int m, int d, int hh, int mm, int sec, int lead)
{
    return next_alarm(g_lessons, L_COUNT, sched_local_to_unix(y, m, d, hh, mm) + (uint32_t)sec, lead);
}

/* ---------------- share link ---------------- */
static const char *const kExampleLink =
    "https://nusmods.com/timetable/sem-1/share?CG2028=LAB:02,LEC:01,TUT:05&CG2271=LAB:02,LEC:01,TUT:02"
    "&GEN2061Y=TUT:01&QF2104=LEC:1,TUT:4&ST3131=LEC:1,TUT:12";

static void test_parse_example_link(void)
{
    ShareLink s;

    CHECK(parse_share_link(kExampleLink, &s));
    CHECK(s.count == 11);
    CHECK(s.skipped == 0);
    CHECK(strcmp(s.slots[0].module, "CG2028") == 0);
    CHECK(strcmp(s.slots[0].abbrev, "LAB") == 0);
    CHECK(strcmp(s.slots[0].class_no, "02") == 0);
    CHECK(strcmp(s.slots[5].module, "CG2271") == 0);
    CHECK(strcmp(s.slots[5].abbrev, "TUT") == 0);
    CHECK(strcmp(s.slots[5].class_no, "02") == 0);
    CHECK(strcmp(s.slots[6].module, "GEN2061Y") == 0);
    CHECK(strcmp(s.slots[7].module, "QF2104") == 0);
    CHECK(strcmp(s.slots[7].abbrev, "LEC") == 0);
    CHECK(strcmp(s.slots[7].class_no, "1") == 0);       /* raw, unpadded */
    CHECK(strcmp(s.slots[8].class_no, "4") == 0);
    CHECK(strcmp(s.slots[10].module, "ST3131") == 0);
    CHECK(strcmp(s.slots[10].class_no, "12") == 0);
}

static void test_class_no_padding(void)
{
    CHECK(sched_class_no_equal("1", "01"));             /* link "1" vs API "01" */
    CHECK(sched_class_no_equal("01", "1"));
    CHECK(sched_class_no_equal("12", "12"));
    CHECK(sched_class_no_equal("007", "7"));
    CHECK(!sched_class_no_equal("1", "2"));
    CHECK(!sched_class_no_equal("1", "11"));
    CHECK(sched_class_no_equal("A1", "A1"));            /* non-digit: exact string */
    CHECK(!sched_class_no_equal("A1", "a1"));
    CHECK(!sched_class_no_equal("A1", "A01"));
    CHECK(!sched_class_no_equal("", "0"));
    CHECK(!sched_class_no_equal(NULL, "1"));
}

static void test_parse_abbreviations(void)
{
    ShareLink s;
    static const char *const abbrevs[] = { "LEC", "TUT", "LAB", "REC", "SEC", "SEM",
                                           "DLEC", "PLEC", "PTUT", "TUT2", "TUT3", "WS" };
    static const char *const names[] = { "Lecture", "Tutorial", "Laboratory", "Recitation", "Sectional Teaching",
                                         "Seminar-Style Module Teaching", "Design Lecture", "Packaged Lecture",
                                         "Packaged Tutorial", "Tutorial Type 2", "Tutorial Type 3", "Workshop" };
    size_t i;

    for (i = 0; i < sizeof(abbrevs) / sizeof(abbrevs[0]); i++) {
        CHECK(sched_lesson_type_name(abbrevs[i]) != NULL);
        CHECK(sched_lesson_type_name(abbrevs[i]) != NULL && strcmp(sched_lesson_type_name(abbrevs[i]), names[i]) == 0);
    }
    CHECK(sched_lesson_type_name("XYZ") == NULL);
    CHECK(sched_lesson_type_name("lec") == NULL);
    CHECK(sched_lesson_type_name(NULL) == NULL);

    CHECK(parse_share_link("x?CS1010=FOO:1,LEC:2,TUT2:03,BAR:9", &s));
    CHECK(s.count == 2);
    CHECK(s.skipped == 2);                              /* FOO and BAR */
    CHECK(strcmp(s.slots[1].abbrev, "TUT2") == 0);
    CHECK(strcmp(s.slots[1].class_no, "03") == 0);
}

static void test_parse_malformed(void)
{
    ShareLink s;
    char big[1200];
    int i;

    CHECK(!parse_share_link(NULL, &s));
    CHECK(!parse_share_link("https://nusmods.com/timetable/sem-1/share", &s));  /* no query */
    CHECK(s.count == 0);
    CHECK(parse_share_link("x?", &s) && s.count == 0 && s.skipped == 0);
    CHECK(parse_share_link("x?CS1010", &s) && s.count == 0 && s.skipped == 1);  /* no '=' */
    CHECK(parse_share_link("x?=LEC:1", &s) && s.count == 0 && s.skipped == 1);  /* empty module */
    CHECK(parse_share_link("x?CS1010=LEC", &s) && s.count == 0 && s.skipped == 1);  /* no class number */
    CHECK(parse_share_link("x?CS1010=LEC:", &s) && s.count == 0 && s.skipped == 1);
    CHECK(parse_share_link("x?VERYLONGMODULECODE=LEC:1", &s) && s.count == 0 && s.skipped == 1);
    CHECK(parse_share_link("x?CS1010=LEC:123456789", &s) && s.count == 0 && s.skipped == 1);
    CHECK(parse_share_link("x?CS1010=LEC:1#frag", &s) && s.count == 1 && strcmp(s.slots[0].class_no, "1") == 0);

    /* more than SCHED_MAX_SLOTS entries: the extras are counted, never written past the array */
    strcpy(big, "x?");
    for (i = 0; i < SCHED_MAX_SLOTS + 1; i++) {
        strcat(big, i == 0 ? "M=LEC:1" : ",LEC:1");
    }
    CHECK(parse_share_link(big, &s));
    CHECK(s.count == SCHED_MAX_SLOTS);
    CHECK(s.skipped == 1);
}

/* ---------------- calendar ---------------- */
static void test_local_time_and_weekday(void)
{
    CHECK(sched_local_to_unix(2026, 1, 1, 8, 0) == 1767225600u);    /* 2026-01-01 00:00 UTC */
    CHECK(sched_local_to_unix(1970, 1, 1, 8, 0) == 0u);
    CHECK(sched_weekday(2026, 8, 10) == 1);                         /* week 1 Monday */
    CHECK(sched_weekday(2026, 10, 18) == 7);                        /* Sunday */
    CHECK(sched_weekday(2026, 10, 23) == 5);                        /* spec: Week 10 Fri */
    CHECK(sched_weekday(2026, 11, 13) == 5);                        /* spec: Week 13 Fri */
    CHECK(sched_weekday(2024, 2, 29) == 4);                         /* leap day, Thursday */
}

static void test_teaching_week(void)
{
    CHECK(teaching_week(2026, 8, 9) == 0);                          /* Sunday before week 1 */
    CHECK(teaching_week(2026, 8, 10) == 1);
    CHECK(teaching_week(2026, 8, 16) == 1);                         /* Sunday of week 1 */
    CHECK(teaching_week(2026, 8, 17) == 2);
    CHECK(teaching_week(2026, 9, 14) == 6);
    CHECK(teaching_week(2026, 9, 20) == 6);                         /* last day of week 6 */
    CHECK(teaching_week(2026, 9, 21) == 0);                         /* recess */
    CHECK(teaching_week(2026, 9, 24) == 0);
    CHECK(teaching_week(2026, 9, 27) == 0);
    CHECK(teaching_week(2026, 9, 28) == 7);                         /* first day of week 7 */
    CHECK(teaching_week(2026, 10, 12) == 9);
    CHECK(teaching_week(2026, 10, 23) == 10);                       /* spec: Week 10 Fri */
    CHECK(teaching_week(2026, 11, 13) == 13);                       /* spec: Week 13 Fri */
    CHECK(teaching_week(2026, 11, 15) == 13);                       /* Sunday of week 13 */
    CHECK(teaching_week(2026, 11, 16) == 0);                        /* after week 13 */
    CHECK(teaching_week(2027, 1, 11) == 0);
    CHECK(teaching_week(2025, 8, 11) == 0);
}

/* ---------------- next_alarm ---------------- */
static void test_next_alarm_basic(void)
{
    /* Thu 15 Oct 2026 (week 9) 12:00: Thursday's tutorial has passed, Friday 12:00 QF lecture is next. */
    NextAlarm a = alarm_at(2026, 10, 15, 12, 0, 0, 60);

    CHECK(a.found);
    CHECK(a.lesson_idx == L_QF_LEC_FRI);
    CHECK(a.class_start_unix == sched_local_to_unix(2026, 10, 16, 12, 0));
    CHECK(a.alarm_unix == sched_local_to_unix(2026, 10, 16, 11, 0));

    /* A longer lead moves the alarm but not the chosen class. */
    a = alarm_at(2026, 10, 15, 12, 0, 0, 180);
    CHECK(a.found && a.lesson_idx == L_QF_LEC_FRI);
    CHECK(a.alarm_unix == sched_local_to_unix(2026, 10, 16, 9, 0));

    /* Same moment, other lead: the earliest class START wins, not the earliest alarm. */
    a = alarm_at(2026, 10, 16, 13, 0, 0, 60);
    CHECK(a.found && a.lesson_idx == L_CG2271_LEC);
    CHECK(a.alarm_unix == sched_local_to_unix(2026, 10, 16, 15, 0));
}

static void test_next_alarm_lead_edges(void)
{
    /* QF lecture Fri 16 Oct 12:00, lead 60 -> alarm 11:00:00 */
    NextAlarm a = alarm_at(2026, 10, 16, 10, 58, 59, 60);       /* alarm is 61 s away: counts */

    CHECK(a.found && a.lesson_idx == L_QF_LEC_FRI);
    a = alarm_at(2026, 10, 16, 10, 59, 0, 60);                  /* exactly 60 s away: not "> now + 60" */
    CHECK(a.found && a.lesson_idx == L_CG2271_LEC);
    a = alarm_at(2026, 10, 16, 10, 59, 30, 60);                 /* 30 s away */
    CHECK(a.found && a.lesson_idx == L_CG2271_LEC);
    a = alarm_at(2026, 10, 16, 11, 0, 0, 60);                   /* alarm time itself */
    CHECK(a.found && a.lesson_idx == L_CG2271_LEC);

    /* Lead longer than the time to class: the class is skipped even though it has not started. */
    a = alarm_at(2026, 10, 16, 11, 30, 0, 60);
    CHECK(a.found && a.lesson_idx == L_CG2271_LEC);
    a = alarm_at(2026, 10, 16, 8, 0, 0, 180);                   /* 09:00 alarm for 12:00 class: still ahead */
    CHECK(a.found && a.lesson_idx == L_QF_LEC_FRI);
    a = alarm_at(2026, 10, 16, 9, 30, 0, 180);                  /* 09:00 already passed */
    CHECK(a.found && a.lesson_idx == L_CG2271_LEC);
}

static void test_next_alarm_previous_day(void)
{
    /* A class at 01:00 with a 3 h lead rings the evening before. */
    Lesson early = make_lesson("TEST1000", "LEC", 2, 100, 200, kWeeks1to13, "X");
    NextAlarm a = next_alarm(&early, 1, sched_local_to_unix(2026, 10, 12, 12, 0), 180);

    CHECK(a.found && a.lesson_idx == 0);
    CHECK(a.class_start_unix == sched_local_to_unix(2026, 10, 13, 1, 0));
    CHECK(a.alarm_unix == sched_local_to_unix(2026, 10, 12, 22, 0));
}

static void test_next_alarm_week9_only(void)
{
    /* Sun 11 Oct (week 8) 12:00: Monday 12 Oct is week 9, so the week-9-only tutorial is next. */
    NextAlarm a = alarm_at(2026, 10, 11, 12, 0, 0, 60);

    CHECK(a.found && a.lesson_idx == L_GEN_TUT);
    CHECK(a.alarm_unix == sched_local_to_unix(2026, 10, 12, 7, 0));

    /* Sun 18 Oct (week 9) 12:00: Monday 19 Oct is week 10, the tutorial is skipped, Wednesday's lecture is next. */
    a = alarm_at(2026, 10, 18, 12, 0, 0, 60);
    CHECK(a.found && a.lesson_idx == L_QF_LEC_WED);
    CHECK(a.alarm_unix == sched_local_to_unix(2026, 10, 21, 7, 0));

    /* Weeks mask honoured for the QF tutorial (weeks 3..13): Thu 20 Aug (week 2) is skipped. */
    a = alarm_at(2026, 8, 20, 8, 0, 0, 60);                     /* Thu of week 2, after nothing before it */
    CHECK(a.found && a.lesson_idx == L_QF_LEC_FRI);
    a = alarm_at(2026, 8, 27, 8, 0, 0, 60);                     /* Thu of week 3: tutorial at 10:00 counts */
    CHECK(a.found && a.lesson_idx == L_QF_TUT);
}

static void test_next_alarm_recess(void)
{
    NextAlarm a;

    /* Fri 18 Sep 17:00 (week 6, after the lectures): the next 7 days are all recess -> none. */
    a = alarm_at(2026, 9, 18, 17, 0, 0, 60);
    CHECK(!a.found && a.lesson_idx == -1 && a.alarm_unix == 0u);

    /* Mon 21 Sep 06:00 (recess): Wed 23 Sep 08:00 would be next without the recess rule; with it,
     * Wed 30 Sep 08:00 is beyond the 7-day window, so still none. */
    a = alarm_at(2026, 9, 21, 6, 0, 0, 60);
    CHECK(!a.found);

    /* Wed 23 Sep 12:00 (recess): Wed 30 Sep 08:00 (week 7) is inside the window and is picked. */
    a = alarm_at(2026, 9, 23, 12, 0, 0, 60);
    CHECK(a.found && a.lesson_idx == L_QF_LEC_WED);
    CHECK(a.class_start_unix == sched_local_to_unix(2026, 9, 30, 8, 0));
    CHECK(a.alarm_unix == sched_local_to_unix(2026, 9, 30, 7, 0));

    /* Mon 14 Sep (week 6) 12:00 -> Wed 16 Sep, ordinary week-6 lecture. */
    a = alarm_at(2026, 9, 14, 12, 0, 0, 60);
    CHECK(a.found && a.lesson_idx == L_QF_LEC_WED);
}

static void test_next_alarm_semester_end(void)
{
    NextAlarm a;

    /* Fri 13 Nov (week 13) 17:00: no more classes this week; Sat 14 / Sun 15 and after are empty. */
    a = alarm_at(2026, 11, 13, 17, 0, 0, 60);
    CHECK(!a.found);

    /* Sat 14 Nov 12:00, a no-class weekend at semester end -> none. */
    a = alarm_at(2026, 11, 14, 12, 0, 0, 60);
    CHECK(!a.found && a.lesson_idx == -1);

    /* Well after the semester. */
    a = alarm_at(2027, 1, 20, 9, 0, 0, 60);
    CHECK(!a.found);

    /* Thu 12 Nov (week 13) 12:00: Friday's lecture still counts. */
    a = alarm_at(2026, 11, 12, 12, 0, 0, 60);
    CHECK(a.found && a.lesson_idx == L_QF_LEC_FRI);

    /* Before week 1: Wed 5 Aug 2026 12:00, window reaches Wed 12 Aug 08:00 (week 1). */
    a = alarm_at(2026, 8, 5, 12, 0, 0, 60);
    CHECK(a.found && a.lesson_idx == L_QF_LEC_WED);
    CHECK(a.class_start_unix == sched_local_to_unix(2026, 8, 12, 8, 0));
}

static void test_next_alarm_sunday_rollover(void)
{
    NextAlarm a;

    /* Sun 11 Oct 23:30 -> past midnight into Monday 12 Oct 08:00 class. */
    a = alarm_at(2026, 10, 11, 23, 30, 0, 60);
    CHECK(a.found && a.lesson_idx == L_GEN_TUT);

    /* Sun 18 Oct 23:59:30 -> Monday 19 Oct has no class this week, Wednesday is next. */
    a = alarm_at(2026, 10, 18, 23, 59, 30, 60);
    CHECK(a.found && a.lesson_idx == L_QF_LEC_WED);

    /* Sunday never holds a lesson, even if the data says weekday 7 or 0. */
    {
        Lesson odd[2];
        odd[0] = make_lesson("A", "LEC", 7, 900, 1000, kWeeks1to13, "X");
        odd[1] = make_lesson("B", "LEC", 0, 900, 1000, kWeeks1to13, "X");
        a = next_alarm(odd, 2, sched_local_to_unix(2026, 10, 12, 0, 0), 60);
        CHECK(!a.found);
    }
}

static void test_next_alarm_bad_input(void)
{
    NextAlarm a = next_alarm(NULL, 3, sched_local_to_unix(2026, 10, 12, 0, 0), 60);

    CHECK(!a.found && a.lesson_idx == -1);
    a = next_alarm(g_lessons, 0, sched_local_to_unix(2026, 10, 12, 0, 0), 60);
    CHECK(!a.found);
    {
        Lesson bad = make_lesson("A", "LEC", 3, 800, 900, kWeeks1to13, "X");
        bad.start_min = SCHED_MIN_PER_DAY + 60;                                   /* corrupt cache entry */
        a = next_alarm(&bad, 1, sched_local_to_unix(2026, 10, 12, 0, 0), 60);
        CHECK(!a.found);
    }
    {
        /* Two lessons at the same start time: the lower index wins. */
        Lesson tie[2];
        tie[0] = make_lesson("A", "LEC", 3, 900, 1000, kWeeks1to13, "X");
        tie[1] = make_lesson("B", "TUT", 3, 900, 1000, kWeeks1to13, "X");
        a = next_alarm(tie, 2, sched_local_to_unix(2026, 10, 12, 0, 0), 60);
        CHECK(a.found && a.lesson_idx == 0);
    }
}

int main(void)
{
    build_fixtures();
    test_parse_example_link();
    test_class_no_padding();
    test_parse_abbreviations();
    test_parse_malformed();
    test_local_time_and_weekday();
    test_teaching_week();
    test_next_alarm_basic();
    test_next_alarm_lead_edges();
    test_next_alarm_previous_day();
    test_next_alarm_week9_only();
    test_next_alarm_recess();
    test_next_alarm_semester_end();
    test_next_alarm_sunday_rollover();
    test_next_alarm_bad_input();

    printf("test_schedule: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
