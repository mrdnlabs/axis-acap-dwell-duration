/**
 * Object Dwell Timer — shared types.
 *
 * Copyright (c) 2026 MRDN Labs. MIT licensed.
 *
 * Coordinates are normalized 0–1 with origin top-left and Y increasing
 * downward, matching AXIS Scene Metadata bounding boxes exactly (verified on
 * hardware — see learnings/phase0-device-findings.md). No transform is needed
 * between metadata space and zone space.
 */

#ifndef DWELL_H
#define DWELL_H

#include <glib.h>
#include <stdbool.h>
#include <stdint.h>

#define MAX_ZONES       8
#define MAX_ZONE_VERTS  32
#define MAX_TYPES       12
#define TRACK_ID_LEN    48
#define CLASS_LEN       24

typedef enum { REF_BOTTOM_CENTER = 0, REF_CENTROID = 1 } ref_point_t;

typedef struct {
    double x, y;
} point_t;

typedef struct {
    int     id;
    char    name[48];
    bool    enabled;
    int     n_verts;
    point_t verts[MAX_ZONE_VERTS];
} zone_t;

typedef struct {
    zone_t zones[MAX_ZONES];
    int    n_zones;
} zone_set_t;

typedef struct {
    char        types[MAX_TYPES][CLASS_LEN];
    int         n_types;
    double      min_score;
    bool        fallback_to_vehicle;
    ref_point_t ref_point;

    double enter_debounce_s;
    double exit_debounce_s;
    double update_interval_s;
    double threshold_s;

    /* Gap budget is only consumed after TrackEnded — the device tracker reuses
     * one id across absences of 40–50 s, so absence alone must not end a dwell. */
    double occlusion_max_gap_s;
    double stationary_hold_s;
    double stationary_eps;
    double stationary_window_s;

    double max_clock_step_s;

    /* Let the app configure the device's MQTT event bridge for its own events.
     * Writes are read-merge-write, so operator filters are never disturbed. */
    bool mqtt_auto_configure;

    /* Draw zones and elapsed times onto the video stream. */
    bool overlay_enabled;
} config_t;

/** One detection lifted out of a metadata frame. */
typedef struct {
    char    track_id[TRACK_ID_LEN];
    char    cls[CLASS_LEN]; /* "" when the frame carried no class object */
    double  score;
    double  left, top, right, bottom;
    point_t ref;
} detection_t;

/** A record on its way to the event system and MQTT. */
typedef struct {
    const char* kind; /* entered | exited | threshold | update */
    char        object_id[TRACK_ID_LEN];
    char        object_type[CLASS_LEN];
    int         zone_id;
    const char* state; /* in | out */
    double      elapsed_s;
    bool        threshold_exceeded;
    double      overage_s;
    int64_t     utc_us;
    bool        test;
} dwell_event_t;

void config_set_defaults(config_t* cfg);

#endif /* DWELL_H */
