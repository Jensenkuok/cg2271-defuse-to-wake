/*
 * Defuse to Wake - timetable parsing and alarm scheduling (pure logic).
 * No Arduino headers, no globals, no heap: it is compiled by the sketch and by the host
 * tests (tools/test_schedule.cpp, C++11). The network task feeds it lessons; it never
 * touches Wi-Fi, flash or the clock itself, so every rule below is testable on a PC.
 *
 * Calendar: NUS AY2026/27 Semester 1. Week 1 starts Mon 2026-08-10, the recess week
 * (Mon 2026-09-21 .. Sun 2026-09-27) sits between weeks 6 and 7, weeks 7..13 follow,
 * and nothing is taught after week 13. All class times are Asia/Singapore (UTC+8, no DST).
 */
#ifndef SCHEDULE_H
#define SCHEDULE_H

#include <stdint.h>

/* ---- Semester calendar (change these for a new semester) ---- */
#define SCHED_WEEK1_YEAR            2026
#define SCHED_WEEK1_MONTH           8
#define SCHED_WEEK1_DAY             10      /* a Monday */
#define SCHED_RECESS_AFTER_WEEK     6
#define SCHED_LAST_WEEK             13
#define SCHED_TZ_OFFSET_S           (8 * 3600)

/* ---- Limits (fixed-size arrays, no dynamic allocation) ---- */
#define SCHED_MODULE_LEN            12      /* "GEN2061Y" + NUL, with room to spare */
#define SCHED_ABBREV_LEN            6       /* "TUT2" / "DLEC" + NUL */
#define SCHED_CLASSNO_LEN           8
#define SCHED_VENUE_LEN             16
#define SCHED_MAX_SLOTS             24      /* module/type/class entries in one share link */
#define SCHED_MAX_LESSONS           40      /* lessons kept in the cache */

/* ---- Alarm rules ---- */
#define SCHED_LOOKAHEAD_DAYS        7       /* only classes that start within this window count */
#define SCHED_MIN_LEAD_S            60      /* the alarm must be more than this far in the future */

#define SCHED_SEC_PER_MIN           60
#define SCHED_SEC_PER_DAY           86400
#define SCHED_MAX_WEEKDAY           6       /* lessons are Monday (1) .. Saturday (6) */
#define SCHED_SUNDAY                7
#define SCHED_MIN_PER_DAY           1440

/* One "MODULE=ABBR:classNo" entry of a NUSMods share link, e.g. CG2271 / LEC / 01. */
struct ShareSlot {
    char module[SCHED_MODULE_LEN];
    char abbrev[SCHED_ABBREV_LEN];
    char class_no[SCHED_CLASSNO_LEN];   /* raw text: padding differs from the API ("1" vs "01") */
};

struct ShareLink {
    ShareSlot slots[SCHED_MAX_SLOTS];
    uint8_t count;                      /* valid entries in slots[] */
    uint8_t skipped;                    /* unknown abbreviations, malformed or overflowing entries */
};

struct Lesson {
    char module[SCHED_MODULE_LEN];
    char type[SCHED_ABBREV_LEN];        /* share-link abbreviation, e.g. "LEC" */
    uint8_t weekday;                    /* 1 = Monday .. 6 = Saturday */
    uint16_t start_min;                 /* minutes after local midnight */
    uint16_t end_min;
    uint16_t weeks;                     /* bit (w - 1) set = taught in teaching week w (1..13) */
    char venue[SCHED_VENUE_LEN];
};

struct NextAlarm {
    bool found;
    uint32_t alarm_unix;                /* class start - lead */
    uint32_t class_start_unix;
    int lesson_idx;                     /* index into the lesson array, -1 if none */
};

/* Parse the query part of a NUSMods share link. Entries whose abbreviation is not in the
 * table below are skipped and counted in out->skipped. False if url/out is NULL or the
 * link has no '?' query. */
bool parse_share_link(const char *url, ShareLink *out);

/* NUSMods lessonType for a share-link abbreviation ("LEC" -> "Lecture"), NULL if unknown. */
const char *sched_lesson_type_name(const char *abbrev);

/* Class numbers match numerically when both are all digits ("1" == "01"), otherwise as
 * exact strings ("A1" != "a1"). */
bool sched_class_no_equal(const char *a, const char *b);

/* NUSMods "day" ("Monday".."Saturday") -> 1..6, 0 if not a teaching day name. */
int sched_weekday_from_name(const char *day);

/* NUSMods "HHMM" -> minutes after midnight, -1 if it is not four digits with hh < 24, mm < 60. */
int sched_parse_hhmm(const char *hhmm);

/* Teaching week 1..13 for a local date, or 0 for recess, before week 1 and after week 13. */
int teaching_week(int year, int month, int day);

/* 1 = Monday .. 7 = Sunday. */
int sched_weekday(int year, int month, int day);

/* Local (UTC+8) wall-clock time to unix seconds. */
uint32_t sched_local_to_unix(int year, int month, int day, int hour, int minute);

/* The earliest lesson start within SCHED_LOOKAHEAD_DAYS of now_unix whose alarm
 * (start - lead_min) is more than SCHED_MIN_LEAD_S after now_unix, honouring the weeks mask,
 * the recess week and the end of semester. Ties go to the lower lesson index. */
NextAlarm next_alarm(const Lesson *lessons, int count, uint32_t now_unix, int lead_min);

#endif /* SCHEDULE_H */
