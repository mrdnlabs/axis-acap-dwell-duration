/**
 * Per-object dwell tracking.
 *
 * Copyright (c) 2026 MRDN Labs. MIT licensed.
 */

#include "tracker.h"

#include <jansson.h>
#include <math.h>
#include <string.h>
#include <syslog.h>

#include "zone.h"

typedef enum { DW_ABSENT = 0, DW_PENDING_IN, DW_IN, DW_PENDING_OUT } dwell_state_t;

typedef struct {
    bool          inside; /* last known membership, carried across absent frames */
    dwell_state_t state;
    int64_t       first_inside_us; /* set on the ABSENT -> inside transition */
    int64_t       entry_us;        /* dwell origin; backdated to first_inside  */
    int64_t       last_inside_us;  /* last confirmed sighting inside; ends the total */
    int64_t       left_at_us;      /* first frame seen outside; starts exit debounce */
    int64_t       last_update_us;
    bool          threshold_fired;
} zstate_t;

typedef struct {
    char     id[TRACK_ID_LEN];
    zstate_t z[MAX_ZONES];

    char   best_class[CLASS_LEN];
    double best_score;

    int64_t first_seen_us;
    int64_t last_seen_us;

    point_t last_ref;
    point_t window_ref; /* position at the start of the stationary window */
    int64_t window_us;
    bool    stationary;

    bool    ended; /* TrackEnded seen; gap budget is now running */
    int64_t ended_us;

    bool seen_this_frame;
} track_t;

struct tracker_t {
    config_t        cfg;
    zone_set_t      zones;
    GHashTable*     tracks; /* id -> track_t*, value-owned */
    tracker_emit_fn emit;
    void*           user;

    int64_t now_us;
    int64_t prev_frame_us;
    int64_t prev_mono_us;
    guint64 clock_steps;
};

/* ------------------------------------------------------------------ helpers */

bool tracker_is_attribute_class(const char* cls) {
    /* Head and LicensePlate are attributes of a parent object and carry their
     * own object_track_id. Counting them would time the same physical object
     * twice — one person emits both a Human track and a Head track. */
    return strcmp(cls, "Head") == 0 || strcmp(cls, "LicensePlate") == 0;
}

static bool is_vehicle_class(const char* cls) {
    return strcmp(cls, "Car") == 0 || strcmp(cls, "Truck") == 0 ||
           strcmp(cls, "Bus") == 0 || strcmp(cls, "Bike") == 0 ||
           strcmp(cls, "VehicleOther") == 0 || strcmp(cls, "Vehicle") == 0;
}

/**
 * Eligibility is evaluated against the track's best-ever class, not the class
 * on the current frame, so a momentary flicker to unclassified does not drop a
 * dwelling object.
 *
 * A track that is not yet eligible still records first_inside_us, so when a
 * late classification arrives the entry time is backdated automatically and no
 * early dwell is lost (EC-5).
 */
static bool class_eligible(const config_t* cfg, const char* cls, double score) {
    if (cls[0] == '\0') {
        return false; /* never classified yet */
    }
    if (tracker_is_attribute_class(cls)) {
        return false;
    }
    if (score < cfg->min_score) {
        return false;
    }

    for (int i = 0; i < cfg->n_types; i++) {
        if (strcmp(cfg->types[i], cls) == 0) {
            return true;
        }
    }

    /* FR-2: an undetermined or unselected vehicle sub-type still counts when
     * the generic Vehicle type is selected. */
    if (cfg->fallback_to_vehicle && is_vehicle_class(cls)) {
        for (int i = 0; i < cfg->n_types; i++) {
            if (strcmp(cfg->types[i], "Vehicle") == 0) {
                return true;
            }
        }
    }
    return false;
}

static void emit(tracker_t* t,
                 const track_t* tr,
                 int zone_idx,
                 const char* kind,
                 const char* state,
                 double elapsed_s,
                 bool threshold_exceeded,
                 double overage_s) {
    if (!t->emit) {
        return;
    }

    dwell_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.kind = kind;
    g_strlcpy(ev.object_id, tr->id, sizeof(ev.object_id));
    g_strlcpy(ev.object_type,
              tr->best_class[0] ? tr->best_class : "Unknown",
              sizeof(ev.object_type));
    ev.zone_id            = t->zones.zones[zone_idx].id;
    ev.state              = state;
    ev.elapsed_s          = elapsed_s;
    ev.threshold_exceeded = threshold_exceeded;
    ev.overage_s          = overage_s;
    ev.utc_us             = t->now_us;
    ev.test               = false;

    t->emit(&ev, t->user);
}

static double secs(int64_t us) {
    return (double)us / 1e6;
}

/** Elapsed never goes negative, whatever the clock did (EC-6). */
static double elapsed_of(const tracker_t* t, const zstate_t* zs) {
    const int64_t d = t->now_us - zs->entry_us;
    return d > 0 ? secs(d) : 0.0;
}

static double overage_of(const tracker_t* t, const zstate_t* zs) {
    const double e = elapsed_of(t, zs);
    const double o = e - t->cfg.threshold_s;
    return o > 0.0 ? o : 0.0;
}

/* ------------------------------------------------------------- lifecycle */

tracker_t* tracker_new(const config_t* cfg,
                       const zone_set_t* zones,
                       tracker_emit_fn emit_fn,
                       void* user) {
    tracker_t* t = g_new0(tracker_t, 1);
    t->cfg       = *cfg;
    t->zones     = *zones;
    t->emit      = emit_fn;
    t->user      = user;
    t->tracks    = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    return t;
}

void tracker_free(tracker_t* t) {
    if (!t) {
        return;
    }
    if (t->tracks) {
        g_hash_table_destroy(t->tracks);
    }
    g_free(t);
}

/* ------------------------------------------------------------ frame input */

/** Shift every stored timestamp so relative durations survive a clock step. */
static void shift_all_timestamps(tracker_t* t, int64_t delta_us) {
    GHashTableIter iter;
    gpointer       key, value;
    g_hash_table_iter_init(&iter, t->tracks);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        track_t* tr = value;
        tr->first_seen_us += delta_us;
        tr->last_seen_us += delta_us;
        tr->window_us += delta_us;
        if (tr->ended) {
            tr->ended_us += delta_us;
        }
        for (int i = 0; i < MAX_ZONES; i++) {
            zstate_t* zs = &tr->z[i];
            zs->first_inside_us += delta_us;
            zs->entry_us += delta_us;
            zs->last_inside_us += delta_us;
            zs->left_at_us += delta_us;
            zs->last_update_us += delta_us;
        }
    }
}

void tracker_begin_frame(tracker_t* t, int64_t frame_us, int64_t monotonic_us) {
    if (t->prev_frame_us != 0) {
        /* Compare how much each clock advanced. A sparse metadata stream moves
         * both together; only a wall-clock step makes them diverge. Comparing
         * frame timestamps alone would misread every quiet period as a step. */
        const int64_t frame_delta = frame_us - t->prev_frame_us;
        const int64_t mono_delta  = monotonic_us - t->prev_mono_us;
        const int64_t divergence  = frame_delta - mono_delta;
        const int64_t limit       = (int64_t)(t->cfg.max_clock_step_s * 1e6);

        if (divergence > limit || divergence < -limit) {
            /* Rebase by the divergence so relative durations survive intact,
             * rather than letting dwell leap or go negative. */
            t->clock_steps++;
            shift_all_timestamps(t, divergence);
            syslog(LOG_WARNING,
                   "DWELL_CLOCKSTEP divergence_s=%.3f frame_delta_s=%.3f rebased=%u tracks",
                   secs(divergence),
                   secs(frame_delta),
                   g_hash_table_size(t->tracks));
        }
    }

    t->now_us        = frame_us;
    t->prev_frame_us = frame_us;
    t->prev_mono_us  = monotonic_us;

    GHashTableIter iter;
    gpointer       key, value;
    g_hash_table_iter_init(&iter, t->tracks);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        ((track_t*)value)->seen_this_frame = false;
    }
}

void tracker_rename(tracker_t* t, const char* from_id, const char* to_id) {
    if (!from_id || !to_id) {
        return;
    }
    track_t* old = g_hash_table_lookup(t->tracks, from_id);
    if (!old) {
        return;
    }

    /* Carry the whole dwell across so the timer continues under the new id.
     * If the target id already exists, keep the older entry — it holds the
     * earlier entry time, which is the one the operator cares about. */
    track_t* existing = g_hash_table_lookup(t->tracks, to_id);
    if (existing && existing->first_seen_us <= old->first_seen_us) {
        g_hash_table_remove(t->tracks, from_id);
        return;
    }

    track_t* moved = g_new0(track_t, 1);
    *moved         = *old;
    g_strlcpy(moved->id, to_id, sizeof(moved->id));
    g_hash_table_remove(t->tracks, from_id);
    g_hash_table_replace(t->tracks, g_strdup(to_id), moved);

    syslog(LOG_INFO, "DWELL_RENAME from=%s to=%s", from_id, to_id);
}

void tracker_track_ended(tracker_t* t, const char* track_id) {
    track_t* tr = g_hash_table_lookup(t->tracks, track_id);
    if (!tr) {
        return;
    }
    /* Deliberately not an exit. The gap budget starts here; the object may be
     * re-acquired, and while it was merely absent the source still owned it. */
    tr->ended    = true;
    tr->ended_us = t->now_us;
}

void tracker_detection(tracker_t* t, const detection_t* d) {
    track_t* tr = g_hash_table_lookup(t->tracks, d->track_id);
    if (!tr) {
        tr = g_new0(track_t, 1);
        g_strlcpy(tr->id, d->track_id, sizeof(tr->id));
        tr->first_seen_us = t->now_us;
        tr->window_ref    = d->ref;
        tr->window_us     = t->now_us;
        g_hash_table_replace(t->tracks, g_strdup(d->track_id), tr);
    }

    tr->seen_this_frame = true;
    tr->last_seen_us    = t->now_us;
    tr->last_ref        = d->ref;

    /* Re-acquisition: a track that ended and came back keeps its dwell. */
    tr->ended = false;

    if (d->cls[0] && d->score > tr->best_score) {
        g_strlcpy(tr->best_class, d->cls, sizeof(tr->best_class));
        tr->best_score = d->score;
    }

    /* Stationary assessment over a sliding window, used to pick the gap budget
     * once the track ends. */
    if (t->now_us - tr->window_us >= (int64_t)(t->cfg.stationary_window_s * 1e6)) {
        const double dx   = tr->last_ref.x - tr->window_ref.x;
        const double dy   = tr->last_ref.y - tr->window_ref.y;
        tr->stationary    = sqrt(dx * dx + dy * dy) < t->cfg.stationary_eps;
        tr->window_ref    = tr->last_ref;
        tr->window_us     = t->now_us;
    }

    for (int zi = 0; zi < t->zones.n_zones; zi++) {
        zstate_t*  zs     = &tr->z[zi];
        const bool inside = zone_contains(&t->zones.zones[zi], d->ref);

        if (inside && !zs->inside) {
            zs->first_inside_us = t->now_us;
        }
        if (inside) {
            zs->last_inside_us = t->now_us;
        }
        zs->inside = inside;
    }
}

/* ------------------------------------------------------------ frame output */

static void force_exit(tracker_t* t, track_t* tr, const char* reason) {
    for (int zi = 0; zi < t->zones.n_zones; zi++) {
        zstate_t* zs = &tr->z[zi];
        if (zs->state != DW_IN && zs->state != DW_PENDING_OUT) {
            continue;
        }

        const int64_t end_us  = zs->last_inside_us > zs->entry_us ? zs->last_inside_us : t->now_us;
        const int64_t total   = end_us - zs->entry_us;
        const double  elapsed = total > 0 ? secs(total) : 0.0;
        const double  over    = elapsed > t->cfg.threshold_s ? elapsed - t->cfg.threshold_s : 0.0;

        syslog(LOG_INFO,
               "DWELL_EXIT id=%s zone=%d total_s=%.3f reason=%s",
               tr->id,
               t->zones.zones[zi].id,
               elapsed,
               reason);

        emit(t, tr, zi, "exited", "out", elapsed, zs->threshold_fired, over);
        memset(zs, 0, sizeof(*zs));
    }
}

void tracker_end_frame(tracker_t* t) {
    GHashTableIter iter;
    gpointer       key, value;

    /* Collect removals rather than mutating the table mid-iteration. */
    GPtrArray* doomed = g_ptr_array_new();

    g_hash_table_iter_init(&iter, t->tracks);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        track_t* tr = value;

        /* --- gap budget, only ever after TrackEnded ------------------------ */
        if (tr->ended) {
            const double budget_s =
                tr->stationary ? t->cfg.stationary_hold_s : t->cfg.occlusion_max_gap_s;
            if (t->now_us - tr->ended_us > (int64_t)(budget_s * 1e6)) {
                force_exit(t, tr, tr->stationary ? "stationary-hold-expired" : "gap-expired");
                g_ptr_array_add(doomed, key);
                continue;
            }
        }

        /* --- hard staleness guard, bounds memory (NFR-4) ------------------- */
        const double stale_s = 2.0 * MAX(t->cfg.stationary_hold_s, t->cfg.occlusion_max_gap_s) + 60.0;
        if (t->now_us - tr->last_seen_us > (int64_t)(stale_s * 1e6)) {
            force_exit(t, tr, "stale");
            g_ptr_array_add(doomed, key);
            continue;
        }

        const bool eligible = class_eligible(&t->cfg, tr->best_class, tr->best_score);

        for (int zi = 0; zi < t->zones.n_zones; zi++) {
            zstate_t* zs = &tr->z[zi];

            /* Re-evaluate while the state keeps changing, so a transition and
             * the condition it immediately satisfies both resolve within one
             * frame. Without this, an object whose classification arrives late
             * would wait an extra frame to enter even though its backdated
             * first_inside_us already cleared the debounce. Bounded — the
             * machine cannot advance more than a few steps per frame. */
            for (int step = 0; step < 4; step++) {
                const dwell_state_t before = zs->state;

            switch (zs->state) {
            case DW_ABSENT:
                /* first_inside_us was recorded even while ineligible, so a late
                 * classification enters with its original, backdated time. */
                if (zs->inside && eligible) {
                    zs->state = DW_PENDING_IN;
                }
                break;

            case DW_PENDING_IN:
                if (!zs->inside || !eligible) {
                    zs->state = DW_ABSENT;
                } else if (t->now_us - zs->first_inside_us >=
                           (int64_t)(t->cfg.enter_debounce_s * 1e6)) {
                    zs->state           = DW_IN;
                    zs->entry_us        = zs->first_inside_us;
                    zs->last_update_us  = t->now_us;
                    zs->threshold_fired = false;

                    syslog(LOG_INFO,
                           "DWELL_ENTER id=%s zone=%d class=%s",
                           tr->id,
                           t->zones.zones[zi].id,
                           tr->best_class);
                    emit(t, tr, zi, "entered", "in", elapsed_of(t, zs), false, 0.0);
                }
                break;

            case DW_IN: {
                if (!zs->inside) {
                    zs->state      = DW_PENDING_OUT;
                    zs->left_at_us = t->now_us;
                    break;
                }

                const double e = elapsed_of(t, zs);

                if (!zs->threshold_fired && t->cfg.threshold_s > 0.0 &&
                    e >= t->cfg.threshold_s) {
                    zs->threshold_fired = true;
                    syslog(LOG_INFO,
                           "DWELL_THRESHOLD id=%s zone=%d elapsed_s=%.3f",
                           tr->id,
                           t->zones.zones[zi].id,
                           e);
                    emit(t, tr, zi, "threshold", "in", e, true, overage_of(t, zs));
                }

                if (t->cfg.update_interval_s > 0.0 &&
                    t->now_us - zs->last_update_us >=
                        (int64_t)(t->cfg.update_interval_s * 1e6)) {
                    zs->last_update_us = t->now_us;
                    emit(t, tr, zi, "update", "in", e, zs->threshold_fired,
                         overage_of(t, zs));
                }
                break;
            }

            case DW_PENDING_OUT:
                if (zs->inside) {
                    zs->state = DW_IN; /* timer untouched — re-entry is not a reset */
                } else if (t->now_us - zs->left_at_us >=
                           (int64_t)(t->cfg.exit_debounce_s * 1e6)) {
                    /* Debounce runs from when it was first seen outside, not
                     * from the last sighting inside — with a sparse metadata
                     * stream those differ by a whole frame interval, which
                     * would otherwise short-circuit the debounce entirely. */
                    const int64_t total = zs->last_inside_us - zs->entry_us;
                    const double  e     = total > 0 ? secs(total) : 0.0;
                    const double  over =
                        e > t->cfg.threshold_s ? e - t->cfg.threshold_s : 0.0;

                    syslog(LOG_INFO,
                           "DWELL_EXIT id=%s zone=%d total_s=%.3f reason=left-zone",
                           tr->id,
                           t->zones.zones[zi].id,
                           e);
                    emit(t, tr, zi, "exited", "out", e, zs->threshold_fired, over);
                    memset(zs, 0, sizeof(*zs));
                }
                break;
            }

                if (zs->state == before) {
                    break; /* settled */
                }
            }
        }
    }

    for (guint i = 0; i < doomed->len; i++) {
        g_hash_table_remove(t->tracks, g_ptr_array_index(doomed, i));
    }
    g_ptr_array_free(doomed, TRUE);
}

/* ------------------------------------------------------------ reconfiguration */

void tracker_set_config(tracker_t* t, const config_t* cfg) {
    t->cfg = *cfg;
}

void tracker_set_zones(tracker_t* t, const zone_set_t* zones) {
    GHashTableIter iter;
    gpointer       key, value;

    g_hash_table_iter_init(&iter, t->tracks);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        force_exit(t, (track_t*)value, "zones-changed");
    }

    t->zones = *zones;

    /* Per-zone slots are indexed by position, so any stale state left over
     * would now describe a different zone. force_exit cleared the active ones;
     * clear the rest so nothing carries across. */
    g_hash_table_iter_init(&iter, t->tracks);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        memset(((track_t*)value)->z, 0, sizeof(((track_t*)value)->z));
    }

    syslog(LOG_INFO, "DWELL_ZONES_APPLIED count=%d", zones->n_zones);
}

/* ---------------------------------------------------------------- reporting */

char* tracker_status_json(tracker_t* t) {
    json_t* arr = json_array();

    GHashTableIter iter;
    gpointer       key, value;
    g_hash_table_iter_init(&iter, t->tracks);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        const track_t* tr = value;

        for (int zi = 0; zi < t->zones.n_zones; zi++) {
            const zstate_t* zs = &tr->z[zi];
            if (zs->state != DW_IN && zs->state != DW_PENDING_OUT) {
                continue;
            }

            const double e = elapsed_of(t, zs);
            json_t*      o = json_object();
            json_object_set_new(o, "objectId", json_string(tr->id));
            json_object_set_new(o,
                                "objectType",
                                json_string(tr->best_class[0] ? tr->best_class : "Unknown"));
            json_object_set_new(o, "score", json_real(tr->best_score));
            json_object_set_new(o, "zoneId", json_integer(t->zones.zones[zi].id));
            json_object_set_new(o, "zoneName", json_string(t->zones.zones[zi].name));
            json_object_set_new(o, "elapsedSeconds", json_real(e));
            json_object_set_new(o, "thresholdExceeded", json_boolean(zs->threshold_fired));
            json_object_set_new(o, "overageSeconds", json_real(overage_of(t, zs)));
            json_object_set_new(o, "leaving", json_boolean(zs->state == DW_PENDING_OUT));
            json_object_set_new(o, "present", json_boolean(tr->seen_this_frame));
            json_object_set_new(o, "stationary", json_boolean(tr->stationary));
            json_object_set_new(o, "bridging", json_boolean(tr->ended));
            json_array_append_new(arr, o);
        }
    }

    char* text = json_dumps(arr, JSON_COMPACT | JSON_REAL_PRECISION(3));
    json_decref(arr);

    char* out = g_strdup(text ? text : "[]");
    free(text);
    return out;
}

int tracker_labels(tracker_t* t, dwell_label_t* out, int max) {
    int            n = 0;
    GHashTableIter iter;
    gpointer       key, value;

    g_hash_table_iter_init(&iter, t->tracks);
    while (g_hash_table_iter_next(&iter, &key, &value) && n < max) {
        const track_t* tr = value;

        for (int zi = 0; zi < t->zones.n_zones && n < max; zi++) {
            const zstate_t* zs = &tr->z[zi];
            if (zs->state != DW_IN && zs->state != DW_PENDING_OUT) {
                continue;
            }

            const double e     = elapsed_of(t, zs);
            const int    total = (int)e;
            const double over  = overage_of(t, zs);

            out[n].x    = tr->last_ref.x;
            out[n].y    = tr->last_ref.y;
            out[n].over = zs->threshold_fired;

            if (zs->threshold_fired) {
                g_snprintf(out[n].text,
                           sizeof(out[n].text),
                           "%s %d:%02d  +%d:%02d",
                           tr->best_class[0] ? tr->best_class : "Object",
                           total / 60,
                           total % 60,
                           (int)over / 60,
                           (int)over % 60);
            } else {
                g_snprintf(out[n].text,
                           sizeof(out[n].text),
                           "%s %d:%02d",
                           tr->best_class[0] ? tr->best_class : "Object",
                           total / 60,
                           total % 60);
            }
            n++;
        }
    }
    return n;
}

guint tracker_in_zone_count(tracker_t* t) {
    guint          n = 0;
    GHashTableIter iter;
    gpointer       key, value;

    g_hash_table_iter_init(&iter, t->tracks);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        const track_t* tr = value;
        for (int zi = 0; zi < t->zones.n_zones; zi++) {
            if (tr->z[zi].state == DW_IN || tr->z[zi].state == DW_PENDING_OUT) {
                n++;
            }
        }
    }
    return n;
}

guint tracker_track_count(tracker_t* t) {
    return g_hash_table_size(t->tracks);
}
