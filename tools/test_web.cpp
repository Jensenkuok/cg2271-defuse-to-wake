/* Host-side tests for esp32/defuse_esp32/src/app/web_logic.* (settings validation, outage expiry,
 * JSON writer / escaping, answer hiding, module total). The page itself is tested by test_page.cpp. */
#include <stdio.h>
#include <string.h>

#include "web_logic.h"
#include "config.h"

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

#define GOOD_LINK "https://nusmods.com/timetable/sem-1/share?CG2271=LAB:02,LEC:01,TUT:02&QF2104=LEC:1,TUT:4"

/* ================= settings ================= */
static void test_settings_accept_good_input(void)
{
    int lead = -1;

    CHECK(web_validate_settings(GOOD_LINK, "60", &lead) == SETTINGS_OK && lead == 60);
    CHECK(web_validate_settings(GOOD_LINK, "15", &lead) == SETTINGS_OK && lead == 15);
    CHECK(web_validate_settings(GOOD_LINK, "180", &lead) == SETTINGS_OK && lead == 180);
    CHECK(web_validate_settings(GOOD_LINK, " 90 ", &lead) == SETTINGS_OK && lead == 90);
    CHECK(web_validate_settings(GOOD_LINK, "090", &lead) == SETTINGS_OK && lead == 90);
    CHECK(web_validate_settings(GOOD_LINK, "45", NULL) == SETTINGS_OK);           /* lead_out is optional */
}

static void test_settings_reject_bad_lead(void)
{
    int lead = 77;

    CHECK(web_validate_settings(GOOD_LINK, NULL, &lead) == SETTINGS_ERR_LEAD_MISSING);
    CHECK(web_validate_settings(GOOD_LINK, "", &lead) == SETTINGS_ERR_LEAD_MISSING);
    CHECK(web_validate_settings(GOOD_LINK, "   ", &lead) == SETTINGS_ERR_LEAD_MISSING);
    CHECK(web_validate_settings(GOOD_LINK, "abc", &lead) == SETTINGS_ERR_LEAD_NOT_NUMBER);
    CHECK(web_validate_settings(GOOD_LINK, "-5", &lead) == SETTINGS_ERR_LEAD_NOT_NUMBER);
    CHECK(web_validate_settings(GOOD_LINK, "6 0", &lead) == SETTINGS_ERR_LEAD_NOT_NUMBER);
    CHECK(web_validate_settings(GOOD_LINK, "1e2", &lead) == SETTINGS_ERR_LEAD_NOT_NUMBER);
    CHECK(web_validate_settings(GOOD_LINK, "60.5", &lead) == SETTINGS_ERR_LEAD_NOT_NUMBER);
    CHECK(web_validate_settings(GOOD_LINK, "14", &lead) == SETTINGS_ERR_LEAD_RANGE);
    CHECK(web_validate_settings(GOOD_LINK, "181", &lead) == SETTINGS_ERR_LEAD_RANGE);
    CHECK(web_validate_settings(GOOD_LINK, "0", &lead) == SETTINGS_ERR_LEAD_RANGE);
    CHECK(web_validate_settings(GOOD_LINK, "99999999999999999999", &lead) == SETTINGS_ERR_LEAD_RANGE);
    CHECK(lead == 77);                                                            /* untouched on failure */
}

static void test_settings_reject_bad_link(void)
{
    char big[APP_LINK_MAX_CHARS + 64];
    size_t i;

    CHECK(web_validate_settings(NULL, "60", NULL) == SETTINGS_ERR_LINK_MISSING);
    CHECK(web_validate_settings("", "60", NULL) == SETTINGS_ERR_LINK_MISSING);
    CHECK(web_validate_settings("http://nusmods.com/timetable/sem-1/share?CG2271=LEC:01", "60", NULL) ==
          SETTINGS_ERR_LINK_PREFIX);                                              /* not https */
    CHECK(web_validate_settings("https://evil.example.com/timetable/sem-1/share?CG2271=LEC:01", "60", NULL) ==
          SETTINGS_ERR_LINK_PREFIX);
    CHECK(web_validate_settings("https://nusmods.com.evil.example/timetable/x?CG2271=LEC:01", "60", NULL) ==
          SETTINGS_ERR_LINK_PREFIX);
    CHECK(web_validate_settings("nusmods.com/timetable/sem-1/share?CG2271=LEC:01", "60", NULL) ==
          SETTINGS_ERR_LINK_PREFIX);
    CHECK(web_validate_settings("https://nusmods.com/timetable/sem-1/share?CG2271=LEC:01 x", "60", NULL) ==
          SETTINGS_ERR_LINK_CHARS);                                               /* space */
    CHECK(web_validate_settings("https://nusmods.com/timetable/sem-1/share?CG2271=LEC:01\n", "60", NULL) ==
          SETTINGS_ERR_LINK_CHARS);                                               /* control char */
    CHECK(web_validate_settings("https://nusmods.com/timetable/sem-1/share", "60", NULL) ==
          SETTINGS_ERR_LINK_NO_SLOTS);                                            /* no '?' query */
    CHECK(web_validate_settings("https://nusmods.com/timetable/sem-1/share?garbage", "60", NULL) ==
          SETTINGS_ERR_LINK_NO_SLOTS);

    /* exactly the maximum is fine as long as it parses; one more is too long */
    strcpy(big, GOOD_LINK);
    for (i = strlen(big); i < (size_t)APP_LINK_MAX_CHARS; i++) {
        big[i] = 'x';                                                             /* padding inside the last value */
    }
    big[APP_LINK_MAX_CHARS] = '\0';
    CHECK(web_validate_settings(big, "60", NULL) == SETTINGS_OK);
    big[APP_LINK_MAX_CHARS] = 'x';
    big[APP_LINK_MAX_CHARS + 1] = '\0';
    CHECK(web_validate_settings(big, "60", NULL) == SETTINGS_ERR_LINK_TOO_LONG);
}

static void test_settings_link_error_wins_over_lead_error(void)
{
    CHECK(web_validate_settings("nope", "abc", NULL) == SETTINGS_ERR_LINK_PREFIX);
}

static void test_settings_messages(void)
{
    CHECK(strlen(web_settings_err_text(SETTINGS_OK)) > 0u);
    CHECK(strstr(web_settings_err_text(SETTINGS_ERR_LEAD_RANGE), "15") != NULL);
    CHECK(strstr(web_settings_err_text(SETTINGS_ERR_LEAD_RANGE), "180") != NULL);
    CHECK(strstr(web_settings_err_text(SETTINGS_ERR_LINK_PREFIX), "nusmods.com/timetable") != NULL);
    CHECK(strstr(web_settings_err_text(SETTINGS_ERR_LINK_TOO_LONG), "512") != NULL);
    CHECK(strlen(web_settings_err_text((SettingsErr)99)) > 0u);
}

/* ================= outage ================= */
static void test_outage_basics(void)
{
    Outage o = { false, 0u };

    CHECK(!outage_active(o, 1000u));
    CHECK(outage_left_s(o, 1000u) == 0u);
    outage_start(&o, 1000u, 60000u);
    CHECK(outage_active(o, 1000u));
    CHECK(outage_active(o, 60999u));
    CHECK(!outage_active(o, 61000u));                                             /* expiry instant is over */
    CHECK(!outage_active(o, 90000u));
    CHECK(outage_left_s(o, 1000u) == 60u);
    CHECK(outage_left_s(o, 1001u) == 60u);                                        /* rounded up */
    CHECK(outage_left_s(o, 31000u) == 30u);
    CHECK(outage_left_s(o, 60999u) == 1u);
    CHECK(outage_left_s(o, 61000u) == 0u);
}

static void test_outage_restart_resets_the_timer(void)
{
    Outage o = { false, 0u };

    outage_start(&o, 0u, 60000u);
    outage_start(&o, 50000u, 60000u);
    CHECK(outage_active(o, 100000u));
    CHECK(!outage_active(o, 110000u));
}

static void test_outage_survives_millis_wraparound(void)
{
    Outage o = { false, 0u };
    const uint32_t start = 0xFFFFFFFFu - 10000u;

    outage_start(&o, start, 60000u);                                              /* until_ms wraps to ~49999 */
    CHECK(o.until_ms == (uint32_t)(start + 60000u));
    CHECK(outage_active(o, start));
    CHECK(outage_active(o, 0u));                                                  /* after the wrap */
    CHECK(outage_active(o, 49998u));
    CHECK(!outage_active(o, 50000u));
    CHECK(outage_left_s(o, 0u) == 50u);
}

static void test_outage_zero_duration_is_never_active(void)
{
    Outage o = { false, 0u };

    outage_start(&o, 500u, 0u);
    CHECK(!outage_active(o, 500u));
    CHECK(outage_left_s(o, 500u) == 0u);
}

/* ================= JSON ================= */
static void test_json_escape(void)
{
    char out[64];

    CHECK(json_escape("plain text 123", out, sizeof(out)) && strcmp(out, "plain text 123") == 0);
    CHECK(json_escape("say \"hi\"", out, sizeof(out)) && strcmp(out, "say \\\"hi\\\"") == 0);
    CHECK(json_escape("a\\b", out, sizeof(out)) && strcmp(out, "a\\\\b") == 0);
    CHECK(json_escape("l1\nl2\r\tx", out, sizeof(out)) && strcmp(out, "l1\\nl2\\r\\tx") == 0);
    CHECK(json_escape("a\001b", out, sizeof(out)) && strcmp(out, "a\\u0001b") == 0);
    CHECK(json_escape("a\177b", out, sizeof(out)) && strcmp(out, "a\\u007fb") == 0);
    CHECK(json_escape("<b>&", out, sizeof(out)) && strcmp(out, "\\u003cb\\u003e\\u0026") == 0);
    CHECK(json_escape("caf\xC3\xA9", out, sizeof(out)) && strcmp(out, "caf??") == 0);   /* non-ASCII -> ? */
    CHECK(json_escape("", out, sizeof(out)) && out[0] == '\0');
    CHECK(json_escape(NULL, out, sizeof(out)) && out[0] == '\0');
}

static void test_json_escape_truncates_cleanly(void)
{
    char out[8];

    CHECK(!json_escape("0123456789", out, sizeof(out)));
    CHECK(strlen(out) == 7u && strncmp(out, "0123456", 7) == 0);
    /* an escape is never cut in half: \" needs 2 bytes, only 1 left after "abcde" */
    CHECK(!json_escape("abcde\"x", out, 7u));
    CHECK(strcmp(out, "abcde") == 0);
    CHECK(!json_escape("x", out, 0u));                                            /* no room at all: no crash */
    CHECK(!json_escape("x", NULL, 4u));
}

static void test_json_writer_builds_nested_output(void)
{
    char buf[256];
    JsonBuf j;

    jb_init(&j, buf, sizeof(buf));
    jb_obj(&j);
    jb_kv_str(&j, "name", "A \"B\"");
    jb_kv_uint(&j, "n", 42u);
    jb_kv_int(&j, "neg", -7);
    jb_kv_bool(&j, "ok", true);
    jb_key(&j, "list");
    jb_arr(&j);
    jb_uint(&j, 1u);
    jb_str(&j, "two");
    jb_obj(&j);
    jb_kv_bool(&j, "x", false);
    jb_end(&j);
    jb_null(&j);
    jb_end(&j);
    jb_key(&j, "empty");
    jb_obj(&j);
    jb_end(&j);
    jb_end(&j);
    CHECK(jb_ok(j));
    CHECK(strcmp(buf, "{\"name\":\"A \\\"B\\\"\",\"n\":42,\"neg\":-7,\"ok\":true,"
                      "\"list\":[1,\"two\",{\"x\":false},null],\"empty\":{}}") == 0);
}

static void test_json_writer_numbers(void)
{
    char buf[128];
    JsonBuf j;

    jb_init(&j, buf, sizeof(buf));
    jb_arr(&j);
    jb_uint(&j, 0u);
    jb_uint(&j, 4294967295u);
    jb_int(&j, -2147483647 - 1);
    jb_int(&j, 2147483647);
    jb_end(&j);
    CHECK(jb_ok(j));
    CHECK(strcmp(buf, "[0,4294967295,-2147483648,2147483647]") == 0);
}

static void test_json_writer_overflow_is_reported_and_terminated(void)
{
    char buf[16];
    JsonBuf j;

    jb_init(&j, buf, sizeof(buf));
    jb_obj(&j);
    jb_kv_str(&j, "key", "a value that is much too long for sixteen bytes");
    jb_end(&j);
    CHECK(!jb_ok(j) && j.overflow);
    CHECK(strlen(buf) < sizeof(buf));                                              /* still a C string */
    /* later calls after the overflow do nothing and do not crash */
    jb_kv_uint(&j, "n", 1u);
    CHECK(strlen(buf) < sizeof(buf));
}

static void test_json_writer_unbalanced_is_not_ok(void)
{
    char buf[64];
    JsonBuf j;

    jb_init(&j, buf, sizeof(buf));
    jb_obj(&j);
    CHECK(!jb_ok(j));                                                              /* object still open */
    jb_end(&j);
    CHECK(jb_ok(j));
    jb_end(&j);                                                                    /* one end too many */
    CHECK(!jb_ok(j));
}

static void test_json_writer_depth_limit(void)
{
    char buf[128];
    JsonBuf j;
    int i;

    jb_init(&j, buf, sizeof(buf));
    for (i = 0; i < JSON_MAX_DEPTH + 2; i++) {
        jb_arr(&j);
    }
    CHECK(!jb_ok(j));
}

static void test_json_writer_tiny_buffers(void)
{
    char buf[1];
    JsonBuf j;

    jb_init(&j, buf, sizeof(buf));
    jb_obj(&j);
    CHECK(!jb_ok(j) && buf[0] == '\0');
    jb_init(&j, NULL, 0u);
    jb_obj(&j);
    CHECK(!jb_ok(j));
}

/* ================= game panel helpers ================= */
static void test_answer_hidden_rule(void)
{
    CHECK(!answer_hidden(true, PROTO_STATE_IDLE, false));
    CHECK(answer_hidden(true, PROTO_STATE_ARMED, false));
    CHECK(answer_hidden(true, PROTO_STATE_BOMB_ACTIVE, true));
    CHECK(answer_hidden(true, PROTO_STATE_BOMB_ACTIVE, false));
    CHECK(answer_hidden(true, PROTO_STATE_EXPLODED, true));                        /* next bomb is 3 s away */
    CHECK(answer_hidden(true, PROTO_STATE_DEFUSED, true));
    CHECK(!answer_hidden(true, PROTO_STATE_DEFUSED, false));
    CHECK(answer_hidden(false, PROTO_STATE_IDLE, false));                          /* MCX state unknown: hide */
}

static void test_modules_total(void)
{
    /* progress_pct = cleared * 100 / total, cleared = module_idx */
    CHECK(modules_total(1, 33) == 3);
    CHECK(modules_total(2, 66) == 3);
    CHECK(modules_total(1, 25) == 4);
    CHECK(modules_total(2, 50) == 4);
    CHECK(modules_total(3, 75) == 4);
    CHECK(modules_total(1, 50) == 2);
    CHECK(modules_total(1, 20) == 0);                                              /* 5 modules: beyond the game's cap */
    CHECK(modules_total(0, 0) == 0);                                               /* first module: cannot tell */
    CHECK(modules_total(1, 0) == 0);
    CHECK(modules_total(2, 99) == 0);                                              /* inconsistent */
    CHECK(modules_total(5, 100) == 0);
}

int main(void)
{
    test_settings_accept_good_input();
    test_settings_reject_bad_lead();
    test_settings_reject_bad_link();
    test_settings_link_error_wins_over_lead_error();
    test_settings_messages();
    test_outage_basics();
    test_outage_restart_resets_the_timer();
    test_outage_survives_millis_wraparound();
    test_outage_zero_duration_is_never_active();
    test_json_escape();
    test_json_escape_truncates_cleanly();
    test_json_writer_builds_nested_output();
    test_json_writer_numbers();
    test_json_writer_overflow_is_reported_and_terminated();
    test_json_writer_unbalanced_is_not_ok();
    test_json_writer_depth_limit();
    test_json_writer_tiny_buffers();
    test_answer_hidden_rule();
    test_modules_total();
    printf("test_web: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
