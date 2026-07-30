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

#define MAX_ZONES      8
#define MAX_ZONE_VERTS 32
#define MAX_CLASSES    12
#define TRACK_ID_LEN   48
#define CLASS_LEN      24
#define NAME_LEN       48

typedef enum { REF_BOTTOM_CENTER = 0, REF_CENTROID = 1 } ref_point_t;

typedef struct {
    double x, y;
} point_t;

/**
 * One object class the camera can emit, and what this application does with it.
 *
 * `name` is what operators see — on this page, on the overlay, and as
 * `objectType` in events. The raw camera class travels alongside it as
 * `objectClass`, so a VMS rule can match either.
 */
typedef struct {
    char   cls[CLASS_LEN]; /* raw camera class, e.g. "Truck"      */
    char   name[NAME_LEN]; /* operator-facing name                */
    bool   enabled;        /* does this class start a timer       */
    double min_score;      /* per-class floor; < 0 = use default  */
} class_cfg_t;

typedef struct {
    int     id;
    char    name[NAME_LEN];
    bool    enabled;
    int     n_verts;
    point_t verts[MAX_ZONE_VERTS];

    /* Optional per-zone overrides. A loading bay may care only about trucks
     * and want a long threshold; a doorway may want people and a short one. */
    double threshold_s;               /* <= 0 → use the global default */
    char   classes[MAX_CLASSES][CLASS_LEN];
    int    n_classes;                 /* 0 → use the globally enabled set */
} zone_t;

typedef struct {
    zone_t zones[MAX_ZONES];
    int    n_zones;
} zone_set_t;

typedef struct {
    class_cfg_t classes[MAX_CLASSES];
    int         n_classes;

    double      min_score; /* default floor for classes without their own */
    bool        fallback_to_vehicle;
    ref_point_t ref_point;

    double enter_debounce_s;
    double exit_debounce_s;
    double update_interval_s;
    double threshold_s; /* default; zones may override */

    /* Gap budget is only consumed after TrackEnded — the device tracker reuses
     * one id across absences of 40–50 s, so absence alone must not end a dwell. */
    double occlusion_max_gap_s;
    double stationary_hold_s;
    double stationary_eps;
    double stationary_window_s;

    double max_clock_step_s;

    bool mqtt_auto_configure;
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
    char        object_type[NAME_LEN];  /* friendly name */
    char        object_class[CLASS_LEN]; /* raw camera class */
    int         zone_id;
    char        zone_name[NAME_LEN];
    const char* state; /* in | out */
    double      elapsed_s;
    bool        threshold_exceeded;
    double      overage_s;
    int64_t     utc_us;
    bool        test;
} dwell_event_t;

void config_set_defaults(config_t* cfg);

/** The class entry for a raw camera class, or NULL if not configured. */
const class_cfg_t* config_find_class(const config_t* cfg, const char* cls);

/** Operator-facing name for a raw class, falling back to the class itself. */
const char* config_display_name(const config_t* cfg, const char* cls);

#endif /* DWELL_H */
