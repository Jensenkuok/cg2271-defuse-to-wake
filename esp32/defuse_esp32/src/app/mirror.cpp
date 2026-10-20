/* Defuse to Wake - Firebase write-only mirror. See mirror.h. */
#include "mirror.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <string.h>

#include "../../secrets.h"
#include "config.h"
#include "fault.h"
#include "flash_store.h"
#include "net_tls.h"

namespace {

struct MirrorBlob {
    uint32_t magic;
    MirrorQueue queue;
};

MirrorQueue s_queue;
MirrorBlob s_blob;
uint32_t s_next_try_ms;
uint32_t s_backoff_ms = APP_MIRROR_PERIOD_MS;
bool s_try_now;                     /* set by push/wifi_up: skip the wait once */

void save_queue(void)
{
    memset(static_cast<void *>(&s_blob), 0, sizeof(s_blob));
    s_blob.magic = APP_MIRROR_MAGIC;
    s_blob.queue = s_queue;
    if (!flash_write_blob(APP_KEY_MIRROR, &s_blob, sizeof(s_blob))) {
        Serial.println("[mirror] could not write the queue to flash");
    }
}

/* FIREBASE_DB_URL may or may not end with '/'. The result holds the auth value: never print it. */
bool build_url(char *url, size_t size)
{
    const char *base = FIREBASE_DB_URL;
    size_t blen = strlen(base);
    int n = snprintf(url, size, "%s%s%s%s", base, (blen > 0 && base[blen - 1u] == '/') ? "" : "/", APP_FIREBASE_PATH,
                     FIREBASE_AUTH);

    return n > 0 && (size_t)n < size;
}

/* POST the record; true only on HTTP 200. Logs the status code, never the URL. */
bool post_record(const HistRecord &rec)
{
    char url[APP_FIREBASE_URL_MAX];
    char body[APP_FIREBASE_BODY_MAX];
    size_t len = history_to_json(rec, body, sizeof(body));
    NetworkClientSecure client;
    HTTPClient http;
    int code;

    if (fault_net_blocked()) {
        Serial.println("[mirror] simulated outage: upload not attempted");   /* the web page's fault button */
        return false;
    }
    if (len == 0u || !build_url(url, sizeof(url))) {
        Serial.println("[mirror] record or URL too long, cannot upload");
        return false;
    }
    net_tls_configure(client);
    http.setConnectTimeout(APP_HTTP_TIMEOUT_MS);
    http.setTimeout(APP_HTTP_TIMEOUT_MS);
    if (!http.begin(client, url)) {
        Serial.println("[mirror] could not start the request");
        return false;
    }
    http.addHeader("Content-Type", "application/json");
    code = http.POST(reinterpret_cast<uint8_t *>(body), len);
    http.end();
    if (code != HTTP_CODE_OK) {
        Serial.printf("[mirror] upload failed: %s %d\n", code < 0 ? "error" : "HTTP", code);
        return false;
    }
    return true;
}

}  // namespace

bool mirror_init(void)
{
    s_queue.clear();
    if (flash_read_blob(APP_KEY_MIRROR, &s_blob, sizeof(s_blob)) && s_blob.magic == APP_MIRROR_MAGIC &&
        s_blob.queue.valid()) {
        s_queue = s_blob.queue;
    }
    Serial.printf("[mirror] %d records waiting for Firebase\n", s_queue.size());
    s_try_now = true;
    return true;
}

void mirror_push(const HistRecord &rec)
{
    if (s_queue.push(rec)) {
        Serial.println("[mirror] queue full: dropped the oldest pending record");
    }
    save_queue();
    s_try_now = true;
    s_backoff_ms = APP_MIRROR_PERIOD_MS;
}

void mirror_wifi_up(void)
{
    s_try_now = true;
    s_backoff_ms = APP_MIRROR_PERIOD_MS;
}

void mirror_service(uint32_t now_ms, bool net_ok)
{
    if (!net_ok || s_queue.size() == 0) {
        return;
    }
    if (!s_try_now && (int32_t)(now_ms - s_next_try_ms) < 0) {
        return;
    }
    s_try_now = false;
    if (post_record(s_queue.oldest(0))) {
        (void)s_queue.pop_oldest();
        save_queue();
        s_backoff_ms = APP_MIRROR_PERIOD_MS;
        s_next_try_ms = now_ms + APP_MIRROR_PERIOD_MS;
        s_try_now = (s_queue.size() > 0);       /* drain a backlog one record per loop pass */
        Serial.printf("[mirror] uploaded, %d pending\n", s_queue.size());
        return;
    }
    s_next_try_ms = now_ms + s_backoff_ms;
    Serial.printf("[mirror] %d pending, retry in %u s\n", s_queue.size(), (unsigned)(s_backoff_ms / 1000u));
    s_backoff_ms = (s_backoff_ms * 2u > APP_MIRROR_BACKOFF_MAX_MS) ? APP_MIRROR_BACKOFF_MAX_MS : s_backoff_ms * 2u;
}

int mirror_pending(void)
{
    return s_queue.size();
}
