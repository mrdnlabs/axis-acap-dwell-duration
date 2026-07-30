/**
 * AXEvent declaration and dispatch.
 *
 * Copyright (c) 2026 MRDN Labs. MIT licensed.
 */

#include "events.h"

#include <axsdk/axevent.h>
#include <string.h>
#include <syslog.h>

#define NS_AXIS "tnsaxis"

typedef enum {
    KIND_ENTERED = 0,
    KIND_EXITED,
    KIND_THRESHOLD,
    KIND_UPDATE,
    KIND_COUNT
} event_kind_t;

static const char* const KIND_TOPIC[KIND_COUNT] = {
    "Entered",
    "Exited",
    "ThresholdExceeded",
    "DwellUpdate",
};

typedef struct {
    guint    declaration;
    gboolean ready;
    gboolean requested;
} decl_t;

static AXEventHandler* handler = NULL;
static decl_t          decls[MAX_ZONES][KIND_COUNT];
static zone_set_t      known_zones;
static guint           declared_total = 0;
static guint           ready_total    = 0;

static event_kind_t kind_from_string(const char* kind) {
    if (strcmp(kind, "entered") == 0) {
        return KIND_ENTERED;
    }
    if (strcmp(kind, "exited") == 0) {
        return KIND_EXITED;
    }
    if (strcmp(kind, "threshold") == 0) {
        return KIND_THRESHOLD;
    }
    return KIND_UPDATE;
}

static void on_declaration_complete(guint declaration, gpointer user_data) {
    (void)user_data;

    for (int z = 0; z < MAX_ZONES; z++) {
        for (int k = 0; k < KIND_COUNT; k++) {
            if (decls[z][k].requested && decls[z][k].declaration == declaration) {
                decls[z][k].ready = TRUE;
                ready_total++;
                syslog(LOG_INFO,
                       "EVENT_DECLARED topic=%s zone=%d id=%u (%u/%u)",
                       KIND_TOPIC[k],
                       known_zones.zones[z].id,
                       declaration,
                       ready_total,
                       declared_total);
                return;
            }
        }
    }
}

/**
 * Build the declaration shape: topic path, the zone source key, and every data
 * key with a placeholder value.
 */
static AXEventKeyValueSet* build_declaration(event_kind_t kind, int zone_id) {
    AXEventKeyValueSet* set = ax_event_key_value_set_new();
    GError*             err = NULL;

    const gint    zero_i = 0;
    const gdouble zero_d = 0.0;
    const gboolean false_b = FALSE;
    const char*   empty  = "";

    ax_event_key_value_set_add_key_value(set, "topic0", "tnsaxis",
                                         "CameraApplicationPlatform",
                                         AX_VALUE_TYPE_STRING, &err);
    ax_event_key_value_set_add_key_value(set, "topic1", NS_AXIS, "ObjectDwellTimer",
                                         AX_VALUE_TYPE_STRING, &err);
    ax_event_key_value_set_add_key_value(set, "topic2", NS_AXIS, KIND_TOPIC[kind],
                                         AX_VALUE_TYPE_STRING, &err);

    /* Source key — makes the zone a selectable instance in a VMS, and puts it
     * in the MQTT topic path. */
    ax_event_key_value_set_add_key_value(set, "zone", NULL, &zone_id,
                                         AX_VALUE_TYPE_INT, &err);
    ax_event_key_value_set_mark_as_source(set, "zone", NULL, &err);

    ax_event_key_value_set_add_key_value(set, "objectId", NULL, empty,
                                         AX_VALUE_TYPE_STRING, &err);
    ax_event_key_value_set_add_key_value(set, "objectType", NULL, empty,
                                         AX_VALUE_TYPE_STRING, &err);
    ax_event_key_value_set_add_key_value(set, "objectClass", NULL, empty,
                                         AX_VALUE_TYPE_STRING, &err);
    ax_event_key_value_set_add_key_value(set, "zoneName", NULL, empty,
                                         AX_VALUE_TYPE_STRING, &err);
    ax_event_key_value_set_add_key_value(set, "state", NULL, empty,
                                         AX_VALUE_TYPE_STRING, &err);
    ax_event_key_value_set_add_key_value(set, "elapsedSeconds", NULL, &zero_d,
                                         AX_VALUE_TYPE_DOUBLE, &err);
    ax_event_key_value_set_add_key_value(set, "thresholdExceeded", NULL, &false_b,
                                         AX_VALUE_TYPE_BOOL, &err);
    ax_event_key_value_set_add_key_value(set, "overageSeconds", NULL, &zero_d,
                                         AX_VALUE_TYPE_DOUBLE, &err);
    ax_event_key_value_set_add_key_value(set, "utcTime", NULL, empty,
                                         AX_VALUE_TYPE_STRING, &err);
    ax_event_key_value_set_add_key_value(set, "test", NULL, &false_b,
                                         AX_VALUE_TYPE_BOOL, &err);
    (void)zero_i;

    /* Mark the payload fields as data so they appear under message/data rather
     * than being treated as part of the event's identity. */
    static const char* const data_keys[] = {"objectId",
                                            "objectType",
                                            "objectClass",
                                            "zoneName",
                                            "state",
                                            "elapsedSeconds",
                                            "thresholdExceeded",
                                            "overageSeconds",
                                            "utcTime",
                                            "test"};
    for (size_t i = 0; i < G_N_ELEMENTS(data_keys); i++) {
        ax_event_key_value_set_mark_as_data(set, data_keys[i], NULL, &err);
    }

    if (err) {
        syslog(LOG_ERR, "EVENT_DECL_BUILD_ERR topic=%s msg=%s", KIND_TOPIC[kind],
               err->message);
        g_clear_error(&err);
    }
    return set;
}

bool events_init(const zone_set_t* zones) {
    known_zones = *zones;

    handler = ax_event_handler_new();
    if (!handler) {
        syslog(LOG_ERR, "EVENT_INIT failed to create handler");
        return false;
    }

    memset(decls, 0, sizeof(decls));
    declared_total = 0;
    ready_total    = 0;

    for (int z = 0; z < zones->n_zones; z++) {
        for (int k = 0; k < KIND_COUNT; k++) {
            AXEventKeyValueSet* set = build_declaration((event_kind_t)k, zones->zones[z].id);
            GError*             err = NULL;

            const gboolean ok = ax_event_handler_declare(handler,
                                                         set,
                                                         TRUE, /* stateless */
                                                         &decls[z][k].declaration,
                                                         on_declaration_complete,
                                                         NULL,
                                                         &err);
            ax_event_key_value_set_free(set);

            if (!ok) {
                syslog(LOG_ERR,
                       "EVENT_DECLARE_FAIL topic=%s zone=%d msg=%s",
                       KIND_TOPIC[k],
                       zones->zones[z].id,
                       err ? err->message : "unknown");
                g_clear_error(&err);
                continue;
            }
            decls[z][k].requested = TRUE;
            declared_total++;
        }
    }

    syslog(LOG_INFO, "EVENT_INIT declarations=%u", declared_total);
    return declared_total > 0;
}

bool events_reinit(const zone_set_t* zones) {
    events_shutdown();
    return events_init(zones);
}

void events_shutdown(void) {
    if (!handler) {
        return;
    }
    for (int z = 0; z < MAX_ZONES; z++) {
        for (int k = 0; k < KIND_COUNT; k++) {
            if (decls[z][k].requested) {
                GError* err = NULL;
                ax_event_handler_undeclare(handler, decls[z][k].declaration, &err);
                g_clear_error(&err);
            }
        }
    }
    ax_event_handler_free(handler);
    handler = NULL;
}

void events_emit(const dwell_event_t* ev, void* user) {
    (void)user;

    if (!handler) {
        return;
    }

    int zone_idx = -1;
    for (int z = 0; z < known_zones.n_zones; z++) {
        if (known_zones.zones[z].id == ev->zone_id) {
            zone_idx = z;
            break;
        }
    }
    if (zone_idx < 0) {
        syslog(LOG_WARNING, "EVENT_EMIT unknown zone=%d", ev->zone_id);
        return;
    }

    const event_kind_t kind = kind_from_string(ev->kind);
    const decl_t*      d    = &decls[zone_idx][kind];

    if (!d->requested || !d->ready) {
        /* Declarations complete asynchronously; anything produced before the
         * event system confirms them cannot be sent. Say so rather than fail
         * silently — a VMS integrator would otherwise see a missing event. */
        syslog(LOG_WARNING,
               "EVENT_DROPPED topic=%s zone=%d reason=declaration-not-ready",
               KIND_TOPIC[kind],
               ev->zone_id);
        return;
    }

    AXEventKeyValueSet* set = ax_event_key_value_set_new();
    GError*             err = NULL;

    /* UTC in ISO-8601. The MQTT bridge stamps its own epoch-millisecond time,
     * so this field is what carries IF-4's NTP-synced UTC to consumers. */
    GDateTime* utc  = g_date_time_new_from_unix_utc(ev->utc_us / 1000000);
    char*      when = utc ? g_date_time_format(utc, "%Y-%m-%dT%H:%M:%S") : NULL;
    char*      when_full =
        when ? g_strdup_printf("%s.%06dZ", when, (int)(ev->utc_us % 1000000)) : g_strdup("");

    const gboolean thr  = ev->threshold_exceeded ? TRUE : FALSE;
    const gboolean test = ev->test ? TRUE : FALSE;

    ax_event_key_value_set_add_key_value(set, "objectId", NULL, ev->object_id,
                                         AX_VALUE_TYPE_STRING, &err);
    ax_event_key_value_set_add_key_value(set, "objectType", NULL, ev->object_type,
                                         AX_VALUE_TYPE_STRING, &err);
    ax_event_key_value_set_add_key_value(set, "objectClass", NULL, ev->object_class,
                                         AX_VALUE_TYPE_STRING, &err);
    ax_event_key_value_set_add_key_value(set, "zoneName", NULL, ev->zone_name,
                                         AX_VALUE_TYPE_STRING, &err);
    ax_event_key_value_set_add_key_value(set, "state", NULL, ev->state,
                                         AX_VALUE_TYPE_STRING, &err);
    ax_event_key_value_set_add_key_value(set, "elapsedSeconds", NULL, &ev->elapsed_s,
                                         AX_VALUE_TYPE_DOUBLE, &err);
    ax_event_key_value_set_add_key_value(set, "thresholdExceeded", NULL, &thr,
                                         AX_VALUE_TYPE_BOOL, &err);
    ax_event_key_value_set_add_key_value(set, "overageSeconds", NULL, &ev->overage_s,
                                         AX_VALUE_TYPE_DOUBLE, &err);
    ax_event_key_value_set_add_key_value(set, "utcTime", NULL, when_full,
                                         AX_VALUE_TYPE_STRING, &err);
    ax_event_key_value_set_add_key_value(set, "test", NULL, &test,
                                         AX_VALUE_TYPE_BOOL, &err);

    if (err) {
        syslog(LOG_ERR, "EVENT_EMIT_BUILD_ERR msg=%s", err->message);
        g_clear_error(&err);
    }

    AXEvent* event = ax_event_new2(set, NULL);
    ax_event_key_value_set_free(set);

    if (!ax_event_handler_send_event(handler, d->declaration, event, &err)) {
        syslog(LOG_ERR,
               "EVENT_SEND_FAIL topic=%s zone=%d msg=%s",
               KIND_TOPIC[kind],
               ev->zone_id,
               err ? err->message : "unknown");
        g_clear_error(&err);
    }

    ax_event_free(event);
    if (utc) {
        g_date_time_unref(utc);
    }
    g_free(when);
    g_free(when_full);
}

guint events_ready_count(void) {
    return ready_total;
}

guint events_declared_count(void) {
    return declared_total;
}
