/* Host-side tests for esp32/defuse_esp32/src/app/difficulty.* (score replay, bands, Auto/manual mode, the
 * level table) and the pieces that use it in puzzle_logic.* (level-aware wire pick, tripwire band O11,
 * BOMB_CONFIG). Planner decisions O9 (score), O10 (level change), O11 (tripwire band). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "difficulty.h"
#include "puzzle_logic.h"

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

/* One stored morning. */
static HistRecord morning(unsigned duration_s, unsigned strikes, unsigned snoozes, unsigned explosions)
{
    HistRecord r;

    memset(&r, 0, sizeof(r));
    r.duration_s = (uint16_t)duration_s;
    r.strikes = (uint8_t)strikes;
    r.snoozes = (uint8_t)snoozes;
    r.explosions = (uint8_t)explosions;
    return r;
}

static HistRecord fast_clean(void)
{
    return morning(DIFF_FAST_S, 0, 0, 0);               /* exactly the limit still counts as fast */
}

/* The ring order is newest first; the tests write histories oldest first (the order they happened in)
 * and flip them here, so a test reads like a calendar. */
static int score_of(const HistRecord *oldest_first, int n)
{
    HistRecord newest_first[HIST_LEN * 2];
    int i;

    for (i = 0; i < n; i++) {
        newest_first[i] = oldest_first[n - 1 - i];
    }
    return difficulty_score(newest_first, n);
}

/* Independent reference: a plain forward loop over the calendar order, written from the O9 text. */
static int reference_score(const HistRecord *oldest_first, int n)
{
    int s = 50;
    int i;

    for (i = 0; i < n; i++) {
        const HistRecord &m = oldest_first[i];

        if (m.strikes == 0 && m.snoozes == 0 && m.explosions == 0 && m.duration_s <= 90) {
            s += 8;
        }
        s -= 10 * m.explosions;
        s -= 5 * m.snoozes;
        if (s < 0) {
            s = 0;
        }
        if (s > 100) {
            s = 100;
        }
    }
    return s;
}

/* ================= score ================= */
static void test_score_examples(void)
{
    HistRecord h[16];
    int i;

    CHECK(difficulty_score(NULL, 0) == 50);
    CHECK(difficulty_score(NULL, 5) == 50);                                  /* no array: nothing to replay */
    h[0] = fast_clean();
    CHECK(difficulty_score(h, 0) == 50);                                     /* n = 0 */
    CHECK(difficulty_score(h, -3) == 50);

    /* fast clean defuse: +8 */
    h[0] = fast_clean();
    CHECK(score_of(h, 1) == 58);
    h[0] = fast_clean();
    h[1] = fast_clean();
    CHECK(score_of(h, 2) == 66);
    /* the limit is inclusive: 90 s is fast, 91 s is not */
    h[0] = morning(90, 0, 0, 0);
    CHECK(score_of(h, 1) == 58);
    h[0] = morning(91, 0, 0, 0);
    CHECK(score_of(h, 1) == 50);
    h[0] = morning(1, 0, 0, 0);
    CHECK(score_of(h, 1) == 58);
    /* any strike, snooze or explosion makes it "not fast clean" -> no bonus (and the penalties) */
    h[0] = morning(60, 1, 0, 0);
    CHECK(score_of(h, 1) == 50);                                             /* a strike alone costs nothing, but no bonus */
    h[0] = morning(60, 0, 1, 0);
    CHECK(score_of(h, 1) == 45);                                             /* snooze: -5, no bonus */
    h[0] = morning(60, 0, 0, 1);
    CHECK(score_of(h, 1) == 40);                                             /* explosion: -10, no bonus */
    h[0] = morning(60, 3, 2, 1);
    CHECK(score_of(h, 1) == 30);                                             /* -10 -5 -5 */
    h[0] = morning(200, 0, 0, 0);
    CHECK(score_of(h, 1) == 50);                                             /* slow clean defuse: 0 */

    /* penalties add up per morning and across mornings */
    h[0] = morning(120, 2, 3, 2);
    CHECK(score_of(h, 1) == 50 - 20 - 15);
    h[0] = morning(120, 0, 1, 0);
    h[1] = morning(120, 0, 1, 0);
    h[2] = morning(120, 0, 0, 1);
    CHECK(score_of(h, 3) == 50 - 5 - 5 - 10);

    /* a long run of the same morning agrees with the plain formula until it hits a clamp */
    for (i = 0; i < 14; i++) {
        h[i] = morning(120, 0, 1, 0);
    }
    CHECK(score_of(h, 6) == 50 - 30);
    CHECK(score_of(h, 14) == 0);                                             /* 50 - 70 clamps at 0 */
}

static void test_score_clamps_after_every_morning(void)
{
    HistRecord h[16];
    int i;

    for (i = 0; i < 7; i++) {
        h[i] = fast_clean();
    }
    CHECK(score_of(h, 6) == 98);
    CHECK(score_of(h, 7) == 100);                                            /* 106 clamps to 100 */
    h[7] = morning(120, 0, 0, 1);
    CHECK(score_of(h, 8) == 90);                                             /* 100 - 10, NOT 106 - 10 = 96 */

    /* ... and from below: 0 does not go negative, and a bonus after it starts from 0 */
    h[0] = morning(120, 0, 5, 3);                                            /* -55 -> 0 */
    CHECK(score_of(h, 1) == 0);
    h[1] = fast_clean();
    CHECK(score_of(h, 2) == 8);                                              /* NOT -5 + 8 = 3 */
    h[0] = morning(120, 0, 5, 3);
    h[1] = morning(120, 0, 5, 3);
    h[2] = fast_clean();
    h[3] = fast_clean();
    CHECK(score_of(h, 4) == 16);

    /* the order matters: replayed from the oldest to the newest */
    h[0] = fast_clean();
    h[1] = fast_clean();
    h[2] = fast_clean();
    h[3] = fast_clean();
    h[4] = fast_clean();
    h[5] = fast_clean();
    h[6] = fast_clean();
    h[7] = morning(120, 0, 0, 1);                                            /* the bad morning is the newest */
    CHECK(score_of(h, 8) == 90);
    h[0] = morning(120, 0, 0, 1);                                            /* the bad morning is the oldest */
    h[7] = fast_clean();
    CHECK(score_of(h, 8) == 96);                                             /* 40, then seven fast mornings: 96 */
    /* the same eight mornings in the other order really differ */
    {
        HistRecord a[2] = { morning(120, 0, 0, 1), fast_clean() };           /* oldest first */
        HistRecord b[2] = { fast_clean(), morning(120, 0, 0, 1) };
        CHECK(score_of(a, 2) == 48 && score_of(b, 2) == 48);                 /* no clamp involved: equal */
    }
    {
        HistRecord a[9];
        HistRecord b[9];

        for (i = 0; i < 8; i++) {
            a[i] = fast_clean();
            b[i + 1] = fast_clean();
        }
        a[8] = morning(120, 0, 0, 1);                                        /* newest bad: 100 -> 90 */
        b[0] = morning(120, 0, 0, 1);                                        /* oldest bad: 40 -> 100 */
        CHECK(score_of(a, 9) == 90 && score_of(b, 9) == 100);
    }
}

static void test_score_matches_reference_on_random_histories(void)
{
    srand(2271);
    for (int round = 0; round < 20000; round++) {
        HistRecord h[HIST_LEN];
        const int n = rand() % (HIST_LEN + 1);

        for (int i = 0; i < n; i++) {
            const int kind = rand() % 4;

            h[i] = morning(kind == 0 ? (unsigned)(rand() % 91) : (unsigned)(rand() % 400), (unsigned)(rand() % 4),
                           (unsigned)(rand() % 3), (unsigned)(rand() % 3));
            if (kind == 1) {
                h[i].strikes = h[i].snoozes = h[i].explosions = 0;           /* more clean mornings */
                h[i].duration_s = (uint16_t)(rand() % 200);
            }
        }
        const int got = score_of(h, n);

        CHECK(got == reference_score(h, n));
        CHECK(got >= 0 && got <= 100);
    }
}

/* ================= bands ================= */
static void test_band_edges(void)
{
    CHECK(DIFF_BAND_EASY_MAX == 33 && DIFF_BAND_NORMAL_MAX == 66);
    CHECK(difficulty_band_level(0) == PR_EASY);
    CHECK(difficulty_band_level(1) == PR_EASY);
    CHECK(difficulty_band_level(32) == PR_EASY);
    CHECK(difficulty_band_level(33) == PR_EASY);
    CHECK(difficulty_band_level(34) == PR_NORMAL);
    CHECK(difficulty_band_level(50) == PR_NORMAL);
    CHECK(difficulty_band_level(65) == PR_NORMAL);
    CHECK(difficulty_band_level(66) == PR_NORMAL);
    CHECK(difficulty_band_level(67) == PR_HARD);
    CHECK(difficulty_band_level(68) == PR_HARD);
    CHECK(difficulty_band_level(100) == PR_HARD);
    CHECK(difficulty_band_level(-40) == PR_EASY);                            /* out of range: clamped first */
    CHECK(difficulty_band_level(900) == PR_HARD);
}

/* The four edge scores reached by real histories (oldest first), so the replay and the band agree. */
static void test_band_edges_from_histories(void)
{
    HistRecord h[16];
    int i;

    /* 33: five snoozes (-25 -> 25), then one fast clean morning (+8) */
    h[0] = morning(150, 0, 5, 0);
    h[1] = fast_clean();
    CHECK(score_of(h, 2) == 33);
    CHECK(difficulty_band_level(score_of(h, 2)) == PR_EASY);
    /* 34: three fast clean mornings (74), then a morning with four explosions (-40) */
    h[0] = fast_clean();
    h[1] = fast_clean();
    h[2] = fast_clean();
    h[3] = morning(150, 0, 0, 4);
    CHECK(score_of(h, 4) == 34);
    CHECK(difficulty_band_level(score_of(h, 4)) == PR_NORMAL);
    /* 66: two fast clean mornings */
    h[0] = fast_clean();
    h[1] = fast_clean();
    CHECK(score_of(h, 2) == 66);
    CHECK(difficulty_band_level(score_of(h, 2)) == PR_NORMAL);
    /* 67: four fast clean mornings (82), then three snoozes (-15) */
    for (i = 0; i < 4; i++) {
        h[i] = fast_clean();
    }
    h[4] = morning(150, 0, 3, 0);
    CHECK(score_of(h, 5) == 67);
    CHECK(difficulty_band_level(score_of(h, 5)) == PR_HARD);
    /* and the extremes */
    for (i = 0; i < 14; i++) {
        h[i] = fast_clean();
    }
    CHECK(score_of(h, 14) == 100 && difficulty_band_level(100) == PR_HARD);
    for (i = 0; i < 14; i++) {
        h[i] = morning(150, 2, 2, 2);
    }
    CHECK(score_of(h, 14) == 0 && difficulty_band_level(0) == PR_EASY);
}

static void test_empty_history_is_normal(void)
{
    Difficulty d = difficulty(NULL, 0, DIFF_MODE_AUTO);

    CHECK(d.score == 50 && d.level == PR_NORMAL && d.mode == DIFF_MODE_AUTO);
    CHECK(d.module_count == 4 && d.time_limit_s == 180);
    CHECK(strcmp(d.explanation, "Normal (Auto, score 50)") == 0);
    {
        HistRecord none[1];

        d = difficulty(none, 0, DIFF_MODE_AUTO);
        CHECK(d.score == 50 && d.level == PR_NORMAL);
    }
}

/* ================= mode ================= */
static void test_mode_override(void)
{
    int score;

    for (score = 0; score <= 100; score++) {
        CHECK(difficulty_effective_level(DIFF_MODE_AUTO, score) == difficulty_band_level(score));
        CHECK(difficulty_effective_level(DIFF_MODE_EASY, score) == PR_EASY);        /* the score is ignored */
        CHECK(difficulty_effective_level(DIFF_MODE_NORMAL, score) == PR_NORMAL);
        CHECK(difficulty_effective_level(DIFF_MODE_HARD, score) == PR_HARD);
        CHECK(difficulty_effective_level(77, score) == difficulty_band_level(score));    /* unknown mode = Auto */
    }
    {
        HistRecord h[8];
        int i;
        Difficulty d;

        for (i = 0; i < 8; i++) {
            h[i] = fast_clean();                                             /* score 100 */
        }
        d = difficulty(h, 8, DIFF_MODE_AUTO);
        CHECK(d.score == 100 && d.level == PR_HARD && d.time_limit_s == 150 && d.module_count == 4);
        d = difficulty(h, 8, DIFF_MODE_EASY);
        CHECK(d.score == 100 && d.level == PR_EASY && d.time_limit_s == 240 && d.module_count == 4);   /* score still shown */
        CHECK(strcmp(d.explanation, "Easy (manual, score 100)") == 0);
        d = difficulty(h, 8, DIFF_MODE_NORMAL);
        CHECK(d.level == PR_NORMAL && strcmp(d.explanation, "Normal (manual, score 100)") == 0);
        d = difficulty(h, 8, DIFF_MODE_HARD);
        CHECK(d.level == PR_HARD && strcmp(d.explanation, "Hard (manual, score 100)") == 0);
        d = difficulty(h, 8, 42);                                            /* a broken stored mode falls back to Auto */
        CHECK(d.mode == DIFF_MODE_AUTO && d.level == PR_HARD);
        d = difficulty(h, 8, -1);
        CHECK(d.mode == DIFF_MODE_AUTO);
        CHECK(strcmp(difficulty(h, 8, DIFF_MODE_AUTO).explanation, "Hard (Auto, score 100)") == 0);
    }
}

static void test_mode_names_and_parse(void)
{
    int m = -1;

    CHECK(strcmp(difficulty_mode_name(DIFF_MODE_AUTO), "auto") == 0);
    CHECK(strcmp(difficulty_mode_name(DIFF_MODE_EASY), "easy") == 0);
    CHECK(strcmp(difficulty_mode_name(DIFF_MODE_NORMAL), "normal") == 0);
    CHECK(strcmp(difficulty_mode_name(DIFF_MODE_HARD), "hard") == 0);
    CHECK(strcmp(difficulty_mode_name(99), "auto") == 0);
    CHECK(DIFF_MODE_DEFAULT == DIFF_MODE_AUTO);
    for (int i = 0; i < DIFF_MODE_COUNT; i++) {
        CHECK(difficulty_mode_valid(i));
        m = -1;
        CHECK(difficulty_mode_parse(difficulty_mode_name(i), &m) && m == i);     /* name <-> number round trip */
    }
    CHECK(!difficulty_mode_valid(-1) && !difficulty_mode_valid(DIFF_MODE_COUNT) && !difficulty_mode_valid(255));
    m = 9;
    CHECK(!difficulty_mode_parse("Hard", &m) && m == 9);
    CHECK(!difficulty_mode_parse("", &m) && !difficulty_mode_parse(NULL, &m) && m == 9);
    CHECK(difficulty_mode_parse("easy", NULL));
}

/* ================= level table ================= */
static void test_level_table(void)
{
    LevelSpec e = difficulty_level_spec(PR_EASY);
    LevelSpec n = difficulty_level_spec(PR_NORMAL);
    LevelSpec h = difficulty_level_spec(PR_HARD);
    LevelSpec odd = difficulty_level_spec(0);

    CHECK(e.time_s == 240 && e.modules == 4 && e.wire_min == 3 && e.wire_max == 4);
    CHECK(n.time_s == 180 && n.modules == 4 && n.wire_min == 4 && n.wire_max == 5);
    CHECK(h.time_s == 150 && h.modules == 4 && h.wire_min == 6 && h.wire_max == 6);
    CHECK(odd.time_s == n.time_s && odd.modules == n.modules && odd.wire_min == n.wire_min);   /* unknown = NORMAL */
    CHECK(difficulty_level_spec(PR_UNSOLVABLE).wire_max == n.wire_max);

    /* every level fits what the MCX accepts */
    for (int level = PR_EASY; level <= PR_HARD; level++) {
        const LevelSpec s = difficulty_level_spec(level);

        CHECK(s.time_s >= GAME_MIN_TIME_LIMIT_S && s.time_s <= GAME_MAX_TIME_LIMIT_S);
        CHECK(s.modules >= GAME_MIN_MODULES && s.modules <= PROTO_MAX_MODULES);
        CHECK(s.wire_min >= GAME_MIN_WIRES && s.wire_max <= GAME_MAX_WIRES && s.wire_min <= s.wire_max);
    }
    /* harder = less time, never fewer wires; every level has all four sensor modules (L4) */
    CHECK(e.time_s > n.time_s && n.time_s > h.time_s);
    CHECK(e.modules == 4 && n.modules == 4 && h.modules == 4);
    CHECK(e.wire_min <= n.wire_min && n.wire_min <= h.wire_min);

    /* difficulty() applies the table for every mode */
    for (int mode = DIFF_MODE_EASY; mode <= DIFF_MODE_HARD; mode++) {
        const Difficulty d = difficulty(NULL, 0, mode);
        const LevelSpec s = difficulty_level_spec(d.level);

        CHECK(d.module_count == s.modules && d.time_limit_s == s.time_s);
        CHECK(strlen(d.explanation) > 0 && strlen(d.explanation) <= DIFF_EXPLAIN_MAX);
    }
}

/* ================= wire pick ================= */
static void test_wire_pick_stays_in_the_levels_range(void)
{
    for (int level = PR_EASY; level <= PR_HARD; level++) {
        const LevelSpec spec = difficulty_level_spec(level);
        int seen_n[GAME_MAX_WIRES + 1] = { 0 };
        int seen_pair[GAME_MAX_WIRES + 1][GAME_MAX_WIRES + 1] = { { 0 } };
        uint32_t r = 12345u;

        for (int i = 0; i < 200000; i++) {
            int n = 0;
            int k = 0;

            r = r * 1664525u + 1013904223u;                                  /* LCG: many different seeds */
            wire_pick_from_random(level, r ^ (r >> 15), &n, &k);
            CHECK(n >= spec.wire_min && n <= spec.wire_max);
            CHECK(k >= 1 && k <= n);
            seen_n[n]++;
            seen_pair[n][k]++;
        }
        for (int n = (int)spec.wire_min; n <= (int)spec.wire_max; n++) {
            CHECK(seen_n[n] > 0);                                            /* the whole range is used */
            for (int k = 1; k <= n; k++) {
                CHECK(seen_pair[n][k] > 0);                                  /* and every wire can be the answer */
            }
        }
    }
    /* the extreme words */
    {
        int n = 0;
        int k = 0;

        wire_pick_from_random(PR_HARD, 0u, &n, &k);
        CHECK(n == 6 && k == 1);
        wire_pick_from_random(PR_HARD, 0xFFFFFFFFu, &n, &k);
        CHECK(n == 6 && k >= 1 && k <= 6);
        wire_pick_from_random(PR_EASY, 0xFFFFFFFFu, &n, &k);
        CHECK(n >= 3 && n <= 4 && k >= 1 && k <= n);
        wire_pick_from_random(99, 7u, &n, &k);                               /* unknown level = NORMAL */
        CHECK(n >= 4 && n <= 5);
    }
}

/* ================= tripwire band (O11) ================= */
static void test_trip_band(void)
{
    int seen_start[64] = { 0 };
    uint32_t r = 99u;

    CHECK(PUZZLE_TRIP_WIDTH_CM == 10 && PUZZLE_TRIP_FAR_LIMIT_CM == 40);
    for (int i = 0; i < 100000; i++) {
        uint8_t lo = 0;
        uint8_t hi = 0;

        r = r * 1664525u + 1013904223u;
        trip_band_from_random(r ^ (r >> 13), &lo, &hi);
        CHECK(lo >= 10 && lo <= 30);
        CHECK(hi == lo + 10);                                                /* the width never changes */
        CHECK(hi <= 40 && hi <= GAME_TRIP_BOUND_MAX_CM && lo >= GAME_TRIP_BOUND_MIN_CM);
        seen_start[lo]++;
    }
    for (int lo = 10; lo <= 30; lo++) {
        CHECK(seen_start[lo] > 0);                                           /* all 21 positions occur: 10-20 .. 30-40 */
    }
    {
        uint8_t lo = 0;
        uint8_t hi = 0;

        trip_band_from_random(0u, &lo, &hi);
        CHECK(lo == 10 && hi == 20);
        trip_band_from_random(20u, &lo, &hi);
        CHECK(lo == 30 && hi == 40);
        trip_band_from_random(21u, &lo, &hi);
        CHECK(lo == 10 && hi == 20);
        trip_band_from_random(0xFFFFFFFFu, &lo, &hi);
        CHECK(lo >= 10 && lo <= 30 && hi == lo + 10);
    }
}

/* ================= BOMB_CONFIG ================= */
static void test_config_follows_level_and_puzzle(void)
{
    static const uint8_t code[4] = { 3, 1, 4, 1 };
    Puzzle dial;
    Puzzle wire;

    puzzle_fill_bank_dial(&dial, code, 0);
    for (int level = PR_EASY; level <= PR_HARD; level++) {
        const int mode = DIFF_MODE_EASY + (level - PR_EASY);                 /* the manual mode of that level */
        const Difficulty d = difficulty(NULL, 0, mode);
        const LevelSpec spec = difficulty_level_spec(level);

        CHECK(d.level == level);
        for (int n = (int)spec.wire_min; n <= (int)spec.wire_max; n++) {
            for (int k = 1; k <= n; k++) {
                proto_bomb_config_t c;

                puzzle_fill_bank_wire(&wire, n, k, 0);
                puzzle_build_config(d, dial, wire, (uint32_t)(level * 100 + n * 10 + k), &c);
                CHECK(c.time_limit_s == spec.time_s && c.module_count == spec.modules);
                CHECK(c.wire_count == n && c.correct_wire == k - 1);         /* from the puzzle used, never from the level */
                CHECK(c.module_order[0] == PROTO_MODULE_DIAL && c.module_order[1] == PROTO_MODULE_WIRES &&
                      c.module_order[2] == PROTO_MODULE_TRIPWIRE);
                CHECK(spec.modules == 4 && c.module_order[3] == PROTO_MODULE_LIGHTS);   /* D, W, T, L at every level */
                CHECK(c.trip_min_cm >= 10 && c.trip_max_cm == c.trip_min_cm + 10 && c.trip_max_cm <= 40);
                CHECK(memcmp(c.dial_code, code, 4) == 0);
            }
        }
    }
    /* a cached puzzle from another level keeps its own wire count (the O10 refill replaces the set, the
     * config never contradicts the clue in the meantime) */
    {
        proto_bomb_config_t c;
        const Difficulty hard = difficulty(NULL, 0, DIFF_MODE_HARD);

        puzzle_fill_bank_wire(&wire, 3, 2, 0);
        puzzle_build_config(hard, dial, wire, 5u, &c);
        CHECK(c.wire_count == 3 && c.correct_wire == 1 && c.time_limit_s == 150 && c.module_count == 4);
    }
    /* different random words move the band between bombs */
    {
        proto_bomb_config_t a;
        proto_bomb_config_t b;
        const Difficulty d = difficulty(NULL, 0, DIFF_MODE_NORMAL);

        puzzle_fill_bank_wire(&wire, 5, 3, 0);
        puzzle_build_config(d, dial, wire, 0u, &a);
        puzzle_build_config(d, dial, wire, 7u, &b);
        CHECK(a.trip_min_cm != b.trip_min_cm);
    }
}

/* ================= history lock timeout (L9) ================= */
static void test_resolve_survives_a_history_timeout(void)
{
    HistRecord h[8];
    HistRecord none[1];
    int last = DIFF_NO_SCORE;
    Difficulty d;
    int i;

    for (i = 0; i < 8; i++) {
        h[i] = fast_clean();                                                 /* score 100 = HARD */
    }
    /* a good read is the same answer as difficulty() and is remembered */
    d = difficulty_resolve(h, 8, DIFF_MODE_AUTO, true, &last);
    CHECK(last == 100 && d.score == 100 && d.level == PR_HARD && d.time_limit_s == 150 && d.module_count == 4);
    /* a timed-out read (zero records, history_ok false): the remembered score is reused, not 50 */
    d = difficulty_resolve(none, 0, DIFF_MODE_AUTO, false, &last);
    CHECK(d.score == 100 && d.level == PR_HARD && d.time_limit_s == 150 && d.module_count == 4);
    CHECK(last == 100);
    /* a manual mode still wins over the remembered score while the history is unreadable */
    d = difficulty_resolve(none, 0, DIFF_MODE_EASY, false, &last);
    CHECK(d.level == PR_EASY && d.time_limit_s == 240 && d.score == 100);
    /* a later good read replaces the remembered score; an empty but readable history is the real 50 */
    d = difficulty_resolve(none, 0, DIFF_MODE_AUTO, true, &last);
    CHECK(last == 50 && d.score == 50 && d.level == PR_NORMAL);
    /* nothing good seen yet: Auto falls back to NORMAL, a manual mode to its own level */
    last = DIFF_NO_SCORE;
    d = difficulty_resolve(none, 0, DIFF_MODE_AUTO, false, &last);
    CHECK(d.score == DIFF_SCORE_START && d.level == PR_NORMAL && last == DIFF_NO_SCORE);
    d = difficulty_resolve(none, 0, DIFF_MODE_HARD, false, &last);
    CHECK(d.level == PR_HARD && d.time_limit_s == 150);
    d = difficulty_resolve(none, 0, DIFF_MODE_EASY, false, &last);
    CHECK(d.level == PR_EASY);
    /* no memory slot given: plain difficulty() behaviour */
    d = difficulty_resolve(h, 8, DIFF_MODE_AUTO, false, NULL);
    CHECK(d.score == 100);
    /* from_score clamps and matches difficulty() */
    CHECK(difficulty_from_score(250, DIFF_MODE_AUTO).score == 100 && difficulty_from_score(-5, DIFF_MODE_AUTO).score == 0);
}

int main(void)
{
    printf("test_difficulty: score and bands\n");
    test_score_examples();
    test_score_clamps_after_every_morning();
    test_score_matches_reference_on_random_histories();
    test_band_edges();
    test_band_edges_from_histories();
    test_empty_history_is_normal();
    printf("test_difficulty: mode and level table\n");
    test_mode_override();
    test_mode_names_and_parse();
    test_level_table();
    printf("test_difficulty: wire pick, tripwire band, config\n");
    test_wire_pick_stays_in_the_levels_range();
    test_trip_band();
    test_config_follows_level_and_puzzle();
    test_resolve_survives_a_history_timeout();
    printf("test_difficulty: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
