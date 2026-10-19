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
#include "puzzle_gen.h"
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

const char *const k_chain[APP_LLM_CHAIN_LEN] = APP_LLM_SETTER_CHAIN;

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
bool s_requested;
bool s_manual;
uint32_t s_seen_runs;
bool s_today_valid;
TodayKey s_today_key;

Lesson s_lessons[SCHED_MAX_LESSONS];
BriefClass s_cls[BRIEF_CLASSES_MAX];
int s_ncls;
char s_summary[HIST_SUMMARY_TEXT_MAX + 1];
char s_prompt[APP_BRIEF_PROMPT_MAX];
char s_reply[APP_LLM_REPLY_MAX];
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
    snprintf(b.briefing, sizeof(b.briefing), "%s", (mask & BRIEF_OK_BRIEFING) ? s_llm.briefing : s_fallback.briefing);
    snprintf(b.coach, sizeof(b.coach), "%s", (mask & BRIEF_OK_COACH) ? s_llm.coach : s_fallback.coach);
    save_brief(b);
    app_state_set_brief(b);
    s_phase = PH_IDLE;
    Serial.printf("[brief] stored for local day %u (briefing: %s, coach: %s, model %s)\n", (unsigned)b.for_day,
                  (mask & BRIEF_OK_BRIEFING) ? "LLM" : "template", (mask & BRIEF_OK_COACH) ? "LLM" : "template", model);
    Serial.printf("[brief] briefing: \"%s\"\n", b.briefing);
    Serial.printf("[brief] coach:    \"%s\"\n", b.coach);
}

/* Collect the inputs for the next alarm's day. False when there is no class to brief. */
bool begin_run(uint32_t unix_now)
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

    if (!app_state_read(&st) || !st.next_valid || st.next_start_unix == 0u || st.alarm_unix == 0u) {
        Serial.println("[brief] no next class: nothing to brief");
        return false;
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
    return true;
}

/* One LLM call; a model that fails or answers unusably hands over to the next one. */
void step_call(void)
{
    const char *model = k_chain[s_pos];
    LlmReply r = llm_chat(model, kBriefSys, s_prompt, APP_BRIEF_MAX_TOKENS, APP_BRIEF_TEMP, s_reply, sizeof(s_reply));
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
    if (s_pos >= APP_LLM_CHAIN_LEN || s_transport_fails >= BRIEF_TRANSPORT_MAX) {
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
    memset(&s_blob, 0, sizeof(s_blob));
    if (flash_read_blob(APP_KEY_BRIEF, &s_blob, sizeof(s_blob)) && s_blob.magic == APP_BRIEF_MAGIC &&
        brief_valid(s_blob.text)) {
        app_state_set_brief(s_blob.text);
        Serial.printf("[brief] loaded from flash: for local day %u\n", (unsigned)s_blob.text.for_day);
    } else {
        Serial.println("[brief] nothing stored: the DEFUSED screen uses today's template");
    }
    s_seen_runs = puzzle_gen_runs_done();
    return true;
}

void briefing_request(void)
{
    s_requested = true;
    s_manual = true;
}

void briefing_service(uint32_t now_ms, uint32_t unix_now, bool net_ok, bool gen_ok)
{
    const uint32_t runs = puzzle_gen_runs_done();

    (void)now_ms;
    if (unix_now != 0u) {
        refresh_today(unix_now);
    }
    if (runs != s_seen_runs) {
        s_seen_runs = runs;
        s_requested = true;                          /* after each finished puzzle run, nightly or not */
        s_manual = false;
        Serial.println("[brief] run wanted (a puzzle run finished)");
    }
    if (s_phase == PH_CALL) {
        if (!net_ok) {
            finish(0, "-");                          /* Wi-Fi or clock lost: the template, now */
        } else if (gen_ok) {
            step_call();
        }                                            /* else paused: a bomb session or the alarm comes first */
        return;
    }
    if (!s_requested) {
        return;
    }
    if (!net_ok) {
        if (s_manual) {
            s_requested = false;
            s_manual = false;
            Serial.println("[brief] no Wi-Fi or clock: the stored text stays");
        }
        return;                                      /* automatic runs wait for the network */
    }
    if (!gen_ok || puzzle_gen_busy()) {
        return;                                      /* never in parallel with a puzzle run */
    }
    s_requested = false;
    s_manual = false;
    (void)begin_run(unix_now);
}
