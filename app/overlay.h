/**
 * Video overlay: zone outlines and per-object elapsed time.
 *
 * Copyright (c) 2026 MRDN Labs. MIT licensed.
 *
 * Uses axoverlay2 with Cairo. Rendering runs on the GPU/CPU and touches no
 * DLPU, so it cannot contend with AXIS Object Analytics.
 *
 * The caller pushes a render model rather than the overlay reaching into
 * application state, which keeps the tracker lock out of the render path.
 */

#ifndef OVERLAY_H
#define OVERLAY_H

#include "dwell.h"
#include "tracker.h"

#define OVERLAY_MAX_LABELS 32

/** Attach to the video system. Safe to call when already started. */
bool overlay_start(void);

/** Detach and release every overlay. Safe to call when not started. */
void overlay_stop(void);

bool overlay_is_running(void);

/** Replace what is drawn. Copies its arguments; the caller keeps ownership. */
void overlay_set_model(const zone_set_t* zones, const dwell_label_t* labels, int n_labels);

#endif /* OVERLAY_H */
