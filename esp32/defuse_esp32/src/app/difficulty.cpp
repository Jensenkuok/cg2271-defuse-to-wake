/* Defuse to Wake - difficulty score, mode and level table. See difficulty.h. */
#include "difficulty.h"

#include <stdio.h>
#include <string.h>

namespace {

int clamp_score(int score)
{
    if (score < DIFF_SCORE_MIN) {
        return DIFF_SCORE_MIN;
    }
    return score > DIFF_SCORE_MAX ? DIFF_SCORE_MAX : score;
}

/* The score change of one morning (O9). */
int morning_delta(const HistRecord &r)
{
    const bool fast_clean = (r.strikes == 0u && r.snoozes == 0u && r.explosions == 0u && r.duration_s <= DIFF_FAST_S);
    int delta = fast_clean ? DIFF_FAST_BONUS : 0;

    delta -= (int)r.explosions * DIFF_EXPLOSION_PENALTY;
    delta -= (int)r.snoozes * DIFF_SNOOZE_PENALTY;
    return delta;
}

const char *level_title(int level)
{
    switch (level) {
    case PR_EASY:   return "Easy";
    case PR_HARD:   return "Hard";
    default:        return "Normal";
    }
}

}  // namespace

int difficulty_score(const HistRecord *recent, int n)
{
    int score = DIFF_SCORE_START;
    int i;

    if (recent == NULL) {
        return score;
    }
    for (i = n - 1; i >= 0; i--) {                      /* the input is newest first: replay from the back */
        score = clamp_score(score + morning_delta(recent[i]));
    }
    return score;
}

int difficulty_band_level(int score)
{
    score = clamp_score(score);
    if (score <= DIFF_BAND_EASY_MAX) {
        return PR_EASY;
    }
    return score <= DIFF_BAND_NORMAL_MAX ? PR_NORMAL : PR_HARD;
}

int difficulty_effective_level(int mode, int score)
{
    switch (mode) {
    case DIFF_MODE_EASY:    return PR_EASY;
    case DIFF_MODE_NORMAL:  return PR_NORMAL;
    case DIFF_MODE_HARD:    return PR_HARD;
    default:                return difficulty_band_level(score);
    }
}

const char *difficulty_mode_name(int mode)
{
    switch (mode) {
    case DIFF_MODE_EASY:    return "easy";
    case DIFF_MODE_NORMAL:  return "normal";
    case DIFF_MODE_HARD:    return "hard";
    default:                return "auto";
    }
}

bool difficulty_mode_valid(int mode)
{
    return mode >= 0 && mode < DIFF_MODE_COUNT;
}

bool difficulty_mode_parse(const char *text, int *mode_out)
{
    int m;

    if (text == NULL) {
        return false;
    }
    for (m = 0; m < DIFF_MODE_COUNT; m++) {
        if (strcmp(text, difficulty_mode_name(m)) == 0) {
            if (mode_out != NULL) {
                *mode_out = m;
            }
            return true;
        }
    }
    return false;
}

Difficulty difficulty(const HistRecord *recent, int n, int mode)
{
    return difficulty_from_score(difficulty_score(recent, n), mode);
}

Difficulty difficulty_resolve(const HistRecord *recent, int n, int mode, bool history_ok, int *last_good_score)
{
    int score;

    if (history_ok || last_good_score == NULL) {
        score = difficulty_score(recent, n);
        if (history_ok && last_good_score != NULL) {
            *last_good_score = score;
        }
    } else {
        score = (*last_good_score >= DIFF_SCORE_MIN) ? *last_good_score : DIFF_SCORE_START;
    }
    return difficulty_from_score(score, mode);
}

Difficulty difficulty_from_score(int score, int mode)
{
    Difficulty d;
    LevelSpec spec;

    memset(&d, 0, sizeof(d));
    if (!difficulty_mode_valid(mode)) {
        mode = DIFF_MODE_DEFAULT;
    }
    d.mode = (uint8_t)mode;
    d.score = (uint8_t)clamp_score(score);
    d.level = (uint8_t)difficulty_effective_level(mode, d.score);
    spec = difficulty_level_spec(d.level);
    d.module_count = spec.modules;
    d.time_limit_s = spec.time_s;
    snprintf(d.explanation, sizeof(d.explanation), "%s (%s, score %d)", level_title(d.level),
             mode == DIFF_MODE_AUTO ? "Auto" : "manual", (int)d.score);
    return d;
}
