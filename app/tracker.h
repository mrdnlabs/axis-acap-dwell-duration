/**
 * Per-object dwell tracking.
 *
 * Copyright (c) 2026 MRDN Labs. MIT licensed.
 *
 * Usage per metadata frame:
 *
 *     tracker_begin_frame(t, frame_us);
 *     tracker_rename(t, from, to);        // apply renames FIRST
 *     tracker_track_ended(t, id);         // then track-end events
 *     tracker_detection(t, &det);         // then every detection
 *     tracker_end_frame(t);               // advances timers and emits
 *
 * The tracker holds no lock of its own — the caller serialises access.
 */

#ifndef TRACKER_H
#define TRACKER_H

#include "dwell.h"

typedef struct tracker_t tracker_t;

/** Called for each record to publish. Runs on the caller's thread. */
typedef void (*tracker_emit_fn)(const dwell_event_t* ev, void* user);

tracker_t* tracker_new(const config_t* cfg,
                       const zone_set_t* zones,
                       tracker_emit_fn emit,
                       void* user);
void       tracker_free(tracker_t* t);

/**
 * Start a frame.
 *
 * @param frame_us      metadata timestamp, camera UTC microseconds
 * @param monotonic_us  a monotonic reading taken at the same moment
 *
 * Both clocks are needed. A gap in the metadata stream and a jump in the wall
 * clock look identical if only frame timestamps are compared — and the frame
 * topic is genuinely sparse (~0.5 fps with an empty scene), so treating every
 * long gap as a clock step would corrupt live dwell times. Divergence between
 * the two clocks is what identifies a real step.
 */
void tracker_begin_frame(tracker_t* t, int64_t frame_us, int64_t monotonic_us);
void tracker_rename(tracker_t* t, const char* from_id, const char* to_id);
void tracker_track_ended(tracker_t* t, const char* track_id);
void tracker_detection(tracker_t* t, const detection_t* d);
void tracker_end_frame(tracker_t* t);

/** Apply new settings without disturbing dwells already in progress. */
void tracker_set_config(tracker_t* t, const config_t* cfg);

/**
 * Replace the zone set. Every dwell in progress is closed with a normal Exited
 * record first — the polygons they were measured against no longer exist, so
 * silently re-anchoring them to new geometry would report a dwell that never
 * happened.
 */
void tracker_set_zones(tracker_t* t, const zone_set_t* zones);

/** Current in-zone objects as a JSON array. Caller frees with g_free(). */
char* tracker_status_json(tracker_t* t);

/** Objects currently counted as dwelling in at least one zone. */
guint tracker_in_zone_count(tracker_t* t);

/** Total tracks held, including ineligible ones. Bounded; for diagnostics. */
guint tracker_track_count(tracker_t* t);

/** True when the class is an attribute of a parent object, not an object. */
bool tracker_is_attribute_class(const char* cls);

#endif /* TRACKER_H */
