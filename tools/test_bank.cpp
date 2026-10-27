/*
 * Host-side tests for the procedural puzzle bank (puzzle_bank*.cpp). Plain checks, no framework.
 *
 * The bank is the safety net of the nightly puzzle run, so these tests do not trust it: every
 * generated text is converted back to rules by this file's OWN converter, proven by pr_check
 * (validated separately in test_rules.cpp) and compared with the answer and level that were asked
 * for. The acceptance gate for LLM replies has its own tests in test_gate.cpp.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <string>

#include "puzzle_bank.h"
#include "puzzle_gate.h"
#include "text_logic.h"

static int g_checks;
static int g_failed;

#define CHECK(cond)                                                              \
    do {                                                                         \
        g_checks++;                                                              \
        if (!(cond)) {                                                           \
            g_failed++;                                                          \
            if (g_failed < 40) {                                                 \
                printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);          \
            }                                                                    \
        }                                                                        \
    } while (0)

#define CHECK_MSG(cond, msg)                                                     \
    do {                                                                         \
        g_checks++;                                                              \
        if (!(cond)) {                                                           \
            g_failed++;                                                          \
            if (g_failed < 40) {                                                 \
                printf("  FAIL %s:%d: %s  [%s]\n", __FILE__, __LINE__, #cond, (msg).c_str()); \
            }                                                                    \
        }                                                                        \
    } while (0)

#define FALLBACK_LIMIT_PERCENT  2.0     /* the plan's bound per level */
#define DIAL_STEP               7       /* every 7th code */
#define WIRE_SEEDS              25      /* seeds per (n, k, level) */

static const int LEVELS[3] = { PR_EASY, PR_NORMAL, PR_HARD };

/* ================= this file's own converters (not the bank's) ================= */
static std::string replace_all(std::string s, const std::string &from, const std::string &to)
{
    for (size_t pos = s.find(from); pos != std::string::npos; pos = s.find(from, pos + to.size())) {
        s.replace(pos, from.size(), to);
    }
    return s;
}

static std::string own_dial_rules(const std::string &text)
{
    std::string s = replace_all(text, "All differ", "alldiff");
    for (int i = 0; i < 4; i++) {
        static const char *const ord[4] = { "1st", "2nd", "3rd", "4th" };
        s = replace_all(s, ord[i], std::string("d") + (char)('1' + i));
    }
    for (int digit = 0; digit <= 9; digit++) {                      /* "Sum 17" is "sum=17" */
        s = replace_all(s, std::string("Sum ") + (char)('0' + digit), std::string("sum=") + (char)('0' + digit));
    }
    return replace_all(s, "Sum", "sum");
}

static std::string own_wire_rules(const std::string &text)
{
    const std::string prefix = "Cut wire w of n: ";
    return text.compare(0, prefix.size(), prefix) == 0 ? text.substr(prefix.size()) : std::string("<no prefix>");
}

static std::string dial_answer_text(int code)
{
    char b[8];
    snprintf(b, sizeof b, "%04d", code);
    return b;
}

static int code_of(const uint8_t d[4]) { return d[0] * 1000 + d[1] * 100 + d[2] * 10 + d[3]; }

static void digits_of(int code, uint8_t d[4])
{
    d[0] = (uint8_t)(code / 1000);
    d[1] = (uint8_t)(code / 100 % 10);
    d[2] = (uint8_t)(code / 10 % 10);
    d[3] = (uint8_t)(code % 10);
}

static int wrap_rows(const char *text)
{
    char out[8][TEXT_COLS + 1];
    size_t rest = 0;
    const int n = wrap_text(text, TEXT_COLS, 8, out[0], TEXT_COLS + 1, &rest);
    return rest == strlen(text) ? n : 99;
}

/* ================= conversion round trips ================= */
static void test_conversion(void)
{
    char text[BANK_RULES_MAX];
    char rules[BANK_RULES_MAX];

    static const char *const dial_rules[] = {
        "d1=3;d2=d1+4;d3=d2-5;d4=d1*3",
        "sum=17;d4-d1=1;alldiff;d1-d2=4",
        "d1+d2=9;d1-d2=1;d3=2*d4;d3+d4=12",
        "odd(d2);sum>=23;even(d1+d3);d4!=0",
        "sum=0;d1<d2",
        "sum=17;sum<20",
    };
    for (size_t i = 0; i < sizeof dial_rules / sizeof dial_rules[0]; i++) {
        CHECK(bank_rules_to_text(dial_rules[i], PUZZLE_DIAL, text, sizeof text));
        CHECK(bank_text_to_rules(text, PUZZLE_DIAL, rules, sizeof rules));
        PrCheck a;
        PrCheck b;
        pr_check(dial_rules[i], PR_KIND_DIAL, 0, 0, &a);
        pr_check(rules, PR_KIND_DIAL, 0, 0, &b);
        CHECK(a.error == PR_OK && b.error == PR_OK);
        CHECK(a.count == b.count && a.solution == b.solution && a.level == b.level);
        CHECK(own_dial_rules(text) == rules);                   /* the two converters agree */
    }
    CHECK(bank_rules_to_text("sum=17", PUZZLE_DIAL, text, sizeof text) && strcmp(text, "Sum 17") == 0);
    CHECK(bank_rules_to_text("alldiff", PUZZLE_DIAL, text, sizeof text) && strcmp(text, "All differ") == 0);
    CHECK(bank_rules_to_text("d1=3;d2=d1+4", PUZZLE_DIAL, text, sizeof text) && strcmp(text, "1st=3; 2nd=1st+4") == 0);
    CHECK(bank_rules_to_text("odd(d2)", PUZZLE_DIAL, text, sizeof text) && strcmp(text, "odd(2nd)") == 0);
    CHECK(bank_rules_to_text("w>2;w<n", PUZZLE_WIRE, text, sizeof text) &&
          strcmp(text, "Cut wire w of n: w>2; w<n") == 0);
    CHECK(bank_text_to_rules("Cut wire w of n: w>2; w<n", PUZZLE_WIRE, rules, sizeof rules) &&
          strcmp(rules, "w>2; w<n") == 0);
    CHECK(!bank_text_to_rules("w>2; w<n", PUZZLE_WIRE, rules, sizeof rules));           /* prefix required */
    CHECK(!bank_text_to_rules("x", 0, rules, sizeof rules));
    CHECK(!bank_text_to_rules(NULL, PUZZLE_DIAL, rules, sizeof rules));
    CHECK(!bank_rules_to_text("d1=3;d2=d1+4;d3=d2-5;d4=d1*3", PUZZLE_DIAL, text, 10));  /* does not fit */
    CHECK(!bank_text_to_rules("1st=3; 2nd=1st+4; 3rd=2nd-5", PUZZLE_DIAL, rules, 10));
    /* the old template bank's own texts convert and rate through the same path */
    {
        uint8_t code[4] = { 4, 2, 7, 1 };
        int lvl = 0;
        for (unsigned v = 0; v < PUZZLE_VARIANTS; v++) {
            char t[PUZZLE_TEXT_BUF];
            CHECK(dial_template(code, v, t, sizeof t) > 0);
            CHECK(bank_recheck(t, PUZZLE_DIAL, 0, 4271, &lvl));
            CHECK(lvl >= PR_EASY && lvl <= PR_HARD);
            CHECK(!bank_recheck(t, PUZZLE_DIAL, 0, 4272, NULL));
        }
    }
}

/* ================= the generated bank ================= */
struct Stat {
    int n;
    int fallbacks;
    int impossible;                 /* wire HARD with 3 wires: no rule set can exist */
    int bad;
    long tries;
    int max_tries;
    int wrap_over;                  /* texts that need more OLED rows than O3 allows when word-wrapped */
    double total_ms;
    double worst_ms;
};

/* All checks on one result; returns false when something is wrong. */
static bool check_result(const BankRequest &req, const BankResult &res, Stat *st)
{
    const bool dial = req.kind == PUZZLE_DIAL;
    const std::string text = res.text;
    const int answer = dial ? code_of(req.code) : req.wire_k;
    const std::string answer_text = dial ? dial_answer_text(answer) : std::to_string(answer);
    const std::string rules = dial ? own_dial_rules(text) : own_wire_rules(text);
    PrCheck chk;
    bool ok = true;

    st->n++;
    st->tries += res.tries;
    if (res.tries > st->max_tries) {
        st->max_tries = res.tries;
    }
    if (res.fallback != 0) {
        if (!dial && req.level == PR_HARD && req.wire_count < 4) {
            st->impossible++;                                       /* by construction, not a failure of the search */
        } else {
            st->fallbacks++;
        }
    }
    if (text.empty() || text.size() >= (size_t)PUZZLE_TEXT_BUF) {
        return false;
    }
    pr_check(rules.c_str(), dial ? PR_KIND_DIAL : PR_KIND_WIRE, dial ? 0 : req.wire_count, answer, &chk);
    ok = ok && chk.error == PR_OK && chk.unique == 1 && chk.matches_answer == 1;        /* proven unique, our answer */
    ok = ok && res.level == chk.level;                                                  /* stored level = measured */
    /* the same bar for generated texts AND fallbacks: valid, never names the answer, fits its rows */
    ok = ok && validate_puzzle(text.c_str(), req.kind, answer_text.c_str());            /* O3, O4, no leak */
    ok = ok && text.size() <= (size_t)(dial ? PUZZLE_DIAL_TEXT_LIMIT : PUZZLE_WIRE_TEXT_LIMIT);
    ok = ok && bank_recheck(text.c_str(), req.kind, dial ? 0 : req.wire_count, answer, NULL);
    ok = ok && puzzle_text_fits_oled(text.c_str(), req.kind);
    if (wrap_rows(text.c_str()) > (dial ? 3 : 2)) {
        st->wrap_over++;
    }
    if (res.fallback == 0) {
        ok = ok && chk.level == req.level;                                              /* rated at the level asked */
    }
    return ok;
}

static double now_ms(void) { return (double)clock() * 1000.0 / CLOCKS_PER_SEC; }

static void print_stat(const char *what, const Stat &st)
{
    printf("  %-18s n=%-5d fallback %d (%.2f%%)%s  avg tries %.1f max %d  gen %.3f ms avg, %.1f ms worst  wrap>rows %d\n",
           what, st.n, st.fallbacks, 100.0 * st.fallbacks / (st.n > 0 ? st.n : 1),
           st.impossible > 0 ? " [+impossible]" : "", (double)st.tries / (st.n > 0 ? st.n : 1), st.max_tries,
           st.total_ms / (st.n > 0 ? st.n : 1), st.worst_ms, st.wrap_over);
}

static void test_dial_bank(void)
{
    for (int li = 0; li < 3; li++) {
        Stat st;
        memset(&st, 0, sizeof st);
        for (int code = 3; code < 10000; code += DIAL_STEP) {
            BankRequest req;
            BankResult res;
            memset(&req, 0, sizeof req);
            req.kind = PUZZLE_DIAL;
            digits_of(code, req.code);
            req.level = LEVELS[li];
            req.seed = (uint32_t)code * 2654435761u + (uint32_t)li;
            const double t0 = now_ms();
            bank_generate(req, &res);
            const double ms = now_ms() - t0;
            st.total_ms += ms;
            if (ms > st.worst_ms) {
                st.worst_ms = ms;
            }
            const bool ok = check_result(req, res, &st);
            if (!ok) {
                st.bad++;
            }
            CHECK_MSG(ok, std::string(pr_level_name(LEVELS[li])) + " " + dial_answer_text(code) + " \"" + res.text + "\"");
        }
        char name[32];
        snprintf(name, sizeof name, "dial %s", pr_level_name(LEVELS[li]));
        print_stat(name, st);
        CHECK(st.bad == 0);
        CHECK(100.0 * st.fallbacks / st.n < FALLBACK_LIMIT_PERCENT);
        CHECK(st.wrap_over == 0);               /* generated texts always fit their OLED rows after word wrap */
    }
}

static void test_wire_bank(void)
{
    for (int li = 0; li < 3; li++) {
        Stat all;
        memset(&all, 0, sizeof all);
        for (int n = 3; n <= 6; n++) {
            Stat st;
            memset(&st, 0, sizeof st);
            for (int k = 1; k <= n; k++) {
                for (int seed = 0; seed < WIRE_SEEDS; seed++) {
                    BankRequest req;
                    BankResult res;
                    memset(&req, 0, sizeof req);
                    req.kind = PUZZLE_WIRE;
                    req.wire_count = n;
                    req.wire_k = k;
                    req.level = LEVELS[li];
                    req.seed = (uint32_t)(seed * 7919 + k * 104729 + n * 1299709 + li);
                    const double t0 = now_ms();
                    bank_generate(req, &res);
                    const double ms = now_ms() - t0;
                    st.total_ms += ms;
                    if (ms > st.worst_ms) {
                        st.worst_ms = ms;
                    }
                    const bool ok = check_result(req, res, &st);
                    if (!ok) {
                        st.bad++;
                    }
                    CHECK_MSG(ok, std::string(pr_level_name(LEVELS[li])) + " n=" + std::to_string(n) + " k=" +
                                      std::to_string(k) + " \"" + res.text + "\"");
                }
            }
            char name[32];
            snprintf(name, sizeof name, "wire %s n=%d", pr_level_name(LEVELS[li]), n);
            print_stat(name, st);
            CHECK(st.bad == 0);
            if (LEVELS[li] == PR_HARD && n == 3) {
                CHECK(st.impossible == st.n);       /* 3 wires: two rules always suffice, HARD is impossible by construction */
            } else {
                CHECK(100.0 * st.fallbacks / st.n < FALLBACK_LIMIT_PERCENT);
            }
            all.n += st.n;
            all.fallbacks += st.fallbacks;
            all.impossible += st.impossible;
            all.wrap_over += st.wrap_over;
        }
        printf("  wire %-6s total: %d puzzles, %d fallbacks, %d impossible (HARD with 3 wires), %d need more rows than O3\n",
               pr_level_name(LEVELS[li]), all.n, all.fallbacks, all.impossible, all.wrap_over);
        CHECK(all.wrap_over == 0);
    }
}

static void test_bank_determinism_and_bounds(void)
{
    BankRequest req;
    BankResult a;
    BankResult b;

    memset(&req, 0, sizeof req);
    req.kind = PUZZLE_DIAL;
    digits_of(4271, req.code);
    for (int li = 0; li < 3; li++) {
        req.level = LEVELS[li];
        req.seed = 12345u;
        bank_generate(req, &a);
        bank_generate(req, &b);
        CHECK(strcmp(a.text, b.text) == 0 && a.level == b.level && a.tries == b.tries && a.fallback == b.fallback);
        req.seed = 54321u;
        bank_generate(req, &b);
        CHECK(strcmp(a.text, b.text) != 0 || li == 0);          /* another seed gives another puzzle (EASY may coincide) */
    }
    /* the wire pool path is deterministic too */
    memset(&req, 0, sizeof req);
    req.kind = PUZZLE_WIRE;
    req.wire_count = 6;
    req.wire_k = 4;
    req.level = PR_NORMAL;
    req.seed = 777u;
    bank_generate(req, &a);
    bank_generate(req, &b);
    CHECK(strcmp(a.text, b.text) == 0);

    /* out-of-range requests are clamped, never crash and never return an empty text */
    memset(&req, 0, sizeof req);
    req.kind = PUZZLE_WIRE;
    req.wire_count = 99;
    req.wire_k = 99;
    req.level = 77;
    bank_generate(req, &a);
    CHECK(a.text[0] != '\0');
    req.wire_count = 0;
    req.wire_k = -4;
    bank_generate(req, &a);
    CHECK(a.text[0] != '\0');
    memset(&req, 0, sizeof req);
    req.kind = PUZZLE_DIAL;
    req.code[0] = 99;
    req.code[1] = 99;
    req.code[2] = 99;
    req.code[3] = 99;
    bank_generate(req, &a);                                     /* digits reduced mod 10: 9999, level defaults to NORMAL */
    CHECK(a.text[0] != '\0' && bank_recheck(a.text, PUZZLE_DIAL, 0, 9999, NULL));

    /* the yield hook runs during a long search */
    {
        static int calls;
        struct H { static void hook(void) { calls++; } };
        calls = 0;
        memset(&req, 0, sizeof req);
        req.kind = PUZZLE_DIAL;
        digits_of(0, req.code);
        req.level = PR_HARD;
        req.max_tries = 40;
        req.seed = 1;
        req.yield = H::hook;
        bank_generate(req, &a);
        /* the hook runs after every BANK_YIELD_EVERY-th failed try */
        CHECK(calls == ((a.fallback != 0 ? a.tries : a.tries - 1) / BANK_YIELD_EVERY));
    }
}

/* Forcing the fallback with a tiny try budget: the old templates must still be correct, rated by
 * measurement, fit O3 and never be empty. */
static void test_fallback(void)
{
    int fallbacks = 0;
    int exact = 0;

    for (int li = 0; li < 3; li++) {
        for (int code = 11; code < 10000; code += 211) {
            BankRequest req;
            BankResult res;
            memset(&req, 0, sizeof req);
            req.kind = PUZZLE_DIAL;
            digits_of(code, req.code);
            req.level = LEVELS[li];
            req.max_tries = 1;
            req.seed = (uint32_t)code;
            bank_generate(req, &res);
            const std::string text = res.text;
            int measured = 0;

            CHECK(!text.empty() && text.size() <= (size_t)PUZZLE_DIAL_TEXT_LIMIT);
            CHECK(bank_recheck(text.c_str(), PUZZLE_DIAL, 0, code, &measured));
            CHECK(res.level == measured);
            CHECK(validate_puzzle(text.c_str(), PUZZLE_DIAL, dial_answer_text(code).c_str()));
            CHECK(puzzle_text_fits_oled(text.c_str(), PUZZLE_DIAL));    /* fallbacks wrap into 3 rows too */
            CHECK(wrap_rows(text.c_str()) <= 3);
            if (res.fallback != 0) {
                fallbacks++;
                exact += (res.level == LEVELS[li]) ? 1 : 0;
            }
            CHECK(res.fallback != 0 || res.level == LEVELS[li]);
        }
    }
    printf("  forced fallback (1 try): %d dial fallbacks, %d of them rated exactly at the level\n", fallbacks, exact);
    CHECK(fallbacks > 20);
    /* wires: k == 1 and k == n (the old templates named the wire there) for every n and level, with the
     * search disabled (1 try) and with the normal budget; every text is proven, valid and rated */
    for (int li = 0; li < 3; li++) {
        for (int n = 3; n <= 6; n++) {
            for (int pass = 0; pass < 2; pass++) {
                for (int k = 1; k <= n; k += (n - 1)) {
                    for (int seed = 0; seed < 40; seed++) {
                        BankRequest req;
                        BankResult res;
                        Stat st;
                        memset(&req, 0, sizeof req);
                        memset(&st, 0, sizeof st);
                        req.kind = PUZZLE_WIRE;
                        req.wire_count = n;
                        req.wire_k = k;
                        req.level = LEVELS[li];
                        req.max_tries = (pass == 0) ? 1 : 0;
                        req.seed = (uint32_t)(n * 1000 + k * 100 + seed + li);
                        bank_generate(req, &res);
                        const std::string msg = std::string(pr_level_name(LEVELS[li])) + " n=" + std::to_string(n) + " k=" +
                                                std::to_string(k) + " pass=" + std::to_string(pass) + " \"" + res.text + "\"";
                        CHECK_MSG(check_result(req, res, &st), msg);
                        CHECK_MSG(validate_puzzle(res.text, PUZZLE_WIRE, std::to_string(k).c_str()), msg);
                        CHECK_MSG(bank_recheck(res.text, PUZZLE_WIRE, n, k, NULL), msg);
                        CHECK_MSG(res.level >= PR_EASY && res.level <= PR_HARD, msg);
                        CHECK_MSG(!puzzle_text_has_number(res.text, k), msg);
                        if (pass == 0 && res.fallback != 0) {
                            CHECK_MSG(res.level == PR_EASY, msg);       /* the fallback is the EASY recipe or its fixed rule */
                        }
                    }
                }
            }
        }
    }
    /* the fixed last-resort wire rule "w+c=k+c" (c = 1, or 2 for k = 1): valid, never names k, proven EASY */
    for (int n = 3; n <= 6; n++) {
        for (int k = 1; k <= n; k++) {
            const int c = (k == 1) ? 2 : 1;
            char rules[32];
            char text[PUZZLE_TEXT_BUF];
            int lvl = 0;

            snprintf(rules, sizeof rules, "w+%d=%d", c, k + c);
            CHECK(bank_rules_to_text(rules, PUZZLE_WIRE, text, sizeof text));
            CHECK(validate_puzzle(text, PUZZLE_WIRE, std::to_string(k).c_str()));
            CHECK(puzzle_text_fits_oled(text, PUZZLE_WIRE));
            CHECK(bank_recheck(text, PUZZLE_WIRE, n, k, &lvl) && lvl == PR_EASY);
        }
    }
    /* the old wire templates fit O3 now (family 3 was 44 characters) and the first valid one is chosen */
    for (int n = 3; n <= 6; n++) {
        for (int k = 1; k <= n; k++) {
            for (unsigned v = 0; v < PUZZLE_VARIANTS; v++) {
                char t[PUZZLE_TEXT_BUF];
                CHECK(wire_template(k, n, v, t, sizeof t) > 0 && strlen(t) <= (size_t)PUZZLE_WIRE_TEXT_LIMIT);
            }
        }
    }
    for (int code = 0; code < 10000; code += 13) {
        uint8_t d[4];
        digits_of(code, d);
        for (unsigned v = 0; v < PUZZLE_VARIANTS; v++) {
            char t[PUZZLE_TEXT_BUF];
            CHECK(dial_template(d, v, t, sizeof t) > 0 && strlen(t) <= (size_t)PUZZLE_DIAL_TEXT_LIMIT);
        }
    }
}

static void test_bank_set(void)
{
    uint32_t rnd[PUZZLE_BANK_WORDS];
    PuzzleSet s;
    int counts[4] = { 0, 0, 0, 0 };

    CHECK(PUZZLE_MAGIC == 0x505A4C32u);                        /* "PZL2": old cached sets (no level) are discarded */
    for (int round = 0; round < 200; round++) {
        for (unsigned i = 0; i < PUZZLE_BANK_WORDS; i++) {
            uint32_t seed = (uint32_t)round * 40503u + i * 2654435761u + 12345u;
            rnd[i] = seed * 2246822519u ^ (seed >> 13);
        }
        const int level = LEVELS[round % 3];
        bank_fill_set(&s, rnd, level);
        CHECK(puzzle_set_valid(s) && s.magic == PUZZLE_MAGIC && s.ai_count == 0 && s.generated_unix == 0);
        for (unsigned i = 0; i < PUZZLE_PAIRS; i++) {
            char ans[8];
            int measured = 0;

            CHECK(s.dial[i].source == PUZZLE_SRC_BANK && s.wire[i].source == PUZZLE_SRC_BANK);
            CHECK(s.dial[i].level >= PR_EASY && s.dial[i].level <= PR_HARD);
            CHECK(s.wire[i].level >= PR_EASY && s.wire[i].level <= PR_HARD);
            CHECK(bank_recheck(s.dial[i].text, PUZZLE_DIAL, 0, code_of(s.dial[i].dial_code), &measured));
            CHECK(measured == s.dial[i].level);
            CHECK(puzzle_answer_text(s.dial[i], ans, sizeof ans));
            CHECK(strlen(s.dial[i].text) <= (size_t)PUZZLE_DIAL_TEXT_LIMIT);
            CHECK(strlen(s.wire[i].text) <= (size_t)PUZZLE_WIRE_TEXT_MAX);
            /* P3: the set is built for one level - its wire count comes from that level's range (EASY 3-4,
             * NORMAL 4-5, HARD 6), the wire puzzle is proven for that count and answer, and the set remembers it */
            CHECK(s.level == level);
            CHECK(s.wire[i].wire_count >= difficulty_level_spec(level).wire_min &&
                  s.wire[i].wire_count <= difficulty_level_spec(level).wire_max);
            CHECK(bank_recheck(s.wire[i].text, PUZZLE_WIRE, s.wire[i].wire_count, s.wire[i].wire_k, &measured));
            CHECK(measured == s.wire[i].level);
            counts[s.dial[i].level]++;
        }
    }
    printf("  bank_fill_set: dial levels EASY %d NORMAL %d HARD %d over 200 sets\n", counts[PR_EASY], counts[PR_NORMAL],
           counts[PR_HARD]);
    CHECK(counts[PR_EASY] > 100 && counts[PR_NORMAL] > 100 && counts[PR_HARD] > 100);
    /* the level field is range-checked when a set is read back from flash */
    s.dial[1].level = (uint8_t)(PUZZLE_LEVEL_MAX + 1);
    CHECK(!puzzle_set_valid(s));
    s.dial[1].level = PR_HARD;
    s.wire[2].level = 200;
    CHECK(!puzzle_set_valid(s));
    s.wire[2].level = 0;
    CHECK(puzzle_set_valid(s));
    s.magic = 0x505A4C31u;                                      /* the old "PZL1" blob */
    CHECK(!puzzle_set_valid(s));
    CHECK(sizeof(PuzzleSet) < 1500);
    printf("  sizeof(Puzzle) = %u, sizeof(PuzzleSet) = %u\n", (unsigned)sizeof(Puzzle), (unsigned)sizeof(PuzzleSet));
}

/* ================= the gate accepts what the bank makes ================= */
static std::string reply_of(const std::string &puzzle, const std::string &rules)
{
    return "{\"puzzle\":\"" + puzzle + "\",\"rules\":\"" + rules + "\"}";
}

struct Gate {
    int status;
    std::string why;
};

static Gate run_setter(const std::string &reply, const GateTarget &t)
{
    char json[2048];
    char text[PUZZLE_TEXT_BUF];
    char why[GATE_WHY_MAX];
    Gate g;
    g.status = gate_check_setter(reply.c_str(), t, json, sizeof json, text, sizeof text, why, sizeof why);
    g.why = why;
    return g;
}

/* The whole path with a bank puzzle standing in for the LLM: text and rules of a generated puzzle
 * must pass the setter gate at their own level, at every level. */
static void test_gate_accepts_bank_puzzles(void)
{
    int accepted = 0;
    int total = 0;

    for (int li = 0; li < 3; li++) {
        for (int code = 17; code < 10000; code += 997) {
            BankRequest req;
            BankResult res;
            char rules[BANK_RULES_MAX];
            GateTarget t;

            memset(&req, 0, sizeof req);
            req.kind = PUZZLE_DIAL;
            digits_of(code, req.code);
            req.level = LEVELS[li];
            req.seed = (uint32_t)code;
            bank_generate(req, &res);
            if (res.fallback != 0) {
                continue;
            }
            CHECK(bank_text_to_rules(res.text, PUZZLE_DIAL, rules, sizeof rules));
            t.kind = PUZZLE_DIAL;
            t.wire_count = 0;
            t.answer = code;
            t.level = LEVELS[li];
            const Gate g = run_setter(reply_of(res.text, replace_all(rules, " ", "")), t);
            total++;
            accepted += (g.status == GATE_OK) ? 1 : 0;
            CHECK_MSG(g.status == GATE_OK, g.why + " / " + res.text);
            t.level = LEVELS[(li + 1) % 3];                     /* the same reply for another level is refused */
            CHECK(run_setter(reply_of(res.text, replace_all(rules, " ", "")), t).status == GATE_WRONG_LEVEL);
        }
    }
    printf("  gate accepts %d of %d bank puzzles at their own level\n", accepted, total);
}

int main(void)
{
    printf("test_bank: conversion between display text and rules\n");
    test_conversion();
    printf("test_bank: procedural bank (dial: every %dth code, wire: every n x k x %d seeds)\n", DIAL_STEP, WIRE_SEEDS);
    test_dial_bank();
    test_wire_bank();
    test_bank_determinism_and_bounds();
    test_fallback();
    test_bank_set();
    printf("test_bank: the LLM gate accepts what the bank makes\n");
    test_gate_accepts_bank_puzzles();
    printf("test_bank: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
