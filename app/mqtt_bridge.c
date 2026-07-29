/**
 * Device MQTT event bridge configuration.
 *
 * Copyright (c) 2026 MRDN Labs. MIT licensed.
 */

#include "mqtt_bridge.h"

#include <curl/curl.h>
#include <gio/gio.h>
#include <jansson.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>

/* A service account name of our own. The device mints credentials for it. */
#define VAPIX_USER "object-dwell-timer"

/* The documented virtual host for D-Bus service-account credentials. Some
 * device models answer only on plain loopback, so both are tried. */
static const char* const VAPIX_HOSTS[] = {"127.0.0.12", "127.0.0.1"};

/* One subtree filter covers every event this application declares, for every
 * zone, which also makes add/remove trivially idempotent. */
#define OUR_TOPIC_FILTER "axis:CameraApplicationPlatform/ObjectDwellTimer//."
#define OUR_FILTER_MARK  "ObjectDwellTimer"

#define EVENT_BASE "CameraApplicationPlatform/ObjectDwellTimer"

static const char* const EVENT_KINDS[] = {"Entered", "Exited", "ThresholdExceeded", "DwellUpdate"};

static char* credentials = NULL; /* "id:password", never written to disk */

/* ------------------------------------------------------------- credentials */

static char* fetch_credentials(void) {
    GError*          error      = NULL;
    GDBusConnection* connection = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
    if (!connection) {
        syslog(LOG_ERR, "MQTT_DBUS_FAIL msg=%s", error ? error->message : "unknown");
        g_clear_error(&error);
        return NULL;
    }

    GVariant* result = g_dbus_connection_call_sync(
        connection,
        "com.axis.HTTPConf1",
        "/com/axis/HTTPConf1/VAPIXServiceAccounts1",
        "com.axis.HTTPConf1.VAPIXServiceAccounts1",
        "GetCredentials",
        g_variant_new("(s)", VAPIX_USER),
        NULL,
        G_DBUS_CALL_FLAGS_NONE,
        10000,
        NULL,
        &error);

    char* out = NULL;
    if (!result) {
        syslog(LOG_ERR, "MQTT_CREDS_FAIL msg=%s", error ? error->message : "unknown");
        g_clear_error(&error);
    } else {
        const char* raw = NULL;
        g_variant_get(result, "(&s)", &raw);
        if (raw && strchr(raw, ':')) {
            out = g_strdup(raw);
        }
        g_variant_unref(result);
    }

    g_object_unref(connection);
    return out;
}

/* ---------------------------------------------------------------- transport */

typedef struct {
    GString* body;
} response_t;

static size_t collect(char* ptr, size_t size, size_t nmemb, void* userdata) {
    const size_t n = size * nmemb;
    g_string_append_len(((response_t*)userdata)->body, ptr, n);
    return n;
}

/** POST JSON to a local VAPIX endpoint. Returns the parsed reply or NULL. */
static json_t* vapix_post(const char* endpoint, const char* request) {
    if (!credentials) {
        return NULL;
    }

    json_t* parsed = NULL;

    for (size_t h = 0; h < G_N_ELEMENTS(VAPIX_HOSTS) && !parsed; h++) {
        CURL* curl = curl_easy_init();
        if (!curl) {
            return NULL;
        }

        response_t resp = {.body = g_string_new(NULL)};
        char*      url  = g_strdup_printf("http://%s/axis-cgi/%s", VAPIX_HOSTS[h], endpoint);

        struct curl_slist* headers = curl_slist_append(NULL, "Content-Type: application/json");

        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_USERPWD, credentials);
        curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, collect);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

        const CURLcode rc = curl_easy_perform(curl);
        long           code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);

        if (rc == CURLE_OK && code == 200) {
            json_error_t jerr;
            parsed = json_loads(resp.body->str, 0, &jerr);
            if (!parsed) {
                syslog(LOG_ERR, "MQTT_VAPIX_BADJSON endpoint=%s msg=%s", endpoint, jerr.text);
            }
        } else {
            /* 401/403 on the virtual host is a known per-model quirk; the loop
             * falls through to plain loopback rather than giving up. */
            syslog(LOG_WARNING,
                   "MQTT_VAPIX_FAIL host=%s endpoint=%s curl=%d http=%ld",
                   VAPIX_HOSTS[h],
                   endpoint,
                   (int)rc,
                   code);
        }

        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        g_string_free(resp.body, TRUE);
        g_free(url);
    }

    return parsed;
}

/* ------------------------------------------------------------------- helpers */

static json_t* get_client_status(void) {
    return vapix_post("mqtt/client.cgi",
                      "{\"apiVersion\":\"1.0\",\"method\":\"getClientStatus\"}");
}

static json_t* get_publication_config(void) {
    return vapix_post("mqtt/event.cgi",
                      "{\"apiVersion\":\"1.0\",\"method\":\"getEventPublicationConfig\"}");
}

/** Compose the literal MQTT topic an event will be published on. */
static char* resolve_topic(const char* device_prefix,
                           const char* topic_prefix_mode,
                           const char* custom_prefix,
                           bool namespaces,
                           bool append_event_topic,
                           const char* kind,
                           int zone_id) {
    const bool is_default = !topic_prefix_mode || strcmp(topic_prefix_mode, "default") == 0;

    /* Measured on hardware: the default mode yields "<deviceTopicPrefix>/event". */
    char* base = is_default ? g_strdup_printf("%s/event", device_prefix ? device_prefix : "axis")
                            : g_strdup(custom_prefix ? custom_prefix : "");

    if (!append_event_topic) {
        return base;
    }

    /* With namespaces on, the separator is a colon: "tns:axis", not "tnsaxis". */
    char* path = namespaces ? g_strdup_printf("tns:axis/%s/%s", EVENT_BASE, kind)
                            : g_strdup_printf("%s/%s", EVENT_BASE, kind);

    char* out = (zone_id > 0) ? g_strdup_printf("%s/%s/$source/zone/%d", base, path, zone_id)
                              : g_strdup_printf("%s/%s", base, path);

    g_free(base);
    g_free(path);
    return out;
}

static bool is_our_filter(json_t* entry) {
    const char* f = json_string_value(json_object_get(entry, "topicFilter"));
    return f && strstr(f, OUR_FILTER_MARK) != NULL;
}

/* --------------------------------------------------------------------- API */

bool mqtt_bridge_init(void) {
    curl_global_init(CURL_GLOBAL_DEFAULT);

    /* Re-fetched at startup, held only in memory, never persisted. */
    credentials = fetch_credentials();
    if (!credentials) {
        syslog(LOG_WARNING, "MQTT_INIT no VAPIX credentials; bridge control unavailable");
        return false;
    }
    syslog(LOG_INFO, "MQTT_INIT ready");
    return true;
}

void mqtt_bridge_shutdown(void) {
    if (credentials) {
        /* Overwrite before releasing rather than leaving it in freed memory. */
        memset(credentials, 0, strlen(credentials));
        g_free(credentials);
        credentials = NULL;
    }
    curl_global_cleanup();
}

char* mqtt_bridge_state_json(const zone_set_t* zones) {
    json_t* out = json_object();

    json_t* status = get_client_status();
    json_t* config = get_publication_config();

    const char* device_prefix = "axis";
    bool        connected     = false;
    bool        active        = false;

    if (status) {
        json_t* data = json_object_get(status, "data");
        json_t* st   = json_object_get(data, "status");
        json_t* cfg  = json_object_get(data, "config");

        const char* state = json_string_value(json_object_get(st, "state"));
        const char* conn  = json_string_value(json_object_get(st, "connectionStatus"));
        active            = state && strcmp(state, "active") == 0;
        connected         = conn && strcmp(conn, "connected") == 0;

        const char* dp = json_string_value(json_object_get(cfg, "deviceTopicPrefix"));
        if (dp) {
            device_prefix = dp;
        }

        json_t* server = json_object_get(cfg, "server");
        json_object_set(out, "server", server ? server : json_null());
    }

    json_object_set_new(out, "clientActive", json_boolean(active));
    json_object_set_new(out, "clientConnected", json_boolean(connected));
    json_object_set_new(out, "available", json_boolean(credentials != NULL));

    const char* mode       = "default";
    const char* custom     = "";
    bool        namespaces = true;
    bool        append     = true;
    bool        ours       = false;

    if (config) {
        json_t* pub = json_object_get(json_object_get(config, "data"), "eventPublicationConfig");
        const char* m = json_string_value(json_object_get(pub, "topicPrefix"));
        const char* c = json_string_value(json_object_get(pub, "customTopicPrefix"));
        if (m) {
            mode = m;
        }
        if (c) {
            custom = c;
        }
        json_t* ns = json_object_get(pub, "includeTopicNamespaces");
        json_t* ae = json_object_get(pub, "appendEventTopic");
        if (ns) {
            namespaces = json_is_true(ns);
        }
        if (ae) {
            append = json_is_true(ae);
        }

        json_t* list = json_object_get(pub, "eventFilterList");
        size_t  idx;
        json_t* entry;
        json_array_foreach(list, idx, entry) {
            if (is_our_filter(entry)) {
                ours = true;
            }
        }
        json_object_set_new(out, "otherFilters",
                            json_integer((json_int_t)json_array_size(list) - (ours ? 1 : 0)));
    }

    json_object_set_new(out, "configured", json_boolean(ours));
    json_object_set_new(out, "topicPrefixMode", json_string(mode));

    /* The literal strings an integrator can copy. */
    json_t* topics = json_array();
    for (size_t k = 0; k < G_N_ELEMENTS(EVENT_KINDS); k++) {
        for (int z = 0; z < zones->n_zones; z++) {
            char* topic = resolve_topic(device_prefix, mode, custom, namespaces, append,
                                        EVENT_KINDS[k], zones->zones[z].id);
            json_t* item = json_object();
            json_object_set_new(item, "event", json_string(EVENT_KINDS[k]));
            json_object_set_new(item, "zoneId", json_integer(zones->zones[z].id));
            json_object_set_new(item, "zoneName", json_string(zones->zones[z].name));
            json_object_set_new(item, "topic", json_string(topic));
            json_array_append_new(topics, item);
            g_free(topic);
        }
    }
    json_object_set_new(out, "topics", topics);

    char* wildcard = resolve_topic(device_prefix, mode, custom, namespaces, append, "#", 0);
    json_object_set_new(out, "wildcard", json_string(wildcard));
    g_free(wildcard);

    /* Values are strings on the wire — the bridge stringifies everything. */
    json_object_set_new(
        out,
        "samplePayload",
        json_string("{\"topic\":\"axis:CameraApplicationPlatform/ObjectDwellTimer/Exited\","
                    "\"timestamp\":1785332731984,"
                    "\"message\":{\"source\":{\"zone\":\"1\"},\"key\":{},"
                    "\"data\":{\"objectId\":\"8e653185-8e54-4dca-8af1-9f7202aca120\","
                    "\"objectType\":\"Truck\",\"state\":\"out\","
                    "\"elapsedSeconds\":\"112.000000\",\"thresholdExceeded\":\"1\","
                    "\"overageSeconds\":\"12.000000\","
                    "\"utcTime\":\"2026-07-29T13:45:31.984598Z\",\"test\":\"0\"}}}"));

    if (status) {
        json_decref(status);
    }
    if (config) {
        json_decref(config);
    }

    char* text = json_dumps(out, JSON_COMPACT);
    json_decref(out);
    char* result = g_strdup(text ? text : "{}");
    free(text);
    return result;
}

char* mqtt_bridge_configure(bool enable) {
    if (!credentials) {
        return g_strdup("no VAPIX credentials — cannot reach the MQTT configuration");
    }

    json_t* current = get_publication_config();
    if (!current) {
        return g_strdup("could not read the current event publication configuration");
    }

    json_t* pub  = json_object_get(json_object_get(current, "data"), "eventPublicationConfig");
    json_t* list = json_object_get(pub, "eventFilterList");

    /* Rebuild the list, dropping any entry we previously added. Everything the
     * operator configured is carried across untouched. */
    json_t* merged = json_array();
    size_t  idx;
    json_t* entry;
    int     preserved = 0;
    json_array_foreach(list, idx, entry) {
        if (is_our_filter(entry)) {
            continue;
        }
        json_array_append(merged, entry);
        preserved++;
    }

    if (enable) {
        json_t* ours = json_object();
        json_object_set_new(ours, "topicFilter", json_string(OUR_TOPIC_FILTER));
        json_object_set_new(ours, "qos", json_integer(0));
        json_object_set_new(ours, "retain", json_string("none"));
        json_array_append_new(merged, ours);
    }

    json_t* params = json_object();
    json_object_set_new(params, "eventFilterList", merged);

    json_t* request = json_object();
    json_object_set_new(request, "apiVersion", json_string("1.0"));
    json_object_set_new(request, "method", json_string("configureEventPublication"));
    json_object_set_new(request, "params", params);

    char* body = json_dumps(request, JSON_COMPACT);
    json_decref(request);
    json_decref(current);

    json_t* reply = vapix_post("mqtt/event.cgi", body);
    free(body);

    if (!reply) {
        return g_strdup("the device rejected the event publication update");
    }

    json_t*     err  = json_object_get(reply, "error");
    const char* emsg = err ? json_string_value(json_object_get(err, "message")) : NULL;
    char*       out  = emsg ? g_strdup(emsg) : NULL;
    json_decref(reply);

    if (!out) {
        syslog(LOG_INFO,
               "MQTT_BRIDGE_%s preserved_filters=%d",
               enable ? "ENABLED" : "DISABLED",
               preserved);
    }
    return out;
}
