/* Host-side checks of the embedded web page (web_page.h at "/", web_eng_page.h at "/eng" in esp32/defuse_esp32/src/app): size budget,
 * no external assets, no unsafe DOM sinks, and the endpoints the firmware serves. */
#include <stdio.h>
#include <string.h>

#include "web_page.h"
#include "web_eng_page.h"

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

/* Checks shared by both pages: budget, self-contained, no unsafe DOM sinks, viewport, banner. */
static void check_common(const char *page, size_t max_bytes)
{
    const size_t size = strlen(page);

    CHECK(size > 2000u);
    CHECK(size <= max_bytes);                                                      /* each page <= 20 KB */
    CHECK(max_bytes == 20480u);
    /* no external assets, no CDNs, nothing fetched from another origin */
    CHECK(strstr(page, "src=\"http") == NULL && strstr(page, "src='http") == NULL);
    CHECK(strstr(page, "href=\"http") == NULL && strstr(page, "href='http") == NULL);
    CHECK(strstr(page, "url(http") == NULL && strstr(page, "@import") == NULL);
    CHECK(strstr(page, "//cdn") == NULL && strstr(page, "googleapis") == NULL);
    /* untrusted text (LLM puzzles, module codes) must never go through innerHTML */
    CHECK(strstr(page, "innerHTML") == NULL && strstr(page, "insertAdjacentHTML") == NULL &&
          strstr(page, "document.write") == NULL && strstr(page, "eval(") == NULL);
    CHECK(strstr(page, "/api/status") != NULL);
    CHECK(strstr(page, "X-Requested-With") != NULL);                               /* POSTs carry the anti-CSRF header */
    CHECK(strstr(page, "2000") != NULL);                                           /* 2 s fetch timeout */
    CHECK(strstr(page, "viewport") != NULL);
    CHECK(strstr(page, "Connection lost") != NULL);
    CHECK(strstr(page, "prefers-color-scheme") != NULL);
}

static void test_main_page(void)
{
    check_common(WEB_PAGE, WEB_PAGE_MAX_BYTES);
    CHECK(strstr(WEB_PAGE, "/api/settings") != NULL);
    CHECK(strstr(WEB_PAGE, "/api/ring") != NULL);
    CHECK(strstr(WEB_PAGE, "setInterval(poll,2000)") != NULL);                     /* 2 s poll */
    /* lead time (1..180, whole minutes): a number input AND a range slider, both min 1 / max 180 / step 1 */
    CHECK(strstr(WEB_PAGE, "type=\"range\"") != NULL && strstr(WEB_PAGE, "type=\"number\" id=\"ln\"") != NULL);
    CHECK(strstr(WEB_PAGE, "id=\"ln\" min=\"1\" max=\"180\" step=\"1\"") != NULL &&
          strstr(WEB_PAGE, "id=\"ld\" min=\"1\" max=\"180\" step=\"1\"") != NULL);
    CHECK(strstr(WEB_PAGE, "min=\"15\"") == NULL && strstr(WEB_PAGE, "step=\"5\"") == NULL);   /* the old 15..180 / 5-min slider is gone */
    CHECK(strstr(WEB_PAGE, "id=\"lm\"") != NULL && strstr(WEB_PAGE, "id=\"lp\"") != NULL);      /* - and + buttons */
    CHECK(strstr(WEB_PAGE, "numErr($(\"ln\").value.trim(),1,180)") != NULL);                    /* inline validation of the typed value */
    CHECK(strstr(WEB_PAGE, "id=\"lne\" role=\"alert\"") != NULL && strstr(WEB_PAGE, "Whole minutes only") != NULL);
    CHECK(strstr(WEB_PAGE, "id=\"sv\" type=\"submit\" disabled") != NULL && strstr(WEB_PAGE, "Unsaved changes") != NULL);
    CHECK(strstr(WEB_PAGE, "lead_min:c.ld") != NULL);                                           /* Save sends the typed value */
    CHECK(strstr(WEB_PAGE, "Next alarm") != NULL && strstr(WEB_PAGE, "No alarm set") != NULL);
    CHECK(strstr(WEB_PAGE, "NUSMods share link") != NULL && strstr(WEB_PAGE, "Wake up before first class") != NULL);
    /* difficulty: a small <select> with the four ids, sent with the form, and the "Now:" line from /api/status */
    CHECK(strstr(WEB_PAGE, "<select id=\"df\">") != NULL && strstr(WEB_PAGE, "value=\"auto\"") != NULL &&
          strstr(WEB_PAGE, "value=\"easy\"") != NULL && strstr(WEB_PAGE, "value=\"normal\"") != NULL &&
          strstr(WEB_PAGE, "value=\"hard\"") != NULL);
    CHECK(strstr(WEB_PAGE, "difficulty:c.df") != NULL && strstr(WEB_PAGE, "s.diff.why") != NULL);
    CHECK(strstr(WEB_PAGE, "Save") != NULL && strstr(WEB_PAGE, "Ring now") != NULL);
    /* polling never overwrites a field being edited (dirty check + focus guard over all four inputs) */
    CHECK(strstr(WEB_PAGE, "document.activeElement") != NULL && strstr(WEB_PAGE, "FIELDS=[\"lk\",\"ln\",\"ld\",\"df\"]") != NULL &&
          strstr(WEB_PAGE, "isDirty()") != NULL);
    /* next-alarm hero: class line, live countdown every second, demo badge */
    CHECK(strstr(WEB_PAGE, "setInterval(tick,1000)") != NULL && strstr(WEB_PAGE, "a.in_s") != NULL);
    CHECK(strstr(WEB_PAGE, "id=\"bdg\" hidden>Demo alarm<") != NULL && strstr(WEB_PAGE, "a.demo") != NULL);
    /* "This morning": briefing + coach from /api/status */
    CHECK(strstr(WEB_PAGE, "This morning") != NULL && strstr(WEB_PAGE, "b.briefing") != NULL && strstr(WEB_PAGE, "b.coach") != NULL);
    /* timetable grid: its own endpoint, fetched on load / after Save / when settings.ver moves, not per poll */
    CHECK(strstr(WEB_PAGE, "/api/timetable") != NULL && strstr(WEB_PAGE, "s.settings.ver!==ttVer") != NULL);
    CHECK(strstr(WEB_PAGE, "[0,5,12].forEach(s=>setTimeout(loadTT") != NULL);
    CHECK(strstr(WEB_PAGE, "Paste your NUSMods link and Save") != NULL && strstr(WEB_PAGE, "Outside teaching weeks") != NULL);
    CHECK(strstr(WEB_PAGE, "l.weeks.includes(week)") != NULL && strstr(WEB_PAGE, "week>0") != NULL);   /* fade lessons not taught this week */
    CHECK(strstr(WEB_PAGE, "overflow-x:auto") != NULL && strstr(WEB_PAGE, "id=\"gw\"") != NULL);      /* the grid scrolls inside its card */
    CHECK(strstr(WEB_PAGE, "Wk ") != NULL && strstr(WEB_PAGE, "hue(") != NULL && strstr(WEB_PAGE, "hsl(var(--h)") != NULL);
    /* demo morning alarm: minutes 1..30, Start, Cancel (minutes=0), the status message */
    CHECK(strstr(WEB_PAGE, "/api/demo_alarm") != NULL);
    CHECK(strstr(WEB_PAGE, "id=\"dm\" min=\"1\" max=\"30\" step=\"1\"") != NULL && strstr(WEB_PAGE, "numErr($(\"dm\").value.trim(),1,30)") != NULL);
    CHECK(strstr(WEB_PAGE, "id=\"dms\"") != NULL && strstr(WEB_PAGE, ">Start<") != NULL && strstr(WEB_PAGE, ">Cancel<") != NULL);
    CHECK(strstr(WEB_PAGE, "demo(\"0\")") != NULL && strstr(WEB_PAGE, "\"minutes=\"") != NULL);
    CHECK(strstr(WEB_PAGE, "a.demo_msg") != NULL && strstr(WEB_PAGE, "Demo alarm at ") != NULL);
    CHECK(strstr(WEB_PAGE, "like a real morning alarm") != NULL);
    /* Ring now starts a bomb: it asks first */
    CHECK(strstr(WEB_PAGE, "window.confirm(") != NULL);
    /* accessibility: labelled controls, visible focus, 44 px touch targets, no sideways page scroll */
    CHECK(strstr(WEB_PAGE, "for=\"lk\"") != NULL && strstr(WEB_PAGE, "for=\"ln\"") != NULL && strstr(WEB_PAGE, "for=\"df\"") != NULL &&
          strstr(WEB_PAGE, "for=\"dm\"") != NULL);
    CHECK(strstr(WEB_PAGE, ":focus-visible") != NULL && strstr(WEB_PAGE, "min-height:44px") != NULL);
    CHECK(strstr(WEB_PAGE, "aria-live=\"polite\"") != NULL);
    /* minimal: no tabs, no engineering controls, and no link to /eng */
    CHECK(strstr(WEB_PAGE, "/eng") == NULL && strstr(WEB_PAGE, "role=\"tab") == NULL);
    CHECK(strstr(WEB_PAGE, "/api/fault") == NULL && strstr(WEB_PAGE, "/api/puzzles") == NULL);
    CHECK(strstr(WEB_PAGE, "<table") == NULL);
}

static void test_eng_page(void)
{
    check_common(WEB_ENG_PAGE, WEB_ENG_PAGE_MAX_BYTES);
    CHECK(strstr(WEB_ENG_PAGE, "/api/ring") != NULL);
    CHECK(strstr(WEB_ENG_PAGE, "/api/fault/corrupt") != NULL);
    CHECK(strstr(WEB_ENG_PAGE, "/api/fault/outage") != NULL);
    CHECK(strstr(WEB_ENG_PAGE, "/api/puzzles") != NULL);
    CHECK(strstr(WEB_ENG_PAGE, "setInterval(poll,500)") != NULL);                  /* 500 ms poll */
    CHECK(strstr(WEB_ENG_PAGE, "href=\"/\"") != NULL && strstr(WEB_ENG_PAGE, ">Settings<") != NULL);
    CHECK(strstr(WEB_ENG_PAGE, "Link to the MCX") != NULL && strstr(WEB_ENG_PAGE, "AI puzzles") != NULL);
    CHECK(strstr(WEB_ENG_PAGE, "Fault injection") != NULL);
    CHECK(strstr(WEB_ENG_PAGE, "s.diff.level") != NULL && strstr(WEB_ENG_PAGE, "s.diff.why") != NULL);   /* difficulty line */
}

int main(void)
{
    test_main_page();
    test_eng_page();
    printf("test_page: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
