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
    CHECK(strstr(WEB_PAGE, "type=\"range\"") != NULL && strstr(WEB_PAGE, "min=\"15\"") != NULL &&
          strstr(WEB_PAGE, "max=\"180\"") != NULL && strstr(WEB_PAGE, "step=\"5\"") != NULL);
    CHECK(strstr(WEB_PAGE, "Next alarm") != NULL && strstr(WEB_PAGE, "No alarm set") != NULL);
    CHECK(strstr(WEB_PAGE, "NUSMods share link") != NULL && strstr(WEB_PAGE, "Wake up before first class") != NULL);
    CHECK(strstr(WEB_PAGE, "Save") != NULL && strstr(WEB_PAGE, "Ring now") != NULL);
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
}

int main(void)
{
    test_main_page();
    test_eng_page();
    printf("test_page: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
