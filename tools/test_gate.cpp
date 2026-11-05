/*
 * Host-side tests for the LLM acceptance gate (puzzle_gate.cpp) and the text rules it uses (O3 length,
 * O4 wire-number leak). Plain checks, no framework. The gate is fed canned LLM replies for every way
 * a reply can be wrong; the expected reason of each rejection is checked, because that line is what
 * the nightly run logs.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>

#include "puzzle_gate.h"

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

/* ================= text helpers ================= */
static void test_text_rules(void)
{
    CHECK(puzzle_text_has_number("wire 3 of 5", 3));
    CHECK(puzzle_text_has_number("the 3rd wire", 3));
    CHECK(puzzle_text_has_number("2nd from the left", 2));
    CHECK(puzzle_text_has_number("n-3", 3));
    CHECK(puzzle_text_has_number("(3)", 3));
    CHECK(puzzle_text_has_number("w=3;", 3));
    CHECK(puzzle_text_has_number("w*2-1>=3", 3));
    CHECK(!puzzle_text_has_number("w3", 3));                    /* glued to a letter before */
    CHECK(!puzzle_text_has_number("3m long", 3));               /* glued to a letter after */
    CHECK(!puzzle_text_has_number("the 13th wire", 3));         /* a different number */
    CHECK(!puzzle_text_has_number("wire 30", 3));
    CHECK(!puzzle_text_has_number("wire 03x", 3));
    CHECK(puzzle_text_has_number("wire 03", 3));                /* leading zero: still the number 3 */
    CHECK(!puzzle_text_has_number("third wire of five", 3));    /* words are not tokens (O4 is about digits) */
    CHECK(!puzzle_text_has_number("", 3));
    CHECK(!puzzle_text_has_number(NULL, 3));
    CHECK(!puzzle_text_has_number("99999999999999999999", 3));  /* saturates, no overflow */

    CHECK(puzzle_text_problem("Cut wire w of n: w=n-2", PUZZLE_WIRE, "3") == PTP_OK);
    CHECK(puzzle_text_problem("Cut wire w of n: w=3", PUZZLE_WIRE, "3") == PTP_LEAKS_NUMBER);
    CHECK(puzzle_text_problem("short", PUZZLE_WIRE, "3") == PTP_TOO_SHORT);
    CHECK(puzzle_text_problem("0123456789012345678901234567890123456789012", PUZZLE_WIRE, "9") == PTP_TOO_LONG);   /* 43 */
    CHECK(puzzle_text_problem("0123456789012345678901234567890123456789012", PUZZLE_DIAL, "9999") == PTP_OK);      /* 43 <= 63 */
    CHECK(puzzle_text_problem("caf\xC3\xA9 not ascii here", PUZZLE_WIRE, "3") == PTP_BAD_CHAR);
    CHECK(puzzle_text_problem("The code is 4271 exactly", PUZZLE_DIAL, "4271") == PTP_LEAKS_CODE);
    CHECK(puzzle_text_problem("Digits 4 2 7 1 in that order, ok", PUZZLE_DIAL, "4271") == PTP_LEAKS_DIGITS);
    CHECK(puzzle_text_problem(NULL, PUZZLE_DIAL, "4271") == PTP_ARGS);
    CHECK(strcmp(puzzle_text_problem_name(PTP_LEAKS_NUMBER), "states the wire number") == 0);
    CHECK(strcmp(puzzle_text_problem_name(PTP_TOO_LONG), "too long") == 0);
    CHECK(strcmp(puzzle_text_problem_name(99), "?") == 0);
}

/* ================= the acceptance gate with canned LLM replies ================= */
struct Gate {
    int status;
    std::string text;
    std::string why;
};

static Gate run_setter(const std::string &reply, const GateTarget &t)
{
    char json[2048];
    char text[PUZZLE_TEXT_BUF];
    char why[GATE_WHY_MAX];
    Gate g;
    g.status = gate_check_setter(reply.c_str(), t, json, sizeof json, text, sizeof text, why, sizeof why);
    g.text = text;
    g.why = why;
    return g;
}

static std::string reply_of(const std::string &puzzle, const std::string &rules)
{
    return "{\"puzzle\":\"" + puzzle + "\",\"rules\":\"" + rules + "\"}";
}

static bool has(const std::string &s, const char *part) { return s.find(part) != std::string::npos; }

static void test_gate_setter_dial(void)
{
    GateTarget t;
    t.kind = PUZZLE_DIAL;
    t.wire_count = 0;
    t.answer = 5484;
    t.level = PR_NORMAL;
    const std::string good_text = "1st+2nd=9; 1st-2nd=1; 3rd=2*4th; 3rd+4th=12";
    const std::string good_rules = "d1+d2=9;d1-d2=1;d3=2*d4;d3+d4=12";

    Gate g = run_setter(reply_of(good_text, good_rules), t);
    CHECK(g.status == GATE_OK && g.text == good_text && g.why == "accepted");
    /* prose, a code fence and a think block around the JSON are tolerated */
    g = run_setter("<think>hmm</think>Here you go:\n```json\n" + reply_of(good_text, good_rules) + "\n```", t);
    CHECK(g.status == GATE_OK && g.text == good_text);

    /* rules ambiguous: the exact solution count is in the reason */
    g = run_setter(reply_of(good_text, "d1+d2=9;d1-d2=1"), t);
    CHECK(g.status == GATE_RULES_AMBIGUOUS && has(g.why, "rules ambiguous (100 solutions)") && g.text.empty());
    /* a cheap check that fails first hides the later ones: ambiguous rules with a leaking text report the text */
    g = run_setter(reply_of("code is 5484, ok", "d1+d2=9;d1-d2=1"), t);
    CHECK(g.status == GATE_TEXT_REJECTED);

    /* rules with a wrong answer: unique, but not our code */
    g = run_setter(reply_of(good_text, "d1+d2=9;d1-d2=1;d3=2*d4;d3+d4=12;d4=5"), t);
    CHECK(g.status == GATE_RULES_UNSOLVABLE && has(g.why, "no solution"));
    g = run_setter(reply_of(good_text, "d1=5;d2=4;d3=8;d4=5"), t);
    CHECK(g.status == GATE_RULES_WRONG_ANSWER && has(g.why, "rules give 5485, expected 5484"));
    g = run_setter(reply_of(good_text, "d1=0;d2=4;d3=2;d4=1"), t);
    CHECK(g.status == GATE_RULES_WRONG_ANSWER && has(g.why, "rules give 0421, expected 5484"));      /* leading zero kept */

    /* wrong level: pins only = EASY, asked NORMAL */
    g = run_setter(reply_of("1st is five, 2nd is four, 3rd eight, 4th four-ish", "d1=5;d2=4;d3=8;d4=4"), t);
    CHECK(g.status == GATE_WRONG_LEVEL && has(g.why, "level EASY != NORMAL"));
    t.level = PR_EASY;
    g = run_setter(reply_of(good_text, good_rules), t);
    CHECK(g.status == GATE_WRONG_LEVEL && has(g.why, "level NORMAL != EASY"));
    t.level = PR_HARD;
    g = run_setter(reply_of(good_text, good_rules), t);
    CHECK(g.status == GATE_WRONG_LEVEL && has(g.why, "level NORMAL != HARD"));
    t.level = PR_NORMAL;

    /* text problems: leaks the code, spells it, too long, too short, not ASCII */
    g = run_setter(reply_of("the code is 5484, trust me", good_rules), t);
    CHECK(g.status == GATE_TEXT_REJECTED && has(g.why, "prints the code"));
    g = run_setter(reply_of("digits 5 4 8 4 in order, ok", good_rules), t);
    CHECK(g.status == GATE_TEXT_REJECTED && has(g.why, "spells the code"));
    g = run_setter(reply_of(std::string(64, 'a'), good_rules), t);
    CHECK(g.status == GATE_TEXT_REJECTED && has(g.why, "text too long (64 > 63)"));
    g = run_setter(reply_of(std::string(63, 'a'), good_rules), t);
    CHECK(g.status == GATE_OK);                                 /* exactly the limit: the rules decide, text is fine */
    g = run_setter(reply_of(std::string(150, 'a'), good_rules), t);
    CHECK(g.status == GATE_TEXT_REJECTED && has(g.why, "too long (150 > 63)"));
    g = run_setter(reply_of(std::string(260, 'a'), good_rules), t);
    CHECK(g.status == GATE_NO_PUZZLE);                          /* does not even fit the scratch buffer */
    g = run_setter(reply_of("tiny", good_rules), t);
    CHECK(g.status == GATE_TEXT_REJECTED && has(g.why, "too short"));
    g = run_setter(reply_of("caf\xC3\xA9 is not ascii at all", good_rules), t);
    CHECK(g.status == GATE_TEXT_REJECTED && has(g.why, "not plain ASCII"));

    /* OLED rows: <= 63 characters is not enough when the words wrap past 3 rows of 21 */
    g = run_setter(reply_of("abcdefghijkl abcdefghijkl abcdefghijkl abcdefghijkl", good_rules), t);   /* 51 chars, 4 rows */
    CHECK(g.status == GATE_TEXT_REJECTED && has(g.why, "text wraps past 3 OLED rows"));
    g = run_setter(reply_of("abcdefghijkl abcdefghijkl abcdefghijkl", good_rules), t);                 /* 3 rows */
    CHECK(g.status == GATE_OK);
    CHECK(puzzle_text_fits_oled("1st+2nd=9; 1st-2nd=1; 3rd=2*4th; 3rd+4th=12", PUZZLE_DIAL));
    CHECK(!puzzle_text_fits_oled(NULL, PUZZLE_DIAL));

    /* malformed replies */
    g = run_setter("I cannot do that.", t);
    CHECK(g.status == GATE_NO_JSON && has(g.why, "no JSON"));
    g = run_setter("{\"puzzle\":\"1st=5; 2nd=1st-1\",\"rules\":\"d1=5", t);
    CHECK(g.status == GATE_NO_JSON);
    g = run_setter("", t);
    CHECK(g.status == GATE_NO_JSON);
    g = run_setter("{\"rules\":\"d1=5\"}", t);
    CHECK(g.status == GATE_NO_PUZZLE);
    g = run_setter("{\"puzzle\":\"1st=5; 2nd=1st-1\"}", t);
    CHECK(g.status == GATE_NO_RULES);
    g = run_setter("{\"puzzle\":42,\"rules\":\"d1=5\"}", t);
    CHECK(g.status == GATE_TEXT_REJECTED || g.status == GATE_NO_PUZZLE);        /* a bare token is read as text "42": too short */
    g = run_setter(reply_of(good_text, std::string(400, 'x')), t);
    CHECK(g.status == GATE_NO_RULES);
    g = run_setter(reply_of(good_text, "d1=5;d2=4;;"), t);
    CHECK(g.status == GATE_RULES_SYNTAX && has(g.why, "rules do not parse (PR_ERR_SYNTAX)"));
    g = run_setter(reply_of(good_text, "w=5"), t);
    CHECK(g.status == GATE_RULES_SYNTAX && has(g.why, "PR_ERR_TERM_KIND"));      /* a wire term in a dial puzzle */
    g = run_setter(reply_of(good_text, "d1=500"), t);
    CHECK(g.status == GATE_RULES_SYNTAX && has(g.why, "PR_ERR_NUMBER"));
    g = run_setter(reply_of(good_text, "d1=1;d2=2;d3=3;d4=4;alldiff;sum=10;d1=1"), t);
    CHECK(g.status == GATE_RULES_SYNTAX && has(g.why, "TOO_MANY_RULES"));
    g = run_setter(reply_of(good_text, "sum>36"), t);
    CHECK(g.status == GATE_RULES_UNSOLVABLE);
    CHECK(gate_check_setter(NULL, t, NULL, 0, NULL, 0, NULL, 0) == GATE_NO_JSON);      /* NULLs never crash */
}

static void test_gate_setter_wire(void)
{
    GateTarget t;
    t.kind = PUZZLE_WIRE;
    t.wire_count = 5;
    t.answer = 5;
    t.level = PR_NORMAL;
    const std::string good_text = "Cut wire w of n: odd(w); w>3";
    Gate g = run_setter(reply_of(good_text, "odd(w);w>3"), t);

    CHECK(g.status == GATE_OK && g.text == good_text);
    g = run_setter(reply_of(good_text, "w>3"), t);
    CHECK(g.status == GATE_RULES_AMBIGUOUS && has(g.why, "(2 solutions)"));
    g = run_setter(reply_of(good_text, "odd(w);w>3;w<5"), t);
    CHECK(g.status == GATE_RULES_UNSOLVABLE);
    g = run_setter(reply_of("Cut wire w of n: even(w); w>3", "even(w);w>3"), t);
    CHECK(g.status == GATE_RULES_WRONG_ANSWER && has(g.why, "rules give 4, expected 5"));
    g = run_setter(reply_of("Cut wire w of n: w=n-2", "w=n-2"), t);
    CHECK(g.status == GATE_RULES_WRONG_ANSWER);
    g = run_setter(reply_of("Cut wire w of n: w=n", "w=n"), t);
    CHECK(g.status == GATE_WRONG_LEVEL && has(g.why, "level EASY != NORMAL"));
    t.level = PR_EASY;
    g = run_setter(reply_of("Cut wire w of n: w=n", "w=n"), t);
    CHECK(g.status == GATE_OK);
    t.level = PR_HARD;
    g = run_setter(reply_of("Cut wire w of n: w=n", "w=n"), t);
    CHECK(g.status == GATE_WRONG_LEVEL && has(g.why, "level EASY != HARD"));
    t.level = PR_NORMAL;

    /* O4: a standalone number token equal to the answer; ordinals count */
    g = run_setter(reply_of("Cut wire w of n: w=5 only", "odd(w);w>3"), t);
    CHECK(g.status == GATE_TEXT_REJECTED && has(g.why, "states the wire number"));
    g = run_setter(reply_of("Cut the 5th wire, odd and last", "odd(w);w>3"), t);
    CHECK(g.status == GATE_TEXT_REJECTED && has(g.why, "states the wire number"));
    g = run_setter(reply_of("Odd wire, more than 3, of 5 wires", "odd(w);w>3"), t);
    CHECK(g.status == GATE_TEXT_REJECTED);                      /* "of 5 wires" is the answer 5 as a token */
    g = run_setter(reply_of("Cut the odd wire right of the 3rd", "odd(w);w>3"), t);
    CHECK(g.status == GATE_OK);                                 /* 3rd is not the answer (5) */
    /* O3 for wires: 42 characters */
    g = run_setter(reply_of(std::string(43, 'b'), "odd(w);w>3"), t);
    CHECK(g.status == GATE_TEXT_REJECTED && has(g.why, "text too long (43 > 42)"));
    g = run_setter(reply_of(std::string(42, 'b'), "odd(w);w>3"), t);
    CHECK(g.status == GATE_OK);
    /* wires get 2 rows: 3 words of 13 characters (41 total) wrap to 3 rows */
    g = run_setter(reply_of("abcdefghijklm abcdefghijklm abcdefghijklm", "odd(w);w>3"), t);
    CHECK(g.status == GATE_TEXT_REJECTED && has(g.why, "text wraps past 2 OLED rows"));
    g = run_setter(reply_of("abcdefghijklm abcdefghijklm", "odd(w);w>3"), t);
    CHECK(g.status == GATE_OK);
    CHECK(puzzle_text_fits_oled("Cut wire w of n: odd(w); w>3", PUZZLE_WIRE));
    CHECK(!puzzle_text_fits_oled("abcdefghijklm abcdefghijklm abcdefghijklm", PUZZLE_WIRE));
    /* wire terms only */
    g = run_setter(reply_of(good_text, "d1=5"), t);
    CHECK(g.status == GATE_RULES_SYNTAX && has(g.why, "PR_ERR_TERM_KIND"));
}

/* The rewrite-only gate: the facts came from the ESP32, the reply only has to be a clean riddle. */
static Gate run_phrase(const std::string &reply, const GateTarget &t)
{
    char json[2048];
    char text[PUZZLE_TEXT_BUF];
    char why[GATE_WHY_MAX];
    Gate g;
    g.status = gate_check_phrase(reply.c_str(), t, json, sizeof json, text, sizeof text, why, sizeof why);
    g.text = text;
    g.why = why;
    return g;
}

static std::string phrase_of(const std::string &puzzle)
{
    return "{\"puzzle\":\"" + puzzle + "\"}";
}

static void test_gate_phrase(void)
{
    GateTarget t;
    t.kind = PUZZLE_DIAL;
    t.wire_count = 0;
    t.answer = 5484;
    t.level = PR_HARD;
    const std::string good = "Sum 26; 4th-1st=-1; 2nd=4th";

    Gate g = run_phrase(phrase_of(good), t);
    CHECK(g.status == GATE_OK && g.text == good && g.why == "accepted");
    g = run_phrase("<think>ok</think>```json\n" + phrase_of(good) + "\n```", t);     /* wrappers tolerated */
    CHECK(g.status == GATE_OK && g.text == good);
    g = run_phrase("{\"puzzle\":\"" + good + "\",\"rules\":\"ignored\"}", t);         /* extra fields are ignored */
    CHECK(g.status == GATE_OK && g.text == good);

    g = run_phrase("I cannot do that", t);
    CHECK(g.status == GATE_NO_JSON && g.text.empty() && has(g.why, "no JSON object"));
    g = run_phrase("{\"riddle\":\"" + good + "\"}", t);
    CHECK(g.status == GATE_NO_PUZZLE && has(g.why, "no \"puzzle\" string"));
    g = run_phrase("{\"puzzle\":42}", t);
    CHECK(g.status == GATE_NO_PUZZLE || g.status == GATE_TEXT_REJECTED);       /* a bare token is read as text "42": too short */
    g = run_phrase(phrase_of(std::string(64, 'b')), t);                            /* dial limit is 63 */
    CHECK(g.status == GATE_TEXT_REJECTED && has(g.why, "text too long (64 > 63)") && g.text.empty());
    g = run_phrase(phrase_of("The code is 5484, enter it"), t);
    CHECK(g.status == GATE_TEXT_REJECTED && g.text.empty());                       /* leaked code */
    g = run_phrase(phrase_of("Digits 5 4 8 4 in that order, ok"), t);
    CHECK(g.status == GATE_TEXT_REJECTED);                                         /* leaked digits */
    g = run_phrase(phrase_of("caf\u00e9 sum is large and then some"), t);
    CHECK(g.status == GATE_TEXT_REJECTED);                                         /* not printable ASCII */
    g = run_phrase(phrase_of("abcdefghijklmnopqrstu abcdefghijklmnopqrstu abcdefghijklmnopqrstu"), t);
    CHECK(g.status == GATE_TEXT_REJECTED);                                         /* 65 chars: too long */
    g = run_phrase(phrase_of("abcdefghijklmn abcdefghijklmn abcdefghijklmn abcdefghijklmn"), t);   /* 4 rows */
    CHECK(g.status == GATE_TEXT_REJECTED && has(g.why, "text wraps past 3 OLED rows"));

    t.kind = PUZZLE_WIRE;
    t.wire_count = 5;
    t.answer = 5;
    t.level = PR_NORMAL;
    g = run_phrase(phrase_of("Cut the odd wire right of the 3rd"), t);
    CHECK(g.status == GATE_OK && g.text == "Cut the odd wire right of the 3rd");
    g = run_phrase(phrase_of("Cut the 5th wire, odd and last"), t);
    CHECK(g.status == GATE_TEXT_REJECTED && has(g.why, "states the wire number"));   /* leaked wire number */
    g = run_phrase(phrase_of(std::string(43, 'b')), t);
    CHECK(g.status == GATE_TEXT_REJECTED && has(g.why, "text too long (43 > 42)"));
    g = run_phrase(phrase_of("abcdefghijklm abcdefghijklm abcdefghijklm"), t);
    CHECK(g.status == GATE_TEXT_REJECTED && has(g.why, "text wraps past 2 OLED rows"));
    CHECK(gate_check_phrase(NULL, t, NULL, 0, NULL, 0, NULL, 0) == GATE_NO_JSON);   /* NULLs never crash */
}

static void test_gate_checker(void)
{
    char json[1024];
    char said[PUZZLE_ANSWER_LEN];
    char why[GATE_WHY_MAX];
    GateTarget t;

    t.kind = PUZZLE_DIAL;
    t.wire_count = 0;
    t.answer = 5484;
    t.level = PR_NORMAL;
    CHECK(gate_check_checker("{\"answer\":\"5484\"}", t, "qwen3.6:35b", "x-test-1", json, sizeof json, said, sizeof said,
                             why, sizeof why) == GATE_OK);
    CHECK(strcmp(said, "5484") == 0 && has(why, "x-test-1 agrees"));
    CHECK(gate_check_checker("<think>so</think>```json\n{\"answer\": \" 5484 \"}\n```", t, "qwen3.6:35b", "x-test-1", json,
                             sizeof json, said, sizeof said, why, sizeof why) == GATE_OK);

    /* checker disagrees */
    CHECK(gate_check_checker("{\"answer\":\"5485\"}", t, "qwen3.6:35b", "x-test-1", json, sizeof json, said, sizeof said,
                             why, sizeof why) == GATE_CHECKER_DISAGREES);
    CHECK(strcmp(said, "5485") == 0 && has(why, "x-test-1 answered 5485, expected 5484"));
    /* the answer's leading zeros matter */
    t.answer = 421;
    CHECK(gate_check_checker("{\"answer\":\"0421\"}", t, "a", "b", json, sizeof json, said, sizeof said, why, sizeof why) ==
          GATE_OK);
    CHECK(gate_check_checker("{\"answer\":\"421\"}", t, "a", "b", json, sizeof json, said, sizeof said, why, sizeof why) ==
          GATE_CHECKER_UNUSABLE);                               /* not 4 digits */
    t.answer = 5484;

    /* the checker is the setter's model (O5): refused even when its answer is right, and no answer is kept */
    CHECK(gate_check_checker("{\"answer\":\"5484\"}", t, "x-test-1", "x-test-1", json, sizeof json, said, sizeof said, why,
                             sizeof why) == GATE_CHECKER_SAME_MODEL);
    CHECK(said[0] == '\0' && has(why, "is the setter's model"));
    CHECK(gate_check_checker("{\"answer\":\"5484\"}", t, NULL, "x-test-1", json, sizeof json, said, sizeof said, why,
                             sizeof why) == GATE_CHECKER_SAME_MODEL);                /* unknown setter: no self-check either */
    CHECK(gate_check_checker("{\"answer\":\"5484\"}", t, "a", NULL, json, sizeof json, said, sizeof said, why, sizeof why) ==
          GATE_CHECKER_SAME_MODEL);
    CHECK(gate_check_checker("{\"answer\":\"5484\"}", t, "X-TEST-1", "x-test-1", json, sizeof json, said, sizeof said, why,
                             sizeof why) == GATE_OK);                                 /* IDs are compared exactly */

    /* unusable replies */
    CHECK(gate_check_checker("no idea", t, "a", "b", json, sizeof json, said, sizeof said, why, sizeof why) ==
          GATE_CHECKER_UNUSABLE);
    CHECK(gate_check_checker("{\"answer\":\"abcd\"}", t, "a", "b", json, sizeof json, said, sizeof said, why, sizeof why) ==
          GATE_CHECKER_UNUSABLE);
    CHECK(gate_check_checker("{\"answer\":\"12345\"}", t, "a", "b", json, sizeof json, said, sizeof said, why, sizeof why) ==
          GATE_CHECKER_UNUSABLE);
    CHECK(gate_check_checker("{\"result\":\"5484\"}", t, "a", "b", json, sizeof json, said, sizeof said, why, sizeof why) ==
          GATE_CHECKER_UNUSABLE);
    CHECK(gate_check_checker(NULL, t, "a", "b", json, sizeof json, said, sizeof said, why, sizeof why) ==
          GATE_CHECKER_UNUSABLE);

    /* wire */
    t.kind = PUZZLE_WIRE;
    t.wire_count = 5;
    t.answer = 4;
    CHECK(gate_check_checker("{\"answer\":\"4\"}", t, "a", "b", json, sizeof json, said, sizeof said, why, sizeof why) == GATE_OK);
    CHECK(gate_check_checker("{\"answer\":\"04\"}", t, "a", "b", json, sizeof json, said, sizeof said, why, sizeof why) == GATE_OK);
    CHECK(gate_check_checker("{\"answer\":\"3\"}", t, "a", "b", json, sizeof json, said, sizeof said, why, sizeof why) ==
          GATE_CHECKER_DISAGREES);
    CHECK(gate_check_checker("{\"answer\":\"6\"}", t, "a", "b", json, sizeof json, said, sizeof said, why, sizeof why) ==
          GATE_CHECKER_UNUSABLE);                               /* beyond the wire count */
    char buf[8];
    gate_answer_text(t, buf, sizeof buf);
    CHECK(strcmp(buf, "4") == 0);
}

int main(void)
{
    printf("test_gate: text rules (O3, O4)\n");
    test_text_rules();
    printf("test_gate: acceptance gate with canned replies\n");
    test_gate_setter_dial();
    test_gate_setter_wire();
    test_gate_phrase();
    test_gate_checker();
    printf("test_gate: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
