/**
 * Object Dwell Timer — Phase 0 spike.
 *
 * Copyright (c) 2026 MRDN Labs. MIT licensed — see LICENSE.
 *
 * This build contains NO dwell logic. Its only job is to answer the four open
 * design questions against real hardware before the real application is built:
 *
 *   OQ-1  Does com.axis.scene.frame.v1 actually exist and deliver over the
 *         Device Data Hub on this device / AXIS OS build? (The startup topic
 *         enumeration answers this outright.)
 *   OQ-2  What is the bounding-box coordinate frame and Y orientation?
 *   OQ-3  Which class.type strings does this camera really emit — do vehicle
 *         sub-types (Truck/Car/Bus/Bike) appear, or only the generic Vehicle?
 *   OQ-4  How does track continuity behave — are Rename events emitted, how
 *         long do tracks survive, and how long does a *stationary* track live?
 *
 * Every log line carries a parseable SPIKE_* prefix so findings can be pulled
 * out of syslog mechanically rather than by eye.
 *
 * Built against ACAP Native SDK 12.11 (AXIS OS 12.11.77). NOTE: the Device
 * Data Hub C API differs between SDK 12.10 and 12.11 — 12.10 uses
 * DHClientError + listener objects + dh_subscriber_subscribe(topics...),
 * whereas 12.11 uses DHError + DHFilter + DHSubscribeOptions. This file
 * targets 12.11.
 *
 * Deliberately subscribes with NO instance-key filter so that a wrong guess
 * about channel_id cannot masquerade as "no metadata". channel_id is read from
 * the payload and logged instead. Filtering is introduced in Phase 1.
 *
 * Privacy: object snapshot imagery is never logged. If an `image` object
 * appears its `data` member is stripped before the payload is written anywhere.
 */

#define _GNU_SOURCE

#include <glib.h>
#include <glib-unix.h>
#include <inttypes.h>
#include <jansson.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include <datahub/client.h>
#include <datahub/common.h>
#include <datahub/subscriber.h>

#define TOPIC_FRAME       "com.axis.scene.frame.v1"
#define TOPIC_OBJECTTRACK "com.axis.scene.object_track.v1"

/* Legacy topics, reported if present so any fallback is evidence-based. */
#define TOPIC_LEGACY_FRAME "com.axis.analytics_scene_description.v0.beta"
#define TOPIC_LEGACY_TRACK "com.axis.consolidated_track.v1.beta"

#define RAW_DUMP_FIRST 3
#define RAW_DUMP_EVERY 600
#define SYSLOG_CHUNK   780

/* A track counts as stationary if its reference point never wandered further
 * than this (normalized units) from where it was first seen. */
#define STATIONARY_EPS 0.02

typedef struct {
    char    id[64];
    int64_t first_seen_us;
    int64_t last_seen_us;
    int64_t max_gap_us; /* largest observed absence, mid-track */
    double  first_x, first_y;
    double  last_x, last_y;
    double  max_excursion; /* furthest it ever got from its first position */
    guint64 frames;
    char    best_class[32];
    double  best_score;
    bool    ever_classified;
} track_stat_t;

typedef struct {
    GMutex      lock;
    GHashTable* tracks;       /* id -> track_stat_t*   (live tracks) */
    GHashTable* class_counts; /* class string -> count (OQ-3)        */
    guint64     frames_seen;
    guint64     raw_dumped;
    guint64     detections_seen;
    guint64     renames_seen;
    guint64     trackends_seen;
    guint64     objecttracks_seen;
    guint64     unclassified_seen;
    int64_t     first_frame_us;
    int64_t     last_frame_us;
    int64_t     longest_track_us;
    int64_t     longest_stationary_us; /* the number FR-5 hangs on */
    int         last_channel_id;
} spike_state_t;

static spike_state_t state;
static GMainLoop*    main_loop       = NULL;
static DHClient*     client          = NULL;
static DHSubscriber* data_subscriber = NULL;

static bool     check_err(DHError* err, const char* context);
static void     cleanup_resources(void);
static bool     initialize_client(void);
static void     enumerate_topics(void);
static bool     setup_subscription(void);
static void     on_data(const DHTopicSample* sample, void* user_data);
static void     on_topic_update(const char* topic_name,
                                DHTopicUpdateType update_type,
                                void* user_data);
static gboolean on_summary_timer(gpointer user_data);
static gboolean on_signal(gpointer user_data);
static int64_t  parse_iso8601_us(const char* ts);
static void     log_chunked(const char* tag, const char* text);
static void     strip_image_data(json_t* root);
static void     handle_frame(json_t* root);
static void     handle_object_track(json_t* root);
static void     finish_track(const char* id, const char* reason);

/* ------------------------------------------------------------------------- */
/* Helpers                                                                    */
/* ------------------------------------------------------------------------- */

static bool check_err(DHError* err, const char* context) {
    if (err) {
        syslog(LOG_ERR, "SPIKE_ERR ctx=%s msg=%s", context, dh_error_to_string(err));
        dh_error_destroy(err);
        return true;
    }
    return false;
}

/**
 * Parse "2026-03-20T10:17:48.024761Z" to microseconds since the UTC epoch.
 * Returns -1 if the string is not in the expected shape.
 */
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
        /* Scale a short fraction up to microseconds, skip any extra digits. */
        for (; digits < 6; digits++) {
            frac_us *= 10;
        }
        while (*rest >= '0' && *rest <= '9') {
            rest++;
        }
    }

    const time_t secs = timegm(&tm_val);
    if (secs == (time_t)-1) {
        return -1;
    }
    return (int64_t)secs * 1000000 + frac_us;
}

/** syslog truncates long lines, so split big payloads across numbered lines. */
static void log_chunked(const char* tag, const char* text) {
    const size_t len = strlen(text);
    if (len <= SYSLOG_CHUNK) {
        syslog(LOG_INFO, "%s %s", tag, text);
        return;
    }

    const size_t parts = (len + SYSLOG_CHUNK - 1) / SYSLOG_CHUNK;
    for (size_t i = 0; i < parts; i++) {
        const size_t off = i * SYSLOG_CHUNK;
        size_t       n   = len - off;
        if (n > SYSLOG_CHUNK) {
            n = SYSLOG_CHUNK;
        }
        syslog(LOG_INFO, "%s [%zu/%zu] %.*s", tag, i + 1, parts, (int)n, text + off);
    }
}

/** Never let snapshot imagery reach the log. Keeps Gate 0's privacy answer true. */
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

/* ------------------------------------------------------------------------- */
/* Track bookkeeping                                                          */
/* ------------------------------------------------------------------------- */

/** Emit the per-track summary that answers OQ-4 and sizes the FR-5 defaults. */
static void finish_track(const char* id, const char* reason) {
    track_stat_t* t = g_hash_table_lookup(state.tracks, id);
    if (!t) {
        return;
    }

    const int64_t lifetime_us = t->last_seen_us - t->first_seen_us;
    const bool    stationary  = t->max_excursion < STATIONARY_EPS;

    if (lifetime_us > state.longest_track_us) {
        state.longest_track_us = lifetime_us;
    }
    if (stationary && lifetime_us > state.longest_stationary_us) {
        state.longest_stationary_us = lifetime_us;
    }

    syslog(LOG_INFO,
           "SPIKE_TRACKEND id=%s reason=%s lifetime_s=%.3f frames=%" G_GUINT64_FORMAT
           " maxgap_s=%.3f excursion=%.4f stationary=%s class=%s score=%.3f "
           "lastpos=(%.4f,%.4f)",
           t->id,
           reason,
           (double)lifetime_us / 1e6,
           t->frames,
           (double)t->max_gap_us / 1e6,
           t->max_excursion,
           stationary ? "yes" : "no",
           t->ever_classified ? t->best_class : "none",
           t->best_score,
           t->last_x,
           t->last_y);

    g_hash_table_remove(state.tracks, id);
}

/* ------------------------------------------------------------------------- */
/* Payload handling                                                           */
/* ------------------------------------------------------------------------- */

static void handle_frame(json_t* root) {
    const char*   ts_str       = json_string_value(json_object_get(root, "timestamp"));
    const int64_t frame_us     = parse_iso8601_us(ts_str);
    json_t*       detections   = json_object_get(root, "detections");
    json_t*       track_events = json_object_get(root, "track_events");
    json_t*       channel      = json_object_get(root, "channel_id");

    state.frames_seen++;
    if (json_is_integer(channel)) {
        const json_int_t ch   = json_integer_value(channel);
        state.last_channel_id = (int)ch;
    }
    if (state.first_frame_us == 0 && frame_us > 0) {
        state.first_frame_us = frame_us;
    }
    state.last_frame_us = frame_us;

    /* --- Rename first: it is the device's own re-identification result. ---- */
    size_t  idx;
    json_t* ev;
    json_array_foreach(track_events, idx, ev) {
        const char* type = json_string_value(json_object_get(ev, "type"));
        if (!type) {
            continue;
        }

        if (strcmp(type, "Rename") == 0) {
            const char* from = json_string_value(json_object_get(ev, "from_id"));
            const char* to   = json_string_value(json_object_get(ev, "to_id"));
            state.renames_seen++;
            syslog(LOG_INFO, "SPIKE_RENAME from=%s to=%s", from ? from : "?", to ? to : "?");

            /* Carry accumulated stats across the rename so lifetime stays true
             * to the physical object rather than the track fragment. */
            if (from && to) {
                track_stat_t* old = g_hash_table_lookup(state.tracks, from);
                if (old) {
                    track_stat_t* moved = g_new0(track_stat_t, 1);
                    *moved              = *old;
                    g_strlcpy(moved->id, to, sizeof(moved->id));
                    g_hash_table_remove(state.tracks, from);
                    g_hash_table_replace(state.tracks, g_strdup(to), moved);
                }
            }
        } else if (strcmp(type, "TrackEnded") == 0) {
            const char* tid = json_string_value(json_object_get(ev, "object_track_id"));
            state.trackends_seen++;
            if (tid) {
                finish_track(tid, "TrackEnded");
            }
        } else {
            syslog(LOG_INFO, "SPIKE_TRACKEVT_UNKNOWN type=%s", type);
        }
    }

    /* --- Detections -------------------------------------------------------- */
    json_t* det;
    json_array_foreach(detections, idx, det) {
        const char* tid = json_string_value(json_object_get(det, "object_track_id"));
        if (!tid) {
            continue;
        }
        state.detections_seen++;

        json_t*      bbox   = json_object_get(det, "bounding_box");
        const double left   = json_number_value(json_object_get(bbox, "left"));
        const double top    = json_number_value(json_object_get(bbox, "top"));
        const double right  = json_number_value(json_object_get(bbox, "right"));
        const double bottom = json_number_value(json_object_get(bbox, "bottom"));

        /* Reference point under the plan's default rule: bbox bottom-centre. */
        const double ref_x = (left + right) / 2.0;
        const double ref_y = bottom;

        json_t*      cls       = json_object_get(det, "class");
        const char*  cls_type  = json_string_value(json_object_get(cls, "type"));
        const double cls_score = json_number_value(json_object_get(cls, "score"));

        if (cls_type) {
            gpointer prev = g_hash_table_lookup(state.class_counts, cls_type);
            g_hash_table_replace(state.class_counts,
                                 g_strdup(cls_type),
                                 GUINT_TO_POINTER(GPOINTER_TO_UINT(prev) + 1));
        } else {
            state.unclassified_seen++;
        }

        track_stat_t* t = g_hash_table_lookup(state.tracks, tid);
        if (!t) {
            t                = g_new0(track_stat_t, 1);
            t->first_seen_us = frame_us;
            t->last_seen_us  = frame_us;
            t->first_x       = ref_x;
            t->first_y       = ref_y;
            g_strlcpy(t->id, tid, sizeof(t->id));
            g_hash_table_replace(state.tracks, g_strdup(tid), t);

            /* sanity_bottom_gt_top is the OQ-2 evidence: if bottom > top then
             * the origin is top-left with Y increasing downward. */
            syslog(LOG_INFO,
                   "SPIKE_NEWTRACK id=%s class=%s score=%.3f bbox=[l=%.4f,t=%.4f,r=%.4f,b=%.4f] "
                   "ref=(%.4f,%.4f) sanity_bottom_gt_top=%s sanity_right_gt_left=%s",
                   tid,
                   cls_type ? cls_type : "none",
                   cls_score,
                   left,
                   top,
                   right,
                   bottom,
                   ref_x,
                   ref_y,
                   (bottom > top) ? "yes" : "NO",
                   (right > left) ? "yes" : "NO");
        } else {
            const int64_t gap = frame_us - t->last_seen_us;
            if (gap > t->max_gap_us) {
                t->max_gap_us = gap;
            }
        }

        const double dx        = ref_x - t->first_x;
        const double dy        = ref_y - t->first_y;
        const double excursion = sqrt(dx * dx + dy * dy);
        if (excursion > t->max_excursion) {
            t->max_excursion = excursion;
        }

        t->last_seen_us = frame_us;
        t->last_x       = ref_x;
        t->last_y       = ref_y;
        t->frames++;

        if (cls_type && cls_score > t->best_score) {
            g_strlcpy(t->best_class, cls_type, sizeof(t->best_class));
            t->best_score      = cls_score;
            t->ever_classified = true;
        }
    }
}

static void handle_object_track(json_t* root) {
    state.objecttracks_seen++;

    const char*  id       = json_string_value(json_object_get(root, "id"));
    const double duration = json_number_value(json_object_get(root, "duration"));
    json_t*      classes  = json_object_get(root, "classes");
    json_t*      parts    = json_object_get(root, "parts");

    /* classes[] is ranked — the spread across entries is exactly the EC-5
     * class-flip problem, and parts[] is the device's own re-association. */
    GString* cls_summary = g_string_new(NULL);
    size_t   idx;
    json_t*  c;
    json_array_foreach(classes, idx, c) {
        const char*  type  = json_string_value(json_object_get(c, "type"));
        const double score = json_number_value(json_object_get(c, "score"));
        g_string_append_printf(cls_summary,
                               "%s%s:%.3f",
                               idx ? "," : "",
                               type ? type : "?",
                               score);
    }

    syslog(LOG_INFO,
           "SPIKE_OBJTRACK id=%s duration_s=%.3f parts=%zu path_points=%zu classes=[%s]",
           id ? id : "?",
           duration,
           json_array_size(parts),
           json_array_size(json_object_get(root, "path")),
           cls_summary->str);

    g_string_free(cls_summary, TRUE);
}

static void on_data(const DHTopicSample* sample, void* user_data) {
    (void)user_data;

    const char*        topic_name = dh_topic_sample_get_topic_name(sample);
    const DHTopicData* topic_data = dh_topic_sample_get_data(sample);
    const char*        json_text  = topic_data ? dh_topic_data_get_json_data(topic_data) : NULL;
    if (!json_text) {
        return;
    }

    json_error_t err;
    json_t*      root = json_loads(json_text, 0, &err);
    if (!root) {
        syslog(LOG_ERR,
               "SPIKE_PARSE_FAIL topic=%s line=%d msg=%s",
               topic_name ? topic_name : "?",
               err.line,
               err.text);
        return;
    }

    strip_image_data(root);

    /* Callbacks run on internal library threads — everything below touches
     * shared state, so it is all under the one lock. */
    g_mutex_lock(&state.lock);

    const bool is_frame = topic_name && strcmp(topic_name, TOPIC_FRAME) == 0;

    const guint64 seq = state.frames_seen + state.objecttracks_seen;
    if (state.raw_dumped < RAW_DUMP_FIRST || (seq % RAW_DUMP_EVERY) == 0) {
        char* compact = json_dumps(root, JSON_COMPACT | JSON_PRESERVE_ORDER);
        if (compact) {
            log_chunked(is_frame ? "SPIKE_RAW_FRAME" : "SPIKE_RAW_OTHER", compact);
            free(compact);
            state.raw_dumped++;
        }
    }

    if (is_frame) {
        handle_frame(root);
    } else {
        handle_object_track(root);
    }

    g_mutex_unlock(&state.lock);
    json_decref(root);
}

static void on_topic_update(const char* topic_name,
                            DHTopicUpdateType update_type,
                            void* user_data) {
    (void)user_data;
    /* Watching this covers NFR-5: an AOA restart should show the topic going
     * away and coming back, and the subscription should survive it. */
    syslog(LOG_INFO,
           "SPIKE_TOPIC_UPDATE topic=%s event=%s",
           topic_name ? topic_name : "?",
           update_type == DH_TOPIC_CREATED ? "created" : "deleted");
}

/* ------------------------------------------------------------------------- */
/* Periodic summary — the actual OQ answers, in one greppable place           */
/* ------------------------------------------------------------------------- */

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

    const double elapsed_s = (state.last_frame_us > state.first_frame_us)
                                 ? (double)(state.last_frame_us - state.first_frame_us) / 1e6
                                 : 0.0;

    const DHConnectionState conn = dh_client_get_connection_state(client);

    syslog(LOG_INFO,
           "SPIKE_SUMMARY conn_state=%d frames=%" G_GUINT64_FORMAT
           " detections=%" G_GUINT64_FORMAT " objtracks=%" G_GUINT64_FORMAT
           " renames=%" G_GUINT64_FORMAT " trackends=%" G_GUINT64_FORMAT
           " unclassified=%" G_GUINT64_FORMAT " live_tracks=%u channel_id=%d"
           " span_s=%.1f fps=%.2f",
           (int)conn,
           state.frames_seen,
           state.detections_seen,
           state.objecttracks_seen,
           state.renames_seen,
           state.trackends_seen,
           state.unclassified_seen,
           g_hash_table_size(state.tracks),
           state.last_channel_id,
           elapsed_s,
           elapsed_s > 0.0 ? (double)state.frames_seen / elapsed_s : 0.0);

    syslog(LOG_INFO, "SPIKE_CLASSES seen=[%s]", classes->str);

    syslog(LOG_INFO,
           "SPIKE_LIFETIME longest_track_s=%.1f longest_stationary_s=%.1f",
           (double)state.longest_track_us / 1e6,
           (double)state.longest_stationary_us / 1e6);

    /* Live stationary tracks are the FR-5 evidence — report them as they run,
     * not only once they die, so a parked object can be watched in real time. */
    g_hash_table_iter_init(&iter, state.tracks);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        const track_stat_t* t = value;
        if (t->max_excursion < STATIONARY_EPS && t->frames > 10) {
            syslog(LOG_INFO,
                   "SPIKE_STATIONARY id=%s alive_s=%.1f frames=%" G_GUINT64_FORMAT
                   " excursion=%.4f class=%s",
                   t->id,
                   (double)(t->last_seen_us - t->first_seen_us) / 1e6,
                   t->frames,
                   t->max_excursion,
                   t->ever_classified ? t->best_class : "none");
        }
    }

    g_string_free(classes, TRUE);
    g_mutex_unlock(&state.lock);

    return G_SOURCE_CONTINUE;
}

/* ------------------------------------------------------------------------- */
/* Lifecycle                                                                  */
/* ------------------------------------------------------------------------- */

static bool initialize_client(void) {
    DHError* err = NULL;

    client = dh_client_create("object_dwell_timer_spike", &err);
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
    syslog(LOG_INFO, "SPIKE_CONNECTED state=%d", (int)conn);
    return true;
}

/**
 * OQ-1, answered directly: list every topic the Device Data Hub offers on this
 * device, and say explicitly whether the ones the design depends on are there.
 */
static void enumerate_topics(void) {
    DHError*     err  = NULL;
    DHTopicList* list = dh_client_get_topic_list(client, &err);
    if (!list) {
        check_err(err, "dh_client_get_topic_list");
        syslog(LOG_ERR, "SPIKE_TOPICLIST unavailable");
        return;
    }

    const uint32_t count = dh_topic_list_get_count(list);
    bool have_frame = false, have_track = false;
    bool have_legacy_frame = false, have_legacy_track = false;

    syslog(LOG_INFO, "SPIKE_TOPICLIST count=%u", count);
    for (uint32_t i = 0; i < count; i++) {
        const char* name = dh_topic_list_get_name(list, i);
        if (!name) {
            continue;
        }
        syslog(LOG_INFO, "SPIKE_TOPIC [%u/%u] %s", i + 1, count, name);

        if (strcmp(name, TOPIC_FRAME) == 0) {
            have_frame = true;
        } else if (strcmp(name, TOPIC_OBJECTTRACK) == 0) {
            have_track = true;
        } else if (strcmp(name, TOPIC_LEGACY_FRAME) == 0) {
            have_legacy_frame = true;
        } else if (strcmp(name, TOPIC_LEGACY_TRACK) == 0) {
            have_legacy_track = true;
        }
    }

    syslog(LOG_INFO,
           "SPIKE_OQ1 frame_v1=%s object_track_v1=%s legacy_beta_frame=%s legacy_beta_track=%s",
           have_frame ? "PRESENT" : "ABSENT",
           have_track ? "PRESENT" : "ABSENT",
           have_legacy_frame ? "present" : "absent",
           have_legacy_track ? "present" : "absent");

    if (!have_frame) {
        syslog(LOG_WARNING,
               "SPIKE_OQ1_FALLBACK %s not offered — Phase 1 must use the legacy path",
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

    err = NULL;
    if (!dh_subscriber_set_topic_update_callback(data_subscriber, on_topic_update, NULL, &err)) {
        check_err(err, "dh_subscriber_set_topic_update_callback");
        return false;
    }

    DHFilter* filter = dh_filter_create();
    if (!filter) {
        syslog(LOG_ERR, "SPIKE_ERR ctx=dh_filter_create msg=null");
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

    /* No instance-key filter on purpose — see the file header. */

    DHSubscribeOptions* options = dh_subscribe_options_create();
    if (!options) {
        syslog(LOG_ERR, "SPIKE_ERR ctx=dh_subscribe_options_create msg=null");
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
    dh_subscribe_options_set_enable_topic_updates(options, true);
    dh_subscribe_options_set_start_from(options, DH_START_FROM_NOW);

    err = NULL;
    const bool ok = dh_subscriber_subscribe(data_subscriber, options, &err);
    dh_subscribe_options_destroy(options);

    if (!ok) {
        check_err(err, "dh_subscriber_subscribe");
        return false;
    }

    for (size_t i = 0; i < G_N_ELEMENTS(topics); i++) {
        syslog(LOG_INFO, "SPIKE_SUBSCRIBED topic=%s", topics[i]);
    }
    return true;
}

static void cleanup_resources(void) {
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
    if (state.tracks) {
        g_hash_table_destroy(state.tracks);
        state.tracks = NULL;
    }
    if (state.class_counts) {
        g_hash_table_destroy(state.class_counts);
        state.class_counts = NULL;
    }
    g_mutex_clear(&state.lock);
}

static gboolean on_signal(gpointer user_data) {
    (void)user_data;
    syslog(LOG_INFO, "SPIKE_SHUTDOWN");
    g_main_loop_quit(main_loop);
    return G_SOURCE_REMOVE;
}

int main(void) {
    openlog(NULL, LOG_PID, LOG_USER);
    syslog(LOG_INFO, "SPIKE_START build=phase0 sdk=12.11");

    g_mutex_init(&state.lock);
    state.tracks          = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    state.class_counts    = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    state.last_channel_id = -1;

    main_loop = g_main_loop_new(NULL, FALSE);
    g_unix_signal_add(SIGTERM, on_signal, NULL);
    g_unix_signal_add(SIGINT, on_signal, NULL);

    if (!initialize_client()) {
        syslog(LOG_ERR, "SPIKE_FATAL connect failed");
        cleanup_resources();
        g_main_loop_unref(main_loop);
        return EXIT_FAILURE;
    }

    enumerate_topics();

    if (!setup_subscription()) {
        syslog(LOG_ERR, "SPIKE_FATAL subscribe failed");
        cleanup_resources();
        g_main_loop_unref(main_loop);
        return EXIT_FAILURE;
    }

    g_timeout_add_seconds(30, on_summary_timer, NULL);

    g_main_loop_run(main_loop);

    on_summary_timer(NULL);
    cleanup_resources();
    g_main_loop_unref(main_loop);
    syslog(LOG_INFO, "SPIKE_STOPPED");
    return EXIT_SUCCESS;
}
