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

#include <NetworkClientSecure.h>

#include "config.h"

static inline void net_tls_configure(NetworkClientSecure &client)
{
#if APP_TLS_INSECURE
    client.setInsecure();
#else
    client.useBuiltinCACertBundle();
#endif
    client.setTimeout(APP_HTTP_TIMEOUT_S);   /* seconds */
}

#endif /* NET_TLS_H */
