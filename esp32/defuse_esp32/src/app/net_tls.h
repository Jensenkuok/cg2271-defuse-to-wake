/*
 * Defuse to Wake - TLS setup shared by every HTTPS client (NUSMods, Firebase).
 * Default: verify the server with the CA bundle built into the ESP32 core
 * (NetworkClientSecure::useBuiltinCACertBundle). That needs a valid wall clock, which the
 * network task guarantees before any request (it waits for SNTP).
 * APP_TLS_INSECURE = 1 switches to setInsecure(): the traffic is still encrypted, but the
 * server is NOT authenticated, so a man-in-the-middle on the hotspot could read or alter it.
 * Acceptable only as a temporary diagnostic for a certificate problem.
 */
#ifndef NET_TLS_H
#define NET_TLS_H

#include <Arduino.h>
#include <HTTPClient.h>
#include <NetworkClientSecure.h>

#include "config.h"
#include "mem_big.h"

static inline void net_tls_configure(NetworkClientSecure &client)
{
#if APP_TLS_INSECURE
    client.setInsecure();
#else
    client.useBuiltinCACertBundle();
#endif
    client.setTimeout(APP_HTTP_TIMEOUT_S);   /* seconds */
}

/* One line for a failed HTTPS call, printed BEFORE http.end() (end() closes the client): the
 * HTTP status or HTTPClient error text, the mbedTLS error of the TLS client and the internal
 * heap right now. A handshake that cannot get ~32 KB of contiguous internal RAM shows up here
 * as a small int_largest and a TLS allocation error. */
static inline void net_tls_log_failure(const char *tag, int code, NetworkClientSecure &client)
{
    char tls_text[96] = "";
    const int tls_err = client.lastError(tls_text, sizeof(tls_text));

    Serial.printf("[%s] failure: code=%d (%s) tls_err=%d (%s) int_free=%u int_largest=%u\n", tag, code,
                  code < 0 ? HTTPClient::errorToString(code).c_str() : "HTTP status", tls_err,
                  tls_err != 0 ? tls_text : "none", (unsigned)mem_internal_free(), (unsigned)mem_internal_largest());
}

#endif /* NET_TLS_H */
