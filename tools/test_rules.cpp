/*
 * Host-side tests for esp32/defuse_esp32/src/app/puzzle_rules.*. Plain checks, no framework.
 *
 * The module under test proves how many answers a rule set has and rates its difficulty, so
 * these tests do not trust it: random rule sets are generated as an expression tree, rendered
 * to text (precedence-aware parentheses, random extra parentheses, random whitespace) and
 * solved by an independent evaluator working on the tree. Hand-written sets cover every error
 * kind, every tier and the limits. A fixed seed keeps the run reproducible.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <string>

#include "puzzle_rules.h"

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

/* Reports which text failed, for table-driven checks. */
#define CHECK_TEXT(cond, text)                                                   \
    do {                                                                         \
        g_checks++;                                                              \
        if (!(cond)) {                                                           \
            g_failed++;                                                          \
            if (g_failed < 40) {                                                 \
                printf("  FAIL %s:%d: %s  [%s]\n", __FILE__, __LINE__, #cond, text); \
            }                                                                    \
        }                                                                        \
    } while (0)

/* ================= helpers ================= */
static int parse_dial(const char *text, PrSet *set) { return pr_parse(text, PR_KIND_DIAL, 0, set); }
static int parse_wire(const char *text, int n, PrSet *set) { return pr_parse(text, PR_KIND_WIRE, n, set); }

static int parse_error_dial(const char *text) { PrSet s; return parse_dial(text, &s); }
static int parse_error_wire(const char *text, int n) { PrSet s; return parse_wire(text, n, &s); }

/* Rating of a dial text, or -(error code) when it does not parse. */
static int dial_level(const char *text)
{
    PrSet s;
    const int e = parse_dial(text, &s);
    return e == PR_OK ? pr_rate(&s) : -e;
}

static int wire_level(const char *text, int n)
{
    PrSet s;
    const int e = parse_wire(text, n, &s);
    return e == PR_OK ? pr_rate(&s) : -e;
}

/* Full count (no limit) of a dial text, -1 when it does not parse. */
static int dial_count(const char *text, int *first)
{
    PrSet s;
    if (parse_dial(text, &s) != PR_OK) {
        return -1;
    }
    return pr_count_solutions(&s, first, 0);
}

static int wire_count_of(const char *text, int n, int *first)
{
    PrSet s;
    if (parse_wire(text, n, &s) != PR_OK) {
        return -1;
    }
    return pr_count_solutions(&s, first, 0);
}

/* ================= parser ================= */
static void test_parser_valid(void)
{
    PrSet s;
    CHECK(parse_dial("d1=4", &s) == PR_OK);
    CHECK(s.rule_count == 1 && s.error_pos == -1);
    CHECK(parse_dial("d1=4;d2=d1-2;d3=d2+5;d4=d3-6", &s) == PR_OK && s.rule_count == 4);
    CHECK(parse_dial("alldiff", &s) == PR_OK && s.rule_count == 1);
    CHECK(parse_dial("odd(d1);even(d2+d3);odd(sum)", &s) == PR_OK && s.rule_count == 3);
    CHECK(parse_dial("sum>=10;sum<=20", &s) == PR_OK);
    CHECK(parse_dial("d1+d2*2=7", &s) == PR_OK);
    CHECK(parse_dial("(d1+d2)*(d3-d4)!=0", &s) == PR_OK);
    CHECK(parse_dial("((((d1))))=1", &s) == PR_OK);                 /* 4 levels of parentheses */
    CHECK(parse_dial("d1=99;d2=0", &s) == PR_OK);
    CHECK(parse_dial("d1=007", &s) == PR_OK);                       /* leading zeros are just a 7 */
    CHECK(parse_dial("99*99*99=0", &s) == PR_OK);                   /* 970299, still under the limit */
    CHECK(parse_wire("w=n-2", 5, &s) == PR_OK && s.rule_count == 1);
    CHECK(parse_wire("odd(w);w>n-3;w<n", 6, &s) == PR_OK && s.rule_count == 3);
    CHECK(parse_wire("w*2<=n+1", 3, &s) == PR_OK);
    CHECK(parse_dial("d1=1;d2=2;d3=3;d4=4;alldiff;sum=10", &s) == PR_OK && s.rule_count == PR_MAX_RULES);
}

static void test_parser_whitespace_and_trailing(void)
{
    int first = -1;
    /* whitespace of every kind is ignored; one trailing ';' is tolerated */
    CHECK(dial_count("  d1 = 4 ;\td2\t=\n2 ; d3=0 ;d4=0;", &first) == 1 && first == 4200);
    CHECK(dial_count("d1=4;d2=2;d3=0;d4=0", &first) == 1 && first == 4200);
    CHECK(dial_count("d1=4;d2=2;d3=0;d4=0 ;  ", &first) == 1 && first == 4200);
    /* but not inside a name or a number, nor an empty rule in the middle */
    CHECK(parse_error_dial("d 1=4") == PR_ERR_UNKNOWN_TERM);
    CHECK(parse_error_dial("d1=1 0") == PR_ERR_SYNTAX);
    CHECK(parse_error_dial("d1=4;;d2=2") == PR_ERR_SYNTAX);
    CHECK(parse_error_dial(";d1=4") == PR_ERR_SYNTAX);
    CHECK(parse_error_dial("d1=4;;") == PR_ERR_SYNTAX);
}

static void test_parser_precedence(void)
{
    int first = -1;
    /* * binds tighter than + : d1 + 2*d2 = 7 has d2 = 0..3 -> 4 solutions */
    CHECK(dial_count("d1+d2*2=7;d3=0;d4=0", &first) == 4);
    CHECK(first == 1300);                                           /* codes 1300 3200 5100 7000 */
    CHECK(dial_count("d2*2+d1=7;d3=0;d4=0", NULL) == 4);
    CHECK(dial_count("(d1+d2)*2=8;d3=0;d4=0", NULL) == 5);          /* d1+d2 = 4 */
    CHECK(dial_count("(d1+d2)*2=7;d3=0;d4=0", NULL) == 0);
    /* left to right: (d1-d2)-d3 = 0 , not d1-(d2-d3) */
    CHECK(dial_count("d1-d2-d3=0;d4=0;d1=5;d2=1", &first) == 1 && first == 5140);
    CHECK(dial_count("d1-(d2-d3)=0;d4=0;d1=5;d2=9", &first) == 1 && first == 5940);
    CHECK(dial_count("2+3*4=14;d1=0;d2=0;d3=0;d4=0", NULL) == 1);
    CHECK(dial_count("(2+3)*4=20;d1=0;d2=0;d3=0;d4=0", NULL) == 1);
    CHECK(dial_count("10-2-3=5;d1=0;d2=0;d3=0;d4=0", NULL) == 1);
    CHECK(dial_count("10-2*3=4;d1=0;d2=0;d3=0;d4=0", NULL) == 1);
    CHECK(dial_count("2*3*4=24;d1=0;d2=0;d3=0;d4=0", NULL) == 1);
    /* negative intermediate results are fine */
    CHECK(dial_count("d1-9<0;d2=0;d3=0;d4=0", NULL) == 9);          /* d1 = 0..8 */
    CHECK(wire_count_of("w*2-1=n+2", 5, &first) == 1 && first == 4);
}

static void test_parser_relations(void)
{
    /* d1 REL 4 with the other digits pinned: 1 + the count of d1 values that satisfy it */
    struct Row { const char *text; int expected; };
    static const Row rows[] = {
        { "d1=4;d2=0;d3=0;d4=0", 1 },
        { "d1!=4;d2=0;d3=0;d4=0", 9 },
        { "d1<4;d2=0;d3=0;d4=0", 4 },
        { "d1>4;d2=0;d3=0;d4=0", 5 },
        { "d1<=4;d2=0;d3=0;d4=0", 5 },
        { "d1>=4;d2=0;d3=0;d4=0", 6 },
        { "odd(d1);d2=0;d3=0;d4=0", 5 },
        { "even(d1);d2=0;d3=0;d4=0", 5 },
        { "odd(d1*3);d2=0;d3=0;d4=0", 5 },
        { "even(d1-9);d2=0;d3=0;d4=0", 5 },                         /* negative operand: -9 is odd */
        { "alldiff;d1=1;d2=2;d3=3", 7 },                            /* d4 not in {1,2,3} */
        { "sum=36", 1 },
        { "sum=0", 1 },
        { "sum>=35", 5 },                                           /* 9999 and the four with one 8 */
    };
    for (size_t i = 0; i < sizeof rows / sizeof rows[0]; i++) {
        CHECK_TEXT(dial_count(rows[i].text, NULL) == rows[i].expected, rows[i].text);
    }
}

static void test_parser_errors(void)
{
    /* PR_ERR_ARGS */
    PrSet s;
    CHECK(pr_parse(NULL, PR_KIND_DIAL, 0, &s) == PR_ERR_ARGS);
    CHECK(pr_parse("d1=1", PR_KIND_DIAL, 0, NULL) == PR_ERR_ARGS);
    CHECK(pr_parse("d1=1", 0, 0, &s) == PR_ERR_ARGS);
    CHECK(pr_parse("d1=1", 3, 0, &s) == PR_ERR_ARGS);
    CHECK(parse_error_wire("w=1", PR_WIRE_MIN - 1) == PR_ERR_ARGS);
    CHECK(parse_error_wire("w=1", PR_WIRE_MAX + 1) == PR_ERR_ARGS);
    CHECK(parse_error_wire("w=1", PR_WIRE_MIN) == PR_OK);
    CHECK(parse_error_wire("w=1", PR_WIRE_MAX) == PR_OK);
    CHECK(parse_error_dial("d1=1") == PR_OK);                        /* the wire count is ignored for a dial */

    /* PR_ERR_EMPTY */
    CHECK(parse_error_dial("") == PR_ERR_EMPTY);
    CHECK(parse_error_dial("   \t ") == PR_ERR_EMPTY);

    /* PR_ERR_CHAR */
    CHECK(parse_error_dial("d1=4@") == PR_ERR_CHAR);
    CHECK(parse_error_dial("D1=4") == PR_ERR_CHAR);                  /* names are lower case */
    CHECK(parse_error_dial("d1=#") == PR_ERR_CHAR);
    CHECK(parse_error_dial("d1/2=1") == PR_ERR_CHAR);
    CHECK(parse_error_dial("d1=4 && d2=1") == PR_ERR_CHAR);
    CHECK(parse_error_dial("d1 ~ 4") == PR_ERR_CHAR);
    CHECK(parse_error_dial("d1=\"4\"") == PR_ERR_CHAR);
    CHECK(parse_error_dial("d1,d2=4") == PR_ERR_CHAR);

    /* PR_ERR_SYNTAX */
    static const char *const syntax[] = {
        "d1", "d1==4", "d1=", "=4", "d1=4=5", "d1<>4", "d1!4", "d1+=3", "d1+", "*d1=4", "d1**2=4",
        "-d1=4", "d1=-4", "(d1=4", "d1=4)", "()=1", "(d1+)=1", "odd", "odd d1", "odd(d1", "odd()",
        "odd(d1)=1", "even(d1))", "alldiff=1", "alldiff(d1)", "odd(alldiff)", "d1=sum+odd", "d1 d2=4",
        "d1=4 d2=5", "d1=4;", ";", " ; ", "d1=4;;d2=5"
    };
    for (size_t i = 0; i < sizeof syntax / sizeof syntax[0]; i++) {
        const int e = parse_error_dial(syntax[i]);
        /* "d1=4;" is the tolerated trailing ';' and parses */
        const int expected = (strcmp(syntax[i], "d1=4;") == 0) ? PR_OK : PR_ERR_SYNTAX;
        CHECK_TEXT(e == expected, syntax[i]);
    }

    /* PR_ERR_NUMBER */
    CHECK(parse_error_dial("d1=100") == PR_ERR_NUMBER);
    CHECK(parse_error_dial("d1=99") == PR_OK);
    CHECK(parse_error_dial("d1+150=4") == PR_ERR_NUMBER);
    CHECK(parse_error_dial("d1=99999999999") == PR_ERR_NUMBER);
    CHECK(parse_error_wire("w=1000", 4) == PR_ERR_NUMBER);

    /* PR_ERR_UNKNOWN_TERM */
    CHECK(parse_error_dial("d5=1") == PR_ERR_UNKNOWN_TERM);
    CHECK(parse_error_dial("d0=1") == PR_ERR_UNKNOWN_TERM);
    CHECK(parse_error_dial("d12=1") == PR_ERR_UNKNOWN_TERM);
    CHECK(parse_error_dial("x=1") == PR_ERR_UNKNOWN_TERM);
    CHECK(parse_error_dial("digit=1") == PR_ERR_UNKNOWN_TERM);
    CHECK(parse_error_dial("sumx=1") == PR_ERR_UNKNOWN_TERM);
    CHECK(parse_error_dial("oddd(d1)") == PR_ERR_UNKNOWN_TERM);
    CHECK(parse_error_dial("averyveryverylongname=1") == PR_ERR_UNKNOWN_TERM);
    CHECK(parse_error_wire("v=1", 4) == PR_ERR_UNKNOWN_TERM);
    CHECK(parse_error_wire("w=nn", 4) == PR_ERR_UNKNOWN_TERM);

    /* PR_ERR_TERM_KIND */
    CHECK(parse_error_dial("w=1") == PR_ERR_TERM_KIND);
    CHECK(parse_error_dial("d1=n") == PR_ERR_TERM_KIND);
    CHECK(parse_error_dial("odd(w)") == PR_ERR_TERM_KIND);
    CHECK(parse_error_wire("d1=1", 4) == PR_ERR_TERM_KIND);
    CHECK(parse_error_wire("w=sum", 4) == PR_ERR_TERM_KIND);
    CHECK(parse_error_wire("alldiff", 4) == PR_ERR_TERM_KIND);
    CHECK(parse_error_wire("w=1;d4=2", 4) == PR_ERR_TERM_KIND);

    /* PR_ERR_TOO_COMPLEX: parentheses nested deeper than PR_MAX_DEPTH */
    CHECK(parse_error_dial("((((((d1))))))=1") == PR_OK);            /* exactly 6 levels */
    CHECK(parse_error_dial("(((((((d1)))))))=1") == PR_ERR_TOO_COMPLEX);
    CHECK(parse_error_dial("odd((((((d1))))))") == PR_OK);            /* odd( counts as a level: 1 + 5 */
    CHECK(parse_error_dial("odd(((((((d1)))))))") == PR_ERR_TOO_COMPLEX);

    /* PR_ERR_RANGE */
    CHECK(parse_error_dial("99*99*99*99=0") == PR_ERR_RANGE);
    CHECK(parse_error_dial("sum*sum*sum*sum*sum=0") == PR_ERR_RANGE);
    CHECK(parse_error_dial("99*99*99*99-99*99*99*99=0") == PR_ERR_RANGE);
    CHECK(parse_error_dial("d1*d2*d3*d4*99=0") == PR_OK);            /* 649,539 */
}

static void test_parser_error_positions(void)
{
    PrSet s;
    CHECK(parse_dial("d1=4;d2=@", &s) == PR_ERR_CHAR && s.error_pos == 8);
    CHECK(parse_dial("d1=4;d9=1", &s) == PR_ERR_UNKNOWN_TERM && s.error_pos == 5);
    CHECK(parse_dial("d1=4;d2=200", &s) == PR_ERR_NUMBER && s.error_pos > 5);
    CHECK(parse_dial("d1=4", &s) == PR_OK && s.error_pos == -1);
}

static void test_parser_limits(void)
{
    PrSet s;
    /* exactly PR_MAX_CHARS characters parse; one more is too long (whitespace counts) */
    std::string t = "d1=4";
    t.append(PR_MAX_CHARS - t.size(), ' ');
    CHECK((int)t.size() == PR_MAX_CHARS);
    CHECK(parse_dial(t.c_str(), &s) == PR_OK);
    t += " ";
    CHECK((int)t.size() == PR_MAX_CHARS + 1);
    CHECK(parse_dial(t.c_str(), &s) == PR_ERR_TOO_LONG);
    std::string longrule = "d1=1";
    while ((int)longrule.size() < PR_MAX_CHARS + 20) {
        longrule += "+1";
    }
    CHECK(parse_dial(longrule.c_str(), &s) == PR_ERR_TOO_LONG);
    /* the longest legal single rule: 120 characters of 1+1+1... (the node pool is never the limit) */
    std::string dense = "d1=1";
    while ((int)dense.size() + 2 <= PR_MAX_CHARS) {
        dense += "+1";
    }
    CHECK((int)dense.size() <= PR_MAX_CHARS && (int)dense.size() >= PR_MAX_CHARS - 1);
    CHECK(parse_dial(dense.c_str(), &s) == PR_OK);
    CHECK(s.node_count <= PR_MAX_NODES);

    /* PR_MAX_RULES rules parse, one more does not */
    CHECK(parse_error_dial("d1=1;d2=2;d3=3;d4=4;alldiff;sum=10") == PR_OK);
    CHECK(parse_error_dial("d1=1;d2=2;d3=3;d4=4;alldiff;sum=10;odd(d1)") == PR_ERR_TOO_MANY_RULES);
    CHECK(parse_error_dial("d1=1;d2=2;d3=3;d4=4;alldiff;sum=10;sum=10;d1=1;d2=2") == PR_ERR_TOO_MANY_RULES);
    CHECK(parse_error_wire("w>1;w>1;w>1;w>1;w>1;w>1", 4) == PR_OK);
    CHECK(parse_error_wire("w>1;w>1;w>1;w>1;w>1;w>1;w>1", 4) == PR_ERR_TOO_MANY_RULES);
    CHECK(PR_MAX_RULES == 6 && PR_MAX_CHARS == 120);

    /* a failed parse never leaves a set that looks valid */
    CHECK(parse_dial("d1=", &s) == PR_ERR_SYNTAX);
    CHECK(s.error_pos >= 0);
}

/* ================= counting ================= */
static void test_counting(void)
{
    int first = 12345;
    PrSet s;
    /* unique */
    CHECK(dial_count("d1=4;d2=2;d3=7;d4=1", &first) == 1 && first == 4271);
    /* none */
    CHECK(dial_count("d1=4;d1=5", &first) == 0 && first == PR_NO_SOLUTION);
    CHECK(dial_count("sum>36", &first) == 0 && first == PR_NO_SOLUTION);
    CHECK(dial_count("alldiff;d1=d2", NULL) == 0);
    /* many */
    CHECK(dial_count("d1=1", &first) == 1000 && first == 1000);
    CHECK(dial_count("sum>=0", &first) == PR_DIAL_CODES && first == 0);
    CHECK(dial_count("alldiff", NULL) == 10 * 9 * 8 * 7);
    CHECK(dial_count("odd(sum)", NULL) == 5000);
    CHECK(dial_count("sum=1", NULL) == 4);

    /* the limit stops the count (and keeps the first solution) */
    CHECK(parse_dial("d1=1", &s) == PR_OK);
    CHECK(pr_count_solutions(&s, &first, 2) == 2 && first == 1000);
    CHECK(pr_count_solutions(&s, &first, 1) == 1 && first == 1000);
    CHECK(pr_count_solutions(&s, NULL, 5) == 5);
    CHECK(pr_count_solutions(&s, NULL, 0) == 1000);
    CHECK(pr_count_solutions(&s, NULL, -1) == 1000);
    CHECK(pr_count_solutions(NULL, &first, 2) == 0 && first == PR_NO_SOLUTION);

    /* wires */
    CHECK(wire_count_of("w=n-2", 5, &first) == 1 && first == 3);
    CHECK(wire_count_of("w>0", 4, &first) == 4 && first == 1);
    CHECK(wire_count_of("w>n", 6, &first) == 0 && first == PR_NO_SOLUTION);
    CHECK(wire_count_of("even(w)", 6, NULL) == 3);
    CHECK(wire_count_of("even(w)", 5, NULL) == 2);
    CHECK(wire_count_of("w=n", 6, &first) == 1 && first == 6);
    CHECK(parse_wire("w>0", 6, &s) == PR_OK);
    CHECK(pr_count_solutions(&s, &first, 2) == 2 && first == 1);
}

/* ================= independent reference: expression trees ================= */
struct TExpr {
    int op;                 /* 0 const, 1 digit, 2 sum, 3 wire, 4 add, 5 sub, 6 mul, 7 n */
    int a;                  /* digit index or left child */
    int b;                  /* right child */
    int val;                /* const */
};

struct TRule {
    int kind;               /* 0 rel, 1 odd, 2 even, 3 alldiff */
    int rel;                /* 0 = , 1 != , 2 < , 3 > , 4 <= , 5 >= */
    int l;
    int r;
};

struct TSet {
    TExpr e[256];
    int ne;
    TRule r[PR_MAX_RULES];
    int nr;
    int wire;               /* 0 = dial set, else the wire count */
};

static uint32_t g_seed = 20271027u;

static uint32_t rnd(uint32_t n)
{
    g_seed = g_seed * 1664525u + 1013904223u;
    return (g_seed >> 8) % n;
}

static int t_new(TSet *ts, int op, int a, int b, int val)
{
    ts->e[ts->ne].op = op;
    ts->e[ts->ne].a = a;
    ts->e[ts->ne].b = b;
    ts->e[ts->ne].val = val;
    return ts->ne++;
}

static int t_leaf(TSet *ts)
{
    const uint32_t k = rnd(10);
    if (ts->wire != 0) {
        if (k < 4) return t_new(ts, 3, 0, 0, 0);
        if (k < 6) return t_new(ts, 7, 0, 0, 0);
        return t_new(ts, 0, 0, 0, (int)rnd(8));
    }
    if (k < 5) return t_new(ts, 1, (int)rnd(4), 0, 0);
    if (k < 6) return t_new(ts, 2, 0, 0, 0);
    if (k == 9 && rnd(4) == 0) return t_new(ts, 0, 0, 0, 99);
    return t_new(ts, 0, 0, 0, (int)rnd(12));
}

static int t_expr(TSet *ts, int depth)
{
    if (depth == 0 || rnd(3) == 0) {
        return t_leaf(ts);
    }
    const uint32_t k = rnd(4);
    const int op = (k < 2) ? 4 : (k == 2 ? 5 : 6);
    const int l = t_expr(ts, depth - 1);
    const int r = t_expr(ts, depth - 1);
    return t_new(ts, op, l, r, 0);
}

static void t_rule(TSet *ts, TRule *r)
{
    const uint32_t k = rnd(100);
    r->rel = 0;
    r->l = 0;
    r->r = 0;
    if (k < 60) {
        r->kind = 0;
        r->rel = (rnd(2) == 0) ? 0 : (int)rnd(6);
        r->l = t_expr(ts, 2);
        r->r = t_expr(ts, 2);
    } else if (k < 72) {
        r->kind = 1;
        r->l = t_expr(ts, 2);
    } else if (k < 84) {
        r->kind = 2;
        r->l = t_expr(ts, 2);
    } else if (ts->wire == 0) {
        r->kind = 3;
    } else {
        r->kind = 1;
        r->l = t_expr(ts, 1);
    }
}

static long long t_eval(const TSet *ts, int i, const int d[4], int w)
{
    const TExpr *e = &ts->e[i];
    switch (e->op) {
    case 0: return e->val;
    case 1: return d[e->a];
    case 2: return d[0] + d[1] + d[2] + d[3];
    case 3: return w;
    case 4: return t_eval(ts, e->a, d, w) + t_eval(ts, e->b, d, w);
    case 5: return t_eval(ts, e->a, d, w) - t_eval(ts, e->b, d, w);
    case 6: return t_eval(ts, e->a, d, w) * t_eval(ts, e->b, d, w);
    default: return ts->wire;
    }
}

static bool t_rule_holds(const TSet *ts, const TRule *r, const int d[4], int w)
{
    if (r->kind == 3) {
        for (int i = 0; i < 4; i++) {
            for (int j = i + 1; j < 4; j++) {
                if (d[i] == d[j]) return false;
            }
        }
        return true;
    }
    const long long a = t_eval(ts, r->l, d, w);
    if (r->kind == 1) return (a % 2 + 2) % 2 == 1;
    if (r->kind == 2) return (a % 2 + 2) % 2 == 0;
    const long long b = t_eval(ts, r->r, d, w);
    switch (r->rel) {
    case 0: return a == b;
    case 1: return a != b;
    case 2: return a < b;
    case 3: return a > b;
    case 4: return a <= b;
    default: return a >= b;
    }
}

/* ---- rendering ---- */
static int t_prec(const TSet *ts, int i)
{
    const int op = ts->e[i].op;
    return op == 4 || op == 5 ? 1 : (op == 6 ? 2 : 3);
}

static void sp(std::string *out)
{
    const uint32_t k = rnd(8);
    if (k == 0) *out += " ";
    if (k == 1) *out += "  ";
    if (k == 2) *out += "\t";
}

static void t_render(const TSet *ts, int i, int parent_prec, bool parent_sub_right, std::string *out)
{
    const TExpr *e = &ts->e[i];
    const int prec = t_prec(ts, i);
    bool paren = prec < parent_prec || (parent_sub_right && prec == 1);
    if (!paren && rnd(7) == 0) paren = true;                        /* redundant parentheses */
    if (paren) { *out += "("; sp(out); }
    switch (e->op) {
    case 0: { char num[12]; snprintf(num, sizeof num, "%d", e->val); *out += num; break; }
    case 1: *out += "d"; *out += (char)('1' + e->a); break;
    case 2: *out += "sum"; break;
    case 3: *out += "w"; break;
    case 7: *out += "n"; break;
    default: {
        t_render(ts, e->a, prec, false, out);
        sp(out);
        *out += (e->op == 4) ? "+" : (e->op == 5 ? "-" : "*");
        sp(out);
        /* a right operand of the same precedence needs parentheses only below '-'; a '*' child
         * under '*' and a '+'/'-' child under '+' are value-preserving without them */
        t_render(ts, e->b, prec, e->op == 5, out);
        break;
    }
    }
    if (paren) { sp(out); *out += ")"; }
}

static void t_render_rule(const TSet *ts, const TRule *r, std::string *out)
{
    static const char *const rels[] = { "=", "!=", "<", ">", "<=", ">=" };
    sp(out);
    if (r->kind == 3) {
        *out += "alldiff";
    } else if (r->kind == 0) {
        t_render(ts, r->l, 0, false, out);
        sp(out);
        *out += rels[r->rel];
        sp(out);
        t_render(ts, r->r, 0, false, out);
    } else {
        *out += (r->kind == 1) ? "odd(" : "even(";
        t_render(ts, r->l, 0, false, out);
        *out += ")";
    }
    sp(out);
}

static std::string t_text(const TSet *ts)
{
    std::string out;
    for (int i = 0; i < ts->nr; i++) {
        if (i > 0) out += ";";
        t_render_rule(ts, &ts->r[i], &out);
    }
    return out;
}

/* Independent brute force over the tree. Returns the count; *first = first solution or -1. */
static int t_count(const TSet *ts, int *first)
{
    int count = 0;
    *first = -1;
    if (ts->wire != 0) {
        const int d[4] = { 0, 0, 0, 0 };
        for (int w = 1; w <= ts->wire; w++) {
            bool ok = true;
            for (int i = 0; i < ts->nr && ok; i++) ok = t_rule_holds(ts, &ts->r[i], d, w);
            if (ok) { if (count == 0) *first = w; count++; }
        }
        return count;
    }
    for (int code = 0; code < 10000; code++) {
        const int d[4] = { code / 1000, code / 100 % 10, code / 10 % 10, code % 10 };
        bool ok = true;
        for (int i = 0; i < ts->nr && ok; i++) ok = t_rule_holds(ts, &ts->r[i], d, 0);
        if (ok) { if (count == 0) *first = code; count++; }
    }
    return count;
}

/* Independent wire rating: smallest subset of rules with exactly one solution. */
static int t_wire_level(const TSet *ts)
{
    const int d[4] = { 0, 0, 0, 0 };
    int best = ts->nr;
    for (int mask = 1; mask < (1 << ts->nr); mask++) {
        int size = 0;
        for (int i = 0; i < ts->nr; i++) if (mask & (1 << i)) size++;
        int hits = 0;
        for (int w = 1; w <= ts->wire; w++) {
            bool ok = true;
            for (int i = 0; i < ts->nr && ok; i++) {
                if (mask & (1 << i)) ok = t_rule_holds(ts, &ts->r[i], d, w);
            }
            if (ok) hits++;
        }
        if (hits == 1 && size < best) best = size;
    }
    return best <= 1 ? PR_EASY : (best == 2 ? PR_NORMAL : PR_HARD);
}

/* Builds a random set. With planted, every rule is true at the target (so there is >= 1 solution). */
static void t_make_set(TSet *ts, bool wire, bool planted, int *target_code, int *target_w)
{
    ts->ne = 0;
    ts->nr = 0;
    ts->wire = wire ? (int)(PR_WIRE_MIN + rnd(PR_WIRE_MAX - PR_WIRE_MIN + 1)) : 0;
    const int code = (int)rnd(10000);
    const int w = wire ? (int)(1 + rnd((uint32_t)ts->wire)) : 0;
    const int d[4] = { code / 1000, code / 100 % 10, code / 10 % 10, code % 10 };
    if (target_code != NULL) *target_code = code;
    if (target_w != NULL) *target_w = w;
    /* planted dial sets often start with "dN = value" pins so that a good share of the sets is unique */
    if (planted && !wire && rnd(2) == 0) {
        const int pins = 2 + (int)rnd(3);
        int order[4] = { 0, 1, 2, 3 };
        for (int i = 3; i > 0; i--) {                               /* Fisher-Yates: distinct digits */
            const int j = (int)rnd((uint32_t)(i + 1));
            const int tmp = order[i];
            order[i] = order[j];
            order[j] = tmp;
        }
        for (int i = 0; i < pins; i++) {
            const int di = order[i];
            TRule r;
            r.kind = 0;
            r.rel = 0;
            r.l = t_new(ts, 1, di, 0, 0);
            r.r = t_new(ts, 0, 0, 0, d[di]);
            ts->r[ts->nr++] = r;
        }
    }
    const int want = ts->nr + 1 + (int)rnd((uint32_t)(PR_MAX_RULES - ts->nr));
    for (int tries = 0; ts->nr < want && tries < 400; tries++) {
        const int save = ts->ne;
        TRule r;
        t_rule(ts, &r);
        if (planted && !t_rule_holds(ts, &r, d, w)) {
            ts->ne = save;
            continue;
        }
        ts->r[ts->nr++] = r;
    }
}

static const int RANDOM_SETS = 400;

static void test_random_counting(void)
{
    int skipped = 0;
    int unique = 0;
    int many = 0;
    int none = 0;
    for (int it = 0; it < RANDOM_SETS; it++) {
        const bool wire = (it % 4) == 3;
        const bool planted = (it % 2) == 0;
        TSet ts;
        int tc = 0;
        int tw = 0;
        t_make_set(&ts, wire, planted, &tc, &tw);
        if (ts.nr == 0) { continue; }
        const std::string text = t_text(&ts);
        PrSet s;
        const int err = wire ? parse_wire(text.c_str(), ts.wire, &s) : parse_dial(text.c_str(), &s);
        if (err == PR_ERR_RANGE || err == PR_ERR_TOO_LONG || err == PR_ERR_TOO_COMPLEX) {
            skipped++;                                              /* legal refusals of an oversized set */
            continue;
        }
        CHECK_TEXT(err == PR_OK, text.c_str());
        if (err != PR_OK) { continue; }
        int ref_first = -2;
        const int ref = t_count(&ts, &ref_first);
        int first = -2;
        const int got = pr_count_solutions(&s, &first, 0);
        CHECK_TEXT(got == ref, text.c_str());
        CHECK_TEXT(first == ref_first, text.c_str());
        int first2 = -2;
        const int got2 = pr_count_solutions(&s, &first2, 2);
        CHECK_TEXT(got2 == (ref < 2 ? ref : 2), text.c_str());
        CHECK_TEXT(first2 == ref_first, text.c_str());
        if (planted) { CHECK_TEXT(ref >= 1, text.c_str()); }
        const int level = pr_rate(&s);
        if (ref == 0) { CHECK_TEXT(level == PR_UNSOLVABLE, text.c_str()); }
        else if (ref >= 2) { CHECK_TEXT(level == PR_AMBIGUOUS, text.c_str()); }
        else { CHECK_TEXT(level >= PR_EASY && level <= PR_HARD, text.c_str()); }
        if (wire && ref == 1) { CHECK_TEXT(level == t_wire_level(&ts), text.c_str()); }
        unique += (ref == 1);
        many += (ref >= 2);
        none += (ref == 0);
    }
    printf("  random counting: %d sets, unique %d, many %d, none %d, oversize skipped %d\n",
           RANDOM_SETS, unique, many, none, skipped);
    CHECK(unique >= 20 && many >= 20 && none >= 5);
    CHECK(skipped < RANDOM_SETS / 10);
}

/* ================= independent tier checker =================
 * Written separately from puzzle_rules.cpp (O7): the production solver labels the tiers, so the
 * tests need a second implementation of the DEFINITION of the tiers to compare against.
 *   EASY   = single-rule filtering to a fixpoint: a rule with exactly one undecided digit removes
 *            the values of that digit that break it.
 *   NORMAL = EASY plus (a) pairwise arc consistency: a rule with exactly two undecided digits
 *            keeps a value of one only while some value of the other satisfies the rule, and
 *            (b) rule-pair joint filtering: two rules over the same two undecided digits keep an
 *            (a, b) pair only when BOTH hold.
 * A set is solved at a tier when every digit domain ends up with one value.
 * The model is a callback pair (rule holds, rule digit mask) so that it can run on a rule set
 * built as an expression tree (no parser involved) and on a parsed PrSet (own evaluator). */
struct TierModel {
    int nrules;
    unsigned vars[PR_MAX_RULES];            /* bit i = digit i is used by the rule */
    bool (*holds)(const void *ctx, int rule, const int d[4]);
    const void *ctx;
};

struct IDom {
    bool in[4][10];
};

static int ic_size(const IDom &dom, int v)
{
    int n = 0;
    for (int a = 0; a < 10; a++) n += dom.in[v][a] ? 1 : 0;
    return n;
}

static int ic_first(const IDom &dom, int v)
{
    for (int a = 0; a < 10; a++) if (dom.in[v][a]) return a;
    return 0;
}

/* Digits of the rule that are still undecided. */
static unsigned ic_undecided(const TierModel &m, const IDom &dom, int rule)
{
    unsigned u = 0;
    for (int v = 0; v < 4; v++) {
        if ((m.vars[rule] & (1u << v)) && ic_size(dom, v) > 1) u |= 1u << v;
    }
    return u;
}

/* Keeps, for each digit in freev[0..nf), only the values that have a support: an assignment of the
 * free digits (the others stay at their first value) satisfying all `nr` rules. Returns true when
 * a domain shrank. */
static bool ic_filter(const TierModel &m, IDom *dom, const int *rules, int nr, const int *freev, int nf)
{
    bool keep[2][10];
    memset(keep, 0, sizeof keep);
    int d[4];
    for (int v = 0; v < 4; v++) d[v] = ic_first(*dom, v);
    for (int a = 0; a < 10; a++) {
        if (!dom->in[freev[0]][a]) continue;
        for (int b = 0; b < (nf == 2 ? 10 : 1); b++) {
            if (nf == 2 && !dom->in[freev[1]][b]) continue;
            d[freev[0]] = a;
            if (nf == 2) d[freev[1]] = b;
            bool ok = true;
            for (int i = 0; i < nr && ok; i++) ok = m.holds(m.ctx, rules[i], d);
            if (ok) {
                keep[0][a] = true;
                if (nf == 2) keep[1][b] = true;
            }
        }
    }
    bool changed = false;
    for (int k = 0; k < nf; k++) {
        for (int a = 0; a < 10; a++) {
            if (dom->in[freev[k]][a] && !keep[k][a]) {
                dom->in[freev[k]][a] = false;
                changed = true;
            }
        }
    }
    return changed;
}

static void ic_digits(unsigned u, int *x)
{
    int n = 0;
    for (int v = 0; v < 4; v++) if (u & (1u << v)) x[n++] = v;
}

static bool ic_popcount_is(unsigned u, int want)
{
    int n = 0;
    for (int v = 0; v < 4; v++) if (u & (1u << v)) n++;
    return n == want;
}

static bool ic_solves(const TierModel &m, bool normal)
{
    IDom dom;
    for (int v = 0; v < 4; v++) for (int a = 0; a < 10; a++) dom.in[v][a] = true;
    for (;;) {
        bool changed = false;
        for (int i = 0; i < m.nrules; i++) {
            const unsigned u = ic_undecided(m, dom, i);
            int x[2] = { 0, 0 };
            if (ic_popcount_is(u, 1)) {
                ic_digits(u, x);
                changed = ic_filter(m, &dom, &i, 1, x, 1) || changed;
            } else if (normal && ic_popcount_is(u, 2)) {
                ic_digits(u, x);
                changed = ic_filter(m, &dom, &i, 1, x, 2) || changed;
            }
        }
        if (normal) {
            for (int i = 0; i < m.nrules; i++) {
                for (int j = i + 1; j < m.nrules; j++) {
                    const unsigned u = ic_undecided(m, dom, i);
                    if (!ic_popcount_is(u, 2) || u != ic_undecided(m, dom, j)) continue;
                    int x[2] = { 0, 0 };
                    const int pair[2] = { i, j };
                    ic_digits(u, x);
                    changed = ic_filter(m, &dom, pair, 2, x, 2) || changed;
                }
            }
        }
        for (int v = 0; v < 4; v++) if (ic_size(dom, v) == 0) return false;
        if (!changed) break;
    }
    for (int v = 0; v < 4; v++) if (ic_size(dom, v) != 1) return false;
    return true;
}

/* The level the tier definitions give for a set known to have exactly one solution. */
static int ic_level(const TierModel &m)
{
    if (ic_solves(m, false)) return PR_EASY;
    if (ic_solves(m, true)) return PR_NORMAL;
    return PR_HARD;
}

/* ---- adapter 1: the random expression tree (no parser involved) ---- */
static unsigned t_vars_of(const TSet *ts, int i)
{
    const TExpr *e = &ts->e[i];
    switch (e->op) {
    case 1: return 1u << e->a;
    case 2: return 0xFu;
    case 4: case 5: case 6: return t_vars_of(ts, e->a) | t_vars_of(ts, e->b);
    default: return 0;
    }
}

static bool tm_holds(const void *ctx, int rule, const int d[4])
{
    const TSet *ts = (const TSet *)ctx;
    return t_rule_holds(ts, &ts->r[rule], d, 0);
}

static void tm_from_tset(const TSet *ts, TierModel *m)
{
    m->nrules = ts->nr;
    m->holds = tm_holds;
    m->ctx = ts;
    for (int i = 0; i < ts->nr; i++) {
        const TRule *r = &ts->r[i];
        m->vars[i] = (r->kind == 3) ? 0xFu : (t_vars_of(ts, r->l) | (r->kind == 0 ? t_vars_of(ts, r->r) : 0u));
    }
}

/* ---- adapter 2: a parsed PrSet, evaluated by this file's own walker over the public nodes ---- */
static long long pn_eval(const PrSet *s, int i, const int d[4])
{
    const PrNode *n = &s->nodes[i];
    switch (n->op) {
    case PR_N_CONST: return n->value;
    case PR_N_DIGIT: return d[n->a];
    case PR_N_SUM: return d[0] + d[1] + d[2] + d[3];
    case PR_N_ADD: return pn_eval(s, n->a, d) + pn_eval(s, n->b, d);
    case PR_N_SUB: return pn_eval(s, n->a, d) - pn_eval(s, n->b, d);
    case PR_N_MUL: return pn_eval(s, n->a, d) * pn_eval(s, n->b, d);
    default: return 0;
    }
}

static unsigned pn_vars(const PrSet *s, int i)
{
    const PrNode *n = &s->nodes[i];
    switch (n->op) {
    case PR_N_DIGIT: return 1u << n->a;
    case PR_N_SUM: return 0xFu;
    case PR_N_ADD: case PR_N_SUB: case PR_N_MUL: return pn_vars(s, n->a) | pn_vars(s, n->b);
    default: return 0;
    }
}

static bool pm_holds(const void *ctx, int rule, const int d[4])
{
    const PrSet *s = (const PrSet *)ctx;
    const PrRule *r = &s->rules[rule];
    if (r->kind == PR_R_ALLDIFF) {
        for (int i = 0; i < 4; i++) for (int j = i + 1; j < 4; j++) if (d[i] == d[j]) return false;
        return true;
    }
    const long long a = pn_eval(s, r->lhs, d);
    if (r->kind == PR_R_ODD) return (a % 2 + 2) % 2 == 1;
    if (r->kind == PR_R_EVEN) return (a % 2 + 2) % 2 == 0;
    const long long b = pn_eval(s, r->rhs, d);
    switch (r->rel) {
    case PR_REL_EQ: return a == b;
    case PR_REL_NE: return a != b;
    case PR_REL_LT: return a < b;
    case PR_REL_GT: return a > b;
    case PR_REL_LE: return a <= b;
    default: return a >= b;
    }
}

static void pm_from_prset(const PrSet *s, TierModel *m)
{
    m->nrules = s->rule_count;
    m->holds = pm_holds;
    m->ctx = s;
    for (int i = 0; i < s->rule_count; i++) {
        const PrRule *r = &s->rules[i];
        m->vars[i] = (r->kind == PR_R_ALLDIFF) ? 0xFu : (pn_vars(s, r->lhs) | (r->kind == PR_R_REL ? pn_vars(s, r->rhs) : 0u));
    }
}

/* Level the independent checker gives a unique parsed dial text, -1 if it does not parse. */
static int ic_level_of_text(const char *text)
{
    PrSet s;
    TierModel m;
    if (parse_dial(text, &s) != PR_OK) return -1;
    pm_from_prset(&s, &m);
    return ic_level(m);
}

/* ================= rater ================= */
static void check_dial_set(const char *text, int solution, int level)
{
    PrSet s;
    int first = -1;
    CHECK_TEXT(parse_dial(text, &s) == PR_OK, text);
    CHECK_TEXT(pr_count_solutions(&s, &first, 0) == 1 && first == solution, text);
    CHECK_TEXT(pr_rate(&s) == level, text);
    CHECK_TEXT(ic_level_of_text(text) == level, text);               /* independent tier checker agrees */
    int code = -1;
    const int easy = pr_propagate_dial(&s, PR_TIER_EASY, &code);
    CHECK_TEXT(easy == (level == PR_EASY ? 1 : 0), text);
    if (easy) { CHECK_TEXT(code == solution, text); }
    code = -1;
    const int normal = pr_propagate_dial(&s, PR_TIER_NORMAL, &code);
    CHECK_TEXT(normal == (level <= PR_NORMAL ? 1 : 0), text);
    if (normal) { CHECK_TEXT(code == solution, text); }
}

static void test_rater_dial(void)
{
    /* EASY: every rule has one unknown once the previous digit is fixed */
    check_dial_set("d1=4;d2=d1-2;d3=d2+5;d4=d3-6", 4271, PR_EASY);
    check_dial_set("d1=9;d2=0;d3=1;d4=7", 9017, PR_EASY);
    check_dial_set("d4=d3-6;d3=d2+5;d2=d1-2;d1=4", 4271, PR_EASY);     /* rule order does not matter */
    check_dial_set("d1=3;d2=d1*2;d3=d2+d1;sum=19", 3691, PR_EASY);

    /* NORMAL: pairs of rules over two unknown digits */
    check_dial_set("d1+d2=9;d1-d2=1;d3=2*d4;d3+d4=12", 5484, PR_NORMAL);
    /* two rules over the same two unknown digits: only arc consistency / joint filtering pins them */
    check_dial_set("d1=2;d2=2;d3+d4=17;d3>d4", 2298, PR_NORMAL);
    check_dial_set("d1=0;d2=0;d3*d4=63;d3<d4", 79, PR_NORMAL);

    /* HARD: exactly one solution, but only search finds it. Each set below was found by random
     * search over planted sets and is verified here by brute force and by both solver tiers. */
    check_dial_set("sum=17;d4-d1=1;alldiff;d1-d2=4", 4085, PR_HARD);
    check_dial_set("d3-d4=1;d1+d3=5;d2*d1=0;sum=7", 2032, PR_HARD);
    check_dial_set("d3-d2=4;sum=11;alldiff;d1!=1;d4+d3=7", 3152, PR_HARD);
    check_dial_set("d1+d4=8;d4<=d1;sum=25;d1+d2=12", 4894, PR_HARD);
    check_dial_set("odd(d2);sum=23;d4-d2=0;d2*d3=18", 3929, PR_HARD);

    /* the tiers are nested: a set an easier tier solves is never rated harder */
    CHECK(dial_level("d1=4;d2=2;d3=7;d4=1") == PR_EASY);
    CHECK(dial_level("d1=4;d2=2;d3=7;d4=1;alldiff;sum=14") == PR_EASY);  /* redundant rules do not harden it */
}

static void test_rater_ambiguous_unsolvable(void)
{
    CHECK(dial_level("d1=1;d2=2;d3=3") == PR_AMBIGUOUS);               /* d4 is free: 10 codes */
    CHECK(dial_level("alldiff") == PR_AMBIGUOUS);
    CHECK(dial_level("d1=4;d2=2;d3=7;d4>=0;d4<=1") == PR_AMBIGUOUS);   /* 4270 and 4271 */
    CHECK(dial_level("d1=1;d2=2;d3=3;d4!=0;d4<=1;sum>=0") == PR_EASY); /* d4 = 1 exactly */
    CHECK(dial_level("d1=1;d1=2") == PR_UNSOLVABLE);
    CHECK(dial_level("sum>36") == PR_UNSOLVABLE);
    CHECK(dial_level("d1<d2;d2<d3;d3<d4;d4<d1") == PR_UNSOLVABLE);
    CHECK(dial_level("alldiff;d1=d2") == PR_UNSOLVABLE);
    CHECK(dial_level("d1=3;odd(d1+1)") == PR_UNSOLVABLE);
    CHECK(dial_level("d1=") == -PR_ERR_SYNTAX);                        /* helper reports a parse failure */
    /* the solver never "solves" an ambiguous or unsolvable set */
    PrSet s;
    CHECK(parse_dial("alldiff", &s) == PR_OK);
    CHECK(pr_propagate_dial(&s, PR_TIER_NORMAL, NULL) == 0);
    CHECK(parse_dial("d1=1;d1=2;d2=0;d3=0;d4=0", &s) == PR_OK);
    CHECK(pr_propagate_dial(&s, PR_TIER_NORMAL, NULL) == 0);
    CHECK(pr_propagate_dial(&s, PR_TIER_EASY, NULL) == 0);
    CHECK(parse_wire("w=n-2", 5, &s) == PR_OK);
    CHECK(pr_propagate_dial(&s, PR_TIER_EASY, NULL) == 0);             /* dial solver on a wire set */
    CHECK(pr_propagate_dial(NULL, PR_TIER_EASY, NULL) == 0);
    CHECK(pr_rate(NULL) == PR_LEVEL_NONE);
}

static void test_rater_wire(void)
{
    int first = -1;
    CHECK(wire_count_of("w=n-2", 5, &first) == 1 && first == 3);
    CHECK(wire_level("w=n-2", 5) == PR_EASY);
    /* the same rule means a different wire for a different n */
    CHECK(wire_count_of("w=n-2", 6, &first) == 1 && first == 4);
    CHECK(wire_level("w=n-2", 3) == PR_EASY);
    /* redundant rules do not change the level: the smallest sub-set counts */
    CHECK(wire_level("w=3;w>1;odd(w)", 5) == PR_EASY);
    CHECK(wire_level("w>1;odd(w);w=3", 5) == PR_EASY);
    /* NORMAL: two rules are needed */
    CHECK(wire_count_of("odd(w);w>3", 5, &first) == 1 && first == 5);
    CHECK(wire_level("odd(w);w>3", 5) == PR_NORMAL);
    CHECK(wire_count_of("w>2;w<4", 5, &first) == 1 && first == 3);
    CHECK(wire_level("w>2;w<4", 5) == PR_NORMAL);
    CHECK(wire_level("w>2;w<4;w>=3", 5) == PR_NORMAL);
    /* HARD: three rules are needed (each pair leaves two candidates) */
    CHECK(wire_count_of("even(w);w>3;w<6", 6, &first) == 1 && first == 4);
    CHECK(wire_level("even(w);w>3;w<6", 6) == PR_HARD);
    CHECK(wire_level("w<6;even(w);w>3", 6) == PR_HARD);
    CHECK(wire_level("w>1;w<6;even(w);w>3", 6) == PR_HARD);
    /* AMBIGUOUS: the rules leave two candidates */
    CHECK(wire_count_of("w>3", 5, NULL) == 2);
    CHECK(wire_level("w>3", 5) == PR_AMBIGUOUS);
    CHECK(wire_level("even(w)", 5) == PR_AMBIGUOUS);
    CHECK(wire_level("w>0", 3) == PR_AMBIGUOUS);
    /* UNSOLVABLE */
    CHECK(wire_level("w>n", 5) == PR_UNSOLVABLE);
    CHECK(wire_level("w=2;w=3", 5) == PR_UNSOLVABLE);
    CHECK(wire_level("w=0", 4) == PR_UNSOLVABLE);
}

/* planted random sets: soundness and consistency of the solver tiers */
static void test_random_rater(void)
{
    int levels[6] = { 0, 0, 0, 0, 0, 0 };
    int sets = 0;
    for (int it = 0; it < 300; it++) {
        TSet ts;
        int tc = 0;
        int tw = 0;
        t_make_set(&ts, false, true, &tc, &tw);
        if (ts.nr == 0) { continue; }
        const std::string text = t_text(&ts);
        PrSet s;
        if (parse_dial(text.c_str(), &s) != PR_OK) { continue; }
        int ref_first = -1;
        const int ref = t_count(&ts, &ref_first);
        int code_e = -1;
        int code_n = -1;
        const int easy = pr_propagate_dial(&s, PR_TIER_EASY, &code_e);
        const int normal = pr_propagate_dial(&s, PR_TIER_NORMAL, &code_n);
        /* soundness: when a tier says "solved" the set is unique and the code is its solution */
        if (easy) { CHECK_TEXT(ref == 1 && code_e == ref_first, text.c_str()); }
        if (normal) { CHECK_TEXT(ref == 1 && code_n == ref_first, text.c_str()); }
        CHECK_TEXT(!easy || normal, text.c_str());                  /* NORMAL is at least as strong */
        const int level = pr_rate(&s);
        if (ref == 1) {
            const int expected = easy ? PR_EASY : (normal ? PR_NORMAL : PR_HARD);
            CHECK_TEXT(level == expected, text.c_str());
        }
        levels[level]++;
        sets++;
    }
    /* most of these are ambiguous (soundness: the solver never claims a solution then); unique ones follow */
    printf("  random rater (free planted): %d sets, easy %d, normal %d, hard %d, ambiguous %d\n",
           sets, levels[PR_EASY], levels[PR_NORMAL], levels[PR_HARD], levels[PR_AMBIGUOUS]);
}

/* Planted sets that are mostly unique: pin a random structure of equations through the target. */
static void test_random_unique_rater(void)
{
    int levels[6] = { 0, 0, 0, 0, 0, 0 };
    int sets = 0;
    for (int it = 0; it < 3000 && sets < 400; it++) {
        const int code = (int)rnd(10000);
        const int d[4] = { code / 1000, code / 100 % 10, code / 10 % 10, code % 10 };
        /* 4-6 rules of the shapes the LLM is expected to write, all true at the target */
        std::string text;
        int rules = 0;
        const int want = 4 + (int)rnd(3);
        for (int tries = 0; rules < want && tries < 60; tries++) {
            const int a = (int)rnd(4);
            const int b = (int)rnd(4);
            char buf[64];
            switch (rnd(8)) {
            case 0: snprintf(buf, sizeof buf, "d%d+d%d=%d", a + 1, b + 1, d[a] + d[b]); break;
            case 1: snprintf(buf, sizeof buf, "d%d-d%d=%d", a + 1, b + 1, d[a] - d[b] < 0 ? 0 : d[a] - d[b]);
                    if (d[a] - d[b] < 0) continue;
                    break;
            case 2: snprintf(buf, sizeof buf, "sum=%d", d[0] + d[1] + d[2] + d[3]); break;
            case 3: snprintf(buf, sizeof buf, "d%d*d%d=%d", a + 1, b + 1, d[a] * d[b]); break;
            case 4: snprintf(buf, sizeof buf, "%s(d%d)", (d[a] & 1) ? "odd" : "even", a + 1); break;
            case 5: if (d[a] == d[b]) continue;
                    snprintf(buf, sizeof buf, "d%d%sd%d", a + 1, d[a] < d[b] ? "<" : ">", b + 1);
                    break;
            case 6: snprintf(buf, sizeof buf, "d%d=%d", a + 1, d[a]); break;
            default: if (d[0] == d[1] || d[0] == d[2] || d[0] == d[3] || d[1] == d[2] || d[1] == d[3] || d[2] == d[3])
                         continue;
                     snprintf(buf, sizeof buf, "alldiff");
                     break;
            }
            if (rules > 0) text += ";";
            text += buf;
            rules++;
        }
        PrSet s;
        if (rules == 0 || parse_dial(text.c_str(), &s) != PR_OK) { continue; }
        int first = -1;
        if (pr_count_solutions(&s, &first, 2) != 1) { continue; }
        CHECK_TEXT(first == code, text.c_str());
        int code_e = -1;
        int code_n = -1;
        const int easy = pr_propagate_dial(&s, PR_TIER_EASY, &code_e);
        const int normal = pr_propagate_dial(&s, PR_TIER_NORMAL, &code_n);
        if (easy) { CHECK_TEXT(code_e == code, text.c_str()); }
        if (normal) { CHECK_TEXT(code_n == code, text.c_str()); }
        CHECK_TEXT(!easy || normal, text.c_str());
        const int level = pr_rate(&s);
        CHECK_TEXT(level == (easy ? PR_EASY : (normal ? PR_NORMAL : PR_HARD)), text.c_str());
        CHECK_TEXT(ic_level_of_text(text.c_str()) == level, text.c_str());
        levels[level]++;
        sets++;
    }
    printf("  random unique rater: %d unique sets, easy %d, normal %d, hard %d\n",
           sets, levels[PR_EASY], levels[PR_NORMAL], levels[PR_HARD]);
    CHECK(levels[PR_EASY] >= 5 && levels[PR_NORMAL] >= 5 && levels[PR_HARD] >= 5);
}

/* Planted random expression trees that are unique (several hundred): the tree is the ground truth
 * (no parser in the loop for the count or the tiers), the parsed text goes through pr_rate. */
static void test_independent_tiers(void)
{
    int levels[6] = { 0, 0, 0, 0, 0, 0 };
    int sets = 0;
    int disagreements = 0;
    for (int it = 0; it < 6000 && sets < 400; it++) {
        TSet ts;
        int tc = 0;
        int tw = 0;
        t_make_set(&ts, false, true, &tc, &tw);
        if (ts.nr == 0) continue;
        int first = -1;
        if (t_count(&ts, &first) != 1) continue;
        const std::string text = t_text(&ts);
        PrSet s;
        if (parse_dial(text.c_str(), &s) != PR_OK) continue;
        TierModel m;
        tm_from_tset(&ts, &m);
        const int expected = ic_level(m);
        const int got = pr_rate(&s);
        if (got != expected) disagreements++;
        CHECK_TEXT(got == expected, text.c_str());
        CHECK_TEXT(ic_level_of_text(text.c_str()) == expected, text.c_str());
        levels[got]++;
        sets++;
    }
    printf("  independent tiers: %d unique random trees, easy %d, normal %d, hard %d, disagreements %d\n",
           sets, levels[PR_EASY], levels[PR_NORMAL], levels[PR_HARD], disagreements);
    /* unique trees are almost all pinned by "dN = value" rules, hence EASY; the NORMAL and HARD
     * coverage of the independent checker comes from test_random_unique_rater (400 text sets) */
    CHECK(sets >= 300);
    CHECK(levels[PR_EASY] >= 5);
}

/* A lone relation character at the end of the text must give a syntax error without reading
 * text[pos + 1] past the terminator (O7a). The buffers are exact-size heap blocks so that a run
 * under address sanitizer would flag an over-read. */
static void test_parse_rel_no_overread(void)
{
    static const char *const cases[] = { "d1!", "d1<", "d1>", "d1=", "d1", "d1 !", "d1 <", "d1 >" };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        const size_t len = strlen(cases[i]);
        char *buf = (char *)malloc(len + 1);
        memcpy(buf, cases[i], len + 1);
        PrSet s;
        CHECK_TEXT(parse_dial(buf, &s) != PR_OK, cases[i]);
        free(buf);
    }
}

/* ================= pr_check ================= */
static void test_check(void)
{
    PrCheck c;
    pr_check("d1=4;d2=d1-2;d3=d2+5;d4=d3-6", PR_KIND_DIAL, 0, 4271, &c);
    CHECK(c.error == PR_OK && c.count == 1 && c.unique == 1 && c.solution == 4271);
    CHECK(c.level == PR_EASY && c.matches_answer == 1);
    pr_check("d1=4;d2=d1-2;d3=d2+5;d4=d3-6", PR_KIND_DIAL, 0, 4272, &c);
    CHECK(c.unique == 1 && c.solution == 4271 && c.matches_answer == 0);
    pr_check("d1=5;d2=4;d3=8;d4=4;sum=21", PR_KIND_DIAL, 0, 5484, &c);
    CHECK(c.matches_answer == 1 && c.level == PR_EASY);
    pr_check("alldiff", PR_KIND_DIAL, 0, 1234, &c);
    CHECK(c.error == PR_OK && c.count == 2 && c.unique == 0 && c.level == PR_AMBIGUOUS);
    CHECK(c.solution == 123 && c.matches_answer == 0);               /* first alldiff code is 0123 */
    pr_check("sum>36", PR_KIND_DIAL, 0, 0, &c);
    CHECK(c.count == 0 && c.solution == PR_NO_SOLUTION && c.level == PR_UNSOLVABLE && c.matches_answer == 0);
    pr_check("d1=", PR_KIND_DIAL, 0, 0, &c);
    CHECK(c.error == PR_ERR_SYNTAX && c.level == PR_LEVEL_NONE && c.count == 0 && c.matches_answer == 0);
    pr_check("w=1", PR_KIND_DIAL, 0, 1, &c);
    CHECK(c.error == PR_ERR_TERM_KIND && c.matches_answer == 0);
    pr_check("w=n-2", PR_KIND_WIRE, 5, 3, &c);
    CHECK(c.error == PR_OK && c.unique == 1 && c.solution == 3 && c.level == PR_EASY && c.matches_answer == 1);
    pr_check("w=n-2", PR_KIND_WIRE, 5, 2, &c);
    CHECK(c.unique == 1 && c.matches_answer == 0);
    pr_check("w>3", PR_KIND_WIRE, 5, 4, &c);
    CHECK(c.count == 2 && c.level == PR_AMBIGUOUS && c.matches_answer == 0);
    pr_check("w=1", PR_KIND_WIRE, 9, 1, &c);
    CHECK(c.error == PR_ERR_ARGS && c.level == PR_LEVEL_NONE);
    pr_check(NULL, PR_KIND_WIRE, 5, 1, &c);
    CHECK(c.error == PR_ERR_ARGS);
    pr_check("w=1", PR_KIND_WIRE, 5, 1, NULL);                       /* must not crash */
    CHECK(strcmp(pr_level_name(PR_HARD), "HARD") == 0);
    CHECK(strcmp(pr_level_name(PR_AMBIGUOUS), "AMBIGUOUS") == 0);
    CHECK(strcmp(pr_error_name(PR_ERR_RANGE), "PR_ERR_RANGE") == 0);
    CHECK(pr_level_name(99) != NULL && pr_error_name(99) != NULL);
}

/* ================= timing (R6) ================= */
static double time_ms(const char *text, int kind, int n, int reps)
{
    PrCheck c;
    const clock_t t0 = clock();
    for (int i = 0; i < reps; i++) {
        pr_check(text, kind, n, 0, &c);
    }
    const double ms = (double)(clock() - t0) * 1000.0 / CLOCKS_PER_SEC / reps;
    return ms;
}

static void test_timing(void)
{
    /* slowest shapes: the solution is the last code (no early exit at limit 2), every rule is
     * evaluated for every code, and the solver has pair rules to filter */
    static const char *const heavy[] = {
        "sum>=0;sum>=0;sum>=0;sum>=0;sum>=0;sum=36",
        "d1*d2*d3*d4+d1*d2*d3>=0;d1*d2*d3*d4+d1*d2*d3>=0;d1*d2*d3*d4+d1*d2*d3>=0;"
        "d1*d2*d3*d4+d1*d2*d3>=0;sum>=0;sum=36",
        "d1+d2=9;d1-d2=1;d3=2*d4;d3+d4=12",
        "sum=17;d4-d1=1;alldiff;d1-d2=4",
        "d3-d2=4;sum=11;alldiff;d1!=1;d4+d3=7",
    };
    double worst = 0.0;
    for (size_t i = 0; i < sizeof heavy / sizeof heavy[0]; i++) {
        PrSet s;
        CHECK_TEXT(parse_dial(heavy[i], &s) == PR_OK, heavy[i]);
        const double ms = time_ms(heavy[i], PR_KIND_DIAL, 0, 20);
        if (ms > worst) worst = ms;
        printf("  timing dial set %u: %.3f ms (count + rate)\n", (unsigned)i, ms);
    }
    const double wire_ms = time_ms("even(w);w>3;w<6;w>1;w<7;odd(w+1)", PR_KIND_WIRE, 6, 200);
    printf("  timing wire set: %.3f ms\n", wire_ms);
    printf("  slowest dial set: %.3f ms (budget 50 ms)\n", worst);
    CHECK(worst < 50.0);
    CHECK(wire_ms < 50.0);
    /* the same budget over the whole random corpus: no single check may be slow */
    double corpus_worst = 0.0;
    for (int it = 0; it < 100; it++) {
        TSet ts;
        t_make_set(&ts, false, true, NULL, NULL);
        if (ts.nr == 0) continue;
        const std::string text = t_text(&ts);
        PrSet s;
        if (parse_dial(text.c_str(), &s) != PR_OK) continue;
        const clock_t t0 = clock();
        (void)pr_rate(&s);
        const double ms = (double)(clock() - t0) * 1000.0 / CLOCKS_PER_SEC;
        if (ms > corpus_worst) corpus_worst = ms;
    }
    printf("  slowest random-corpus rating: %.3f ms\n", corpus_worst);
    CHECK(corpus_worst < 50.0);
    CHECK(sizeof(PrSet) <= 1200);                                     /* caller-stack size of one set */
    printf("  sizeof(PrSet) = %u bytes\n", (unsigned)sizeof(PrSet));
}

int main(void)
{
    printf("test_rules: parser\n");
    test_parser_valid();
    test_parser_whitespace_and_trailing();
    test_parser_precedence();
    test_parser_relations();
    test_parser_errors();
    test_parser_error_positions();
    test_parser_limits();
    printf("test_rules: counting (hand-written and random vs independent evaluator)\n");
    test_counting();
    test_random_counting();
    printf("test_rules: rater\n");
    test_rater_dial();
    test_rater_ambiguous_unsolvable();
    test_rater_wire();
    test_random_rater();
    test_random_unique_rater();
    test_independent_tiers();
    test_parse_rel_no_overread();
    test_check();
    printf("test_rules: timing\n");
    test_timing();
    printf("test_rules: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
