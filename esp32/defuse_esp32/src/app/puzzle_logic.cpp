/* Defuse to Wake - puzzle pipeline logic (pure). See puzzle_logic.h. */
#include "puzzle_logic.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define THINK_OPEN          "<think>"
#define THINK_CLOSE         "</think>"
#define THINK_OPEN_LEN      7u
#define THINK_CLOSE_LEN     8u
#define JSON_KEY_MAX        32u     /* longer keys never match anything we ask for */
#define JSON_UNICODE_DIGITS 4
#define JSON_ASCII_LIMIT    0x80
#define ASCII_PRINT_FIRST   0x20
#define ASCII_PRINT_LAST    0x7E
#define TOKEN_MAX           16u
#define RENDER_PIECE_MAX    24u
#define DIAL_BASE           10
#define DIAL_CODE_SPACE     10000u
#define TOKEN_VALUE_CAP     100000L /* number tokens saturate here (no overflow on long digit runs) */

static const char *const k_ordinal[] = { "1st", "2nd", "3rd", "4th", "5th", "6th" };
#define ORDINAL_COUNT 6u

static const char *ordinal(unsigned i)
{
    return i < ORDINAL_COUNT ? k_ordinal[i] : "?";
}

static int clamp_i(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* ================= <think> stripping and JSON ================= */
size_t strip_think(const char *text, char *out, size_t out_size)
{
    size_t len = 0u;
    const char *p;
    const char *close;
    const char *open;

    if (out == NULL || out_size == 0u) {
        return 0u;
    }
    out[0] = '\0';
    if (text == NULL) {
        return 0u;
    }
    p = text;
    /* A closing tag with no opening tag before it: all text up to it is reasoning. */
    for (;;) {
        close = strstr(p, THINK_CLOSE);
        if (close == NULL) {
            break;
        }
        open = strstr(p, THINK_OPEN);
        if (open != NULL && open < close) {
            break;
        }
        p = close + THINK_CLOSE_LEN;
    }
    while (*p != '\0') {
        if (strncmp(p, THINK_OPEN, THINK_OPEN_LEN) == 0) {
            close = strstr(p + THINK_OPEN_LEN, THINK_CLOSE);
            if (close == NULL) {
                break;                      /* unterminated: the rest is reasoning */
            }
            p = close + THINK_CLOSE_LEN;
            continue;
        }
        if (len + 1u >= out_size) {
            break;
        }
        out[len++] = *p;
        p++;
    }
    out[len] = '\0';
    return len;
}

bool extract_json_object(const char *text, char *out, size_t out_size)
{
    size_t len = strip_think(text, out, out_size);
    size_t start = 0u;
    size_t i;
    int depth = 0;
    bool in_str = false;
    bool esc = false;

    while (start < len && out[start] != '{') {
        start++;
    }
    for (i = start; i < len; i++) {
        const char c = out[i];

        if (in_str) {
            if (esc) {
                esc = false;
            } else if (c == '\\') {
                esc = true;
            } else if (c == '"') {
                in_str = false;
            }
        } else if (c == '"') {
            in_str = true;
        } else if (c == '{') {
            depth++;
        } else if (c == '}') {
            depth--;
            if (depth == 0) {
                memmove(out, out + start, i - start + 1u);
                out[i - start + 1u] = '\0';
                return true;
            }
        }
    }
    if (out != NULL && out_size > 0u) {
        out[0] = '\0';
    }
    return false;
}

static const char *skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
        p++;
    }
    return p;
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/* p points at the opening quote. Decodes into out (NULL = just skip). Returns the position after
 * the closing quote, NULL if the string is malformed or has a non-ASCII \u escape. */
static const char *json_string(const char *p, char *out, size_t size, bool *overflow)
{
    size_t n = 0u;
    int i;
    int v;
    int h;
    char c;
    char e;

    *overflow = false;
    p++;
    while (*p != '"') {
        c = *p;
        if (c == '\0') {
            return NULL;
        }
        p++;
        if (c == '\\') {
            e = *p;
            if (e == '\0') {
                return NULL;
            }
            p++;
            switch (e) {
            case '"':  c = '"';  break;
            case '\\': c = '\\'; break;
            case '/':  c = '/';  break;
            case 'n':  c = '\n'; break;
            case 'r':  c = '\r'; break;
            case 't':  c = '\t'; break;
            case 'b':  c = '\b'; break;
            case 'f':  c = '\f'; break;
            case 'u':
                v = 0;
                for (i = 0; i < JSON_UNICODE_DIGITS; i++) {
                    h = hex_value(*p);
                    if (h < 0) {
                        return NULL;
                    }
                    v = v * 16 + h;
                    p++;
                }
                if (v == 0 || v >= JSON_ASCII_LIMIT) {
                    return NULL;
                }
                c = (char)v;
                break;
            default:
                return NULL;
            }
        }
        if (out != NULL) {
            if (n + 1u < size) {
                out[n++] = c;
            } else {
                *overflow = true;
            }
        }
    }
    if (out != NULL && size > 0u) {
        out[n] = '\0';
    }
    return p + 1;
}

/* p points at '{' or '['. Returns the position after the matching close, NULL if unbalanced. */
static const char *skip_nested(const char *p)
{
    int depth = 0;
    bool in_str = false;
    bool esc = false;

    for (; *p != '\0'; p++) {
        const char c = *p;

        if (in_str) {
            if (esc) {
                esc = false;
            } else if (c == '\\') {
                esc = true;
            } else if (c == '"') {
                in_str = false;
            }
        } else if (c == '"') {
            in_str = true;
        } else if (c == '{' || c == '[') {
            depth++;
        } else if (c == '}' || c == ']') {
            depth--;
            if (depth == 0) {
                return p + 1;
            }
        }
    }
    return NULL;
}

static bool token_char(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '+' || c == '-' ||
           c == '.' || c == '_';
}

bool json_get_string(const char *json, const char *key, char *out, size_t out_size)
{
    char name[JSON_KEY_MAX];
    const char *p;
    const char *start;
    bool overflow;
    bool match;
    size_t n;

    if (json == NULL || key == NULL || out == NULL || out_size == 0u) {
        return false;
    }
    p = skip_ws(json);
    if (*p != '{') {
        return false;
    }
    p++;
    for (;;) {
        p = skip_ws(p);
        if (*p == ',') {
            p++;
            continue;
        }
        if (*p != '"') {
            return false;                           /* '}' (key not found), end of text or junk */
        }
        p = json_string(p, name, sizeof(name), &overflow);
        if (p == NULL) {
            return false;
        }
        match = !overflow && strcmp(name, key) == 0;
        p = skip_ws(p);
        if (*p != ':') {
            return false;
        }
        p = skip_ws(p + 1);
        if (*p == '"') {
            if (match) {
                return json_string(p, out, out_size, &overflow) != NULL && !overflow;
            }
            p = json_string(p, NULL, 0u, &overflow);
        } else if (*p == '{' || *p == '[') {
            p = skip_nested(p);
        } else {
            start = p;
            while (token_char(*p)) {
                p++;
            }
            n = (size_t)(p - start);
            if (n == 0u) {
                return false;
            }
            if (match) {
                if (n + 1u > out_size) {
                    return false;
                }
                memcpy(out, start, n);
                out[n] = '\0';
                return true;
            }
        }
        if (p == NULL) {
            return false;
        }
    }
}

/* ================= validation ================= */
/* True when the text's digits alone, every other character removed, contain the answer. */
static bool digits_spell(const char *text, const char *answer)
{
    char only[PUZZLE_DIAL_TEXT_MAX + 1];
    size_t n = 0u;

    for (; *text != '\0' && n < sizeof(only) - 1u; text++) {
        if (*text >= '0' && *text <= '9') {
            only[n++] = *text;
        }
    }
    only[n] = '\0';
    return strstr(only, answer) != NULL;
}

static bool is_alnum_char(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

/* "st" "nd" "rd" "th" after a number, then not a letter or digit: an ordinal like 3rd. */
static bool ordinal_suffix_at(const char *p)
{
    const bool suffix = (p[0] == 's' && p[1] == 't') || (p[0] == 'n' && p[1] == 'd') ||
                        (p[0] == 'r' && p[1] == 'd') || (p[0] == 't' && p[1] == 'h');

    return suffix && !is_alnum_char(p[2]);
}

bool puzzle_text_has_number(const char *text, int value)
{
    size_t i = 0u;
    size_t j;
    long number;

    if (text == NULL) {
        return false;
    }
    while (text[i] != '\0') {
        if (text[i] < '0' || text[i] > '9') {
            i++;
            continue;
        }
        number = 0;
        for (j = i; text[j] >= '0' && text[j] <= '9'; j++) {
            number = number * DIAL_BASE + (text[j] - '0');
            if (number > TOKEN_VALUE_CAP) {
                number = TOKEN_VALUE_CAP;                   /* far above any answer: stays a non-match */
            }
        }
        const bool glued_before = (i > 0u && is_alnum_char(text[i - 1u]));
        const bool glued_after = is_alnum_char(text[j]) && !ordinal_suffix_at(text + j);

        if (!glued_before && !glued_after && number == (long)value) {
            return true;
        }
        i = j;
    }
    return false;
}

const char *puzzle_text_problem_name(int problem)
{
    switch (problem) {
    case PTP_OK:            return "ok";
    case PTP_ARGS:          return "bad arguments";
    case PTP_TOO_SHORT:     return "too short";
    case PTP_TOO_LONG:      return "too long";
    case PTP_BAD_CHAR:      return "not plain ASCII";
    case PTP_LEAKS_CODE:    return "prints the code";
    case PTP_LEAKS_DIGITS:  return "spells the code in its digits";
    case PTP_LEAKS_NUMBER:  return "states the wire number";
    default:                return "?";
    }
}

int puzzle_text_problem(const char *text, int kind, const char *answer)
{
    size_t len;
    size_t i;
    size_t max_len;
    unsigned char c;

    if (text == NULL || (kind != PUZZLE_DIAL && kind != PUZZLE_WIRE)) {
        return PTP_ARGS;
    }
    max_len = (kind == PUZZLE_WIRE) ? PUZZLE_WIRE_TEXT_LIMIT : PUZZLE_DIAL_TEXT_LIMIT;
    len = strlen(text);
    if (len < PUZZLE_TEXT_MIN) {
        return PTP_TOO_SHORT;
    }
    if (len > max_len) {
        return PTP_TOO_LONG;
    }
    for (i = 0u; i < len; i++) {
        c = (unsigned char)text[i];
        if (c < ASCII_PRINT_FIRST || c > ASCII_PRINT_LAST) {
            return PTP_BAD_CHAR;
        }
    }
    if (kind == PUZZLE_DIAL) {
        if (answer == NULL || strlen(answer) != PROTO_DIAL_DIGITS) {
            return PTP_ARGS;
        }
        if (strstr(text, answer) != NULL) {
            return PTP_LEAKS_CODE;                  /* the riddle must not just print the code */
        }
        if (digits_spell(text, answer)) {
            return PTP_LEAKS_DIGITS;                /* nor list its digits: "2,5,8,7" or "2 5 8 7" */
        }
    } else if (answer != NULL && answer[0] >= '0' && answer[0] <= '9' &&
               puzzle_text_has_number(text, atoi(answer))) {
        return PTP_LEAKS_NUMBER;                    /* O4: "wire 3", "the 3rd", "n-3"... when 3 is the answer */
    }
    return PTP_OK;
}

bool validate_puzzle(const char *text, int kind, const char *answer)
{
    return puzzle_text_problem(text, kind, answer) == PTP_OK;
}

bool parse_checker_answer(const char *json, int kind, int wire_count, char *out, size_t out_size)
{
    char tok[TOKEN_MAX];
    const char *p;
    size_t len;
    size_t i;
    int value = 0;

    if (out == NULL || out_size == 0u || !json_get_string(json, "answer", tok, sizeof(tok))) {
        return false;
    }
    p = tok;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    len = strlen(p);
    while (len > 0u && (p[len - 1u] == ' ' || p[len - 1u] == '\t')) {
        len--;
    }
    if (len == 0u) {
        return false;
    }
    for (i = 0u; i < len; i++) {
        if (p[i] < '0' || p[i] > '9') {
            return false;
        }
    }
    if (kind == PUZZLE_DIAL) {
        if (len != PROTO_DIAL_DIGITS || out_size < PROTO_DIAL_DIGITS + 1u) {
            return false;
        }
        memcpy(out, p, len);
        out[len] = '\0';
        return true;
    }
    if (kind != PUZZLE_WIRE || len > 2u || wire_count < 1) {
        return false;
    }
    for (i = 0u; i < len; i++) {
        value = value * DIAL_BASE + (p[i] - '0');
    }
    if (value < 1 || value > wire_count) {
        return false;
    }
    return snprintf(out, out_size, "%d", value) > 0;
}

/* ================= answers ================= */
void dial_code_text(const uint8_t code[PROTO_DIAL_DIGITS], char out[PROTO_DIAL_DIGITS + 1])
{
    unsigned i;

    for (i = 0u; i < PROTO_DIAL_DIGITS; i++) {
        out[i] = (char)('0' + (code[i] % DIAL_BASE));
    }
    out[PROTO_DIAL_DIGITS] = '\0';
}

bool puzzle_answer_text(const Puzzle &p, char *out, size_t out_size)
{
    if (out == NULL || out_size == 0u) {
        return false;
    }
    if (p.kind == PUZZLE_DIAL && out_size >= PROTO_DIAL_DIGITS + 1u) {
        dial_code_text(p.dial_code, out);
        return true;
    }
    if (p.kind == PUZZLE_WIRE) {
        return snprintf(out, out_size, "%u", (unsigned)p.wire_k) > 0;
    }
    return false;
}

void dial_code_from_random(uint32_t r, uint8_t code[PROTO_DIAL_DIGITS])
{
    unsigned v = (unsigned)(r % DIAL_CODE_SPACE);
    int i;

    for (i = (int)PROTO_DIAL_DIGITS - 1; i >= 0; i--) {
        code[i] = (uint8_t)(v % DIAL_BASE);
        v /= DIAL_BASE;
    }
}

void wire_pick_from_random(int level, uint32_t r, int *wire_count, int *k)
{
    const LevelSpec spec = difficulty_level_spec(level);
    const unsigned span = (unsigned)spec.wire_max - spec.wire_min + 1u;
    const int n = (int)(spec.wire_min + (r & 0xFFFFu) % span);

    *wire_count = n;
    *k = 1 + (int)((r >> 16) % (uint32_t)n);
}

void trip_band_from_random(uint32_t r, uint8_t *min_cm, uint8_t *max_cm)
{
    const uint8_t lo = (uint8_t)(PUZZLE_TRIP_MIN_BASE_CM + r % PUZZLE_TRIP_START_VALUES);

    *min_cm = lo;
    *max_cm = (uint8_t)(lo + PUZZLE_TRIP_WIDTH_CM);
}

/* ================= dial templates ================= */
static DialRule make_rule(uint8_t rel, uint8_t a, uint8_t b, int c)
{
    DialRule r;

    r.rel = rel;
    r.a = a;
    r.b = b;
    r.c = (int8_t)c;
    return r;
}

/* Describe digit t from the known digit s: "t = s * m" when that is exact and asked for, else
 * "t = s + c" with a signed c (always possible, digits are 0..9). */
static DialRule relate(const uint8_t d[PROTO_DIAL_DIGITS], uint8_t t, uint8_t s, bool prefer_mul)
{
    if (prefer_mul && d[s] >= 1u && d[t] >= 2u * d[s] && (d[t] % d[s]) == 0u) {
        return make_rule(DREL_MUL, t, s, d[t] / d[s]);
    }
    return make_rule(DREL_ADD, t, s, (int)d[t] - (int)d[s]);
}

/* Family 0 (forward) and 1 (backward): one anchor digit is given, the others follow from it. */
static void build_chain(DialClue *cl, const uint8_t d[PROTO_DIAL_DIGITS], bool backward, unsigned sub)
{
    uint8_t order[PROTO_DIAL_DIGITS];
    const bool prefer_mul = (sub & 1u) != 0u;
    const bool from_anchor = (sub & 2u) != 0u;
    unsigned i;
    uint8_t src;

    for (i = 0u; i < PROTO_DIAL_DIGITS; i++) {
        order[i] = (uint8_t)(backward ? (PROTO_DIAL_DIGITS - 1u - i) : i);
    }
    cl->rule[0] = make_rule(DREL_EQ, order[0], 0, d[order[0]]);
    for (i = 1u; i < PROTO_DIAL_DIGITS; i++) {
        src = (from_anchor && i >= 2u) ? order[0] : order[i - 1u];
        cl->rule[i] = relate(d, order[i], src, prefer_mul);
    }
    cl->n = PROTO_DIAL_DIGITS;
}

/* Family 2: all digits as offsets from a pivot digit, plus the sum of all four. */
static void build_pivot(DialClue *cl, const uint8_t d[PROTO_DIAL_DIGITS], unsigned sub)
{
    const uint8_t pivot = (uint8_t)(sub % PROTO_DIAL_DIGITS);
    unsigned i;
    unsigned n = 1u;
    int sum = 0;

    for (i = 0u; i < PROTO_DIAL_DIGITS; i++) {
        sum += d[i];
    }
    cl->rule[0] = make_rule(DREL_SUM_ALL, 0, 0, sum);
    for (i = 0u; i < PROTO_DIAL_DIGITS; i++) {
        if (i != pivot) {
            cl->rule[n++] = relate(d, (uint8_t)i, pivot, false);
        }
    }
    cl->n = (uint8_t)n;
}

/* Family 3: two digit pairs, each given by its sum and its difference. */
static void build_pairs(DialClue *cl, const uint8_t d[PROTO_DIAL_DIGITS], unsigned sub)
{
    static const uint8_t k_pairing[3][4] = { { 0, 1, 2, 3 }, { 0, 2, 1, 3 }, { 0, 3, 1, 2 } };
    const uint8_t *pr = k_pairing[(sub == 3u) ? 0u : sub];
    unsigned p;
    unsigned n = 0u;
    uint8_t x;
    uint8_t y;
    uint8_t hi;
    uint8_t lo;

    for (p = 0u; p < 2u; p++) {
        const unsigned at = (sub == 3u) ? (1u - p) : p;     /* variant 3 lists the pairs backwards */

        x = pr[2u * at];
        y = pr[2u * at + 1u];
        hi = (d[x] >= d[y]) ? x : y;
        lo = (hi == x) ? y : x;
        cl->rule[n++] = make_rule(DREL_SUM_PAIR, x, y, d[x] + d[y]);
        cl->rule[n++] = make_rule(DREL_DIFF_PAIR, hi, lo, (int)d[hi] - (int)d[lo]);
    }
    cl->n = (uint8_t)n;
}

static DialClue build_variant(const uint8_t code[PROTO_DIAL_DIGITS], unsigned variant)
{
    DialClue cl;
    uint8_t d[PROTO_DIAL_DIGITS];
    const unsigned family = variant % DIAL_FAMILIES;
    const unsigned sub = (variant / DIAL_FAMILIES) % 4u;
    unsigned i;

    memset(&cl, 0, sizeof(cl));
    for (i = 0u; i < PROTO_DIAL_DIGITS; i++) {
        d[i] = (uint8_t)(code[i] % DIAL_BASE);
    }
    cl.family = (uint8_t)family;
    switch (family) {
    case 0:
        build_chain(&cl, d, false, sub);
        break;
    case 1:
        build_chain(&cl, d, true, sub);
        break;
    case 2:
        build_pivot(&cl, d, sub);
        break;
    default:
        build_pairs(&cl, d, sub);
        break;
    }
    return cl;
}

/* Some codes make a clue's digits-only form spell the code by coincidence (the constants happen to
 * line up, e.g. 0000 in the pair family). Those are skipped: the other details of the same family
 * come first, then the other families, so the result is still a true unique-solution clue. */
DialClue dial_clue_build(const uint8_t code[PROTO_DIAL_DIGITS], unsigned variant)
{
    char text[PUZZLE_TEXT_BUF];
    char answer[PROTO_DIAL_DIGITS + 1];
    unsigned fam;
    unsigned sub;
    DialClue cl;

    dial_code_text(code, answer);
    for (fam = 0u; fam < DIAL_FAMILIES; fam++) {
        for (sub = 0u; sub < PUZZLE_VARIANTS / DIAL_FAMILIES; sub++) {
            const unsigned v = (variant + fam) % DIAL_FAMILIES +
                               DIAL_FAMILIES * ((variant / DIAL_FAMILIES + sub) % (PUZZLE_VARIANTS / DIAL_FAMILIES));

            cl = build_variant(code, v);
            if (dial_clue_render(cl, text, sizeof(text)) > 0u && validate_puzzle(text, PUZZLE_DIAL, answer)) {
                return cl;
            }
        }
    }
    return build_variant(code, variant);
}

static void render_rule(const DialRule &r, char *piece, size_t size)
{
    const int c = r.c;

    switch (r.rel) {
    case DREL_EQ:
        snprintf(piece, size, "%s=%d", ordinal(r.a), c);
        break;
    case DREL_ADD:
        if (c == 0) {
            snprintf(piece, size, "%s=%s", ordinal(r.a), ordinal(r.b));
        } else {
            snprintf(piece, size, "%s=%s%c%d", ordinal(r.a), ordinal(r.b), c > 0 ? '+' : '-', c > 0 ? c : -c);
        }
        break;
    case DREL_MUL:
        snprintf(piece, size, "%s=%s*%d", ordinal(r.a), ordinal(r.b), c);
        break;
    case DREL_SUM_ALL:
        snprintf(piece, size, "Sum %d", c);
        break;
    case DREL_SUM_PAIR:
        snprintf(piece, size, "%s+%s=%d", ordinal(r.a), ordinal(r.b), c);
        break;
    case DREL_DIFF_PAIR:
        snprintf(piece, size, "%s-%s=%d", ordinal(r.a), ordinal(r.b), c);
        break;
    default:
        snprintf(piece, size, "?");
        break;
    }
}

size_t dial_clue_render(const DialClue &clue, char *out, size_t out_size)
{
    char piece[RENDER_PIECE_MAX];
    size_t len = 0u;
    size_t need;
    unsigned i;

    if (out == NULL || out_size == 0u) {
        return 0u;
    }
    out[0] = '\0';
    for (i = 0u; i < clue.n && i < DIAL_MAX_RULES; i++) {
        render_rule(clue.rule[i], piece, sizeof(piece));
        need = strlen(piece) + (i > 0u ? 2u : 0u);
        if (len + need + 1u > out_size) {
            out[0] = '\0';
            return 0u;
        }
        if (i > 0u) {
            memcpy(out + len, "; ", 2u);
            len += 2u;
        }
        memcpy(out + len, piece, strlen(piece));
        len += strlen(piece);
        out[len] = '\0';
    }
    return len;
}

size_t dial_template(const uint8_t code[PROTO_DIAL_DIGITS], unsigned variant, char *out, size_t out_size)
{
    return dial_clue_render(dial_clue_build(code, variant), out, out_size);
}

/* ================= wire templates ================= */
WireClue wire_clue_build(int k, int n, unsigned variant)
{
    WireClue w;
    const unsigned sub = (variant / WIRE_FAMILIES) % 4u;
    int a;

    memset(&w, 0, sizeof(w));
    n = clamp_i(n, (int)GAME_MIN_WIRES, (int)GAME_MAX_WIRES);
    k = clamp_i(k, 1, n);
    w.n = (uint8_t)n;
    w.family = (uint8_t)(variant % WIRE_FAMILIES);
    switch (w.family) {
    case 0:
        w.a = (uint8_t)k;
        break;
    case 1:
        w.a = (uint8_t)(n - k + 1);
        break;
    case 2:
        if (sub == 1u && k >= 2) {                      /* a + b */
            w.op = WOP_ADD;
            w.a = (uint8_t)(k / 2);
            w.b = (uint8_t)(k - k / 2);
        } else if (sub == 2u) {                         /* a * b, a the largest divisor <= sqrt(k) */
            w.op = WOP_MUL;
            w.a = 1;
            for (a = 2; a * a <= k; a++) {
                if (k % a == 0) {
                    w.a = (uint8_t)a;
                }
            }
            w.b = (uint8_t)(k / w.a);
        } else {                                        /* a - b, b = 1..3 */
            w.op = WOP_SUB;
            w.b = (uint8_t)((sub == 3u) ? (2 + (k % 2)) : (1 + (k % 3)));
            w.a = (uint8_t)(k + w.b);
        }
        break;
    default:
        w.from_right = (uint8_t)(sub & 1u);
        w.a = (uint8_t)(w.from_right ? (n - k) : (k - 1));
        break;
    }
    return w;
}

size_t wire_clue_render(const WireClue &w, char *out, size_t out_size)
{
    int len = 0;

    if (out == NULL || out_size == 0u) {
        return 0u;
    }
    switch (w.family) {
    case 0:
        len = snprintf(out, out_size, "%u wires. Cut the %s from the left", (unsigned)w.n, ordinal(w.a - 1u));
        break;
    case 1:
        len = snprintf(out, out_size, "%u wires. Cut the %s from the right", (unsigned)w.n, ordinal(w.a - 1u));
        break;
    case 2:
        len = snprintf(out, out_size, "%u wires, 1 is leftmost. Cut wire %u%c%u", (unsigned)w.n, (unsigned)w.a,
                       w.op == WOP_ADD ? '+' : (w.op == WOP_MUL ? '*' : '-'), (unsigned)w.b);
        break;
    default:
        len = snprintf(out, out_size, "%u wires. Skip %u from %s, cut next", (unsigned)w.n, (unsigned)w.a,
                       w.from_right ? "right" : "left");
        break;
    }
    if (len <= 0 || (size_t)len >= out_size) {
        out[0] = '\0';
        return 0u;
    }
    return (size_t)len;
}

size_t wire_template(int k, int n, unsigned variant, char *out, size_t out_size)
{
    return wire_clue_render(wire_clue_build(k, n, variant), out, out_size);
}

void puzzle_fill_bank_dial(Puzzle *p, const uint8_t code[PROTO_DIAL_DIGITS], unsigned variant)
{
    unsigned i;

    memset(p, 0, sizeof(*p));
    p->kind = PUZZLE_DIAL;
    p->source = PUZZLE_SRC_BANK;
    for (i = 0u; i < PROTO_DIAL_DIGITS; i++) {
        p->dial_code[i] = (uint8_t)(code[i] % DIAL_BASE);
    }
    (void)dial_template(p->dial_code, variant, p->text, sizeof(p->text));
}

void puzzle_fill_bank_wire(Puzzle *p, int wire_count, int k, unsigned variant)
{
    memset(p, 0, sizeof(*p));
    p->kind = PUZZLE_WIRE;
    p->source = PUZZLE_SRC_BANK;
    p->wire_count = (uint8_t)clamp_i(wire_count, (int)GAME_MIN_WIRES, (int)GAME_MAX_WIRES);
    p->wire_k = (uint8_t)clamp_i(k, 1, p->wire_count);
    (void)wire_template(p->wire_k, p->wire_count, variant, p->text, PUZZLE_WIRE_TEXT_MAX + 1u);
}

void puzzle_set_fill_bank(PuzzleSet *s, const uint32_t rnd[PUZZLE_BANK_WORDS])
{
    unsigned i;
    uint8_t code[PROTO_DIAL_DIGITS];
    int n;
    int k;

    memset(s, 0, sizeof(*s));
    s->magic = PUZZLE_MAGIC;
    for (i = 0u; i < PUZZLE_PAIRS; i++) {
        dial_code_from_random(rnd[3u * i], code);
        puzzle_fill_bank_dial(&s->dial[i], code, rnd[3u * i + 2u] % PUZZLE_VARIANTS);
        wire_pick_from_random(PR_NORMAL, rnd[3u * i + 1u], &n, &k);
        puzzle_fill_bank_wire(&s->wire[i], n, k, (rnd[3u * i + 2u] >> 8) % PUZZLE_VARIANTS);
    }
}

static bool text_ok(const char *text, size_t cap, size_t max_len)
{
    return memchr(text, '\0', cap) != NULL && strlen(text) <= max_len;
}

bool puzzle_set_valid(const PuzzleSet &s)
{
    unsigned i;
    unsigned d;

    if (s.magic != PUZZLE_MAGIC) {
        return false;
    }
    for (i = 0u; i < PUZZLE_PAIRS; i++) {
        const Puzzle &dp = s.dial[i];
        const Puzzle &wp = s.wire[i];

        if (dp.level > PUZZLE_LEVEL_MAX || wp.level > PUZZLE_LEVEL_MAX) {
            return false;
        }
        if (dp.kind != PUZZLE_DIAL || wp.kind != PUZZLE_WIRE || dp.source > PUZZLE_SRC_AI || wp.source > PUZZLE_SRC_AI) {
            return false;
        }
        for (d = 0u; d < PROTO_DIAL_DIGITS; d++) {
            if (dp.dial_code[d] >= DIAL_BASE) {
                return false;
            }
        }
        if (wp.wire_count < GAME_MIN_WIRES || wp.wire_count > GAME_MAX_WIRES || wp.wire_k < 1u ||
            wp.wire_k > wp.wire_count) {
            return false;
        }
        if (!text_ok(dp.text, sizeof(dp.text), PUZZLE_DIAL_TEXT_MAX) ||
            !text_ok(wp.text, sizeof(wp.text), PUZZLE_WIRE_TEXT_MAX) ||
            !memchr(dp.setter, '\0', sizeof(dp.setter)) || !memchr(dp.checker, '\0', sizeof(dp.checker)) ||
            !memchr(wp.setter, '\0', sizeof(wp.setter)) || !memchr(wp.checker, '\0', sizeof(wp.checker))) {
            return false;
        }
    }
    return true;
}

/* ================= BOMB_CONFIG ================= */
/* Each Fisher-Yates swap draws its index from the top half of a tiny LCG (Numerical Recipes constants), seeded
 * with the random word: a pure function of rnd, so the host tests can replay it. The low bits of an LCG are
 * weak, hence the shift by ORDER_LCG_SHIFT before the modulo. */
#define ORDER_LCG_MUL    1664525u
#define ORDER_LCG_ADD    1013904223u
#define ORDER_LCG_SHIFT  16u

void module_order_shuffle(uint32_t rnd, uint8_t out[PROTO_MAX_MODULES])
{
    uint32_t state = rnd;
    unsigned i;

    out[0] = PROTO_MODULE_DIAL;
    out[1] = PROTO_MODULE_WIRES;
    out[2] = PROTO_MODULE_TRIPWIRE;
    out[3] = PROTO_MODULE_LIGHTS;                 /* the wake-up module: every bomb has all four sensors */
    for (i = PROTO_MAX_MODULES - 1u; i > 0u; i--) {
        unsigned j;
        uint8_t tmp;

        state = state * ORDER_LCG_MUL + ORDER_LCG_ADD;
        j = (unsigned)((state >> ORDER_LCG_SHIFT) % (i + 1u));
        tmp = out[i];
        out[i] = out[j];
        out[j] = tmp;
    }
}

void puzzle_build_config(const Difficulty &d, const Puzzle &dial, const Puzzle &wire, uint32_t trip_rnd,
                         uint32_t order_rnd, proto_bomb_config_t *out)
{
    unsigned i;

    memset(out, 0, sizeof(*out));
    out->module_count = (uint8_t)clamp_i(d.module_count, (int)GAME_MIN_MODULES, (int)PROTO_MAX_MODULES);
    /* All four modules are shuffled; if a level ever asks for fewer than four, the MCX plays the first
     * module_count entries. Every level asks for all four today (D5). */
    module_order_shuffle(order_rnd, out->module_order);
    out->time_limit_s = (uint16_t)clamp_i(d.time_limit_s, (int)GAME_MIN_TIME_LIMIT_S, (int)GAME_MAX_TIME_LIMIT_S);
    for (i = 0u; i < PROTO_DIAL_DIGITS; i++) {
        out->dial_code[i] = (uint8_t)(dial.dial_code[i] % DIAL_BASE);
    }
    out->wire_count = (uint8_t)clamp_i(wire.wire_count, (int)GAME_MIN_WIRES, (int)GAME_MAX_WIRES);
    out->correct_wire = (uint8_t)clamp_i((int)wire.wire_k - 1, 0, (int)out->wire_count - 1);
    trip_band_from_random(trip_rnd, &out->trip_min_cm, &out->trip_max_cm);
    out->max_strikes = GAME_DEFAULT_MAX_STRIKES;
}

/* ================= selection rules ================= */
int pair_next_unused(uint8_t spent_mask, int count)
{
    int i;

    for (i = 0; i < count && i < 8; i++) {
        if ((spent_mask & (1u << i)) == 0u) {
            return i;
        }
    }
    return -1;
}

uint8_t pair_mark_spent(uint8_t spent_mask, int idx, int count)
{
    if (idx < 0 || idx >= count || count > 8) {
        return spent_mask;
    }
    return (uint8_t)(spent_mask | (1u << idx));
}

uint8_t pair_clear_spent(uint8_t spent_mask, int idx, int count)
{
    if (idx < 0 || idx >= count || count > 8) {
        return spent_mask;
    }
    return (uint8_t)(spent_mask & ~(1u << idx));
}

int pair_oldest_spent(uint8_t spent_mask, const uint32_t *spend_order, int count)
{
    int best = -1;
    int i;

    for (i = 0; i < count && i < 8; i++) {
        if ((spent_mask & (1u << i)) != 0u && (best < 0 || spend_order[i] < spend_order[best])) {
            best = i;                               /* strict <: a tie keeps the lowest index */
        }
    }
    return best;
}

int pair_pick(uint8_t spent_mask, const uint32_t *spend_order, int count, bool *reused)
{
    const int fresh = pair_next_unused(spent_mask, count);

    if (reused != NULL) {
        *reused = (fresh < 0);
    }
    return fresh >= 0 ? fresh : pair_oldest_spent(spent_mask, spend_order, count);
}

int refill_next_slot(uint8_t spent_mask, uint8_t refilled_slots)
{
    int pair;

    for (pair = 0; pair < PUZZLE_PAIRS; pair++) {
        if ((spent_mask & (1u << pair)) == 0u) {
            continue;
        }
        if ((refilled_slots & (1u << pair)) == 0u) {
            return pair;                            /* the dial slot */
        }
        if ((refilled_slots & (1u << (PUZZLE_PAIRS + pair))) == 0u) {
            return PUZZLE_PAIRS + pair;             /* the wire slot */
        }
    }
    return -1;
}

static const Puzzle &set_slot(const PuzzleSet &s, int slot)
{
    return slot < PUZZLE_PAIRS ? s.dial[slot] : s.wire[slot - PUZZLE_PAIRS];
}

int upgrade_next_slot(const PuzzleSet &s, int target_level, uint8_t spent_mask, uint8_t force_slots, int last_slot)
{
    int step;

    for (step = 1; step <= PUZZLE_SLOTS; step++) {
        const int slot = (last_slot + step + PUZZLE_SLOTS) % PUZZLE_SLOTS;   /* last_slot -1 starts at 0 */
        const Puzzle &p = set_slot(s, slot);

        if ((spent_mask & (1u << (slot % PUZZLE_PAIRS))) != 0u) {
            continue;                               /* a used pair is refilled by the bank, never upgraded */
        }
        if (p.source == PUZZLE_SRC_BANK || (int)p.level != target_level || (force_slots & (1u << slot)) != 0u) {
            return slot;
        }
    }
    return -1;
}

uint8_t puzzle_set_count_ai(const PuzzleSet &s)
{
    uint8_t n = 0u;
    unsigned i;

    for (i = 0u; i < PUZZLE_PAIRS; i++) {
        n = (uint8_t)(n + (s.dial[i].source == PUZZLE_SRC_AI ? 1u : 0u) + (s.wire[i].source == PUZZLE_SRC_AI ? 1u : 0u));
    }
    return n;
}

int checker_order(const char *const *chain, int n, const char *setter, int *order_out)
{
    int count = 0;
    int i;

    for (i = 0; i < n; i++) {
        if (setter == NULL || strcmp(chain[i], setter) != 0) {
            order_out[count++] = i;                 /* O5: the setter's own model is never a checker */
        }
    }
    return count;
}

bool puzzle_set_fresh(uint32_t generated_unix, uint8_t ai_count, uint32_t now_unix, uint32_t max_age_s)
{
    if (ai_count == 0u || generated_unix == 0u) {
        return false;
    }
    if (now_unix == 0u || now_unix < generated_unix) {
        return true;
    }
    return (now_unix - generated_unix) <= max_age_s;
}

bool config_trigger_needed(uint8_t prev_state, uint8_t state, bool session_active)
{
    return session_active && state != prev_state && (state == PROTO_STATE_EXPLODED || state == PROTO_STATE_ARMED);
}

bool bomb_just_started(uint8_t prev_state, uint8_t state)
{
    return state == PROTO_STATE_BOMB_ACTIVE && prev_state != PROTO_STATE_BOMB_ACTIVE;
}
