/* Defuse to Wake - puzzle pipeline logic (pure). See puzzle_logic.h. */
#include "puzzle_logic.h"

#include <stdio.h>
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
#define DIFF_SHOWN_MAX      99u     /* snooze count printed in the explanation */

static const char *const k_ordinal[] = { "1st", "2nd", "3rd", "4th", "5th", "6th" };
#define ORDINAL_COUNT 6u

static const char *ordinal(unsigned i)
{
    return i < ORDINAL_COUNT ? k_ordinal[i] : "?";
}

static unsigned min_u(unsigned a, unsigned b)
{
    return a < b ? a : b;
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
bool validate_puzzle(const char *text, int kind, const char *answer)
{
    size_t len;
    size_t i;
    size_t max_len;
    unsigned char c;

    if (text == NULL || (kind != PUZZLE_DIAL && kind != PUZZLE_WIRE)) {
        return false;
    }
    max_len = (kind == PUZZLE_WIRE) ? PUZZLE_WIRE_TEXT_MAX : PUZZLE_DIAL_TEXT_MAX;
    len = strlen(text);
    if (len < PUZZLE_TEXT_MIN || len > max_len) {
        return false;
    }
    for (i = 0u; i < len; i++) {
        c = (unsigned char)text[i];
        if (c < ASCII_PRINT_FIRST || c > ASCII_PRINT_LAST) {
            return false;
        }
    }
    if (kind == PUZZLE_DIAL) {
        if (answer == NULL || strlen(answer) != PROTO_DIAL_DIGITS) {
            return false;
        }
        if (strstr(text, answer) != NULL) {
            return false;                           /* the riddle must not just print the code */
        }
    }
    return true;
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

void wire_pick_from_random(uint32_t r, int *wire_count, int *k)
{
    const unsigned span = GAME_MAX_WIRES - GAME_MIN_WIRES + 1u;
    const int n = (int)(GAME_MIN_WIRES + (r & 0xFFFFu) % span);

    *wire_count = n;
    *k = 1 + (int)((r >> 16) % (uint32_t)n);
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

DialClue dial_clue_build(const uint8_t code[PROTO_DIAL_DIGITS], unsigned variant)
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
        len = snprintf(out, out_size, "%u wires. Skip %u from the %s, cut the next", (unsigned)w.n, (unsigned)w.a,
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
        wire_pick_from_random(rnd[3u * i + 1u], &n, &k);
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

/* ================= difficulty ================= */
static void explain_add(Difficulty *d, size_t *len, const char *piece)
{
    if (*len > 0u) {
        snprintf(d->explanation + *len, sizeof(d->explanation) - *len, "; ");
        *len = strlen(d->explanation);
    }
    snprintf(d->explanation + *len, sizeof(d->explanation) - *len, "%s", piece);
    *len = strlen(d->explanation);
}

Difficulty difficulty(const HistRecord *recent, int n)
{
    Difficulty d;
    unsigned w = (recent != NULL && n > 0) ? min_u((unsigned)n, DIFF_WINDOW) : 0u;
    unsigned snoozes = 0u;
    unsigned strikes = 0u;
    bool clean = (w >= DIFF_CLEAN_RUN);
    bool more_modules;
    bool pressure;
    int time_s = DIFF_BASE_TIME_S;
    char piece[PUZZLE_EXPLAIN_MAX + 1];
    size_t len = 0u;
    unsigned i;

    memset(&d, 0, sizeof(d));
    for (i = 0u; i < w; i++) {
        snoozes += recent[i].snoozes;
        strikes += recent[i].strikes;
        if (i < DIFF_CLEAN_RUN && recent[i].snoozes != 0u) {
            clean = false;
        }
    }
    more_modules = (snoozes >= DIFF_SNOOZE_LIMIT);
    pressure = (w > 0u && strikes >= DIFF_STRIKE_AVG * w);       /* average strikes >= 2 */
    d.module_count = (uint8_t)(more_modules ? DIFF_BASE_MODULES + 1 : DIFF_BASE_MODULES);
    if (d.module_count > DIFF_MODULES_MAX) {
        d.module_count = DIFF_MODULES_MAX;
    }
    if (pressure) {
        time_s += DIFF_TIME_STEP_S;
    }
    if (clean) {
        time_s -= DIFF_TIME_STEP_S;
    }
    d.time_limit_s = (uint16_t)clamp_i(time_s, DIFF_TIME_MIN_S, DIFF_TIME_MAX_S);

    if (more_modules) {
        snprintf(piece, sizeof(piece), "4 mod: %u snoozes/7d", min_u(snoozes, DIFF_SHOWN_MAX));
        explain_add(&d, &len, piece);
    }
    if (pressure) {
        snprintf(piece, sizeof(piece), "+30s: avg strikes>=%u", (unsigned)DIFF_STRIKE_AVG);
        explain_add(&d, &len, piece);
    }
    if (clean) {
        snprintf(piece, sizeof(piece), "-30s: %u clean", (unsigned)DIFF_CLEAN_RUN);
        explain_add(&d, &len, piece);
    }
    if (len == 0u) {
        snprintf(d.explanation, sizeof(d.explanation), "%s",
                 w == 0u ? "No history yet: standard bomb" : "Steady mornings: standard bomb");
    }
    return d;
}

/* ================= BOMB_CONFIG ================= */
void puzzle_build_config(const Difficulty &d, const Puzzle &dial, const Puzzle &wire, proto_bomb_config_t *out)
{
    unsigned i;

    memset(out, 0, sizeof(*out));
    out->module_count = (uint8_t)clamp_i(d.module_count, (int)GAME_MIN_MODULES, (int)PROTO_MAX_MODULES);
    out->module_order[0] = PROTO_MODULE_DIAL;
    out->module_order[1] = PROTO_MODULE_WIRES;
    out->module_order[2] = PROTO_MODULE_TRIPWIRE;
    out->module_order[3] = PROTO_MODULE_DIAL;
    out->time_limit_s = d.time_limit_s;
    for (i = 0u; i < PROTO_DIAL_DIGITS; i++) {
        out->dial_code[i] = (uint8_t)(dial.dial_code[i] % DIAL_BASE);
    }
    out->wire_count = (uint8_t)clamp_i(wire.wire_count, (int)GAME_MIN_WIRES, (int)GAME_MAX_WIRES);
    out->correct_wire = (uint8_t)clamp_i((int)wire.wire_k - 1, 0, (int)out->wire_count - 1);
    out->trip_min_cm = GAME_DEFAULT_TRIP_MIN_CM;
    out->trip_max_cm = GAME_DEFAULT_TRIP_MAX_CM;
    out->max_strikes = GAME_DEFAULT_MAX_STRIKES;
}

/* ================= selection rules ================= */
int pair_next_unused(uint8_t used_mask, int count)
{
    int i;

    for (i = 0; i < count && i < 8; i++) {
        if ((used_mask & (1u << i)) == 0u) {
            return i;
        }
    }
    return -1;
}

uint8_t pair_mark_used(uint8_t used_mask, int idx, int count, bool *cycled)
{
    unsigned full;

    if (cycled != NULL) {
        *cycled = false;
    }
    if (idx < 0 || idx >= count || count > 8) {
        return used_mask;
    }
    used_mask = (uint8_t)(used_mask | (1u << idx));
    full = (1u << count) - 1u;
    if ((used_mask & full) == full) {
        if (cycled != NULL) {
            *cycled = true;
        }
        return 0u;
    }
    return used_mask;
}

int checker_order(const char *const *chain, int n, const char *setter, int *order_out)
{
    int count = 0;
    int own = -1;
    int i;

    for (i = 0; i < n; i++) {
        if (setter != NULL && strcmp(chain[i], setter) == 0) {
            own = i;
        } else {
            order_out[count++] = i;
        }
    }
    if (own >= 0) {
        order_out[count++] = own;
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
