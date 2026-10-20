/* Defuse to Wake - SoCLaaS chat client. See llm_client.h. */
#include "llm_client.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <string.h>

#include "../../secrets.h"
#include "config.h"
#include "fault.h"
#include "net_tls.h"
#include "puzzle_logic.h"

namespace {

void log_result(const char *model, const LlmReply &r)
{
    if (r.status == LLM_OK) {
        Serial.printf("[llm] model=%s HTTP %d %u ms\n", model, r.http_code, (unsigned)r.latency_ms);
    } else if (r.status == LLM_ERR_OUTAGE) {
        Serial.printf("[llm] model=%s FAILED simulated outage\n", model);
    } else if (r.http_code < 0) {
        Serial.printf("[llm] model=%s FAILED %s (%s) %u ms\n", model, llm_status_name(r.status),
                      HTTPClient::errorToString(r.http_code).c_str(), (unsigned)r.latency_ms);
    } else {
        Serial.printf("[llm] model=%s FAILED %s HTTP %d %u ms\n", model, llm_status_name(r.status), r.http_code,
                      (unsigned)r.latency_ms);
    }
}

/* {"model":..,"max_tokens":..,"temperature":..,"messages":[{system},{user}]} */
void build_request(String &payload, const char *model, const char *system, const char *user, int max_tokens,
                   float temperature)
{
    JsonDocument req;
    JsonArray messages;
    JsonObject m;

    req["model"] = model;
    req["max_tokens"] = max_tokens;
    req["temperature"] = temperature;
    messages = req["messages"].to<JsonArray>();
    m = messages.add<JsonObject>();
    m["role"] = "system";
    m["content"] = system;
    m = messages.add<JsonObject>();
    m["role"] = "user";
    m["content"] = user;
    serializeJson(req, payload);
}

/* The only field we need is choices[0].message.content: the filter drops reasoning_content,
 * usage and the rest while the body is parsed. */
LlmStatus read_content(const String &body, char *out_text, size_t out_size)
{
    JsonDocument filter;
    JsonDocument doc;
    DeserializationError err;
    const char *content;

    filter["choices"][0]["message"]["content"] = true;
    err = deserializeJson(doc, body, DeserializationOption::Filter(filter));
    if (err) {
        Serial.printf("[llm] JSON error %s\n", err.c_str());
        return LLM_ERR_PARSE;
    }
    content = doc["choices"][0]["message"]["content"] | "";
    if (strip_think(content, out_text, out_size) == 0u) {
        return LLM_ERR_EMPTY;
    }
    return LLM_OK;
}

}  // namespace

const char *llm_status_name(LlmStatus s)
{
    switch (s) {
    case LLM_OK:          return "ok";
    case LLM_ERR_NO_KEY:  return "no-key";
    case LLM_ERR_REQUEST: return "request";
    case LLM_ERR_HTTP:    return "http";
    case LLM_ERR_BODY:    return "body";
    case LLM_ERR_PARSE:   return "parse";
    case LLM_ERR_EMPTY:   return "empty";
    case LLM_ERR_OUTAGE:  return "simulated outage";
    }
    return "?";
}

LlmReply llm_chat(const char *model, const char *system, const char *user, int max_tokens, float temperature,
                  char *out_text, size_t out_size)
{
    LlmReply rep = { LLM_ERR_REQUEST, 0, 0 };
    const uint32_t t0 = millis();
    NetworkClientSecure client;
    HTTPClient http;
    String payload;
    String body;
    int size;

    if (out_text == NULL || out_size == 0u) {
        return rep;
    }
    out_text[0] = '\0';
    if (fault_net_blocked()) {
        rep.status = LLM_ERR_OUTAGE;
        rep.http_code = HTTPC_ERROR_CONNECTION_REFUSED;   /* negative: callers count it as a transport failure */
        log_result(model, rep);
        return rep;
    }
    if (SOCLAAS_API_KEY[0] == '\0') {
        rep.status = LLM_ERR_NO_KEY;
        log_result(model, rep);
        return rep;
    }
    build_request(payload, model, system, user, max_tokens, temperature);

    net_tls_configure(client);
    client.setTimeout(APP_LLM_TIMEOUT_S);
    http.setConnectTimeout(APP_LLM_TIMEOUT_MS);
    http.setTimeout(APP_LLM_TIMEOUT_MS);
    http.useHTTP10(true);                      /* no chunked encoding from the server */
    if (!http.begin(client, APP_SOCLAAS_BASE_URL APP_SOCLAAS_CHAT_PATH)) {
        rep.latency_ms = millis() - t0;
        log_result(model, rep);
        return rep;
    }
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", String("Bearer ") + SOCLAAS_API_KEY);   /* never printed */
    rep.http_code = http.POST(payload);
    payload = String();
    if (rep.http_code != HTTP_CODE_OK) {
        rep.status = LLM_ERR_HTTP;
        http.end();
        rep.latency_ms = millis() - t0;
        log_result(model, rep);
        return rep;
    }
    size = http.getSize();
    if (size > (int)APP_LLM_BODY_MAX) {
        rep.status = LLM_ERR_BODY;
    } else {
        body = http.getString();
        rep.status = (body.length() == 0u || body.length() > APP_LLM_BODY_MAX) ? LLM_ERR_BODY : LLM_OK;
    }
    http.end();
    if (rep.status == LLM_OK) {
        rep.status = read_content(body, out_text, out_size);
    }
    rep.latency_ms = millis() - t0;
    log_result(model, rep);
    return rep;
}
