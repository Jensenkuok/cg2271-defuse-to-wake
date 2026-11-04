/* Defuse to Wake - acceptance gate for LLM puzzles. See puzzle_gate.h. */
#include "puzzle_gate.h"
#include "text_logic.h"

#include <stdio.h>
#include <string.h>

#define GATE_TEXT_SCRATCH   200     /* longer riddles are refused as "no usable puzzle" before any check */

namespace {

int rule_kind(const GateTarget &t)
{
    return t.kind == PUZZLE_WIRE ? PR_KIND_WIRE : PR_KIND_DIAL;
}

void why_str(char *why, size_t why_size, const char *text)
{
    if (why != NULL && why_size > 0u) {
        snprintf(why, why_size, "%s", text);
    }
}

void answer_text_of(int kind, int answer, char *out, size_t size)
{
    if (kind == PUZZLE_DIAL) {
        snprintf(out, size, "%04d", answer);
    } else {
        snprintf(out, size, "%d", answer);
    }
}

/* Solution count for the log: pr_check caps it at 2, this one counts them all. */
int exact_count(const char *rules, const GateTarget &t)
{
    PrSet set;

    if (pr_parse(rules, rule_kind(t), t.wire_count, &set) != PR_OK) {
        return 0;
    }
    return pr_count_solutions(&set, NULL, 0);
}

}  // namespace

bool puzzle_text_fits_oled(const char *text, int kind)
{
    char lines[GATE_DIAL_ROWS][TEXT_COLS + 1];
    size_t rest = 0u;

    if (text == NULL) {
        return false;
    }
    (void)wrap_text(text, TEXT_COLS, kind == PUZZLE_WIRE ? GATE_WIRE_ROWS : GATE_DIAL_ROWS, lines[0], TEXT_COLS + 1,
                    &rest);
    return rest == strlen(text);
}

void gate_answer_text(const GateTarget &target, char *out, size_t size)
{
    if (out != NULL && size > 0u) {
        answer_text_of(target.kind, target.answer, out, size);
    }
}

int gate_check_setter(const char *reply, const GateTarget &target, char *json_buf, size_t json_size,
                      char *text_out, size_t text_size, char *why, size_t why_size)
{
    char text[GATE_TEXT_SCRATCH];
    char rules[GATE_RULES_BUF];
    char answer[PUZZLE_ANSWER_LEN];
    PrCheck chk;
    int problem;

    if (text_out != NULL && text_size > 0u) {
        text_out[0] = '\0';
    }
    if (reply == NULL || json_buf == NULL || text_out == NULL || !extract_json_object(reply, json_buf, json_size)) {
        why_str(why, why_size, "no JSON object in the reply");
        return GATE_NO_JSON;
    }
    if (!json_get_string(json_buf, "puzzle", text, sizeof(text))) {
        if (why != NULL) {
            snprintf(why, why_size, "no \"puzzle\" string (missing, or over %d characters)", GATE_TEXT_SCRATCH - 1);
        }
        return GATE_NO_PUZZLE;
    }
    if (!json_get_string(json_buf, "rules", rules, sizeof(rules))) {
        if (why != NULL) {
            snprintf(why, why_size, "no \"rules\" string (missing, or over %d characters)", GATE_RULES_BUF - 1);
        }
        return GATE_NO_RULES;
    }
    gate_answer_text(target, answer, sizeof(answer));
    problem = puzzle_text_problem(text, target.kind, answer);
    if (problem != PTP_OK) {
        if (why != NULL) {
            if (problem == PTP_TOO_LONG) {
                snprintf(why, why_size, "text too long (%u > %u)", (unsigned)strlen(text),
                         (unsigned)(target.kind == PUZZLE_WIRE ? PUZZLE_WIRE_TEXT_LIMIT : PUZZLE_DIAL_TEXT_LIMIT));
            } else {
                snprintf(why, why_size, "text %s", puzzle_text_problem_name(problem));
            }
        }
        return GATE_TEXT_REJECTED;
    }
    if (!puzzle_text_fits_oled(text, target.kind)) {
        if (why != NULL) {
            snprintf(why, why_size, "text wraps past %d OLED rows", target.kind == PUZZLE_WIRE ? GATE_WIRE_ROWS : GATE_DIAL_ROWS);
        }
        return GATE_TEXT_REJECTED;
    }
    pr_check(rules, rule_kind(target), target.wire_count, target.answer, &chk);
    if (chk.error != PR_OK) {
        if (why != NULL) {
            snprintf(why, why_size, "rules do not parse (%s)", pr_error_name(chk.error));
        }
        return GATE_RULES_SYNTAX;
    }
    if (chk.count == 0) {
        why_str(why, why_size, "rules have no solution");
        return GATE_RULES_UNSOLVABLE;
    }
    if (chk.count >= 2) {
        if (why != NULL) {
            snprintf(why, why_size, "rules ambiguous (%d solutions)", exact_count(rules, target));
        }
        return GATE_RULES_AMBIGUOUS;
    }
    if (chk.matches_answer == 0) {
        if (why != NULL) {
            char got[PUZZLE_ANSWER_LEN];

            answer_text_of(target.kind, chk.solution, got, sizeof(got));
            snprintf(why, why_size, "rules give %s, expected %s", got, answer);
        }
        return GATE_RULES_WRONG_ANSWER;
    }
    if (chk.level != target.level) {
        if (why != NULL) {
            snprintf(why, why_size, "level %s != %s", pr_level_name(chk.level), pr_level_name(target.level));
        }
        return GATE_WRONG_LEVEL;
    }
    snprintf(text_out, text_size, "%s", text);
    why_str(why, why_size, "accepted");
    return GATE_OK;
}

int gate_check_checker(const char *reply, const GateTarget &target, const char *setter_model,
                       const char *checker_model, char *json_buf, size_t json_size, char *said,
                       size_t said_size, char *why, size_t why_size)
{
    char got[PUZZLE_ANSWER_LEN];
    char answer[PUZZLE_ANSWER_LEN];

    if (said != NULL && said_size > 0u) {
        said[0] = '\0';
    }
    if (setter_model == NULL || checker_model == NULL || strcmp(setter_model, checker_model) == 0) {
        if (why != NULL) {
            snprintf(why, why_size, "checker %s is the setter's model", checker_model != NULL ? checker_model : "?");
        }
        return GATE_CHECKER_SAME_MODEL;
    }
    if (reply == NULL || json_buf == NULL || !extract_json_object(reply, json_buf, json_size) ||
        !parse_checker_answer(json_buf, target.kind, target.wire_count, got, sizeof(got))) {
        if (why != NULL) {
            snprintf(why, why_size, "checker %s gave no usable answer", checker_model);
        }
        return GATE_CHECKER_UNUSABLE;
    }
    if (said != NULL && said_size > 0u) {
        snprintf(said, said_size, "%s", got);
    }
    gate_answer_text(target, answer, sizeof(answer));
    if (strcmp(got, answer) != 0) {
        if (why != NULL) {
            snprintf(why, why_size, "checker %s answered %s, expected %s", checker_model, got, answer);
        }
        return GATE_CHECKER_DISAGREES;
    }
    if (why != NULL) {
        snprintf(why, why_size, "checker %s agrees", checker_model);
    }
    return GATE_OK;
}
