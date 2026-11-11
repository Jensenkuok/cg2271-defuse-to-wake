/* Defuse to Wake - morning briefing + coach message. See briefing.h. */
#include "briefing.h"

#include <Arduino.h>
#include <string.h>
#include <time.h>

#include "app_state.h"
#include "bomb_config.h"
#include "config.h"
#include "flash_store.h"
#include "history.h"
#include "llm_client.h"
#include "mem_big.h"
#include "text_logic.h"
#include "timetable.h"

namespace {

#define BRIEF_JSON_MAX      512     /* the reply's JSON object: two short strings fit easily */
#define BRIEF_ALARM_TEXT    24
#define BRIEF_TRANSPORT_MAX 2       /* transport failures (timeout, no route) in a row before giving up on the LLM */

/* The system prompt asks for exactly what brief_parse() validates. */
const char kBriefSys[] =
    "You write the morning message of a wake-up bomb game for a university student. "
    "Reply ONLY with JSON: {\"briefing\":\"...\",\"coach\":\"...\"}. "
    "briefing: at most 120 characters, plain ASCII: the first class of the day with its venue, then the other "
    "classes. "
    "coach: at most 80 characters, plain ASCII, one personal light-hearted push or roast based on the history "
    "and the difficulty. Never insult appearance or identity. No emojis.";

const char *const k_chain[APP_BRIEF_CHAIN_LEN] = APP_BRIEF_CHAIN;

struct BriefBlob {
    uint32_t magic;
    BriefText text;
};

/* Identity of the data the "today" template was built from: rebuilt only when one of them changes. */
struct TodayKey {
    uint32_t day;
    uint32_t tt_fetched;
    uint32_t tt_count;
    uint32_t hist_count;
    uint32_t hist_newest;
};

enum Phase { PH_IDLE = 0, PH_CALL };

/* Everything below is touched by the network task only. */
Phase s_phase;
bool s_requested;                   /* only the Serial "brief" command sets it */
bool s_failed_before;               /* a run ended with template text (D1/D2): wait APP_BRIEF_RETRY_MS before the next */
uint32_t s_last_fail_ms;
uint32_t s_now_ms;                  /* millis() of the current service pass, for finish() */
BriefText s_saved;                  /* what is in flash, so an identical failed run does not rewrite it every minute */
bool s_today_valid;
TodayKey s_today_key;

Lesson *s_lessons;                  /* PSRAM, SCHED_MAX_LESSONS entries (mem_big.h) */
BriefClass s_cls[BRIEF_CLASSES_MAX];
int s_ncls;
char s_summary[HIST_SUMMARY_TEXT_MAX + 1];
char s_prompt[APP_BRIEF_PROMPT_MAX];
char *s_reply;                      /* PSRAM, APP_LLM_REPLY_MAX bytes */
char s_json[BRIEF_JSON_MAX];
BriefText s_llm;                    /* what brief_parse accepted */
BriefText s_fallback;               /* the template pair for the day being briefed */
BriefBlob s_blob;                   /* flash buffer */
uint32_t s_target_day;
uint8_t s_pos;                      /* next model of the chain */
uint8_t s_transport_fails;

void civil_of(uint32_t unix_s, int *y, int *m, int *d, int *wday, int *hh, int *mm)
{
    const time_t at = (time_t)(unix_s + (uint32_t)APP_TZ_OFFSET_S);
    struct tm t;

    gmtime_r(&at, &t);
    *y = t.tm_year + 1900;
    *m = t.tm_mon + 1;
    *d = t.tm_mday;
    *wday = t.tm_wday % 7;
    *hh = t.tm_hour;
    *mm = t.tm_min;
}

HistSummary collect_summary(uint32_t unix_now)
{
    HistRecord recs[HIST_SUMMARY_WINDOW];
    const int n = history_recent(recs, HIST_SUMMARY_WINDOW);

    return history_summary(recs, n, history_current_streak(unix_now));
}

/* Template pair for the classes of one local date. */
void build_templates(uint32_t day_unix, const HistSummary &sum, uint32_t day, BriefText *out)
{
    int y;
    int m;
    int d;
    int wd;
    int hh;
    int mm;
    const int nl = timetable_copy(s_lessons, SCHED_MAX_LESSONS);

    civil_of(day_unix, &y, &m, &d, &wd, &hh, &mm);
    s_ncls = classes_on_day(s_lessons, nl, y, m, d, s_cls, BRIEF_CLASSES_MAX);
    memset(out, 0, sizeof(*out));
    out->for_day = day;
    (void)briefing_template(s_cls, s_ncls, out->briefing, sizeof(out->briefing));
    (void)coach_template(sum, out->coach, sizeof(out->coach));
}

void save_brief(const BriefText &b)
{
    if (s_saved.for_day == b.for_day && s_saved.from_llm == b.from_llm && strcmp(s_saved.briefing, b.briefing) == 0 &&
        strcmp(s_saved.coach, b.coach) == 0) {
        return;                                      /* a template retried every minute would wear the flash for nothing */
    }
    s_saved = b;
    memset(static_cast<void *>(&s_blob), 0, sizeof(s_blob));   /* no uninitialised padding in flash */
    s_blob.magic = APP_BRIEF_MAGIC;
    s_blob.text = b;
    if (!flash_write_blob(APP_KEY_BRIEF, &s_blob, sizeof(s_blob))) {
        Serial.println("[brief] could not write the briefing to flash");
    }
}

/* The run is over: LLM fields where brief_parse accepted them, the template for the rest. */
void finish(int mask, const char *model)
{
    BriefText b;

    memset(&b, 0, sizeof(b));
    b.for_day = s_target_day;
    b.from_llm = (mask == (BRIEF_OK_BRIEFING | BRIEF_OK_COACH)) ? 1u : 0u;   /* a mixed text is retried too */
    snprintf(b.briefing, sizeof(b.briefing), "%s", (mask & BRIEF_OK_BRIEFING) ? s_llm.briefing : s_fallback.briefing);
    snprintf(b.coach, sizeof(b.coach), "%s", (mask & BRIEF_OK_COACH) ? s_llm.coach : s_fallback.coach);
    save_brief(b);
    app_state_set_brief(b);
    s_phase = PH_IDLE;
    s_failed_before = (b.from_llm == 0u);            /* the template is shown, the LLM is asked again after the gap */
    s_last_fail_ms = s_now_ms;
    Serial.printf("[brief] stored for local day %u (briefing: %s, coach: %s, model %s)\n", (unsigned)b.for_day,
                  (mask & BRIEF_OK_BRIEFING) ? "LLM" : "template", (mask & BRIEF_OK_COACH) ? "LLM" : "template", model);
    Serial.printf("[brief] briefing: \"%s\"\n", b.briefing);
    Serial.printf("[brief] coach:    \"%s\"\n", b.coach);
}

enum BeginResult { BEGIN_STARTED = 0, BEGIN_NO_CLASS, BEGIN_DEFERRED };

bool s_defer_logged;                /* "waiting for the demo alarm" is printed once per wait */

/* Collect the inputs for the next alarm's day. BEGIN_NO_CLASS when there is no class to brief,
 * BEGIN_DEFERRED while a demo alarm is the scheduled one (it has no class, but the real alarm is
 * only held back, so the request must stay pending until the demo is gone). */
BeginResult begin_run(uint32_t unix_now)
{
    static const char *const k_weekday[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    AppState st;
    HistSummary sum;
    Difficulty diff;
    char alarm_text[BRIEF_ALARM_TEXT];
    int y;
    int m;
    int d;
    int wd;
    int hh;
    int mm;

    if (!app_state_read(&st)) {
        return BEGIN_DEFERRED;                       /* state busy: try again on the next pass */
    }
    if (st.alarm_is_demo) {
        if (!s_defer_logged) {
            s_defer_logged = true;
            Serial.println("[brief] demo alarm pending: the briefing run waits until it is gone");
        }
        return BEGIN_DEFERRED;
    }
    s_defer_logged = false;
    if (!st.next_valid || st.next_start_unix == 0u || st.alarm_unix == 0u) {
        Serial.println("[brief] no next class: nothing to brief");
        return BEGIN_NO_CLASS;
    }
    s_target_day = local_day(st.next_start_unix);
    sum = collect_summary(unix_now);
    build_templates(st.next_start_unix, sum, s_target_day, &s_fallback);
    civil_of(st.alarm_unix, &y, &m, &d, &wd, &hh, &mm);
    snprintf(alarm_text, sizeof(alarm_text), "%s %02d:%02d", k_weekday[wd], hh, mm);
    (void)history_summary_text(sum, s_summary, sizeof(s_summary));
    bomb_config_difficulty(&diff);
    (void)brief_user_prompt(alarm_text, s_cls, s_ncls, s_summary, diff.explanation, s_prompt, sizeof(s_prompt));
    s_pos = 0u;
    s_transport_fails = 0u;
    s_phase = PH_CALL;
    Serial.printf("[brief] generating for local day %u: %d classes, alarm %s (chain starts with %s)\n",
                  (unsigned)s_target_day, s_ncls, alarm_text, k_chain[0]);
    return BEGIN_STARTED;
}

/* One LLM call; a model that fails or answers unusably hands over to the next one. */
void step_call(void)
{
    const char *model = k_chain[s_pos];
    /* ===== Q&A [Online function(s) 5 (Generative AI)] =====
     * Q: What does the LLM do? A: writes the morning briefing + coach message from today's
     *    classes, the alarm time and recent mornings. Invalid replies -> a template.
     * Q: Why not AI puzzles? A: board tests showed slow and wrong answers on hard riddles,
     *    so puzzles come from the ESP32's proven bank (APP_PUZZLE_USE_LLM 0).
     * ===== end Q&A ===== */
    LlmReply r = llm_chat(model, kBriefSys, s_prompt, APP_BRIEF_MAX_TOKENS, APP_BRIEF_TEMP, s_reply, APP_LLM_REPLY_MAX);
    int mask = 0;

    if (r.status == LLM_ERR_NO_KEY) {
        finish(0, "-");                              /* no key: every model would fail the same way */
        return;
    }
    if (r.status == LLM_OK) {
        s_transport_fails = 0u;
        mask = brief_parse(s_reply, s_json, sizeof(s_json), &s_llm);
        if (mask != 0) {
            finish(mask, model);
            return;
        }
        Serial.printf("[brief] %s: no usable {\"briefing\",\"coach\"} in the reply\n", model);
    } else if (r.http_code < 0) {
        s_transport_fails++;                         /* timeout / no route: the Wi-Fi or the API is down */
    }
    s_pos++;
    if (s_pos >= APP_BRIEF_CHAIN_LEN || s_transport_fails >= BRIEF_TRANSPORT_MAX) {
        Serial.println("[brief] no model gave a usable text: the template is used");
        finish(0, "-");
    }
}

/* Today's template, for when the stored text is for another day. Cheap: rebuilt only on a change. */
void refresh_today(uint32_t unix_now)
{
    HistRecord recs[1];
    TodayKey key;
    HistSummary sum;
    BriefText t;
    const int nh = history_recent(recs, 1);

    key.day = local_day(unix_now);
    key.tt_fetched = timetable_fetched_unix();
    key.tt_count = (uint32_t)timetable_count();
    key.hist_count = (uint32_t)history_count();
    key.hist_newest = (nh > 0) ? recs[0].end_unix : 0u;
    if (s_today_valid && memcmp(&key, &s_today_key, sizeof(key)) == 0) {
        return;
    }
    s_today_key = key;
    s_today_valid = true;
    sum = collect_summary(unix_now);
    build_templates(unix_now, sum, key.day, &t);
    app_state_set_brief_today(t);
}

}  // namespace

bool briefing_init(void)
{
    s_lessons = mem_big_new<Lesson>(SCHED_MAX_LESSONS, "briefing lessons");
    s_reply = mem_big_new<char>(APP_LLM_REPLY_MAX, "briefing LLM reply");
    if (s_lessons == NULL || s_reply == NULL) {
        return false;
    }
    memset(&s_blob, 0, sizeof(s_blob));
    if (flash_read_blob(APP_KEY_BRIEF, &s_blob, sizeof(s_blob)) && s_blob.magic == APP_BRIEF_MAGIC &&
        brief_valid(s_blob.text)) {
        app_state_set_brief(s_blob.text);
        s_saved = s_blob.text;
        Serial.printf("[brief] loaded from flash: for local day %u (%s)\n", (unsigned)s_blob.text.for_day,
                      s_blob.text.from_llm ? "LLM" : "template");
    } else {
        Serial.println("[brief] nothing stored: the DEFUSED screen uses today's template");
    }
    return true;
}

void briefing_request(void)
{
    s_requested = true;
}

/* Does the automatic rule (brief_needs_run) want a run now? A demo alarm defers it: the demo has no class, and
 * the real alarm's briefing is only held back until the demo is gone. Cheap: one app_state copy. */
static bool auto_run_due(void)
{
    AppState st;
    const bool has_class = app_state_read(&st) && !st.alarm_is_demo && st.next_valid && st.next_start_unix != 0u &&
                           st.alarm_unix != 0u;

    if (!has_class) {
        return false;
    }
    return brief_needs_run(st.brief, true, local_day(st.next_start_unix), s_failed_before, s_last_fail_ms, s_now_ms,
                           APP_BRIEF_RETRY_MS);
}

bool briefing_service(uint32_t now_ms, uint32_t unix_now, bool net_ok, bool gen_ok)
{
    s_now_ms = now_ms;
    if (unix_now != 0u) {
        refresh_today(unix_now);
    }
    if (s_phase == PH_CALL) {
        if (!net_ok) {
            finish(0, "-");                          /* Wi-Fi or clock lost: the template, now */
        } else if (gen_ok) {
            step_call();
            return true;                             /* this pass's LLM call is spent */
        }                                            /* else paused: a bomb session or the alarm comes first */
        return false;
    }
    if (s_requested) {                               /* Serial "brief": forced, ignores the retry gap */
        if (!net_ok) {
            s_requested = false;
            Serial.println("[brief] no Wi-Fi or clock: the stored text stays");
            return false;
        }
        if (!gen_ok) {
            return false;                            /* wait for the game state to allow it */
        }
        s_requested = false;
        if (begin_run(unix_now) == BEGIN_DEFERRED) {
            s_requested = true;                      /* a demo alarm is the scheduled one: ask again next pass */
        }
        return false;
    }
    /* Automatic: every pass while online and allowed, until the next class day has an LLM-written text (D2).
     * Not held back by puzzle upgrades: the network task offers this service first (R4). */
    if (net_ok && gen_ok && auto_run_due()) {
        (void)begin_run(unix_now);
    }
    return false;
}
