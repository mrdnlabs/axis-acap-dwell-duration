/**
 * Minimal loopback JSON server.
 *
 * Copyright (c) 2026 MRDN Labs. MIT licensed.
 */

#include "httpd.h"

#include <gio/gio.h>
#include <string.h>
#include <syslog.h>

#define APP_PREFIX  "/local/object_dwell_timer"
#define MAX_REQUEST 2048
#define IO_TIMEOUT_S 3

static GSocketService* service = NULL;
static httpd_body_fn   body_fn = NULL;
static void*           body_user = NULL;

/**
 * Map a request path to a logical endpoint.
 *
 * Strictly an allowlist of exact paths — nothing is derived from the request,
 * so there is no way to reach the filesystem or build a path from user input.
 */
static const char* endpoint_for(const char* path) {
    static const struct {
        const char* suffix;
        const char* endpoint;
    } routes[] = {
        {"/status", "status"},
        {"/api/health", "health"},
        {"/api/zones", "zones"},
        {"/api/test", "test"},
    };

    for (size_t i = 0; i < G_N_ELEMENTS(routes); i++) {
        if (strcmp(path, routes[i].suffix) == 0) {
            return routes[i].endpoint;
        }
        char* full = g_strconcat(APP_PREFIX, routes[i].suffix, NULL);
        const bool hit = strcmp(path, full) == 0;
        g_free(full);
        if (hit) {
            return routes[i].endpoint;
        }
    }
    return NULL;
}

static void write_all(GOutputStream* out, const char* text) {
    gsize written = 0;
    g_output_stream_write_all(out, text, strlen(text), &written, NULL, NULL);
}

static void respond(GOutputStream* out, int code, const char* reason, const char* body) {
    char* head = g_strdup_printf("HTTP/1.1 %d %s\r\n"
                                 "Content-Type: application/json\r\n"
                                 "Content-Length: %zu\r\n"
                                 "Cache-Control: no-store\r\n"
                                 "X-Content-Type-Options: nosniff\r\n"
                                 "Connection: close\r\n"
                                 "\r\n",
                                 code,
                                 reason,
                                 strlen(body));
    write_all(out, head);
    write_all(out, body);
    g_free(head);
}

static gboolean on_incoming(GSocketService* svc,
                            GSocketConnection* connection,
                            GObject* source_object,
                            gpointer user_data) {
    (void)svc;
    (void)source_object;
    (void)user_data;

    /* Bound the time any single request can hold the main loop. */
    GSocket* sock = g_socket_connection_get_socket(connection);
    if (sock) {
        g_socket_set_timeout(sock, IO_TIMEOUT_S);
    }

    GInputStream*  in  = g_io_stream_get_input_stream(G_IO_STREAM(connection));
    GOutputStream* out = g_io_stream_get_output_stream(G_IO_STREAM(connection));

    char    buf[MAX_REQUEST + 1];
    gssize  n = g_input_stream_read(in, buf, MAX_REQUEST, NULL, NULL);
    if (n <= 0) {
        g_io_stream_close(G_IO_STREAM(connection), NULL, NULL);
        return TRUE;
    }
    buf[n] = '\0';

    /* Request line only: METHOD SP PATH SP VERSION */
    char* line_end = strstr(buf, "\r\n");
    if (line_end) {
        *line_end = '\0';
    }

    char** parts = g_strsplit(buf, " ", 3);
    const char* method = parts[0] ? parts[0] : "";
    const char* target = parts[1] ? parts[1] : "";

    const bool is_get  = strcmp(method, "GET") == 0;
    const bool is_post = strcmp(method, "POST") == 0;
    if (!is_get && !is_post) {
        respond(out, 405, "Method Not Allowed", "{\"error\":\"only GET and POST are supported\"}");
        goto done;
    }

    char*       path  = g_strdup(target);
    const char* query = "";
    char*       q     = strchr(path, '?');
    if (q) {
        *q    = '\0';
        query = q + 1;
    }

    const char* endpoint = endpoint_for(path);
    if (!endpoint) {
        respond(out, 404, "Not Found", "{\"error\":\"no such endpoint\"}");
        g_free(path);
        goto done;
    }

    /* Anything that changes state or emits must not be reachable by GET — a
     * link or a prefetch should never be able to fire an event into a VMS. */
    if (strcmp(endpoint, "test") == 0 && !is_post) {
        respond(out, 405, "Method Not Allowed", "{\"error\":\"POST required\"}");
        g_free(path);
        goto done;
    }

    char* body = body_fn ? body_fn(endpoint, method, query, body_user) : NULL;
    if (body) {
        respond(out, 200, "OK", body);
        g_free(body);
    } else {
        respond(out, 503, "Service Unavailable", "{\"error\":\"not ready\"}");
    }
    g_free(path);

done:
    g_strfreev(parts);
    g_output_stream_flush(out, NULL, NULL);
    g_io_stream_close(G_IO_STREAM(connection), NULL, NULL);
    return TRUE;
}

bool httpd_start(guint16 port, httpd_body_fn fn, void* user) {
    body_fn   = fn;
    body_user = user;

    service = g_socket_service_new();

    GInetAddress*   addr = g_inet_address_new_loopback(G_SOCKET_FAMILY_IPV4);
    GSocketAddress* sa   = g_inet_socket_address_new(addr, port);
    GError*         err  = NULL;

    const gboolean ok = g_socket_listener_add_address(G_SOCKET_LISTENER(service),
                                                      sa,
                                                      G_SOCKET_TYPE_STREAM,
                                                      G_SOCKET_PROTOCOL_TCP,
                                                      NULL,
                                                      NULL,
                                                      &err);
    g_object_unref(sa);
    g_object_unref(addr);

    if (!ok) {
        syslog(LOG_ERR, "HTTPD_BIND_FAIL port=%u msg=%s", port,
               err ? err->message : "unknown");
        g_clear_error(&err);
        g_object_unref(service);
        service = NULL;
        return false;
    }

    g_signal_connect(service, "incoming", G_CALLBACK(on_incoming), NULL);
    g_socket_service_start(service);

    syslog(LOG_INFO, "HTTPD_LISTENING addr=127.0.0.1 port=%u", port);
    return true;
}

void httpd_stop(void) {
    if (!service) {
        return;
    }
    g_socket_service_stop(service);
    g_socket_listener_close(G_SOCKET_LISTENER(service));
    g_object_unref(service);
    service = NULL;
}
