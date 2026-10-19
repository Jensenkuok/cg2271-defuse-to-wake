/*
 * Defuse to Wake - SoCLaaS chat client (OpenAI-compatible /v1/chat/completions).
 * One blocking call, run by the network task only (lowest priority): a request has a 30 s
 * limit for connect and read (APP_LLM_TIMEOUT_MS), so no other task is ever blocked by it.
 * The API key comes from secrets.h (SOCLAAS_API_KEY) and is never logged.
 */
#ifndef LLM_CLIENT_H
#define LLM_CLIENT_H

#include <stddef.h>
#include <stdint.h>

enum LlmStatus {
    LLM_OK = 0,
    LLM_ERR_NO_KEY,     /* no API key configured */
    LLM_ERR_REQUEST,    /* could not build or start the request */
    LLM_ERR_HTTP,       /* transport error / timeout (code < 0) or HTTP status != 200 */
    LLM_ERR_BODY,       /* reply body too large or unreadable */
    LLM_ERR_PARSE,      /* reply is not the JSON we expect */
    LLM_ERR_EMPTY       /* choices[0].message.content missing, empty, or only <think> text */
};

struct LlmReply {
    LlmStatus status;
    int http_code;          /* HTTP status, or a negative HTTPClient error code */
    uint32_t latency_ms;    /* request start to end, also for failures */
};

/* POST one chat request. On LLM_OK out_text holds choices[0].message.content without <think>
 * blocks (at most out_size - 1 chars). On any other status out_text is "" and the caller moves on
 * to the next model of its chain. Logs model, status and latency to Serial. */
LlmReply llm_chat(const char *model, const char *system, const char *user, int max_tokens, float temperature,
                  char *out_text, size_t out_size);

const char *llm_status_name(LlmStatus s);

#endif /* LLM_CLIENT_H */
