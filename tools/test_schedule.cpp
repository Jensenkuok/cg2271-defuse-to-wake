/* Host-side tests for esp32/defuse_esp32/src/app/schedule.{h,cpp}. Plain checks, no framework.
 * Fixtures are typed from the verified NUSMods facts in CLAUDE.md (AY2026/27 Sem 1). */
#include <stdio.h>
#include <string.h>

#include "schedule.h"
#include "../shared/protocol.h"

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

static void test_api_field_parsers(void)
{
    CHECK(sched_weekday_from_name("Monday") == 1);
    CHECK(sched_weekday_from_name("Saturday") == 6);
    CHECK(sched_weekday_from_name("Sunday") == 0);
    CHECK(sched_weekday_from_name("monday") == 0);
    CHECK(sched_weekday_from_name(NULL) == 0);
    CHECK(sched_parse_hhmm("0800") == 480);
    CHECK(sched_parse_hhmm("1600") == 960);
    CHECK(sched_parse_hhmm("2359") == 1439);
    CHECK(sched_parse_hhmm("0000") == 0);
    CHECK(sched_parse_hhmm("2400") == -1);
    CHECK(sched_parse_hhmm("0860") == -1);
    CHECK(sched_parse_hhmm("800") == -1);
    CHECK(sched_parse_hhmm("08:00") == -1);
    CHECK(sched_parse_hhmm("08a0") == -1);
    CHECK(sched_parse_hhmm(NULL) == -1);
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

    /* Fri 16 Oct 13:00: Friday's first class (QF 12:00) has started, so the CG2271 16:00 lecture is
     * NOT used (first class of each day only); Mon/Tue have no class in week 10, Wed 21 08:00 is next. */
    a = alarm_at(2026, 10, 16, 13, 0, 0, 60);
    CHECK(a.found && a.lesson_idx == L_QF_LEC_WED);
    CHECK(a.alarm_unix == sched_local_to_unix(2026, 10, 21, 7, 0));
}

static void test_next_alarm_lead_edges(void)
{
    /* QF lecture Fri 16 Oct 12:00, lead 60 -> alarm 11:00:00 */
    NextAlarm a = alarm_at(2026, 10, 16, 10, 58, 59, 60);       /* alarm is 61 s away: counts */

    CHECK(a.found && a.lesson_idx == L_QF_LEC_FRI);
    /* Once the first class's alarm is too close or past, the day is over: the 16:00 CG2271 lecture
     * never takes its place, the next day's first class (Wed 21 Oct 08:00) does. */
    a = alarm_at(2026, 10, 16, 10, 59, 0, 60);                  /* exactly 60 s away: not "> now + 60" */
    CHECK(a.found && a.lesson_idx == L_QF_LEC_WED);
    a = alarm_at(2026, 10, 16, 10, 59, 30, 60);                 /* 30 s away */
    CHECK(a.found && a.lesson_idx == L_QF_LEC_WED);
    a = alarm_at(2026, 10, 16, 11, 0, 0, 60);                   /* alarm time itself */
    CHECK(a.found && a.lesson_idx == L_QF_LEC_WED);

    /* Lead longer than the time to class: the class is skipped even though it has not started. */
    a = alarm_at(2026, 10, 16, 11, 30, 0, 60);
    CHECK(a.found && a.lesson_idx == L_QF_LEC_WED);
    a = alarm_at(2026, 10, 16, 8, 0, 0, 180);                   /* 09:00 alarm for 12:00 class: still ahead */
    CHECK(a.found && a.lesson_idx == L_QF_LEC_FRI);
    a = alarm_at(2026, 10, 16, 9, 30, 0, 180);                  /* 09:00 already passed */
    CHECK(a.found && a.lesson_idx == L_QF_LEC_WED);
    CHECK(a.alarm_unix == sched_local_to_unix(2026, 10, 21, 5, 0));
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

/* ---------------- first class per day ---------------- */
/* Mon (array order scrambled on purpose): 14:00, 09:00, 11:00. Wed: 10:00. Fri: 08:00 only in week 10,
 * 13:00 every week. */
enum { F_MON_1400 = 0, F_MON_0900, F_MON_1100, F_WED_1000, F_FRI_0800_W10, F_FRI_1300, F_COUNT };

static void build_day_fixtures(Lesson *l)
{
    l[F_MON_1400] = make_lesson("AAA1000", "LEC", 1, 1400, 1500, kWeeks1to13, "X");
    l[F_MON_0900] = make_lesson("BBB1000", "LEC", 1, 900, 1000, kWeeks1to13, "X");
    l[F_MON_1100] = make_lesson("CCC1000", "TUT", 1, 1100, 1200, kWeeks1to13, "X");
    l[F_WED_1000] = make_lesson("DDD1000", "LEC", 3, 1000, 1100, kWeeks1to13, "X");
    l[F_FRI_0800_W10] = make_lesson("EEE1000", "LEC", 5, 800, 900, 0x0200, "X");
    l[F_FRI_1300] = make_lesson("FFF1000", "LEC", 5, 1300, 1400, kWeeks1to13, "X");
}

static void test_first_class_only(void)
{
    Lesson l[F_COUNT];
    NextAlarm a;

    build_day_fixtures(l);
    /* Sun 11 Oct 12:00 -> Mon 12 Oct (week 9): the 09:00 class, not 11:00 or 14:00. */
    a = next_alarm(l, F_COUNT, sched_local_to_unix(2026, 10, 11, 12, 0), 60);
    CHECK(a.found && a.lesson_idx == F_MON_0900);
    CHECK(a.class_start_unix == sched_local_to_unix(2026, 10, 12, 9, 0));
    CHECK(a.alarm_unix == sched_local_to_unix(2026, 10, 12, 8, 0));

    /* Mon 12 Oct 06:00 and 07:58:59 (alarm 08:00 is still more than 60 s away): same alarm. */
    a = next_alarm(l, F_COUNT, sched_local_to_unix(2026, 10, 12, 6, 0), 60);
    CHECK(a.found && a.lesson_idx == F_MON_0900);
    a = next_alarm(l, F_COUNT, sched_local_to_unix(2026, 10, 12, 7, 58) + 59u, 60);
    CHECK(a.found && a.lesson_idx == F_MON_0900);

    /* The weeks mask picks the first class that is actually taught: Fri 16 Oct (week 9) has only
     * the 13:00 class (the 08:00 one is week 10 only), Fri 23 Oct (week 10) has 08:00 first. */
    a = next_alarm(l, F_COUNT, sched_local_to_unix(2026, 10, 14, 12, 0), 60);
    CHECK(a.found && a.lesson_idx == F_FRI_1300);
    CHECK(a.alarm_unix == sched_local_to_unix(2026, 10, 16, 12, 0));
    a = next_alarm(l, F_COUNT, sched_local_to_unix(2026, 10, 22, 12, 0), 60);
    CHECK(a.found && a.lesson_idx == F_FRI_0800_W10);
    CHECK(a.alarm_unix == sched_local_to_unix(2026, 10, 23, 7, 0));
}

static void test_first_class_next_day_switch(void)
{
    Lesson l[F_COUNT];
    NextAlarm a;

    build_day_fixtures(l);
    /* Mon 12 Oct 09:00:00 - the first class starts: 11:00 and 14:00 do not take over, Wednesday does. */
    a = next_alarm(l, F_COUNT, sched_local_to_unix(2026, 10, 12, 9, 0), 60);
    CHECK(a.found && a.lesson_idx == F_WED_1000);
    CHECK(a.alarm_unix == sched_local_to_unix(2026, 10, 14, 9, 0));
    a = next_alarm(l, F_COUNT, sched_local_to_unix(2026, 10, 12, 9, 30), 60);          /* mid-class */
    CHECK(a.found && a.lesson_idx == F_WED_1000);
    a = next_alarm(l, F_COUNT, sched_local_to_unix(2026, 10, 12, 23, 59) + 59u, 60);   /* end of the day */
    CHECK(a.found && a.lesson_idx == F_WED_1000);
    a = next_alarm(l, F_COUNT, sched_local_to_unix(2026, 10, 12, 0, 0), 60);           /* local midnight */
    CHECK(a.found && a.lesson_idx == F_MON_0900);
}

static void test_first_class_lead_edge(void)
{
    Lesson l[F_COUNT];
    NextAlarm a;

    build_day_fixtures(l);
    /* Mon 12 Oct 08:30: the 09:00 class has not started but its 08:00 alarm is past. The 11:00 class
     * (alarm 10:00, still ahead) is NOT used: next day's first class. */
    a = next_alarm(l, F_COUNT, sched_local_to_unix(2026, 10, 12, 8, 30), 60);
    CHECK(a.found && a.lesson_idx == F_WED_1000);
    /* A shorter lead keeps today: lead 15 -> alarm 08:45 is ahead. */
    a = next_alarm(l, F_COUNT, sched_local_to_unix(2026, 10, 12, 8, 30), 15);
    CHECK(a.found && a.lesson_idx == F_MON_0900);
    CHECK(a.alarm_unix == sched_local_to_unix(2026, 10, 12, 8, 45));
    /* Edge: exactly 60 s before the alarm does not count, 61 s does. */
    a = next_alarm(l, F_COUNT, sched_local_to_unix(2026, 10, 12, 7, 59), 60);
    CHECK(a.found && a.lesson_idx == F_WED_1000);
    a = next_alarm(l, F_COUNT, sched_local_to_unix(2026, 10, 12, 7, 58) + 59u, 60);
    CHECK(a.found && a.lesson_idx == F_MON_0900);
}

static void test_consumed_class(void)
{
    Lesson l[F_COUNT];
    NextAlarm a;
    const uint32_t mon_start = sched_local_to_unix(2026, 10, 12, 9, 0);

    build_day_fixtures(l);
    /* Mon 12 Oct 06:00 would give Monday's alarm; once it rang (class start consumed) it gives
     * Wednesday, whatever the lead is. */
    a = next_alarm(l, F_COUNT, sched_local_to_unix(2026, 10, 12, 6, 0), 60, mon_start);
    CHECK(a.found && a.lesson_idx == F_WED_1000);
    a = next_alarm(l, F_COUNT, sched_local_to_unix(2026, 10, 12, 6, 0), 15, mon_start);
    CHECK(a.found && a.lesson_idx == F_WED_1000);
    /* A consumed class from an earlier day changes nothing; 0 = nothing consumed. */
    a = next_alarm(l, F_COUNT, sched_local_to_unix(2026, 10, 12, 6, 0), 60, sched_local_to_unix(2026, 10, 5, 9, 0));
    CHECK(a.found && a.lesson_idx == F_MON_0900);
    a = next_alarm(l, F_COUNT, sched_local_to_unix(2026, 10, 12, 6, 0), 60, 0u);
    CHECK(a.found && a.lesson_idx == F_MON_0900);
}

/* ---------------- alarm_action ---------------- */
static const uint32_t kAlarmA = 1800000000u;
static const uint32_t kAlarmB = 1800086400u;

static void test_alarm_action_session(void)
{
    /* Session active: nothing, in every state, with or without a TIME_SYNC. */
    CHECK(alarm_action(kAlarmA, kAlarmA, PROTO_STATE_ARMED, true, true) == ALARM_ACTION_NONE);   /* snooze re-send blocked */
    CHECK(alarm_action(kAlarmA, kAlarmA, PROTO_STATE_ARMED, true, false) == ALARM_ACTION_NONE);
    CHECK(alarm_action(kAlarmB, kAlarmA, PROTO_STATE_ARMED, true, true) == ALARM_ACTION_NONE);   /* even a new alarm */
    CHECK(alarm_action(kAlarmA, 0u, PROTO_STATE_IDLE, true, true) == ALARM_ACTION_NONE);
    CHECK(alarm_action(0u, kAlarmA, PROTO_STATE_ARMED, true, true) == ALARM_ACTION_NONE);        /* no cancel either */
    CHECK(alarm_action(0u, kAlarmA, PROTO_STATE_ARMED, true, false) == ALARM_ACTION_NONE);
    CHECK(alarm_action(kAlarmA, kAlarmA, PROTO_STATE_BOMB_ACTIVE, true, true) == ALARM_ACTION_NONE);
}

static void test_alarm_action_set(void)
{
    /* New or changed alarm: send. */
    CHECK(alarm_action(kAlarmA, 0u, PROTO_STATE_IDLE, false, false) == ALARM_ACTION_SEND_SET_ALARM);
    CHECK(alarm_action(kAlarmB, kAlarmA, PROTO_STATE_ARMED, false, false) == ALARM_ACTION_SEND_SET_ALARM);
    /* Unchanged alarm: only re-sent after a TIME_SYNC. */
    CHECK(alarm_action(kAlarmA, kAlarmA, PROTO_STATE_ARMED, false, false) == ALARM_ACTION_NONE);
    CHECK(alarm_action(kAlarmA, kAlarmA, PROTO_STATE_IDLE, false, false) == ALARM_ACTION_NONE);
    CHECK(alarm_action(kAlarmA, kAlarmA, PROTO_STATE_ARMED, false, true) == ALARM_ACTION_SEND_SET_ALARM);
    CHECK(alarm_action(kAlarmA, kAlarmA, PROTO_STATE_IDLE, false, true) == ALARM_ACTION_SEND_SET_ALARM);
}

static void test_alarm_action_cancel(void)
{
    /* Cancel: only when nothing is wanted, the MCX is ARMED, no session, and something was sent. */
    CHECK(alarm_action(0u, kAlarmA, PROTO_STATE_ARMED, false, false) == ALARM_ACTION_SEND_CANCEL);
    CHECK(alarm_action(0u, kAlarmA, PROTO_STATE_ARMED, false, true) == ALARM_ACTION_SEND_CANCEL);
    CHECK(alarm_action(0u, 0u, PROTO_STATE_ARMED, false, false) == ALARM_ACTION_NONE);          /* nothing was sent */
    CHECK(alarm_action(0u, 0u, PROTO_STATE_ARMED, false, true) == ALARM_ACTION_NONE);
    CHECK(alarm_action(0u, kAlarmA, PROTO_STATE_IDLE, false, false) == ALARM_ACTION_NONE);      /* not ARMED */
    CHECK(alarm_action(0u, kAlarmA, PROTO_STATE_BOMB_ACTIVE, false, false) == ALARM_ACTION_NONE);
    CHECK(alarm_action(0u, kAlarmA, PROTO_STATE_DEFUSED, false, false) == ALARM_ACTION_NONE);
    CHECK(alarm_action(0u, kAlarmA, PROTO_STATE_EXPLODED, false, false) == ALARM_ACTION_NONE);
}

static void test_session_next(void)
{
    CHECK(!session_next(false, PROTO_STATE_IDLE));
    CHECK(!session_next(false, PROTO_STATE_ARMED));                  /* ARMED alone is not a session */
    CHECK(session_next(false, PROTO_STATE_BOMB_ACTIVE));             /* first BOMB_ACTIVE starts it */
    CHECK(session_next(true, PROTO_STATE_BOMB_ACTIVE));
    CHECK(session_next(true, PROTO_STATE_ARMED));                    /* snooze keeps it */
    CHECK(session_next(true, PROTO_STATE_EXPLODED));                 /* explosion -> next bomb keeps it */
    CHECK(session_next(true, PROTO_STATE_DEFUSED));                  /* 30 s hold keeps it */
    CHECK(!session_next(true, PROTO_STATE_IDLE));                    /* IDLE ends it */
    CHECK(!session_next(false, PROTO_STATE_DEFUSED));                /* no BOMB_ACTIVE seen: not a session */
}

int main(void)
{
    build_fixtures();
    test_parse_example_link();
    test_class_no_padding();
    test_parse_abbreviations();
    test_parse_malformed();
    test_api_field_parsers();
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
    test_first_class_only();
    test_first_class_next_day_switch();
    test_first_class_lead_edge();
    test_consumed_class();
    test_alarm_action_session();
    test_alarm_action_set();
    test_alarm_action_cancel();
    test_session_next();

    printf("test_schedule: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
