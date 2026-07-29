/**
 * Minimal loopback JSON server.
 *
 * Copyright (c) 2026 MRDN Labs. MIT licensed.
 *
 * Binds to 127.0.0.1 only and sits behind the manifest's reverseProxy, which
 * supplies TLS, authentication and access level. It must never be reachable
 * directly from the network.
 *
 * Apache forwards the *full* URI unchanged, so handlers match the complete
 * path (/local/<appName>/...), not a stripped suffix.
 */

#ifndef HTTPD_H
#define HTTPD_H

#include "dwell.h"

/**
 * Produce the JSON body for a logical endpoint ("status", "health", "zones",
 * "test").
 *
 * @param endpoint  matched route name
 * @param method    "GET" or "POST"
 * @param query     raw query string without the '?', or "" when absent
 *
 * Returns a newly allocated string the server frees, or NULL for 404.
 */
typedef char* (*httpd_body_fn)(const char* endpoint,
                               const char* method,
                               const char* query,
                               const char* body,
                               void* user);

bool httpd_start(guint16 port, httpd_body_fn body_fn, void* user);
void httpd_stop(void);

#endif /* HTTPD_H */
