/* Host-side checks of the embedded web page (esp32/defuse_esp32/src/app/web_page.h): size budget,
 * no external assets, no unsafe DOM sinks, and the endpoints the firmware serves. */
#include <stdio.h>
#include <string.h>

#include "web_page.h"

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

static void test_page_budget_and_content(void)
{
    const size_t size = strlen(WEB_PAGE);

    CHECK(size > 2000u);
    CHECK(size <= WEB_PAGE_MAX_BYTES);                                             /* total page <= 20 KB */
    CHECK(WEB_PAGE_MAX_BYTES == 20480u);
    /* no external assets, no CDNs, nothing fetched from another origin */
    CHECK(strstr(WEB_PAGE, "src=\"http") == NULL && strstr(WEB_PAGE, "src='http") == NULL);
    CHECK(strstr(WEB_PAGE, "href=\"http") == NULL && strstr(WEB_PAGE, "href='http") == NULL);
    CHECK(strstr(WEB_PAGE, "url(http") == NULL && strstr(WEB_PAGE, "@import") == NULL);
    CHECK(strstr(WEB_PAGE, "//cdn") == NULL && strstr(WEB_PAGE, "googleapis") == NULL);
    /* untrusted text (LLM puzzles, module codes) must never go through innerHTML */
    CHECK(strstr(WEB_PAGE, "innerHTML") == NULL && strstr(WEB_PAGE, "insertAdjacentHTML") == NULL &&
          strstr(WEB_PAGE, "document.write") == NULL && strstr(WEB_PAGE, "eval(") == NULL);
    /* the contract with the firmware */
    CHECK(strstr(WEB_PAGE, "/api/status") != NULL);
    CHECK(strstr(WEB_PAGE, "/api/settings") != NULL);
    CHECK(strstr(WEB_PAGE, "/api/ring") != NULL);
    CHECK(strstr(WEB_PAGE, "/api/fault/corrupt") != NULL);
    CHECK(strstr(WEB_PAGE, "/api/fault/outage") != NULL);
    CHECK(strstr(WEB_PAGE, "/api/puzzles") != NULL);
    CHECK(strstr(WEB_PAGE, "X-Requested-With") != NULL);                           /* POSTs carry the anti-CSRF header */
    CHECK(strstr(WEB_PAGE, "500") != NULL);                                        /* 500 ms poll */
    CHECK(strstr(WEB_PAGE, "2000") != NULL);                                       /* 2 s fetch timeout */
    CHECK(strstr(WEB_PAGE, "My mornings") != NULL && strstr(WEB_PAGE, "Engineering") != NULL);
    CHECK(strstr(WEB_PAGE, "viewport") != NULL);
    CHECK(strstr(WEB_PAGE, "Connection lost") != NULL);
}

int main(void)
{
    test_page_budget_and_content();
    printf("test_page: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
