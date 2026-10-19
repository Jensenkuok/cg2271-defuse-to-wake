/* Defuse to Wake - timetable parsing and alarm scheduling. See schedule.h. */
#include "schedule.h"

#include <string.h>

#include "../shared/protocol.h"

namespace {

struct TypeEntry {
    const char *abbrev;
    const char *name;   /* NUSMods lessonType */
};

const TypeEntry k_types[] = {
    { "LEC",  "Lecture" },
    { "TUT",  "Tutorial" },
    { "LAB",  "Laboratory" },
    { "REC",  "Recitation" },
    { "SEC",  "Sectional Teaching" },
    { "SEM",  "Seminar-Style Module Teaching" },
    { "DLEC", "Design Lecture" },
    { "PLEC", "Packaged Lecture" },
    { "PTUT", "Packaged Tutorial" },
    { "TUT2", "Tutorial Type 2" },
    { "TUT3", "Tutorial Type 3" },
    { "WS",   "Workshop" },
};

const int kMonthsPerYear = 12;
const int kMarch = 3;
const int kMonthShift = 9;                 /* Hinnant: months counted from March */
const int64_t kDaysPer400Years = 146097;
const int64_t kDaysToUnixEpoch = 719468;   /* 0000-03-01 -> 1970-01-01 */
const int64_t kThursdayOffset = 3;         /* 1970-01-01 was a Thursday (Monday = 0 -> +3) */
const int kDaysPerWeek = 7;

/* Days since 1970-01-01 for a proleptic Gregorian date (Howard Hinnant's algorithm). */
int64_t days_from_civil(int y, int m, int d)
{
    const int yy = (m <= 2) ? y - 1 : y;
    const int64_t era = (yy >= 0 ? yy : yy - 399) / 400;
    const int64_t yoe = yy - era * 400;
    const int64_t mp = (m > 2) ? (m - kMarch) : (m + kMonthShift);
    const int64_t doy = (153 * mp + 2) / 5 + d - 1;
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;

    return era * kDaysPer400Years + doe - kDaysToUnixEpoch;
}

void civil_from_days(int64_t z, int *y, int *m, int *d)
{
    int64_t zz = z + kDaysToUnixEpoch;
    const int64_t era = (zz >= 0 ? zz : zz - (kDaysPer400Years - 1)) / kDaysPer400Years;
    const int64_t doe = zz - era * kDaysPer400Years;
    const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const int64_t mp = (5 * doy + 2) / 153;
    const int64_t dd = doy - (153 * mp + 2) / 5 + 1;
    const int64_t mm = (mp < 10) ? mp + kMarch : mp - kMonthShift;

    *y = (int)(yoe + era * 400 + ((mm <= 2) ? 1 : 0));
    *m = (int)mm;
    *d = (int)dd;
}

/* Floor division for negative day counts (local time before 1970 never happens, but be safe). */
int64_t floor_div(int64_t a, int64_t b)
{
    int64_t q = a / b;

    return ((a % b != 0) && ((a < 0) != (b < 0))) ? q - 1 : q;
}

/* Bounded copy that always terminates; false if src did not fit (dst holds a truncated copy). */
bool copy_bounded(char *dst, size_t dst_size, const char *src, size_t src_len)
{
    size_t n = (src_len < dst_size - 1u) ? src_len : dst_size - 1u;

    memcpy(dst, src, n);
    dst[n] = '\0';
    return n == src_len;
}

bool is_all_digits(const char *s)
{
    if (*s == '\0') {
        return false;
    }
    for (; *s != '\0'; s++) {
        if (*s < '0' || *s > '9') {
            return false;
        }
    }
    return true;
}

/* Numeric value of an all-digit string; leading zeros ignored, capped so it cannot overflow. */
uint32_t digits_value(const char *s)
{
    uint32_t v = 0;

    for (; *s != '\0'; s++) {
        if (v < 100000000u) {
            v = v * 10u + (uint32_t)(*s - '0');
        }
    }
    return v;
}

/* One "ABBR:classNo" token inside [p, end). */
void parse_slot_token(const char *module, const char *p, const char *end, ShareLink *out)
{
    const char *colon = p;
    size_t abbrev_len;
    size_t cls_len;
    ShareSlot *slot;
    char abbrev[SCHED_ABBREV_LEN];

    while (colon < end && *colon != ':') {
        colon++;
    }
    abbrev_len = (size_t)(colon - p);
    cls_len = (colon < end) ? (size_t)(end - colon - 1) : 0u;

    if (colon >= end || abbrev_len == 0u || cls_len == 0u ||
        !copy_bounded(abbrev, sizeof(abbrev), p, abbrev_len) || sched_lesson_type_name(abbrev) == NULL ||
        out->count >= SCHED_MAX_SLOTS) {
        out->skipped++;
        return;
    }
    slot = &out->slots[out->count];
    memset(slot, 0, sizeof(*slot));
    strcpy(slot->abbrev, abbrev);                       /* fits: checked above */
    if (!copy_bounded(slot->module, sizeof(slot->module), module, strlen(module)) ||
        !copy_bounded(slot->class_no, sizeof(slot->class_no), colon + 1, cls_len)) {
        out->skipped++;                                 /* over-long module code or class number */
        return;
    }
    out->count++;
}

/* Index of the earliest lesson taught on this weekday/week (ties: lower index), -1 if none. */
int first_lesson_on_day(const Lesson *lessons, int count, int weekday, int week)
{
    int best = -1;
    int i;

    for (i = 0; i < count; i++) {
        if (lessons[i].weekday > SCHED_MAX_WEEKDAY || lessons[i].weekday != weekday ||
            lessons[i].start_min >= SCHED_MIN_PER_DAY || ((lessons[i].weeks >> (week - 1)) & 1u) == 0u) {
            continue;
        }
        if (best < 0 || lessons[i].start_min < lessons[best].start_min) {   /* strict '<': ties keep the lower index */
            best = i;
        }
    }
    return best;
}

}  // namespace

const char *sched_lesson_type_name(const char *abbrev)
{
    size_t i;

    if (abbrev == NULL) {
        return NULL;
    }
    for (i = 0; i < sizeof(k_types) / sizeof(k_types[0]); i++) {
        if (strcmp(abbrev, k_types[i].abbrev) == 0) {
            return k_types[i].name;
        }
    }
    return NULL;
}

bool parse_share_link(const char *url, ShareLink *out)
{
    const char *p;
    const char *eq;
    const char *pair_end;
    const char *tok_end;
    char module[SCHED_MODULE_LEN];

    if (url == NULL || out == NULL) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    p = strchr(url, '?');
    if (p == NULL) {
        return false;
    }
    p++;

    while (*p != '\0' && *p != '#') {
        pair_end = p;                                   /* [p, pair_end) = "MODULE=A:1,B:2" */
        while (*pair_end != '\0' && *pair_end != '&' && *pair_end != '#') {
            pair_end++;
        }
        eq = p;
        while (eq < pair_end && *eq != '=') {
            eq++;
        }
        if (eq == pair_end || eq == p || !copy_bounded(module, sizeof(module), p, (size_t)(eq - p))) {
            out->skipped++;                             /* no '=', empty or over-long module code */
        } else {
            p = eq + 1;
            while (p < pair_end) {
                tok_end = p;
                while (tok_end < pair_end && *tok_end != ',') {
                    tok_end++;
                }
                parse_slot_token(module, p, tok_end, out);
                p = (tok_end < pair_end) ? tok_end + 1 : tok_end;
            }
        }
        p = pair_end;
        if (*p == '&') {
            p++;
        }
    }
    return true;
}

bool sched_class_no_equal(const char *a, const char *b)
{
    if (a == NULL || b == NULL) {
        return false;
    }
    if (is_all_digits(a) && is_all_digits(b)) {
        return digits_value(a) == digits_value(b);
    }
    return strcmp(a, b) == 0;
}

int sched_weekday_from_name(const char *day)
{
    static const char *const k_days[] = { "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
    int i;

    if (day == NULL) {
        return 0;
    }
    for (i = 0; i < SCHED_MAX_WEEKDAY; i++) {
        if (strcmp(day, k_days[i]) == 0) {
            return i + 1;
        }
    }
    return 0;
}

int sched_parse_hhmm(const char *hhmm)
{
    int i;
    int hh;
    int mm;

    if (hhmm == NULL || strlen(hhmm) != 4u) {
        return -1;
    }
    for (i = 0; i < 4; i++) {
        if (hhmm[i] < '0' || hhmm[i] > '9') {
            return -1;
        }
    }
    hh = (hhmm[0] - '0') * 10 + (hhmm[1] - '0');
    mm = (hhmm[2] - '0') * 10 + (hhmm[3] - '0');
    return (hh < 24 && mm < 60) ? hh * 60 + mm : -1;
}

int teaching_week(int year, int month, int day)
{
    const int64_t n = days_from_civil(year, month, day) -
                      days_from_civil(SCHED_WEEK1_YEAR, SCHED_WEEK1_MONTH, SCHED_WEEK1_DAY);
    int64_t idx;

    if (n < 0) {
        return 0;                                       /* before week 1 */
    }
    idx = n / kDaysPerWeek;                             /* 0-based week since week 1 */
    if (idx < SCHED_RECESS_AFTER_WEEK) {
        return (int)idx + 1;
    }
    if (idx == SCHED_RECESS_AFTER_WEEK) {
        return 0;                                       /* recess week */
    }
    /* After recess the 0-based index equals the teaching week number (week 7 -> index 7). */
    return (idx <= SCHED_LAST_WEEK) ? (int)idx : 0;
}

int sched_weekday(int year, int month, int day)
{
    return (int)(((days_from_civil(year, month, day) + kThursdayOffset) % kDaysPerWeek + kDaysPerWeek) %
                 kDaysPerWeek) + 1;
}

uint32_t sched_local_to_unix(int year, int month, int day, int hour, int minute)
{
    const int64_t local = days_from_civil(year, month, day) * SCHED_SEC_PER_DAY +
                          (int64_t)hour * 3600 + (int64_t)minute * SCHED_SEC_PER_MIN;

    return (uint32_t)(local - SCHED_TZ_OFFSET_S);
}

NextAlarm next_alarm(const Lesson *lessons, int count, uint32_t now_unix, int lead_min,
                     uint32_t consumed_class_start_unix)
{
    NextAlarm r;
    const int64_t now = (int64_t)now_unix;
    const int64_t first_day = floor_div(now + SCHED_TZ_OFFSET_S, SCHED_SEC_PER_DAY);
    const int64_t horizon = now + (int64_t)SCHED_LOOKAHEAD_DAYS * SCHED_SEC_PER_DAY;
    const int64_t lead_s = (int64_t)lead_min * SCHED_SEC_PER_MIN;
    int64_t day;
    int64_t start;
    int week;
    int idx;
    int y;
    int m;
    int d;
    int d_off;

    r.found = false;
    r.alarm_unix = 0;
    r.class_start_unix = 0;
    r.lesson_idx = -1;
    if (lessons == NULL || count <= 0) {
        return r;
    }
    if (count > SCHED_MAX_LESSONS) {
        count = SCHED_MAX_LESSONS;
    }

    for (d_off = 0; d_off <= SCHED_LOOKAHEAD_DAYS; d_off++) {
        day = first_day + d_off;
        civil_from_days(day, &y, &m, &d);
        week = teaching_week(y, m, d);
        if (week == 0) {
            continue;                                   /* recess, before week 1 or after week 13 */
        }
        idx = first_lesson_on_day(lessons, count, sched_weekday(y, m, d), week);
        if (idx < 0) {
            continue;
        }
        start = day * SCHED_SEC_PER_DAY + (int64_t)lessons[idx].start_min * SCHED_SEC_PER_MIN - SCHED_TZ_OFFSET_S;
        if (start >= horizon) {
            break;                                      /* every later day starts later still */
        }
        /* The day's FIRST class decides the day: if its alarm is too close or already rang, the
         * later classes of the same day are NOT used, the next day's first class is. */
        if (start - lead_s <= now + SCHED_MIN_LEAD_S || start <= (int64_t)consumed_class_start_unix) {
            continue;
        }
        r.found = true;
        r.lesson_idx = idx;
        r.class_start_unix = (uint32_t)start;
        r.alarm_unix = (uint32_t)(start - lead_s);
        break;
    }
    return r;
}

AlarmAction alarm_action(uint32_t desired_alarm_unix, uint32_t last_sent_alarm_unix, uint8_t mcx_state,
                         bool session_active, bool time_sync_sent)
{
    if (session_active) {
        return ALARM_ACTION_NONE;
    }
    if (desired_alarm_unix == 0u) {
        return (mcx_state == PROTO_STATE_ARMED && last_sent_alarm_unix != 0u) ? ALARM_ACTION_SEND_CANCEL
                                                                              : ALARM_ACTION_NONE;
    }
    return (desired_alarm_unix != last_sent_alarm_unix || time_sync_sent) ? ALARM_ACTION_SEND_SET_ALARM
                                                                          : ALARM_ACTION_NONE;
}

bool session_next(bool session_active, uint8_t mcx_state)
{
    if (mcx_state == PROTO_STATE_BOMB_ACTIVE) {
        return true;
    }
    return (mcx_state == PROTO_STATE_IDLE) ? false : session_active;
}
