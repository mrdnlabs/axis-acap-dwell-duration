/**
 * Object Dwell Timer.
 *
 * Copyright (c) 2026 MRDN Labs. MIT licensed — see LICENSE.
 *
 * Consumes AXIS Scene Metadata (com.axis.scene.frame.v1) over the Device Data
 * Hub, tests each object's reference point against user-drawn zones, and keeps
 * a dwell timer per (object_track_id, zone_id). Runs no inference.
 *
 * Built against ACAP Native SDK 12.11. The Device Data Hub C API differs
 * between SDK 12.10 and 12.11 — see learnings/phase0-device-findings.md.
 *
 * Threading: the Device Data Hub delivers samples on internal library threads
 * while the HTTP server and timers run on the GLib main loop. All shared state
 * lives behind `state_lock`, which is never held across an event send or any
 * other I/O.
 *
 * Privacy: object snapshot imagery is never logged. Any `image.data` is
 * stripped before a payload is written anywhere.
 */

#define _GNU_SOURCE

#include <glib-unix.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <jansson.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>

#include <datahub/client.h>
#include <datahub/common.h>
#include <datahub/subscriber.h>

#include "config.h"
#include "dwell.h"
#include "events.h"
#include "httpd.h"
#include "tracker.h"
#include "zone.h"

#define TOPIC_FRAME       "com.axis.scene.frame.v1"
#define TOPIC_OBJECTTRACK "com.axis.scene.object_track.v1"

#define LOCALDATA_DIR "localdata"
#define ZONES_PATH    LOCALDATA_DIR "/zones.json"

#define HTTP_PORT       2101
#define SUMMARY_EVERY_S 30

typedef struct {
    GMutex      lock;
    config_t    cfg;
    zone_set_t  zones;
    tracker_t*  tracker;
    GHashTable* class_counts; /* class -> count, still needed for OQ-3 */

    guint64 frames_seen;
    guint64 detections_seen;
    guint64 renames_seen;
    guint64 trackends_seen;
    guint64 unclassified_seen;
    guint64 events_emitted;

    int64_t first_frame_us;
    int64_t last_frame_us;
    gint64  last_frame_monotonic;
    int     last_channel_id;
} app_state_t;

static app_state_t   state;
static GMainLoop*    main_loop       = NULL;
static DHClient*     client          = NULL;
static DHSubscriber* data_subscriber = NULL;

static bool     check_err(DHError* err, const char* context);
static int64_t  parse_iso8601_us(const char* ts);
static void     strip_image_data(json_t* root);
static void     on_data(const DHTopicSample* sample, void* user_data);
static void     handle_frame(json_t* root);
static gboolean on_summary_timer(gpointer user_data);
static gboolean on_signal(gpointer user_data);
static char* http_body(const char* endpoint,
                       const char* method,
                       const char* query,
                       const char* body,
                       void* user);
static void on_config_changed(void* user);
static void     on_emit(const dwell_event_t* ev, void* user);

/* ------------------------------------------------------------------ helpers */

static bool check_err(DHError* err, const char* context) {
    if (err) {
        syslog(LOG_ERR, "DDH_ERR ctx=%s msg=%s", context, dh_error_to_string(err));
        dh_error_destroy(err);
        return true;
    }
    return false;
}

/** "2026-03-20T10:17:48.024761Z" to microseconds since the UTC epoch. */
static int64_t parse_iso8601_us(const char* ts) {
    if (!ts) {
        return -1;
    }

    struct tm   tm_val = {0};
    const char* rest   = strptime(ts, "%Y-%m-%dT%H:%M:%S", &tm_val);
    if (!rest) {
        return -1;
    }

    int64_t frac_us = 0;
    if (*rest == '.') {
        rest++;
        int digits = 0;
        while (*rest >= '0' && *rest <= '9' && digits < 6) {
            frac_us = frac_us * 10 + (*rest - '0');
            rest++;
            digits++;
        }
        for (; digits < 6; digits++) {
            frac_us *= 10;
        }
    }

    const time_t secs = timegm(&tm_val);
    if (secs == (time_t)-1) {
        return -1;
    }
    return (int64_t)secs * 1000000 + frac_us;
}

static void strip_image_data(json_t* root) {
    json_t* image = json_object_get(root, "image");
    if (json_is_object(image) && json_object_get(image, "data")) {
        json_object_set_new(image, "data", json_string("<stripped>"));
    }

    json_t* detections = json_object_get(root, "detections");
    size_t  idx;
    json_t* item;
    json_array_foreach(detections, idx, item) {
        json_t* det_image = json_object_get(item, "image");
        if (json_is_object(det_image) && json_object_get(det_image, "data")) {
            json_object_set_new(det_image, "data", json_string("<stripped>"));
        }
    }
}

/* --------------------------------------------------------------- emit path */

static void on_emit(const dwell_event_t* ev, void* user) {
    (void)user;
    state.events_emitted++;
    /* events_emit performs IPC. It is called from tracker_end_frame while the
     * lock is held; keep it cheap and non-blocking. If it ever grows costly,
     * queue records here and drain them on the main loop instead. */
    events_emit(ev, NULL);
}

/* ------------------------------------------------------------ frame handling */

static void handle_frame(json_t* root) {
    const char*   ts_str   = json_string_value(json_object_get(root, "timestamp"));
    const int64_t frame_us = parse_iso8601_us(ts_str);
    if (frame_us < 0) {
        syslog(LOG_WARNING, "FRAME_BAD_TIMESTAMP value=%s", ts_str ? ts_str : "(none)");
        return;
    }

    json_t* channel = json_object_get(root, "channel_id");
    if (json_is_integer(channel)) {
        const json_int_t ch   = json_integer_value(channel);
        state.last_channel_id = (int)ch;
    }

    state.frames_seen++;
    if (state.first_frame_us == 0) {
        state.first_frame_us = frame_us;
    }
    state.last_frame_us        = frame_us;
    state.last_frame_monotonic = g_get_monotonic_time();

    tracker_begin_frame(state.tracker, frame_us, g_get_monotonic_time());

    /* Renames first: they are the device's own re-identification result, and
     * everything after must see the consolidated identity. */
    json_t* track_events = json_object_get(root, "track_events");
    size_t  idx;
    json_t* ev;
    json_array_foreach(track_events, idx, ev) {
        const char* type = json_string_value(json_object_get(ev, "type"));
        if (!type) {
            continue;
        }
        if (strcmp(type, "Rename") == 0) {
            state.renames_seen++;
            tracker_rename(state.tracker,
                           json_string_value(json_object_get(ev, "from_id")),
                           json_string_value(json_object_get(ev, "to_id")));
        }
    }

    json_array_foreach(track_events, idx, ev) {
        const char* type = json_string_value(json_object_get(ev, "type"));
        if (type && strcmp(type, "TrackEnded") == 0) {
            const char* tid = json_string_value(json_object_get(ev, "object_track_id"));
            if (tid) {
                state.trackends_seen++;
                tracker_track_ended(state.tracker, tid);
            }
        }
    }

    json_t* detections = json_object_get(root, "detections");
    json_t* det;
    json_array_foreach(detections, idx, det) {
        const char* tid = json_string_value(json_object_get(det, "object_track_id"));
        if (!tid) {
            continue;
        }

        detection_t d;
        memset(&d, 0, sizeof(d));
        g_strlcpy(d.track_id, tid, sizeof(d.track_id));

        json_t* bbox = json_object_get(det, "bounding_box");
        d.left       = json_number_value(json_object_get(bbox, "left"));
        d.top        = json_number_value(json_object_get(bbox, "top"));
        d.right      = json_number_value(json_object_get(bbox, "right"));
        d.bottom     = json_number_value(json_object_get(bbox, "bottom"));

        json_t*     cls  = json_object_get(det, "class");
        const char* type = json_string_value(json_object_get(cls, "type"));
        if (type) {
            g_strlcpy(d.cls, type, sizeof(d.cls));
            d.score = json_number_value(json_object_get(cls, "score"));

            gpointer prev = g_hash_table_lookup(state.class_counts, type);
            g_hash_table_replace(state.class_counts,
                                 g_strdup(type),
                                 GUINT_TO_POINTER(GPOINTER_TO_UINT(prev) + 1));
        } else {
            state.unclassified_seen++;
        }

        d.ref = zone_ref_point(&d, state.cfg.ref_point);
        state.detections_seen++;

        tracker_detection(state.tracker, &d);
    }

    tracker_end_frame(state.tracker);
}

static void on_data(const DHTopicSample* sample, void* user_data) {
    (void)user_data;

    const char* topic_name = dh_topic_sample_get_topic_name(sample);
    if (!topic_name || strcmp(topic_name, TOPIC_FRAME) != 0) {
        return; /* object_track.v1 is subscribed for diagnostics only */
    }

    const DHTopicData* topic_data = dh_topic_sample_get_data(sample);
    const char*        json_text  = topic_data ? dh_topic_data_get_json_data(topic_data) : NULL;
    if (!json_text) {
        return;
    }

    json_error_t jerr;
    json_t*      root = json_loads(json_text, 0, &jerr);
    if (!root) {
        syslog(LOG_ERR, "FRAME_PARSE_FAIL line=%d msg=%s", jerr.line, jerr.text);
        return;
    }
    strip_image_data(root);

    g_mutex_lock(&state.lock);
    handle_frame(root);
    g_mutex_unlock(&state.lock);

    json_decref(root);
}

/* ------------------------------------------------------------------- HTTP */

/** Value of `key` in a raw query string, or NULL. Caller frees with g_free(). */
static char* query_value(const char* query, const char* key) {
    if (!query || !*query) {
        return NULL;
    }

    char*  found = NULL;
    char** pairs = g_strsplit(query, "&", 16);
    for (int i = 0; pairs[i]; i++) {
        char* eq = strchr(pairs[i], '=');
        if (!eq) {
            continue;
        }
        *eq = '\0';
        if (strcmp(pairs[i], key) == 0) {
            found = g_strdup(eq + 1);
            break;
        }
    }
    g_strfreev(pairs);
    return found;
}

/**
 * FR-10 test trigger. Emits a real event and a real MQTT record flagged
 * test=true, so a VMS rule can be wired and proved with nothing in the scene.
 *
 * Arriving in Phase 1 rather than Phase 2 because it is the only way to verify
 * the emit path end-to-end without waiting for an object to walk into frame.
 */
static char* handle_test(const char* query) {
    char* kind_arg = query_value(query, "kind");
    char* zone_arg = query_value(query, "zone");

    static const char* const allowed[] = {"entered", "exited", "threshold", "update"};
    const char*              kind      = NULL;
    for (size_t i = 0; i < G_N_ELEMENTS(allowed); i++) {
        if (kind_arg && strcmp(kind_arg, allowed[i]) == 0) {
            kind = allowed[i];
            break;
        }
    }
    if (!kind) {
        g_free(kind_arg);
        g_free(zone_arg);
        return g_strdup("{\"error\":\"kind must be entered, exited, threshold or update\"}");
    }

    g_mutex_lock(&state.lock);

    int zone_id = state.zones.n_zones > 0 ? state.zones.zones[0].id : 1;
    if (zone_arg) {
        const int requested = atoi(zone_arg);
        for (int i = 0; i < state.zones.n_zones; i++) {
            if (state.zones.zones[i].id == requested) {
                zone_id = requested;
                break;
            }
        }
    }

    const double threshold = state.cfg.threshold_s;
    g_mutex_unlock(&state.lock);

    /* Values chosen so a VMS rule under test sees something plausible. */
    const double elapsed = (strcmp(kind, "entered") == 0) ? 0.0 : threshold + 12.0;
    const bool   over    = strcmp(kind, "entered") != 0;

    dwell_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.kind = kind;
    g_strlcpy(ev.object_id, "00000000-0000-0000-0000-000000000000", sizeof(ev.object_id));
    g_strlcpy(ev.object_type, "Truck", sizeof(ev.object_type));
    ev.zone_id            = zone_id;
    ev.state              = (strcmp(kind, "exited") == 0) ? "out" : "in";
    ev.elapsed_s          = elapsed;
    ev.threshold_exceeded = over;
    ev.overage_s          = over ? elapsed - threshold : 0.0;
    ev.utc_us             = (int64_t)g_get_real_time();
    ev.test               = true;

    syslog(LOG_INFO, "DWELL_TEST kind=%s zone=%d", kind, zone_id);
    events_emit(&ev, NULL);

    char* out = g_strdup_printf(
        "{\"sent\":true,\"kind\":\"%s\",\"zoneId\":%d,\"test\":true,"
        "\"elapsedSeconds\":%.1f,\"overageSeconds\":%.1f}",
        kind,
        zone_id,
        ev.elapsed_s,
        ev.overage_s);

    g_free(kind_arg);
    g_free(zone_arg);
    return out;
}

/** A JSON string literal for `text`, quotes included. Caller frees. */
static char* json_quoted(const char* text) {
    json_t* s   = json_string(text ? text : "");
    char*   raw = json_dumps(s, JSON_ENCODE_ANY);
    json_decref(s);
    char* out = g_strdup(raw ? raw : "\"\"");
    free(raw);
    return out;
}

/** Reload settings whenever a parameter changes, from any source. */
static void on_config_changed(void* user) {
    (void)user;

    /* Read into a local copy with no lock held — config_reload talks to the
     * parameter service over D-Bus, and the state lock must never be held
     * across I/O. Only installing the result needs the lock. */
    config_t fresh;
    g_mutex_lock(&state.lock);
    fresh = state.cfg;
    g_mutex_unlock(&state.lock);

    config_reload(&fresh);

    g_mutex_lock(&state.lock);
    state.cfg = fresh;
    tracker_set_config(state.tracker, &state.cfg);
    g_mutex_unlock(&state.lock);

    syslog(LOG_INFO,
           "CONFIG_APPLIED types=%d threshold_s=%.1f enter_s=%.2f exit_s=%.2f",
           state.cfg.n_types,
           state.cfg.threshold_s,
           state.cfg.enter_debounce_s,
           state.cfg.exit_debounce_s);
}

static char* handle_config(const char* method, const char* body) {
    if (strcmp(method, "GET") == 0) {
        g_mutex_lock(&state.lock);
        char* out = config_to_json(&state.cfg);
        g_mutex_unlock(&state.lock);
        return out;
    }

    /* Deliberately not holding state.lock: ax_parameter_set can invoke the
     * change callback synchronously, and that callback takes the lock. */
    char* err = config_apply_json(body);
    if (err) {
        char* quoted = json_quoted(err);
        char* out    = g_strdup_printf("{\"error\":%s}", quoted);
        g_free(quoted);
        g_free(err);
        return out;
    }

    /* Re-read rather than trusting the request: parameters may have been
     * clamped, and anything absent from the body is unchanged. */
    on_config_changed(NULL);

    g_mutex_lock(&state.lock);
    char* current = config_to_json(&state.cfg);
    g_mutex_unlock(&state.lock);

    char* out = g_strdup_printf("{\"saved\":true,\"config\":%s}", current);
    g_free(current);
    return out;
}

static char* handle_zones(const char* method, const char* body) {
    if (strcmp(method, "GET") == 0) {
        g_mutex_lock(&state.lock);
        char* out = zone_to_json(&state.zones);
        g_mutex_unlock(&state.lock);
        return out;
    }

    zone_set_t parsed;
    char*      err = zone_parse_json(body, &parsed);
    if (err) {
        char* quoted = json_quoted(err);
        char* out    = g_strdup_printf("{\"error\":%s}", quoted);
        g_free(quoted);
        g_free(err);
        return out;
    }

    g_mutex_lock(&state.lock);
    const bool saved = zone_save(&parsed, ZONES_PATH);
    if (saved) {
        state.zones = parsed;
        /* Closes any dwell in progress with a proper Exited record before the
         * geometry it was measured against disappears. Those events still go
         * out under the current declarations, which is why the re-declaration
         * below happens afterwards. */
        tracker_set_zones(state.tracker, &parsed);
    }
    g_mutex_unlock(&state.lock);

    if (!saved) {
        return g_strdup("{\"error\":\"could not persist zones\"}");
    }

    /* Declarations are keyed by zone id, so a changed zone set needs new ones.
     * Outside the lock — this performs IPC. */
    if (!events_reinit(&parsed)) {
        syslog(LOG_WARNING, "EVENT_REINIT incomplete after zone change");
    }

    char* zones = zone_to_json(&parsed);
    char* out   = g_strdup_printf("{\"saved\":true,\"zones\":%s}", zones);
    g_free(zones);
    return out;
}

static char* http_body(const char* endpoint,
                       const char* method,
                       const char* query,
                       const char* body,
                       void* user) {
    (void)user;

    if (strcmp(endpoint, "test") == 0) {
        return handle_test(query);
    }

    if (strcmp(endpoint, "config") == 0) {
        return handle_config(method, body);
    }

    if (strcmp(endpoint, "zones") == 0) {
        return handle_zones(method, body);
    }

    if (strcmp(endpoint, "status") == 0) {
        g_mutex_lock(&state.lock);
        char* objects = tracker_status_json(state.tracker);
        char* zones   = zone_to_json(&state.zones);
        g_mutex_unlock(&state.lock);

        char* out = g_strdup_printf("{\"objects\":%s,\"zones\":%s}", objects, zones);
        g_free(objects);
        g_free(zones);
        return out;
    }

    if (strcmp(endpoint, "health") == 0) {
        g_mutex_lock(&state.lock);

        const gint64 now_mono = g_get_monotonic_time();
        const double since_frame =
            state.last_frame_monotonic > 0
                ? (double)(now_mono - state.last_frame_monotonic) / 1e6
                : -1.0;

        GString* classes = g_string_new("{");
        GHashTableIter iter;
        gpointer       key, value;
        g_hash_table_iter_init(&iter, state.class_counts);
        bool first = true;
        while (g_hash_table_iter_next(&iter, &key, &value)) {
            g_string_append_printf(classes,
                                   "%s\"%s\":%u",
                                   first ? "" : ",",
                                   (const char*)key,
                                   GPOINTER_TO_UINT(value));
            first = false;
        }
        g_string_append_c(classes, '}');

        /* Health is deliberately not "the process is alive" — an ACAP can
         * report Running while its data source never connects. */
        const bool subscribed = (data_subscriber != NULL);
        const bool receiving  = since_frame >= 0.0 && since_frame < 120.0;

        char* out = g_strdup_printf(
            "{\"ok\":%s,\"subscribed\":%s,\"receiving\":%s,"
            "\"framesSeen\":%" G_GUINT64_FORMAT ",\"detectionsSeen\":%" G_GUINT64_FORMAT ","
            "\"unclassified\":%" G_GUINT64_FORMAT ",\"renames\":%" G_GUINT64_FORMAT ","
            "\"trackEnds\":%" G_GUINT64_FORMAT ",\"eventsEmitted\":%" G_GUINT64_FORMAT ","
            "\"secondsSinceLastFrame\":%.1f,\"inZone\":%u,\"tracks\":%u,\"zones\":%d,"
            "\"channelId\":%d,\"eventsDeclared\":%u,\"eventsReady\":%u,\"classesSeen\":%s}",
            (subscribed && receiving) ? "true" : "false",
            subscribed ? "true" : "false",
            receiving ? "true" : "false",
            state.frames_seen,
            state.detections_seen,
            state.unclassified_seen,
            state.renames_seen,
            state.trackends_seen,
            state.events_emitted,
            since_frame,
            tracker_in_zone_count(state.tracker),
            tracker_track_count(state.tracker),
            state.zones.n_zones,
            state.last_channel_id,
            events_declared_count(),
            events_ready_count(),
            classes->str);

        g_string_free(classes, TRUE);
        g_mutex_unlock(&state.lock);
        return out;
    }

    return NULL;
}

/* ---------------------------------------------------------------- summary */

static gboolean on_summary_timer(gpointer user_data) {
    (void)user_data;

    g_mutex_lock(&state.lock);

    GString*       classes = g_string_new(NULL);
    GHashTableIter iter;
    gpointer       key, value;
    g_hash_table_iter_init(&iter, state.class_counts);
    bool first = true;
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        g_string_append_printf(classes,
                               "%s%s:%u",
                               first ? "" : ",",
                               (const char*)key,
                               GPOINTER_TO_UINT(value));
        first = false;
    }
    if (first) {
        g_string_append(classes, "none");
    }

    const double span_s = (state.last_frame_us > state.first_frame_us)
                              ? (double)(state.last_frame_us - state.first_frame_us) / 1e6
                              : 0.0;

    syslog(LOG_INFO,
           "DWELL_SUMMARY frames=%" G_GUINT64_FORMAT " detections=%" G_GUINT64_FORMAT
           " unclassified=%" G_GUINT64_FORMAT " renames=%" G_GUINT64_FORMAT
           " trackends=%" G_GUINT64_FORMAT " events=%" G_GUINT64_FORMAT
           " in_zone=%u tracks=%u zones=%d fps=%.2f decl=%u/%u",
           state.frames_seen,
           state.detections_seen,
           state.unclassified_seen,
           state.renames_seen,
           state.trackends_seen,
           state.events_emitted,
           tracker_in_zone_count(state.tracker),
           tracker_track_count(state.tracker),
           state.zones.n_zones,
           span_s > 0.0 ? (double)state.frames_seen / span_s : 0.0,
           events_ready_count(),
           events_declared_count());

    /* Still the OQ-3 evidence: which classes this camera actually emits. */
    syslog(LOG_INFO, "DWELL_CLASSES seen=[%s]", classes->str);

    g_string_free(classes, TRUE);
    g_mutex_unlock(&state.lock);

    return G_SOURCE_CONTINUE;
}

/* -------------------------------------------------------------- lifecycle */

static bool initialize_client(void) {
    DHError* err = NULL;

    client = dh_client_create("object_dwell_timer", &err);
    if (!client) {
        check_err(err, "dh_client_create");
        return false;
    }

    err = NULL;
    if (!dh_client_connect(client, &err)) {
        check_err(err, "dh_client_connect");
        return false;
    }

    const DHConnectionState conn = dh_client_get_connection_state(client);
    syslog(LOG_INFO, "DDH_CONNECTED state=%d", (int)conn);
    return true;
}

static void enumerate_topics(void) {
    DHError*     err  = NULL;
    DHTopicList* list = dh_client_get_topic_list(client, &err);
    if (!list) {
        check_err(err, "dh_client_get_topic_list");
        return;
    }

    const uint32_t count = dh_topic_list_get_count(list);
    bool           have_frame = false;
    for (uint32_t i = 0; i < count; i++) {
        const char* name = dh_topic_list_get_name(list, i);
        if (name && strcmp(name, TOPIC_FRAME) == 0) {
            have_frame = true;
        }
    }

    syslog(LOG_INFO, "DDH_TOPICS count=%u frame_v1=%s", count,
           have_frame ? "PRESENT" : "ABSENT");
    if (!have_frame) {
        syslog(LOG_ERR,
               "DDH_MISSING_TOPIC %s not offered — no metadata producer active?",
               TOPIC_FRAME);
    }

    dh_topic_list_destroy(list);
}

static bool setup_subscription(void) {
    DHError* err = NULL;

    data_subscriber = dh_client_create_subscriber(client, "object_dwell_timer_sub", &err);
    if (!data_subscriber) {
        check_err(err, "dh_client_create_subscriber");
        return false;
    }

    err = NULL;
    if (!dh_subscriber_set_data_callback(data_subscriber, on_data, NULL, &err)) {
        check_err(err, "dh_subscriber_set_data_callback");
        return false;
    }

    DHFilter* filter = dh_filter_create();
    if (!filter) {
        syslog(LOG_ERR, "DDH_ERR ctx=dh_filter_create msg=null");
        return false;
    }

    const char* topics[] = {TOPIC_FRAME, TOPIC_OBJECTTRACK};
    for (size_t i = 0; i < G_N_ELEMENTS(topics); i++) {
        err = NULL;
        if (!dh_filter_add_topic_name(filter, topics[i], &err)) {
            check_err(err, "dh_filter_add_topic_name");
            dh_filter_destroy(filter);
            return false;
        }
    }

    DHSubscribeOptions* options = dh_subscribe_options_create();
    if (!options) {
        dh_filter_destroy(filter);
        return false;
    }

    err = NULL;
    if (!dh_subscribe_options_add_filter(options, filter, &err)) {
        check_err(err, "dh_subscribe_options_add_filter");
        dh_filter_destroy(filter);
        dh_subscribe_options_destroy(options);
        return false;
    }
    dh_filter_destroy(filter);

    dh_subscribe_options_set_enable_data_updates(options, true);
    dh_subscribe_options_set_start_from(options, DH_START_FROM_NOW);

    err = NULL;
    const bool ok = dh_subscriber_subscribe(data_subscriber, options, &err);
    dh_subscribe_options_destroy(options);
    if (!ok) {
        check_err(err, "dh_subscriber_subscribe");
        return false;
    }

    syslog(LOG_INFO, "DDH_SUBSCRIBED topic=%s", TOPIC_FRAME);
    return true;
}

static void load_zones(void) {
    if (g_mkdir_with_parents(LOCALDATA_DIR, 0755) != 0) {
        syslog(LOG_WARNING, "LOCALDATA_MKDIR failed dir=%s", LOCALDATA_DIR);
    }

    if (zone_load(&state.zones, ZONES_PATH)) {
        syslog(LOG_INFO, "ZONES_LOADED count=%d path=%s", state.zones.n_zones, ZONES_PATH);
        return;
    }

    zone_set_defaults(&state.zones);
    if (zone_save(&state.zones, ZONES_PATH)) {
        syslog(LOG_INFO,
               "ZONES_DEFAULTED count=%d path=%s (draw real zones in the config UI)",
               state.zones.n_zones,
               ZONES_PATH);
    }
}

static void cleanup_resources(void) {
    httpd_stop();

    if (data_subscriber) {
        dh_subscriber_destroy(data_subscriber);
        data_subscriber = NULL;
    }
    if (client) {
        DHError* err = NULL;
        dh_client_disconnect(client, &err);
        check_err(err, "dh_client_disconnect");
        dh_client_destroy(client);
        client = NULL;
    }

    events_shutdown();
    config_shutdown();

    if (state.tracker) {
        tracker_free(state.tracker);
        state.tracker = NULL;
    }
    if (state.class_counts) {
        g_hash_table_destroy(state.class_counts);
        state.class_counts = NULL;
    }
    g_mutex_clear(&state.lock);
}

static gboolean on_signal(gpointer user_data) {
    (void)user_data;
    syslog(LOG_INFO, "SHUTDOWN");
    g_main_loop_quit(main_loop);
    return G_SOURCE_REMOVE;
}

int main(void) {
    openlog(NULL, LOG_PID, LOG_USER);
    syslog(LOG_INFO, "START version=0.1.0 sdk=12.11");

    g_mutex_init(&state.lock);
    state.class_counts    = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    state.last_channel_id = -1;

    config_set_defaults(&state.cfg);
    load_zones();

    state.tracker = tracker_new(&state.cfg, &state.zones, on_emit, NULL);

    /* After the tracker exists — a parameter callback can fire during init. */
    if (!config_init(&state.cfg, on_config_changed, NULL)) {
        syslog(LOG_WARNING, "CONFIG_INIT failed; running with compiled-in defaults");
    }
    tracker_set_config(state.tracker, &state.cfg);

    if (!events_init(&state.zones)) {
        syslog(LOG_WARNING, "EVENT_INIT incomplete — dwell records may not reach the VMS");
    }

    main_loop = g_main_loop_new(NULL, FALSE);
    g_unix_signal_add(SIGTERM, on_signal, NULL);
    g_unix_signal_add(SIGINT, on_signal, NULL);

    if (!httpd_start(HTTP_PORT, http_body, NULL)) {
        syslog(LOG_ERR, "FATAL http server failed to bind");
        cleanup_resources();
        g_main_loop_unref(main_loop);
        return EXIT_FAILURE;
    }

    if (!initialize_client()) {
        syslog(LOG_ERR, "FATAL device data hub connect failed");
        cleanup_resources();
        g_main_loop_unref(main_loop);
        return EXIT_FAILURE;
    }

    enumerate_topics();

    if (!setup_subscription()) {
        syslog(LOG_ERR, "FATAL metadata subscription failed");
        cleanup_resources();
        g_main_loop_unref(main_loop);
        return EXIT_FAILURE;
    }

    g_timeout_add_seconds(SUMMARY_EVERY_S, on_summary_timer, NULL);

    g_main_loop_run(main_loop);

    cleanup_resources();
    g_main_loop_unref(main_loop);
    syslog(LOG_INFO, "STOPPED");
    return EXIT_SUCCESS;
}
