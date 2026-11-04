/*
 * Host-side tests for esp32/defuse_esp32/src/app/puzzle_logic.*. Plain checks, no framework.
 *
 * The template bank is the safety net of the whole puzzle pipeline, so its tests do not trust the
 * generator: they solve the rendered clue TEXT with their own parser and brute force (every
 * 4-digit code, every wire k of n), and evaluate the clue STRUCT with their own rule evaluator.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "puzzle_logic.h"

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

#define VARIANTS 16u
#define CODE_SPACE 10000

static void digits_of(int v, uint8_t d[4])
{
    d[0] = (uint8_t)(v / 1000);
    d[1] = (uint8_t)((v / 100) % 10);
    d[2] = (uint8_t)((v / 10) % 10);
    d[3] = (uint8_t)(v % 10);
}

/* ================= text helpers ================= */
static void test_strip_think(void)
{
    char out[64];

    CHECK(strip_think("plain", out, sizeof(out)) == 5 && strcmp(out, "plain") == 0);
    CHECK(strcmp((strip_think("<think>a</think>X", out, sizeof(out)), out), "X") == 0);
    CHECK(strcmp((strip_think("A<think>a</think>B<think>b</think>C", out, sizeof(out)), out), "ABC") == 0);
    CHECK(strcmp((strip_think("reasoning</think>Y", out, sizeof(out)), out), "Y") == 0);
    CHECK(strcmp((strip_think("r1</think>r2</think>Z", out, sizeof(out)), out), "Z") == 0);
    CHECK(strcmp((strip_think("keep<think>never closed", out, sizeof(out)), out), "keep") == 0);
    CHECK(strcmp((strip_think("0123456789", out, 5), out), "0123") == 0);       /* truncation */
    CHECK(strip_think(NULL, out, sizeof(out)) == 0 && out[0] == '\0');
    CHECK(strip_think("x", NULL, 4) == 0);
    CHECK(strip_think("x", out, 0) == 0);
}

static void test_extract_json(void)
{
    char out[128];

    CHECK(extract_json_object("{\"a\":1}", out, sizeof(out)) && strcmp(out, "{\"a\":1}") == 0);
    CHECK(extract_json_object("<think>maybe {\"x\":1} or not</think>\n{\"puzzle\":\"y\"}", out, sizeof(out)) &&
          strcmp(out, "{\"puzzle\":\"y\"}") == 0);
    CHECK(extract_json_object("Sure! ```json\n{\"answer\":\"4271\"}\n``` hope that helps", out, sizeof(out)) &&
          strcmp(out, "{\"answer\":\"4271\"}") == 0);
    CHECK(extract_json_object("thinking...</think>{\"answer\":\"2\"}", out, sizeof(out)) &&
          strcmp(out, "{\"answer\":\"2\"}") == 0);
    CHECK(extract_json_object("{\"a\":{\"b\":1},\"c\":\"}\"}", out, sizeof(out)) &&
          strcmp(out, "{\"a\":{\"b\":1},\"c\":\"}\"}") == 0);
    CHECK(extract_json_object("{\"p\":\"a } \\\" b\"} tail", out, sizeof(out)) && strcmp(out, "{\"p\":\"a } \\\" b\"}") == 0);
    CHECK(extract_json_object("{not json} {\"a\":1}", out, sizeof(out)) && strcmp(out, "{not json}") == 0);
    CHECK(!extract_json_object("<think>abc {\"x\":1}", out, sizeof(out)));          /* unterminated think */
    CHECK(!extract_json_object("no braces here", out, sizeof(out)));
    CHECK(!extract_json_object("{\"a\":1", out, sizeof(out)));                       /* truncated reply */
    CHECK(!extract_json_object("{\"a\":\"unclosed}", out, sizeof(out)));
    CHECK(!extract_json_object("{\"a\":1}", out, 5));                                /* does not fit */
    CHECK(!extract_json_object(NULL, out, sizeof(out)));
    CHECK(!extract_json_object("{}", NULL, 4));
    CHECK(extract_json_object("{}", out, sizeof(out)) && strcmp(out, "{}") == 0);
}

static void test_json_get_string(void)
{
    char out[32];

    CHECK(json_get_string("{\"puzzle\":\"hello\"}", "puzzle", out, sizeof(out)) && strcmp(out, "hello") == 0);
    CHECK(json_get_string("  { \"a\" : \"b\" , \"puzzle\" : \"x y\" }", "puzzle", out, sizeof(out)) && strcmp(out, "x y") == 0);
    CHECK(json_get_string("{\"answer\":4271}", "answer", out, sizeof(out)) && strcmp(out, "4271") == 0);
    CHECK(json_get_string("{\"answer\":3}", "answer", out, sizeof(out)) && strcmp(out, "3") == 0);
    CHECK(json_get_string("{\"p\":\"a\\\"b\\\\c\\/d\"}", "p", out, sizeof(out)) && strcmp(out, "a\"b\\c/d") == 0);
    CHECK(json_get_string("{\"p\":\"\\u0041\\u007e\"}", "p", out, sizeof(out)) && strcmp(out, "A~") == 0);
    CHECK(json_get_string("{\"p\":\"a\\nb\"}", "p", out, sizeof(out)) && strcmp(out, "a\nb") == 0);
    CHECK(!json_get_string("{\"p\":\"caf\\u00e9\"}", "p", out, sizeof(out)));         /* non-ASCII escape */
    CHECK(!json_get_string("{\"p\":\"\\u0000\"}", "p", out, sizeof(out)));
    CHECK(!json_get_string("{\"p\":\"bad\\q\"}", "p", out, sizeof(out)));
    CHECK(!json_get_string("{\"p\":\"x\"}", "q", out, sizeof(out)));                  /* missing key */
    CHECK(!json_get_string("{\"p\":\"unterminated}", "p", out, sizeof(out)));
    CHECK(!json_get_string("[1,2]", "p", out, sizeof(out)));
    CHECK(!json_get_string("{\"p\":\"0123456789\"}", "p", out, 5));                   /* value does not fit */
    CHECK(!json_get_string("{\"p\":123456}", "p", out, 4));
    /* a key name inside a value, or inside a nested object, must not match */
    CHECK(json_get_string("{\"x\":\"\\\"answer\\\":\\\"1\\\"\",\"answer\":\"2\"}", "answer", out, sizeof(out)) &&
          strcmp(out, "2") == 0);
    CHECK(json_get_string("{\"meta\":{\"answer\":\"9\"},\"list\":[{\"answer\":\"8\"}],\"answer\":\"3\"}", "answer", out,
                          sizeof(out)) && strcmp(out, "3") == 0);
    CHECK(!json_get_string(NULL, "p", out, sizeof(out)));
    CHECK(!json_get_string("{\"p\":\"x\"}", "p", NULL, 4));
}

static const char *filler(char *buf, size_t len)
{
    memset(buf, 'a', len);
    buf[len] = '\0';
    return buf;
}

static void test_validate_puzzle(void)
{
    char text[100];

    CHECK(validate_puzzle("1st=4; 2nd=1st-2; sum is nine", PUZZLE_DIAL, "4271"));
    CHECK(!validate_puzzle("The code is 4271 exactly", PUZZLE_DIAL, "4271"));         /* literal answer */
    CHECK(!validate_puzzle("Digits 4 2 7 1 in that order, ok", PUZZLE_DIAL, "4271")); /* spells the code */
    CHECK(!validate_puzzle("Digits are 2,5,8,7 in order.", PUZZLE_DIAL, "2587"));
    CHECK(!validate_puzzle("2=2,5=5,8=8,7=7. Digits are 2,5,8,7 in order.", PUZZLE_DIAL, "2587"));
    CHECK(validate_puzzle("1st=2nd-3; 3rd is 2nd+3; 4th=3rd-1; sum is 22", PUZZLE_DIAL, "2587"));
    CHECK(!validate_puzzle("1st=4; 2nd=1st-2; sum is nine", PUZZLE_DIAL, NULL));
    CHECK(!validate_puzzle("1st=4; 2nd=1st-2; sum is nine", PUZZLE_DIAL, "427"));
    CHECK(!validate_puzzle(NULL, PUZZLE_DIAL, "4271"));
    CHECK(!validate_puzzle("1st=4; 2nd=1st-2; sum is nine", 0, "4271"));
    /* length limits */
    CHECK(!validate_puzzle(filler(text, 9), PUZZLE_DIAL, "4271"));
    CHECK(validate_puzzle(filler(text, 10), PUZZLE_DIAL, "4271"));
    CHECK(validate_puzzle(filler(text, 63), PUZZLE_DIAL, "4271"));     /* O3: 3 OLED rows of 21 */
    CHECK(!validate_puzzle(filler(text, 64), PUZZLE_DIAL, "4271"));
    CHECK(validate_puzzle(filler(text, 42), PUZZLE_WIRE, "3"));         /* O3: 2 OLED rows */
    CHECK(!validate_puzzle(filler(text, 43), PUZZLE_WIRE, "3"));
    CHECK(!validate_puzzle("", PUZZLE_WIRE, "3"));
    /* character set */
    CHECK(!validate_puzzle("caf\xC3\xA9 is not ascii at all", PUZZLE_WIRE, "3"));
    CHECK(!validate_puzzle("line one\nline two here", PUZZLE_WIRE, "3"));
    CHECK(!validate_puzzle("tab\there is bad text", PUZZLE_WIRE, "3"));
    CHECK(!validate_puzzle("del char \x7F in the middle", PUZZLE_WIRE, "3"));
    CHECK(validate_puzzle(" !\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~ ok", PUZZLE_WIRE, "3"));
    /* wire answers may appear in the text (a single digit is not a giveaway) */
    CHECK(validate_puzzle("Cut the third wire of 5 wires", PUZZLE_WIRE, "3"));
    CHECK(validate_puzzle("wire 4 of 5 is the one!", PUZZLE_WIRE, "3"));
    /* O4 (replaces the old "the wire number may appear" rule): a standalone number token equal to the answer is a leak */
    CHECK(!validate_puzzle("wire 3 of 5 is the one!", PUZZLE_WIRE, "3"));
}

static void test_parse_checker_answer(void)
{
    char out[8];

    CHECK(parse_checker_answer("{\"answer\":\"4271\"}", PUZZLE_DIAL, 0, out, sizeof(out)) && strcmp(out, "4271") == 0);
    CHECK(parse_checker_answer("{\"answer\":\"0427\"}", PUZZLE_DIAL, 0, out, sizeof(out)) && strcmp(out, "0427") == 0);
    CHECK(parse_checker_answer("{\"answer\": 4271 }", PUZZLE_DIAL, 0, out, sizeof(out)) && strcmp(out, "4271") == 0);
    CHECK(parse_checker_answer("{\"answer\":\" 4271 \"}", PUZZLE_DIAL, 0, out, sizeof(out)) && strcmp(out, "4271") == 0);
    CHECK(!parse_checker_answer("{\"answer\":\"427\"}", PUZZLE_DIAL, 0, out, sizeof(out)));
    CHECK(!parse_checker_answer("{\"answer\":\"42713\"}", PUZZLE_DIAL, 0, out, sizeof(out)));
    CHECK(!parse_checker_answer("{\"answer\":\"42a1\"}", PUZZLE_DIAL, 0, out, sizeof(out)));
    CHECK(!parse_checker_answer("{\"answer\":\"-427\"}", PUZZLE_DIAL, 0, out, sizeof(out)));
    CHECK(!parse_checker_answer("{\"answer\":\"4 2 7 1\"}", PUZZLE_DIAL, 0, out, sizeof(out)));
    CHECK(!parse_checker_answer("{\"answer\":\"\"}", PUZZLE_DIAL, 0, out, sizeof(out)));
    CHECK(!parse_checker_answer("{\"answer\":\"4271\"}", PUZZLE_DIAL, 0, out, 4));     /* out too small */
    CHECK(!parse_checker_answer("{\"x\":\"4271\"}", PUZZLE_DIAL, 0, out, sizeof(out)));
    CHECK(parse_checker_answer("{\"answer\":\"3\"}", PUZZLE_WIRE, 5, out, sizeof(out)) && strcmp(out, "3") == 0);
    CHECK(parse_checker_answer("{\"answer\":\"03\"}", PUZZLE_WIRE, 5, out, sizeof(out)) && strcmp(out, "3") == 0);
    CHECK(parse_checker_answer("{\"answer\":5}", PUZZLE_WIRE, 5, out, sizeof(out)) && strcmp(out, "5") == 0);
    CHECK(parse_checker_answer("{\"answer\":\"6\"}", PUZZLE_WIRE, 6, out, sizeof(out)) && strcmp(out, "6") == 0);
    CHECK(!parse_checker_answer("{\"answer\":\"6\"}", PUZZLE_WIRE, 5, out, sizeof(out)));
    CHECK(!parse_checker_answer("{\"answer\":\"0\"}", PUZZLE_WIRE, 5, out, sizeof(out)));
    CHECK(!parse_checker_answer("{\"answer\":\"10\"}", PUZZLE_WIRE, 6, out, sizeof(out)));
    CHECK(!parse_checker_answer("{\"answer\":\"wire 3\"}", PUZZLE_WIRE, 5, out, sizeof(out)));
    CHECK(!parse_checker_answer("{\"answer\":\"3\"}", PUZZLE_WIRE, 0, out, sizeof(out)));
    CHECK(!parse_checker_answer("{\"answer\":\"3\"}", 0, 5, out, sizeof(out)));
    CHECK(!parse_checker_answer(NULL, PUZZLE_WIRE, 5, out, sizeof(out)));
}

/* ================= dial templates: independent solver ================= */
enum { P_EQ, P_ADD, P_MUL, P_SUM_ALL, P_SUM_PAIR, P_DIFF_PAIR };

struct Pred {
    int kind;
    int a;
    int b;
    int c;
};

/* "1st".."6th" -> 0..5, restricted to the first `count` names; -1 if it is none of them. */
static int ordinal_index(const char *s, int count)
{
    static const char *const names[] = { "1st", "2nd", "3rd", "4th", "5th", "6th" };
    int i;

    for (i = 0; i < count && i < 6; i++) {
        if (strncmp(s, names[i], 3) == 0) {
            return i;
        }
    }
    return -1;
}

/* Parses the rendered clue text ("1st=3; 2nd=1st+4; ...") into predicates. False on any token it
 * does not understand, so a renderer that drifts from the grammar fails the tests loudly. */
static bool parse_dial_text(const char *text, Pred *preds, int *count)
{
    char buf[128];
    char *tok;
    char *end;
    int n = 0;

    if (strlen(text) >= sizeof(buf)) {
        return false;
    }
    strcpy(buf, text);
    for (tok = buf; *tok != '\0'; tok = end) {
        Pred p;
        int a;
        int b;
        size_t len;

        end = strchr(tok, ';');                                     /* split "a; b; c" without strtok */
        if (end != NULL) {
            *end++ = '\0';
        } else {
            end = tok + strlen(tok);
        }
        while (*tok == ' ') {
            tok++;
        }
        len = strlen(tok);
        if (n >= 8) {
            return false;
        }
        if (strncmp(tok, "Sum ", 4) == 0) {
            p.kind = P_SUM_ALL;
            p.a = p.b = 0;
            p.c = atoi(tok + 4);
        } else if ((a = ordinal_index(tok, 4)) < 0 || len < 5) {
            return false;
        } else if (tok[3] == '+' || tok[3] == '-') {                /* "1st+2nd=13", "1st-2nd=5" */
            b = ordinal_index(tok + 4, 4);
            if (b < 0 || tok[7] != '=') {
                return false;
            }
            p.kind = (tok[3] == '+') ? P_SUM_PAIR : P_DIFF_PAIR;
            p.a = a;
            p.b = b;
            p.c = atoi(tok + 8);
        } else if (tok[3] == '=') {
            if (len == 5) {                                          /* "1st=3" */
                p.kind = P_EQ;
                p.a = a;
                p.b = 0;
                p.c = tok[4] - '0';
            } else {
                b = ordinal_index(tok + 4, 4);
                if (b < 0) {
                    return false;
                }
                p.a = a;
                p.b = b;
                if (len == 7) {                                      /* "2nd=1st" */
                    p.kind = P_ADD;
                    p.c = 0;
                } else if (tok[7] == '+' || tok[7] == '-') {         /* "2nd=1st+4" */
                    p.kind = P_ADD;
                    p.c = (tok[7] == '+' ? 1 : -1) * atoi(tok + 8);
                } else if (tok[7] == '*') {
                    p.kind = P_MUL;
                    p.c = atoi(tok + 8);
                } else {
                    return false;
                }
            }
        } else {
            return false;
        }
        preds[n++] = p;
    }
    *count = n;
    return n > 0;
}

static bool pred_holds(const Pred &p, const int d[4])
{
    switch (p.kind) {
    case P_EQ:        return d[p.a] == p.c;
    case P_ADD:       return d[p.a] == d[p.b] + p.c;
    case P_MUL:       return d[p.a] == d[p.b] * p.c;
    case P_SUM_ALL:   return d[0] + d[1] + d[2] + d[3] == p.c;
    case P_SUM_PAIR:  return d[p.a] + d[p.b] == p.c;
    case P_DIFF_PAIR: return d[p.a] - d[p.b] == p.c;
    default:          return false;
    }
}

/* Brute force over every 4-digit code: how many satisfy all predicates (first one in *first). */
static int count_text_solutions(const Pred *preds, int n, int *first)
{
    int found = 0;
    int v;
    int i;
    int d[4];

    for (v = 0; v < CODE_SPACE; v++) {
        d[0] = v / 1000;
        d[1] = (v / 100) % 10;
        d[2] = (v / 10) % 10;
        d[3] = v % 10;
        for (i = 0; i < n && pred_holds(preds[i], d); i++) {
        }
        if (i == n) {
            if (found == 0) {
                *first = v;
            }
            found++;
            if (found > 1) {
                return found;
            }
        }
    }
    return found;
}

/* The same check on the clue struct, written separately from the text parser. */
static bool rule_holds(const DialRule &r, const int d[4])
{
    switch (r.rel) {
    case DREL_EQ:        return d[r.a] == r.c;
    case DREL_ADD:       return d[r.a] == d[r.b] + r.c;
    case DREL_MUL:       return r.c >= 2 && d[r.a] == d[r.b] * r.c;
    case DREL_SUM_ALL:   return d[0] + d[1] + d[2] + d[3] == r.c;
    case DREL_SUM_PAIR:  return d[r.a] + d[r.b] == r.c;
    case DREL_DIFF_PAIR: return r.c >= 0 && d[r.a] - d[r.b] == r.c;
    default:             return false;
    }
}

static int count_struct_solutions(const DialClue &cl, int *first)
{
    int found = 0;
    int v;
    int i;
    int d[4];

    for (v = 0; v < CODE_SPACE; v++) {
        d[0] = v / 1000;
        d[1] = (v / 100) % 10;
        d[2] = (v / 10) % 10;
        d[3] = v % 10;
        for (i = 0; i < cl.n && rule_holds(cl.rule[i], d); i++) {
        }
        if (i == cl.n) {
            if (found == 0) {
                *first = v;
            }
            found++;
            if (found > 1) {
                return found;
            }
        }
    }
    return found;
}

static void test_dial_templates_all_codes(void)
{
    char text[PUZZLE_TEXT_BUF + 8];
    Pred preds[8];
    int npred = 0;
    int v;
    int d_int[4];
    uint8_t code[4];
    unsigned variant;
    int first;
    size_t len;
    unsigned long unique_text = 0;

    for (v = 0; v < CODE_SPACE; v++) {
        digits_of(v, code);
        d_int[0] = code[0];
        d_int[1] = code[1];
        d_int[2] = code[2];
        d_int[3] = code[3];
        for (variant = 0; variant < VARIANTS; variant++) {
            const DialClue cl = dial_clue_build(code, variant);
            int i;
            bool all_hold = true;

            len = dial_template(code, variant, text, sizeof(text));
            CHECK(len > 0 && len <= PUZZLE_DIAL_TEXT_MAX && strlen(text) == len);
            CHECK(cl.n >= 1 && cl.n <= DIAL_MAX_RULES);
            for (i = 0; i < cl.n; i++) {
                all_hold = all_hold && rule_holds(cl.rule[i], d_int);
            }
            CHECK(all_hold);                                            /* struct is true for the code */
            CHECK(parse_dial_text(text, preds, &npred));               /* text parses */
            for (i = 0; i < npred && pred_holds(preds[i], d_int); i++) {
            }
            CHECK(i == npred);                                          /* text is true for the code */
            {
                char digits[5];

                dial_code_text(code, digits);
                CHECK(strstr(text, digits) == NULL);                    /* never prints the code */
                CHECK(validate_puzzle(text, PUZZLE_DIAL, digits));      /* nor spells it in its digits */
            }
            /* Uniqueness by brute force: the text for every code in the 4 base families, and the
             * struct plus text for every 37th code in all 16 variants. */
            if (variant < DIAL_FAMILIES || v % 37 == 0) {
                first = -1;
                CHECK(count_text_solutions(preds, npred, &first) == 1 && first == v);
                unique_text++;
            }
            if (v % 37 == 0) {
                first = -1;
                CHECK(count_struct_solutions(cl, &first) == 1 && first == v);
            }
        }
    }
    printf("  dial: %lu clues solved by brute force\n", unique_text);
}

static void test_dial_template_shapes(void)
{
    static const uint8_t code[4] = { 3, 1, 4, 9 };
    char text[PUZZLE_TEXT_BUF];
    char tiny[8];
    size_t family_seen[DIAL_FAMILIES] = { 0, 0, 0, 0 };
    unsigned variant;

    CHECK(strcmp((dial_template(code, 0, text, sizeof(text)), text), "1st=3; 2nd=1st-2; 3rd=2nd+3; 4th=3rd+5") == 0);
    CHECK(strcmp((dial_template(code, 1, text, sizeof(text)), text), "4th=9; 3rd=4th-5; 2nd=3rd-3; 1st=2nd+2") == 0);
    CHECK(strcmp((dial_template(code, 2, text, sizeof(text)), text), "Sum 17; 2nd=1st-2; 3rd=1st+1; 4th=1st+6") == 0);
    CHECK(strcmp((dial_template(code, 3, text, sizeof(text)), text), "1st+2nd=4; 1st-2nd=2; 3rd+4th=13; 4th-3rd=5") == 0);
    /* multiply is used when it is exact: 2 -> 8 */
    {
        static const uint8_t c2[4] = { 2, 8, 0, 1 };

        CHECK(strcmp((dial_template(c2, 4, text, sizeof(text)), text), "1st=2; 2nd=1st*4; 3rd=2nd-8; 4th=3rd+1") == 0);
    }
    for (variant = 0; variant < VARIANTS; variant++) {
        family_seen[dial_clue_build(code, variant).family]++;
    }
    CHECK(family_seen[0] == 4 && family_seen[1] == 4 && family_seen[2] == 4 && family_seen[3] == 4);
    /* too small a buffer: nothing half-written */
    CHECK(dial_template(code, 0, tiny, sizeof(tiny)) == 0 && tiny[0] == '\0');
    CHECK(dial_template(code, 0, NULL, 4) == 0);
    CHECK(dial_template(code, 0, text, 0) == 0);
}

/* ================= wire templates: independent solver ================= */
static int solve_wire_text(const char *text, int *wires)
{
    char word[8];
    char dir[8];
    unsigned n;
    unsigned x;
    unsigned y;
    char op;

    if (sscanf(text, "%u wires. Cut the %3s from the %7s", &n, word, dir) == 3) {
        const int pos = ordinal_index(word, 6) + 1;             /* 1st..6th */

        if (pos <= 0) {
            return -1;
        }
        *wires = (int)n;
        return strcmp(dir, "left") == 0 ? pos : (strcmp(dir, "right") == 0 ? (int)n - pos + 1 : -1);
    }
    if (sscanf(text, "%u wires, 1 is leftmost. Cut wire %u%c%u", &n, &x, &op, &y) == 4) {
        *wires = (int)n;
        if (op == '+') {
            return (int)(x + y);
        }
        if (op == '-') {
            return (int)x - (int)y;
        }
        if (op == '*') {
            return (int)(x * y);
        }
        return -1;
    }
    if (sscanf(text, "%u wires. Skip %u from %7s cut next", &n, &x, dir) == 3) {
        *wires = (int)n;
        if (strncmp(dir, "left,", 5) == 0) {
            return (int)x + 1;
        }
        if (strncmp(dir, "right,", 6) == 0) {
            return (int)n - (int)x;
        }
    }
    return -1;
}

static int solve_wire_struct(const WireClue &w)
{
    switch (w.family) {
    case 0:  return w.a;
    case 1:  return (int)w.n - (int)w.a + 1;
    case 2:  return w.op == WOP_ADD ? w.a + w.b : (w.op == WOP_SUB ? w.a - w.b : (w.op == WOP_MUL ? w.a * w.b : -1));
    default: return w.from_right ? (int)w.n - (int)w.a : (int)w.a + 1;
    }
}

static void test_wire_templates_all(void)
{
    char text[PUZZLE_TEXT_BUF];
    int n;
    int k;
    unsigned variant;
    unsigned long solved = 0;

    for (n = 3; n <= 6; n++) {
        for (k = 1; k <= n; k++) {
            for (variant = 0; variant < VARIANTS; variant++) {
                const WireClue w = wire_clue_build(k, n, variant);
                int wires = 0;
                size_t len = wire_template(k, n, variant, text, sizeof(text));

                CHECK(len > 0 && len <= PUZZLE_WIRE_TEXT_MAX && strlen(text) == len);
                CHECK(w.n == n && (unsigned)w.family == variant % WIRE_FAMILIES);
                CHECK(solve_wire_struct(w) == k);
                CHECK(solve_wire_text(text, &wires) == k && wires == n);
                CHECK(validate_puzzle(text, PUZZLE_WIRE, "99"));        /* printable, fits; "99": no such wire (the old texts name theirs, see O4) */
                solved++;
            }
        }
    }
    printf("  wire: %lu clues solved\n", solved);
}

static void test_wire_template_shapes(void)
{
    char text[PUZZLE_TEXT_BUF];
    char tiny[8];
    WireClue w;

    CHECK(strcmp((wire_template(3, 5, 0, text, sizeof(text)), text), "5 wires. Cut the 3rd from the left") == 0);
    CHECK(strcmp((wire_template(3, 5, 1, text, sizeof(text)), text), "5 wires. Cut the 3rd from the right") == 0);
    CHECK(strcmp((wire_template(2, 6, 1, text, sizeof(text)), text), "6 wires. Cut the 5th from the right") == 0);
    CHECK(strcmp((wire_template(4, 6, 3, text, sizeof(text)), text), "6 wires. Skip 3 from left, cut next") == 0);
    CHECK(strcmp((wire_template(4, 6, 7, text, sizeof(text)), text), "6 wires. Skip 2 from right, cut next") == 0);
    CHECK(wire_template(3, 5, 0, tiny, sizeof(tiny)) == 0 && tiny[0] == '\0');
    /* out-of-range input is clamped, never produces a wrong or crashing clue */
    w = wire_clue_build(9, 4, 0);
    CHECK(w.a == 4 && w.n == 4);
    w = wire_clue_build(0, 9, 0);
    CHECK(w.n == 6 && w.a == 1);
    w = wire_clue_build(2, 1, 0);
    CHECK(w.n == 3);
}

static void test_bank_puzzles(void)
{
    Puzzle p;
    char ans[8];
    static const uint8_t code[4] = { 0, 4, 2, 7 };

    puzzle_fill_bank_dial(&p, code, 5);
    CHECK(p.kind == PUZZLE_DIAL && p.source == PUZZLE_SRC_BANK && p.verified == 0 && p.attempts == 0);
    CHECK(p.text[0] != '\0' && validate_puzzle(p.text, PUZZLE_DIAL, "0427"));
    CHECK(puzzle_answer_text(p, ans, sizeof(ans)) && strcmp(ans, "0427") == 0);
    CHECK(!puzzle_answer_text(p, ans, 4));
    puzzle_fill_bank_wire(&p, 5, 4, 2);
    CHECK(p.kind == PUZZLE_WIRE && p.wire_count == 5 && p.wire_k == 4 && p.source == PUZZLE_SRC_BANK);
    CHECK(validate_puzzle(p.text, PUZZLE_WIRE, "4"));
    CHECK(p.level == 0);                                                /* the old fill functions do not rate */
    CHECK(puzzle_answer_text(p, ans, sizeof(ans)) && strcmp(ans, "4") == 0);
    memset(&p, 0, sizeof(p));
    CHECK(!puzzle_answer_text(p, ans, sizeof(ans)));                    /* unknown kind */
    CHECK(sizeof(PuzzleSet) < 1500);                                    /* fits one NVS blob comfortably */
}

static void test_bank_set(void)
{
    PuzzleSet s;
    uint32_t rnd[PUZZLE_BANK_WORDS];
    uint32_t seed;
    unsigned i;
    unsigned round;
    char ans[8];

    for (round = 0; round < 500; round++) {
        for (i = 0; i < PUZZLE_BANK_WORDS; i++) {
            seed = (round * 40503u + i * 2654435761u + 12345u);
            rnd[i] = seed * 2246822519u ^ (seed >> 13);
        }
        puzzle_set_fill_bank(&s, rnd);
        CHECK(puzzle_set_valid(s) && s.ai_count == 0 && s.generated_unix == 0);
        for (i = 0; i < PUZZLE_PAIRS; i++) {
            char digits[5];

            CHECK(puzzle_answer_text(s.dial[i], ans, sizeof(ans)));
            dial_code_text(s.dial[i].dial_code, digits);
            CHECK(validate_puzzle(s.dial[i].text, PUZZLE_DIAL, digits));
            CHECK(puzzle_answer_text(s.wire[i], ans, sizeof(ans)));
            CHECK(validate_puzzle(s.wire[i].text, PUZZLE_WIRE, "99"));   /* old wire texts name their wire: O4 applies to LLM and procedural texts */
        }
    }
    /* a damaged set is refused */
    puzzle_set_fill_bank(&s, rnd);
    CHECK(puzzle_set_valid(s));
    s.magic ^= 1u;
    CHECK(!puzzle_set_valid(s));
    s.magic ^= 1u;
    s.dial[2].dial_code[1] = 10;
    CHECK(!puzzle_set_valid(s));
    s.dial[2].dial_code[1] = 3;
    s.wire[1].wire_k = (uint8_t)(s.wire[1].wire_count + 1);
    CHECK(!puzzle_set_valid(s));
    s.wire[1].wire_k = 1;
    memset(s.wire[3].text, 'x', sizeof(s.wire[3].text));                 /* no terminator */
    CHECK(!puzzle_set_valid(s));
    puzzle_set_fill_bank(&s, rnd);
    s.dial[0].kind = PUZZLE_WIRE;
    CHECK(!puzzle_set_valid(s));
}

static void test_random_picks(void)
{
    uint8_t code[4];
    int n;
    int k;
    uint32_t r;
    int seen_n[7] = { 0 };

    dial_code_from_random(0, code);
    CHECK(code[0] == 0 && code[1] == 0 && code[2] == 0 && code[3] == 0);
    dial_code_from_random(427, code);
    CHECK(code[0] == 0 && code[1] == 4 && code[2] == 2 && code[3] == 7);
    dial_code_from_random(9999u + 10000u * 12345u, code);
    CHECK(code[0] == 9 && code[1] == 9 && code[2] == 9 && code[3] == 9);
    for (r = 1; r < 200000u; r += 7919u) {
        dial_code_from_random(r * 2654435761u, code);
        CHECK(code[0] < 10 && code[1] < 10 && code[2] < 10 && code[3] < 10);
        wire_pick_from_random(PR_EASY, r * 2654435761u, &n, &k);
        CHECK(n >= 3 && n <= 4 && k >= 1 && k <= n);
        seen_n[n]++;
        wire_pick_from_random(PR_NORMAL, r * 2654435761u, &n, &k);
        CHECK(n >= 4 && n <= 5 && k >= 1 && k <= n);
        seen_n[n]++;
        wire_pick_from_random(PR_HARD, r * 2654435761u, &n, &k);
        CHECK(n == 6 && k >= 1 && k <= n);
        seen_n[n]++;
    }
    CHECK(seen_n[3] > 0 && seen_n[4] > 0 && seen_n[5] > 0 && seen_n[6] > 0);
}

/* ================= BOMB_CONFIG ================= */
static void check_config_ranges(const proto_bomb_config_t &c)
{
    unsigned i;

    CHECK(c.module_count >= GAME_MIN_MODULES && c.module_count <= PROTO_MAX_MODULES);
    for (i = 0; i < c.module_count; i++) {
        CHECK(c.module_order[i] >= GAME_MODULE_TYPE_MIN && c.module_order[i] <= GAME_MODULE_TYPE_MAX);
    }
    for (i = 0; i < PROTO_DIAL_DIGITS; i++) {
        CHECK(c.dial_code[i] < GAME_DIAL_BASE);
    }
    CHECK(c.time_limit_s >= GAME_MIN_TIME_LIMIT_S && c.time_limit_s <= GAME_MAX_TIME_LIMIT_S);
    CHECK(c.wire_count >= GAME_MIN_WIRES && c.wire_count <= GAME_MAX_WIRES);
    CHECK(c.correct_wire < c.wire_count);
    CHECK(c.trip_min_cm < c.trip_max_cm);
    CHECK(c.trip_min_cm >= GAME_TRIP_BOUND_MIN_CM && c.trip_max_cm <= GAME_TRIP_BOUND_MAX_CM);
    CHECK(c.max_strikes >= GAME_MIN_STRIKES && c.max_strikes <= GAME_MAX_STRIKES_LIMIT);
}

static void test_build_config(void)
{
    Puzzle dial;
    Puzzle wire;
    Difficulty d;
    proto_bomb_config_t c;
    uint8_t buf[PROTO_SIZE_BOMB_CONFIG];
    proto_bomb_config_t back;
    static const uint8_t code[4] = { 4, 2, 7, 1 };
    int n;
    int k;

    puzzle_fill_bank_dial(&dial, code, 0);
    puzzle_fill_bank_wire(&wire, 5, 3, 0);
    d = difficulty(NULL, 0, DIFF_MODE_AUTO);                             /* score 50 = NORMAL: 4 modules, 180 s */
    puzzle_build_config(d, dial, wire, 0u, &c);
    check_config_ranges(c);
    CHECK(c.module_count == 4 && c.module_order[0] == PROTO_MODULE_DIAL && c.module_order[1] == PROTO_MODULE_WIRES &&
          c.module_order[2] == PROTO_MODULE_TRIPWIRE && c.module_order[3] == PROTO_MODULE_LIGHTS);   /* D, W, T, L */
    CHECK(c.time_limit_s == 180 && c.dial_code[0] == 4 && c.dial_code[1] == 2 && c.dial_code[2] == 7 && c.dial_code[3] == 1);
    CHECK(c.wire_count == 5 && c.correct_wire == 2);                    /* k = 3 (1-based) -> index 2 */
    CHECK(c.trip_min_cm == PUZZLE_TRIP_MIN_BASE_CM && c.trip_max_cm == PUZZLE_TRIP_MIN_BASE_CM + PUZZLE_TRIP_WIDTH_CM &&
          c.max_strikes == 3);                                           /* rnd 0 -> the nearest band, 10-20 cm */
    CHECK(proto_pack_bomb_config(&c, buf, sizeof(buf)) == PROTO_SIZE_BOMB_CONFIG);
    CHECK(proto_unpack_bomb_config(buf, sizeof(buf), &back) && back.module_count == c.module_count &&
          back.time_limit_s == c.time_limit_s && back.wire_count == c.wire_count &&
          back.correct_wire == c.correct_wire && memcmp(back.dial_code, c.dial_code, 4) == 0);

    d.module_count = 4;
    d.time_limit_s = 210;
    puzzle_build_config(d, dial, wire, 1u, &c);
    CHECK(c.module_count == 4 && c.module_order[3] == PROTO_MODULE_LIGHTS && c.time_limit_s == 210);
    check_config_ranges(c);
    d.module_count = 3;                                                  /* a shorter bomb is a prefix: LIGHTS is dropped */
    puzzle_build_config(d, dial, wire, 1u, &c);
    CHECK(c.module_count == 3 && c.module_order[2] == PROTO_MODULE_TRIPWIRE);
    check_config_ranges(c);
    d.module_count = 4;

    for (n = 3; n <= 6; n++) {
        for (k = 1; k <= n; k++) {
            puzzle_fill_bank_wire(&wire, n, k, 0);
            puzzle_build_config(d, dial, wire, (uint32_t)(n * 7 + k), &c);
            check_config_ranges(c);
            CHECK(c.wire_count == n && c.correct_wire == k - 1);
        }
    }
    /* a broken puzzle (zeroed) still yields a config the MCX accepts */
    memset(&dial, 0, sizeof(dial));
    memset(&wire, 0, sizeof(wire));
    d.module_count = 9;
    d.time_limit_s = 9999;
    puzzle_build_config(d, dial, wire, 0xFFFFFFFFu, &c);
    CHECK(c.module_count == 4 && c.wire_count == 3 && c.correct_wire == 0);
    check_config_ranges(c);
}

/* ================= selection rules ================= */
static void test_pair_rules(void)
{
    uint8_t mask = 0;
    int i;

    CHECK(pair_next_unused(0x00, 4) == 0);
    CHECK(pair_next_unused(0x01, 4) == 1);
    CHECK(pair_next_unused(0x05, 4) == 1);
    CHECK(pair_next_unused(0x07, 4) == 3);
    CHECK(pair_next_unused(0x0F, 4) == -1);
    CHECK(pair_next_unused(0xF0, 4) == 0);                                /* bits above count are ignored */

    for (i = 0; i < 4; i++) {                                             /* spending never wraps around */
        mask = pair_mark_spent(mask, pair_next_unused(mask, 4), 4);
        CHECK(mask == (uint8_t)((1u << (i + 1)) - 1u));
    }
    CHECK(mask == 0x0F && pair_next_unused(mask, 4) == -1);               /* all spent: still all spent */
    mask = pair_mark_spent(0x01, 0, 4);                                   /* spending a spent one again */
    CHECK(mask == 0x01);
    CHECK(pair_mark_spent(0x03, 7, 4) == 0x03);                           /* out of range: unchanged */
    CHECK(pair_mark_spent(0x03, -1, 4) == 0x03);
    CHECK(pair_clear_spent(0x0F, 2, 4) == 0x0B);
    CHECK(pair_clear_spent(0x0B, 2, 4) == 0x0B);                          /* already clear */
    CHECK(pair_clear_spent(0x0B, 4, 4) == 0x0B && pair_clear_spent(0x0B, -1, 4) == 0x0B);
}

/* D4: a pair is never taken twice until it was refilled (its spent bit cleared); D6: with every pair spent the
 * least recently spent one is reused. */
static void test_spent_pairs(void)
{
    uint32_t order[4] = { 0, 0, 0, 0 };
    uint32_t counter = 0;
    uint8_t spent = 0;
    bool reused = true;
    int seen[4] = { 0, 0, 0, 0 };
    int i;
    int p;

    for (i = 0; i < 4; i++) {                                             /* four bombs in a row: four different pairs */
        p = pair_pick(spent, order, 4, &reused);
        CHECK(p == i && !reused);
        CHECK(seen[p] == 0);
        seen[p]++;
        spent = pair_mark_spent(spent, p, 4);
        order[p] = ++counter;
    }
    CHECK(spent == 0x0F);
    p = pair_pick(spent, order, 4, &reused);                              /* fifth bomb, nothing refilled yet */
    CHECK(p == 0 && reused);                                              /* pair 0 was spent first */
    spent = pair_clear_spent(spent, 2, 4);                                /* pair 2 refilled */
    p = pair_pick(spent, order, 4, &reused);
    CHECK(p == 2 && !reused);                                             /* a refilled pair beats any reuse */
    spent = pair_mark_spent(spent, 2, 4);
    order[2] = ++counter;
    p = pair_pick(spent, order, 4, &reused);                              /* oldest is still pair 0, then 1 ... */
    CHECK(p == 0 && reused);
    order[0] = ++counter;                                                 /* pair 0 reused and spent again */
    p = pair_pick(spent, order, 4, &reused);
    CHECK(p == 1 && reused);
    order[1] = ++counter;
    p = pair_pick(spent, order, 4, &reused);
    CHECK(p == 3 && reused);                                              /* 3 (order 4) older than 2 (order 5) */

    CHECK(pair_oldest_spent(0x00, order, 4) == -1);                       /* nothing spent */
    {
        const uint32_t zero[4] = { 0, 0, 0, 0 };                          /* after a reboot every order is 0: lowest index */
        CHECK(pair_oldest_spent(0x0C, zero, 4) == 2);
        CHECK(pair_oldest_spent(0x0F, zero, 4) == 0);
    }
    CHECK(pair_pick(0x00, order, 4, NULL) == 0);                          /* reused may be NULL */
    CHECK(pair_pick(0x0F, order, 4, NULL) == pair_oldest_spent(0x0F, order, 4));
}

static void test_refill_next_slot(void)
{
    CHECK(refill_next_slot(0x00, 0x00) == -1);                            /* nothing spent: nothing to refill */
    CHECK(refill_next_slot(0x00, 0xFF) == -1);
    CHECK(refill_next_slot(0x04, 0x00) == 2);                             /* pair 2: its dial slot first */
    CHECK(refill_next_slot(0x04, 1u << 2) == PUZZLE_PAIRS + 2);           /* then its wire slot */
    CHECK(refill_next_slot(0x04, (1u << 2) | (1u << (PUZZLE_PAIRS + 2))) == -1);   /* both done: the caller clears the bit */
    CHECK(refill_next_slot(0x0A, 0x00) == 1);                             /* lowest spent pair first */
    CHECK(refill_next_slot(0x0A, 1u << 1) == PUZZLE_PAIRS + 1);
    CHECK(refill_next_slot(0x0A, (1u << 1) | (1u << (PUZZLE_PAIRS + 1))) == 3);
    CHECK(refill_next_slot(0x04, 1u << 0) == 2);                          /* a refilled bit of another pair is irrelevant */
}

/* A set whose every slot is a bank puzzle at `level`, or AI at `level` with `ai_mask` (bit per slot). */
static PuzzleSet make_slot_set(uint8_t level, uint8_t ai_mask)
{
    PuzzleSet s;
    uint32_t rnd[PUZZLE_BANK_WORDS];
    int i;

    for (i = 0; i < (int)PUZZLE_BANK_WORDS; i++) {
        rnd[i] = 0x9E3779B9u * (uint32_t)(i + 1);
    }
    puzzle_set_fill_bank(&s, rnd);
    for (i = 0; i < PUZZLE_SLOTS; i++) {
        Puzzle &p = i < PUZZLE_PAIRS ? s.dial[i] : s.wire[i - PUZZLE_PAIRS];

        p.level = level;
        p.source = ((ai_mask >> i) & 1u) ? PUZZLE_SRC_AI : PUZZLE_SRC_BANK;
    }
    s.ai_count = puzzle_set_count_ai(s);
    return s;
}

static void test_upgrade_next_slot(void)
{
    PuzzleSet s = make_slot_set(PR_NORMAL, 0x00);                         /* all bank */
    int slot;
    int visited = 0;
    int i;

    CHECK(puzzle_set_count_ai(s) == 0);
    CHECK(upgrade_next_slot(s, PR_NORMAL, 0x00, 0x00, -1) == 0);          /* starts at slot 0 */
    CHECK(upgrade_next_slot(s, PR_NORMAL, 0x00, 0x00, 0) == 1);           /* round-robin: the one after the last */
    CHECK(upgrade_next_slot(s, PR_NORMAL, 0x00, 0x00, 6) == 7);
    CHECK(upgrade_next_slot(s, PR_NORMAL, 0x00, 0x00, 7) == 0);           /* wraps */

    /* a full round visits every slot once */
    slot = -1;
    for (i = 0; i < PUZZLE_SLOTS; i++) {
        slot = upgrade_next_slot(s, PR_NORMAL, 0x00, 0x00, slot);
        CHECK(slot >= 0);
        visited |= 1 << slot;
    }
    CHECK(visited == 0xFF);

    /* spent pairs are skipped: pair 1 = slots 1 and 5 */
    CHECK(upgrade_next_slot(s, PR_NORMAL, 0x02, 0x00, 0) == 2);
    CHECK(upgrade_next_slot(s, PR_NORMAL, 0x02, 0x00, 4) == 6);
    CHECK(upgrade_next_slot(s, PR_NORMAL, 0x0F, 0x00, -1) == -1);         /* everything spent: nothing to do */

    /* AI slots at the target level are done */
    s = make_slot_set(PR_NORMAL, 0xFF);
    CHECK(puzzle_set_count_ai(s) == 8);
    CHECK(upgrade_next_slot(s, PR_NORMAL, 0x00, 0x00, -1) == -1);
    s = make_slot_set(PR_NORMAL, 0xDF);                                   /* only slot 5 (wire of pair 1) is a bank puzzle */
    CHECK(puzzle_set_count_ai(s) == 7);
    CHECK(upgrade_next_slot(s, PR_NORMAL, 0x00, 0x00, -1) == 5);
    CHECK(upgrade_next_slot(s, PR_NORMAL, 0x00, 0x00, 5) == 5);           /* the only one: also after itself */
    CHECK(upgrade_next_slot(s, PR_NORMAL, 0x02, 0x00, -1) == -1);         /* ...unless its pair is spent */

    /* an AI puzzle at another level than the target is upgraded again */
    s = make_slot_set(PR_EASY, 0xFF);
    CHECK(upgrade_next_slot(s, PR_NORMAL, 0x00, 0x00, -1) == 0);
    CHECK(upgrade_next_slot(s, PR_EASY, 0x00, 0x00, -1) == -1);

    /* manual redo: the force bits make AI slots at the target level candidates, in slot order */
    s = make_slot_set(PR_NORMAL, 0xFF);
    CHECK(upgrade_next_slot(s, PR_NORMAL, 0x00, 0x24, -1) == 2);          /* slots 2 and 5 forced */
    CHECK(upgrade_next_slot(s, PR_NORMAL, 0x00, 0x24, 2) == 5);
    CHECK(upgrade_next_slot(s, PR_NORMAL, 0x00, 0x24, 5) == 2);
    CHECK(upgrade_next_slot(s, PR_NORMAL, 0x04, 0x24, -1) == 5);          /* slot 2 is pair 2 (spent): skipped, slot 5 stays */
    CHECK(upgrade_next_slot(s, PR_NORMAL, 0x06, 0x24, -1) == -1);         /* both pairs spent: nothing */
}

static void test_checker_order(void)
{
    static const char *const chain[] = { "x-test-1", "x-test-2", "qwen3.6:35b", "default" };
    int order[4];

    /* O5: the setter's own model is never a checker (it used to be tried last) */
    CHECK(checker_order(chain, 4, "x-test-2", order) == 3);
    CHECK(order[0] == 0 && order[1] == 2 && order[2] == 3);
    CHECK(checker_order(chain, 4, "x-test-1", order) == 3);
    CHECK(order[0] == 1 && order[1] == 2 && order[2] == 3);
    CHECK(checker_order(chain, 4, "default", order) == 3);
    CHECK(order[0] == 0 && order[1] == 1 && order[2] == 2);
    CHECK(checker_order(chain, 4, "unknown", order) == 4);              /* not in the chain: all four */
    CHECK(order[0] == 0 && order[3] == 3);
    CHECK(checker_order(chain, 4, NULL, order) == 4);
    CHECK(checker_order(chain, 0, "x-test-2", order) == 0);
    {
        static const char *const one[] = { "only" };

        CHECK(checker_order(one, 1, "only", order) == 0);                /* no other model: no checker at all */
        CHECK(checker_order(one, 1, "other", order) == 1 && order[0] == 0);
    }
    {
        int s;
        int i;

        for (s = 0; s < 4; s++) {
            const int n = checker_order(chain, 4, chain[s], order);

            CHECK(n == 3);
            for (i = 0; i < n; i++) {
                CHECK(strcmp(chain[order[i]], chain[s]) != 0);
            }
        }
    }
}

static void test_fresh_and_triggers(void)
{
    const uint32_t day = 86400u;

    CHECK(!puzzle_set_fresh(0, 4, 1000, day));
    CHECK(!puzzle_set_fresh(1000, 0, 1000, day));                         /* bank-only set */
    CHECK(puzzle_set_fresh(1000, 1, 1000 + day, day));                    /* exactly one day old */
    CHECK(!puzzle_set_fresh(1000, 1, 1001 + day, day));
    CHECK(puzzle_set_fresh(1000, 8, 0, day));                             /* clock unknown */
    CHECK(puzzle_set_fresh(1000, 8, 500, day));                           /* clock went backwards */

    CHECK(config_trigger_needed(PROTO_STATE_BOMB_ACTIVE, PROTO_STATE_EXPLODED, true));
    CHECK(config_trigger_needed(PROTO_STATE_BOMB_ACTIVE, PROTO_STATE_ARMED, true));     /* snooze */
    CHECK(!config_trigger_needed(PROTO_STATE_EXPLODED, PROTO_STATE_EXPLODED, true));    /* only on the change */
    CHECK(!config_trigger_needed(PROTO_STATE_ARMED, PROTO_STATE_ARMED, true));
    CHECK(!config_trigger_needed(PROTO_STATE_IDLE, PROTO_STATE_ARMED, false));          /* outside a session */
    CHECK(!config_trigger_needed(PROTO_STATE_EXPLODED, PROTO_STATE_BOMB_ACTIVE, true));
    CHECK(!config_trigger_needed(PROTO_STATE_DEFUSED, PROTO_STATE_IDLE, true));
    CHECK(!config_trigger_needed(PROTO_STATE_BOMB_ACTIVE, PROTO_STATE_DEFUSED, true));

    CHECK(bomb_just_started(PROTO_STATE_ARMED, PROTO_STATE_BOMB_ACTIVE));
    CHECK(bomb_just_started(PROTO_STATE_EXPLODED, PROTO_STATE_BOMB_ACTIVE));
    CHECK(bomb_just_started(0xFF, PROTO_STATE_BOMB_ACTIVE));
    CHECK(!bomb_just_started(PROTO_STATE_BOMB_ACTIVE, PROTO_STATE_BOMB_ACTIVE));
    CHECK(!bomb_just_started(PROTO_STATE_ARMED, PROTO_STATE_EXPLODED));
}

int main(void)
{
    printf("test_puzzle: text helpers\n");
    test_strip_think();
    test_extract_json();
    test_json_get_string();
    test_validate_puzzle();
    test_parse_checker_answer();
    printf("test_puzzle: template bank (exhaustive)\n");
    test_dial_template_shapes();
    test_dial_templates_all_codes();
    test_wire_template_shapes();
    test_wire_templates_all();
    test_bank_puzzles();
    test_bank_set();
    test_random_picks();
    printf("test_puzzle: config, selection\n");
    test_build_config();
    test_pair_rules();
    test_spent_pairs();
    test_refill_next_slot();
    test_upgrade_next_slot();
    test_checker_order();
    test_fresh_and_triggers();
    printf("test_puzzle: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
