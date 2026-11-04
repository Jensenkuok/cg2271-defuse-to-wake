/*
 * Defuse to Wake - puzzle rule language (see puzzle_rules.h for the language and the tiers).
 *
 * RTOS note: this runs in the network task (lowest priority, latency tolerant) while puzzles
 * are generated or checked (bank refill, AI upgrade), never at alarm time. Worst case is 10,000 codes x 6 rules x ~128 nodes, a few ms on
 * the host and tens of ms on the ESP32-S2. No recursion beyond the expression grammar (parser:
 * PR_MAX_DEPTH parentheses; evaluator: tree depth <= PR_MAX_NODES but small in practice), no heap.
 */
#include "puzzle_rules.h"

#include <stddef.h>

/* ======================================================================================== */
/* Parser                                                                                   */
/* ======================================================================================== */

#define PR_WORD_MAX         8       /* longest name is "alldiff" (7) + NUL */
#define PR_SUM_MAX          36      /* 4 * 9 */

namespace {

struct Val {                        /* a parsed (sub-)expression and its value interval */
    uint8_t node;
    int32_t lo;
    int32_t hi;
};

struct Parser {
    const char *text;
    int pos;
    int depth;
    int error;
    int error_pos;
    int kind;
    int wire_count;
    uint8_t vars;                   /* digits used by the rule being parsed */
    PrSet *set;
};

Val zero_val()
{
    Val v;
    v.node = 0;
    v.lo = 0;
    v.hi = 0;
    return v;
}

void fail(Parser *p, int error)
{
    if (p->error == PR_OK) {
        p->error = error;
        p->error_pos = p->pos;
    }
}

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
bool is_digit(char c) { return c >= '0' && c <= '9'; }
bool is_lower(char c) { return c >= 'a' && c <= 'z'; }

/* Every character the language uses (whitespace is handled separately). */
bool is_legal(char c)
{
    return is_digit(c) || is_lower(c) || c == '+' || c == '-' || c == '*' || c == '(' || c == ')' ||
           c == '=' || c == '!' || c == '<' || c == '>' || c == ';' || c == '\0';
}

void skip_ws(Parser *p)
{
    while (is_space(p->text[p->pos])) {
        p->pos++;
    }
}

char peek(Parser *p)
{
    skip_ws(p);
    return p->text[p->pos];
}

bool at_end(Parser *p) { return peek(p) == '\0'; }

bool str_eq(const char *a, const char *b)
{
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/* Reads the identifier at the cursor without consuming it. Returns its length (0 = none). */
int peek_word(Parser *p, char word[PR_WORD_MAX])
{
    skip_ws(p);
    int n = 0;
    int i = p->pos;
    word[0] = '\0';
    if (!is_lower(p->text[i])) {
        return 0;
    }
    while (is_lower(p->text[i]) || is_digit(p->text[i])) {
        if (n < PR_WORD_MAX - 1) {
            word[n] = p->text[i];
        }
        n++;
        i++;
    }
    if (n >= PR_WORD_MAX) {         /* longer than any name: unknown, keep it NUL-terminated */
        word[0] = '?';
        word[1] = '\0';
    } else {
        word[n] = '\0';
    }
    return n;
}

bool expect(Parser *p, char c)
{
    const char got = peek(p);
    if (got != c) {
        fail(p, is_legal(got) ? PR_ERR_SYNTAX : PR_ERR_CHAR);
        return false;
    }
    p->pos++;
    return true;
}

int new_node(Parser *p, uint8_t op, uint8_t a, uint8_t b, int16_t value)
{
    PrSet *s = p->set;
    if (s->node_count >= PR_MAX_NODES) {
        fail(p, PR_ERR_TOO_COMPLEX);
        return 0;
    }
    PrNode *n = &s->nodes[s->node_count];
    n->op = op;
    n->a = a;
    n->b = b;
    n->value = value;
    return s->node_count++;
}

Val make_leaf(Parser *p, uint8_t op, uint8_t a, int16_t value, int32_t lo, int32_t hi)
{
    Val v;
    v.node = (uint8_t)new_node(p, op, a, 0, value);
    v.lo = lo;
    v.hi = hi;
    return v;
}

int64_t min64(int64_t a, int64_t b) { return a < b ? a : b; }
int64_t max64(int64_t a, int64_t b) { return a > b ? a : b; }

/* Combines two values with + - * ; the interval check keeps every runtime value inside int32. */
Val make_binary(Parser *p, uint8_t op, Val l, Val r)
{
    int64_t lo = 0;
    int64_t hi = 0;
    if (op == PR_N_ADD) {
        lo = (int64_t)l.lo + r.lo;
        hi = (int64_t)l.hi + r.hi;
    } else if (op == PR_N_SUB) {
        lo = (int64_t)l.lo - r.hi;
        hi = (int64_t)l.hi - r.lo;
    } else {
        const int64_t a = (int64_t)l.lo * r.lo;
        const int64_t b = (int64_t)l.lo * r.hi;
        const int64_t c = (int64_t)l.hi * r.lo;
        const int64_t d = (int64_t)l.hi * r.hi;
        lo = min64(min64(a, b), min64(c, d));
        hi = max64(max64(a, b), max64(c, d));
    }
    if (lo < -PR_VALUE_LIMIT || hi > PR_VALUE_LIMIT) {
        fail(p, PR_ERR_RANGE);
        return zero_val();
    }
    Val v;
    v.node = (uint8_t)new_node(p, op, l.node, r.node, 0);
    v.lo = (int32_t)lo;
    v.hi = (int32_t)hi;
    return v;
}

Val parse_expr(Parser *p);

Val parse_number(Parser *p)
{
    int32_t value = 0;
    while (is_digit(p->text[p->pos])) {
        value = value * 10 + (p->text[p->pos] - '0');
        if (value > PR_CONST_MAX) {
            fail(p, PR_ERR_NUMBER);
            return zero_val();
        }
        p->pos++;
    }
    return make_leaf(p, PR_N_CONST, 0, (int16_t)value, value, value);
}

/* A name used as a term. The kind check comes first so that `w` in a dial set is a kind error. */
Val parse_name(Parser *p)
{
    char word[PR_WORD_MAX];
    const int len = peek_word(p, word);
    const bool dial = (p->kind == PR_KIND_DIAL);
    Val v = zero_val();
    if (word[0] == 'd' && len == 2 && word[1] >= '1' && word[1] <= '4') {
        if (!dial) {
            fail(p, PR_ERR_TERM_KIND);
            return v;
        }
        const uint8_t idx = (uint8_t)(word[1] - '1');
        p->vars = (uint8_t)(p->vars | (1u << idx));
        p->pos += len;
        return make_leaf(p, PR_N_DIGIT, idx, 0, 0, PR_DIGIT_VALUES - 1);
    }
    if (str_eq(word, "sum")) {
        if (!dial) {
            fail(p, PR_ERR_TERM_KIND);
            return v;
        }
        p->vars = 0x0F;
        p->pos += len;
        return make_leaf(p, PR_N_SUM, 0, 0, 0, PR_SUM_MAX);
    }
    if (str_eq(word, "w") || str_eq(word, "n")) {
        if (dial) {
            fail(p, PR_ERR_TERM_KIND);
            return v;
        }
        const bool is_w = (word[0] == 'w');
        p->pos += len;
        if (is_w) {
            return make_leaf(p, PR_N_WIRE, 0, 0, 1, p->wire_count);
        }
        return make_leaf(p, PR_N_CONST, 0, (int16_t)p->wire_count, p->wire_count, p->wire_count);
    }
    if (str_eq(word, "alldiff") || str_eq(word, "odd") || str_eq(word, "even")) {
        fail(p, PR_ERR_SYNTAX);     /* a predicate cannot be used as a value */
        return v;
    }
    fail(p, PR_ERR_UNKNOWN_TERM);
    return v;
}

Val parse_factor(Parser *p)
{
    const char c = peek(p);
    if (p->error != PR_OK) {
        return zero_val();
    }
    if (is_digit(c)) {
        return parse_number(p);
    }
    if (is_lower(c)) {
        return parse_name(p);
    }
    if (c == '(') {
        if (p->depth >= PR_MAX_DEPTH) {
            fail(p, PR_ERR_TOO_COMPLEX);
            return zero_val();
        }
        p->pos++;
        p->depth++;
        const Val v = parse_expr(p);
        p->depth--;
        if (p->error == PR_OK) {
            expect(p, ')');
        }
        return v;
    }
    if (c == '\0' || c == ';' || c == ')' || c == '+' || c == '-' || c == '*' ||
        c == '=' || c == '!' || c == '<' || c == '>') {
        fail(p, PR_ERR_SYNTAX);
    } else {
        fail(p, PR_ERR_CHAR);
    }
    return zero_val();
}

Val parse_term(Parser *p)
{
    Val v = parse_factor(p);
    while (p->error == PR_OK && peek(p) == '*') {
        p->pos++;
        const Val r = parse_factor(p);
        if (p->error != PR_OK) {
            break;
        }
        v = make_binary(p, PR_N_MUL, v, r);
    }
    return v;
}

Val parse_expr(Parser *p)
{
    Val v = parse_term(p);
    while (p->error == PR_OK && (peek(p) == '+' || peek(p) == '-')) {
        const uint8_t op = (p->text[p->pos] == '+') ? (uint8_t)PR_N_ADD : (uint8_t)PR_N_SUB;
        p->pos++;
        const Val r = parse_term(p);
        if (p->error != PR_OK) {
            break;
        }
        v = make_binary(p, op, v, r);
    }
    return v;
}

/* Reads one of = != < > <= >= . Returns false (error set) when none is there. */
bool parse_rel(Parser *p, uint8_t *rel)
{
    const char c = peek(p);
    /* Reading text[pos + 1] is only legal when c is not the terminator, so it is done after the
     * end check and only for the characters that can start a two-character relation. */
    const char next = (c == '!' || c == '<' || c == '>') ? p->text[p->pos + 1] : 'x';  /* 'x': never a relation character */
    if (c == '=') {
        *rel = PR_REL_EQ;
        p->pos += 1;
    } else if (c == '!' && next == '=') {
        *rel = PR_REL_NE;
        p->pos += 2;
    } else if (c == '<') {
        *rel = (next == '=') ? (uint8_t)PR_REL_LE : (uint8_t)PR_REL_LT;
        p->pos += (next == '=') ? 2 : 1;
    } else if (c == '>') {
        *rel = (next == '=') ? (uint8_t)PR_REL_GE : (uint8_t)PR_REL_GT;
        p->pos += (next == '=') ? 2 : 1;
    } else if (c == '!' || c == '\0' || c == ';' || c == ')' || c == '+' || c == '-' || c == '*' ||
               is_digit(c) || is_lower(c) || c == '(') {
        fail(p, PR_ERR_SYNTAX);
        return false;
    } else {
        fail(p, PR_ERR_CHAR);
        return false;
    }
    return true;
}

void parse_rule(Parser *p)
{
    PrSet *s = p->set;
    if (s->rule_count >= PR_MAX_RULES) {
        fail(p, PR_ERR_TOO_MANY_RULES);
        return;
    }
    PrRule *r = &s->rules[s->rule_count];
    r->kind = PR_R_REL;
    r->rel = 0;
    r->lhs = 0;
    r->rhs = 0;
    p->vars = 0;

    char word[PR_WORD_MAX];
    const int len = peek_word(p, word);
    if (len > 0 && str_eq(word, "alldiff")) {
        if (p->kind != PR_KIND_DIAL) {
            fail(p, PR_ERR_TERM_KIND);
            return;
        }
        p->pos += len;
        r->kind = PR_R_ALLDIFF;
        p->vars = 0x0F;
    } else if (len > 0 && (str_eq(word, "odd") || str_eq(word, "even"))) {
        r->kind = (word[0] == 'o') ? (uint8_t)PR_R_ODD : (uint8_t)PR_R_EVEN;
        p->pos += len;
        if (!expect(p, '(')) {
            return;
        }
        if (p->depth >= PR_MAX_DEPTH) {
            fail(p, PR_ERR_TOO_COMPLEX);
            return;
        }
        p->depth++;
        const Val v = parse_expr(p);
        p->depth--;
        if (p->error != PR_OK || !expect(p, ')')) {
            return;
        }
        r->lhs = v.node;
    } else {
        const Val l = parse_expr(p);
        if (p->error != PR_OK || !parse_rel(p, &r->rel)) {
            return;
        }
        const Val rv = parse_expr(p);
        if (p->error != PR_OK) {
            return;
        }
        r->lhs = l.node;
        r->rhs = rv.node;
    }
    r->vars = p->vars;
    s->rule_count++;
}

bool text_too_long(const char *text)
{
    for (int i = 0; i <= PR_MAX_CHARS; i++) {
        if (text[i] == '\0') {
            return false;
        }
    }
    return true;
}

}  // namespace

const char *pr_error_name(int error)
{
    switch (error) {
    case PR_OK:                  return "PR_OK";
    case PR_ERR_ARGS:            return "PR_ERR_ARGS";
    case PR_ERR_EMPTY:           return "PR_ERR_EMPTY";
    case PR_ERR_TOO_LONG:        return "PR_ERR_TOO_LONG";
    case PR_ERR_TOO_MANY_RULES:  return "PR_ERR_TOO_MANY_RULES";
    case PR_ERR_CHAR:            return "PR_ERR_CHAR";
    case PR_ERR_SYNTAX:          return "PR_ERR_SYNTAX";
    case PR_ERR_NUMBER:          return "PR_ERR_NUMBER";
    case PR_ERR_UNKNOWN_TERM:    return "PR_ERR_UNKNOWN_TERM";
    case PR_ERR_TERM_KIND:       return "PR_ERR_TERM_KIND";
    case PR_ERR_TOO_COMPLEX:     return "PR_ERR_TOO_COMPLEX";
    case PR_ERR_RANGE:           return "PR_ERR_RANGE";
    default:                     return "PR_ERR_?";
    }
}

const char *pr_level_name(int level)
{
    switch (level) {
    case PR_LEVEL_NONE:  return "NONE";
    case PR_EASY:        return "EASY";
    case PR_NORMAL:      return "NORMAL";
    case PR_HARD:        return "HARD";
    case PR_UNSOLVABLE:  return "UNSOLVABLE";
    case PR_AMBIGUOUS:   return "AMBIGUOUS";
    default:             return "?";
    }
}

int pr_parse(const char *text, int kind, int wire_count, PrSet *out_set)
{
    if (out_set == NULL) {
        return PR_ERR_ARGS;
    }
    out_set->kind = (uint8_t)kind;
    out_set->wire_count = 0;
    out_set->rule_count = 0;
    out_set->node_count = 0;
    out_set->error_pos = -1;
    if (text == NULL || (kind != PR_KIND_DIAL && kind != PR_KIND_WIRE)) {
        return PR_ERR_ARGS;
    }
    if (kind == PR_KIND_WIRE) {
        if (wire_count < PR_WIRE_MIN || wire_count > PR_WIRE_MAX) {
            return PR_ERR_ARGS;
        }
        out_set->wire_count = (uint8_t)wire_count;
    }
    if (text_too_long(text)) {
        out_set->error_pos = PR_MAX_CHARS;
        return PR_ERR_TOO_LONG;
    }

    Parser p;
    p.text = text;
    p.pos = 0;
    p.depth = 0;
    p.error = PR_OK;
    p.error_pos = -1;
    p.kind = kind;
    p.wire_count = (kind == PR_KIND_WIRE) ? wire_count : 0;
    p.vars = 0;
    p.set = out_set;

    if (at_end(&p)) {
        out_set->error_pos = p.pos;
        return PR_ERR_EMPTY;
    }
    for (;;) {
        parse_rule(&p);
        if (p.error != PR_OK) {
            break;
        }
        if (at_end(&p)) {
            break;
        }
        if (!expect(&p, ';')) {
            break;
        }
        if (at_end(&p)) {           /* one trailing ';' is tolerated */
            break;
        }
    }
    if (p.error != PR_OK) {
        out_set->error_pos = (int16_t)p.error_pos;
        return p.error;
    }
    return PR_OK;
}

/* ======================================================================================== */
/* Evaluation and brute-force counting                                                      */
/* ======================================================================================== */

namespace {

struct Env {
    int32_t d[PR_DIAL_DIGITS];
    int32_t w;
};

int32_t eval_node(const PrSet *s, uint8_t idx, const Env *e)
{
    const PrNode *n = &s->nodes[idx];
    switch (n->op) {
    case PR_N_CONST: return n->value;
    case PR_N_DIGIT: return e->d[n->a];
    case PR_N_SUM:   return e->d[0] + e->d[1] + e->d[2] + e->d[3];
    case PR_N_WIRE:  return e->w;
    case PR_N_ADD:   return eval_node(s, n->a, e) + eval_node(s, n->b, e);
    case PR_N_SUB:   return eval_node(s, n->a, e) - eval_node(s, n->b, e);
    default:         return eval_node(s, n->a, e) * eval_node(s, n->b, e);   /* PR_N_MUL */
    }
}

bool rule_holds(const PrSet *s, const PrRule *r, const Env *e)
{
    switch (r->kind) {
    case PR_R_ALLDIFF:
        for (int i = 0; i < PR_DIAL_DIGITS; i++) {
            for (int j = i + 1; j < PR_DIAL_DIGITS; j++) {
                if (e->d[i] == e->d[j]) {
                    return false;
                }
            }
        }
        return true;
    case PR_R_ODD:
        return (eval_node(s, r->lhs, e) & 1) != 0;
    case PR_R_EVEN:
        return (eval_node(s, r->lhs, e) & 1) == 0;
    default:
        break;
    }
    const int32_t l = eval_node(s, r->lhs, e);
    const int32_t v = eval_node(s, r->rhs, e);
    switch (r->rel) {
    case PR_REL_EQ: return l == v;
    case PR_REL_NE: return l != v;
    case PR_REL_LT: return l < v;
    case PR_REL_GT: return l > v;
    case PR_REL_LE: return l <= v;
    default:        return l >= v;      /* PR_REL_GE */
    }
}

bool all_rules_hold(const PrSet *s, const Env *e)
{
    for (int i = 0; i < s->rule_count; i++) {
        if (!rule_holds(s, &s->rules[i], e)) {
            return false;
        }
    }
    return true;
}

}  // namespace

int pr_count_solutions(const PrSet *set, int *out_first, int limit)
{
    int count = 0;
    int first = PR_NO_SOLUTION;
    if (set != NULL) {
        Env e;
        e.w = 0;
        e.d[0] = e.d[1] = e.d[2] = e.d[3] = 0;
        if (set->kind == PR_KIND_WIRE) {
            for (int w = 1; w <= set->wire_count; w++) {
                e.w = w;
                if (all_rules_hold(set, &e)) {
                    if (count == 0) {
                        first = w;
                    }
                    count++;
                    if (limit > 0 && count >= limit) {
                        break;
                    }
                }
            }
        } else {
            for (int code = 0; code < PR_DIAL_CODES; code++) {
                e.d[0] = code / 1000;
                e.d[1] = (code / 100) % 10;
                e.d[2] = (code / 10) % 10;
                e.d[3] = code % 10;
                if (all_rules_hold(set, &e)) {
                    if (count == 0) {
                        first = code;
                    }
                    count++;
                    if (limit > 0 && count >= limit) {
                        break;
                    }
                }
            }
        }
    }
    if (out_first != NULL) {
        *out_first = first;
    }
    return count;
}

/* ======================================================================================== */
/* Dial solver (the tiers)                                                                  */
/* ======================================================================================== */

namespace {

const uint16_t DOMAIN_FULL = (uint16_t)((1u << PR_DIGIT_VALUES) - 1u);

int bit_count(unsigned m)
{
    int n = 0;
    while (m != 0) {
        n += (int)(m & 1u);
        m >>= 1;
    }
    return n;
}

int low_value(uint16_t m)
{
    for (int v = 0; v < PR_DIGIT_VALUES; v++) {
        if ((m & (1u << v)) != 0) {
            return v;
        }
    }
    return 0;
}

/* Digits a rule depends on whose domain still has more than one value. */
unsigned unassigned_of(const PrRule *r, const uint16_t dom[PR_DIAL_DIGITS])
{
    unsigned u = 0;
    for (int i = 0; i < PR_DIAL_DIGITS; i++) {
        if ((r->vars & (1u << i)) != 0 && bit_count(dom[i]) > 1) {
            u |= 1u << i;
        }
    }
    return u;
}

void env_from(const uint16_t dom[PR_DIAL_DIGITS], Env *e)
{
    e->w = 0;
    for (int i = 0; i < PR_DIAL_DIGITS; i++) {
        e->d[i] = low_value(dom[i]);
    }
}

/* Rule over ONE unassigned digit x: keep only the values of x that satisfy it. */
bool filter_one(const PrSet *s, const PrRule *r, int x, uint16_t dom[PR_DIAL_DIGITS])
{
    Env e;
    env_from(dom, &e);
    uint16_t keep = 0;
    for (int a = 0; a < PR_DIGIT_VALUES; a++) {
        if ((dom[x] & (1u << a)) == 0) {
            continue;
        }
        e.d[x] = a;
        if (rule_holds(s, r, &e)) {
            keep = (uint16_t)(keep | (1u << a));
        }
    }
    const bool changed = (keep != dom[x]);
    dom[x] = keep;
    return changed;
}

/* Rule `ra` (and, when rb >= 0, rule `rb` as well) over the two unassigned digits x, y:
 * keep a of x / b of y only when some partner value satisfies every given rule (arc consistency
 * for one rule, joint filtering of the (a, b) pairs for two). */
bool filter_pair(const PrSet *s, int ra, int rb, int x, int y, uint16_t dom[PR_DIAL_DIGITS])
{
    Env e;
    env_from(dom, &e);
    uint16_t keep_x = 0;
    uint16_t keep_y = 0;
    for (int a = 0; a < PR_DIGIT_VALUES; a++) {
        if ((dom[x] & (1u << a)) == 0) {
            continue;
        }
        for (int b = 0; b < PR_DIGIT_VALUES; b++) {
            if ((dom[y] & (1u << b)) == 0) {
                continue;
            }
            e.d[x] = a;
            e.d[y] = b;
            if (rule_holds(s, &s->rules[ra], &e) && (rb < 0 || rule_holds(s, &s->rules[rb], &e))) {
                keep_x = (uint16_t)(keep_x | (1u << a));
                keep_y = (uint16_t)(keep_y | (1u << b));
            }
        }
    }
    const bool changed = (keep_x != dom[x]) || (keep_y != dom[y]);
    dom[x] = keep_x;
    dom[y] = keep_y;
    return changed;
}

/* The two set bits of a 2-digit unassigned mask. */
void two_digits(unsigned u, int *x, int *y)
{
    *x = -1;
    *y = -1;
    for (int i = 0; i < PR_DIAL_DIGITS; i++) {
        if ((u & (1u << i)) != 0) {
            if (*x < 0) {
                *x = i;
            } else {
                *y = i;
            }
        }
    }
}

bool any_empty(const uint16_t dom[PR_DIAL_DIGITS])
{
    for (int i = 0; i < PR_DIAL_DIGITS; i++) {
        if (dom[i] == 0) {
            return true;
        }
    }
    return false;
}

/* One sweep of single-rule propagation; with pairs also (a) and (b) of the NORMAL tier. */
bool sweep(const PrSet *s, bool pairs, uint16_t dom[PR_DIAL_DIGITS])
{
    bool changed = false;
    for (int i = 0; i < s->rule_count && !any_empty(dom); i++) {
        const PrRule *r = &s->rules[i];
        const unsigned u = unassigned_of(r, dom);
        const int cnt = bit_count(u);
        int x = 0;
        int y = 0;
        if (cnt == 0) {
            /* every digit it uses is fixed: it can only confirm or contradict (empty domain) */
            Env e;
            env_from(dom, &e);
            if (!rule_holds(s, r, &e)) {
                dom[0] = 0;
                changed = true;
            }
        } else if (cnt == 1) {
            two_digits(u, &x, &y);
            changed = filter_one(s, r, x, dom) || changed;
        } else if (pairs && cnt == 2) {
            two_digits(u, &x, &y);
            changed = filter_pair(s, i, -1, x, y, dom) || changed;
        }
    }
    if (!pairs) {
        return changed;
    }
    for (int i = 0; i < s->rule_count; i++) {
        for (int j = i + 1; j < s->rule_count && !any_empty(dom); j++) {
            const unsigned ui = unassigned_of(&s->rules[i], dom);
            if (bit_count(ui) != 2 || ui != unassigned_of(&s->rules[j], dom)) {
                continue;
            }
            int x = 0;
            int y = 0;
            two_digits(ui, &x, &y);
            changed = filter_pair(s, i, j, x, y, dom) || changed;
        }
    }
    return changed;
}

}  // namespace

int pr_propagate_dial(const PrSet *set, int tier, int *out_code)
{
    if (set == NULL || set->kind != PR_KIND_DIAL) {
        return 0;
    }
    uint16_t dom[PR_DIAL_DIGITS];
    for (int i = 0; i < PR_DIAL_DIGITS; i++) {
        dom[i] = DOMAIN_FULL;
    }
    const bool pairs = (tier >= PR_TIER_NORMAL);
    while (!any_empty(dom) && sweep(set, pairs, dom)) {
        /* repeat until no domain changes: every pass removes values, so this terminates */
    }
    int code = 0;
    for (int i = 0; i < PR_DIAL_DIGITS; i++) {
        if (bit_count(dom[i]) != 1) {
            return 0;
        }
        code = code * 10 + low_value(dom[i]);
    }
    if (out_code != NULL) {
        *out_code = code;
    }
    return 1;
}

/* ======================================================================================== */
/* Rating                                                                                   */
/* ======================================================================================== */

namespace {

int rate_dial(const PrSet *set)
{
    if (pr_propagate_dial(set, PR_TIER_EASY, NULL)) {
        return PR_EASY;
    }
    if (pr_propagate_dial(set, PR_TIER_NORMAL, NULL)) {
        return PR_NORMAL;
    }
    return PR_HARD;
}

/* Smallest number of rules that still pins the wire; the set is known to have exactly one
 * solution, so the smallest subset with exactly one solution has that same solution. */
int rate_wire(const PrSet *set)
{
    uint8_t sat[PR_WIRE_MAX + 1];       /* bit i = rule i holds for wire w */
    Env e;
    e.d[0] = e.d[1] = e.d[2] = e.d[3] = 0;
    for (int w = 1; w <= set->wire_count; w++) {
        e.w = w;
        uint8_t m = 0;
        for (int i = 0; i < set->rule_count; i++) {
            if (rule_holds(set, &set->rules[i], &e)) {
                m = (uint8_t)(m | (1u << i));
            }
        }
        sat[w] = m;
    }
    const unsigned subsets = 1u << set->rule_count;
    int best = set->rule_count;
    for (unsigned mask = 1; mask < subsets; mask++) {
        const int size = bit_count(mask);
        if (size >= best) {
            continue;
        }
        int hits = 0;
        for (int w = 1; w <= set->wire_count; w++) {
            if ((sat[w] & mask) == mask) {
                hits++;
            }
        }
        if (hits == 1) {
            best = size;
        }
    }
    if (best <= 1) {
        return PR_EASY;
    }
    return (best == 2) ? PR_NORMAL : PR_HARD;
}

}  // namespace

int pr_rate(const PrSet *set)
{
    if (set == NULL) {
        return PR_LEVEL_NONE;
    }
    const int count = pr_count_solutions(set, NULL, 2);
    if (count == 0) {
        return PR_UNSOLVABLE;
    }
    if (count >= 2) {
        return PR_AMBIGUOUS;
    }
    return (set->kind == PR_KIND_WIRE) ? rate_wire(set) : rate_dial(set);
}

void pr_check(const char *text, int kind, int wire_count, int answer, PrCheck *out)
{
    if (out == NULL) {
        return;
    }
    out->level = PR_LEVEL_NONE;
    out->count = 0;
    out->solution = PR_NO_SOLUTION;
    out->unique = 0;
    out->matches_answer = 0;
    PrSet set;
    out->error = pr_parse(text, kind, wire_count, &set);
    if (out->error != PR_OK) {
        return;
    }
    out->count = pr_count_solutions(&set, &out->solution, 2);
    out->unique = (out->count == 1) ? 1 : 0;
    out->level = pr_rate(&set);
    out->matches_answer = (out->unique != 0 && out->solution == answer) ? 1 : 0;
}
