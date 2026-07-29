/**
 * AXEvent declaration and dispatch.
 *
 * Copyright (c) 2026 MRDN Labs. MIT licensed.
 *
 * One declaration per (zone, event kind). `zone` is declared as an ONVIF
 * *source* key rather than a data key: sources are what a VMS exposes as a
 * selectable instance, and the device's MQTT bridge renders them into the topic
 * path as $source/zone/<id> — which is what gives per-zone MQTT topics
 * (verified on hardware, see learnings/phase0-device-findings.md).
 */

#ifndef EVENTS_H
#define EVENTS_H

#include "dwell.h"

/** Declares every event for every zone. Declaration completes asynchronously. */
bool events_init(const zone_set_t* zones);

void events_shutdown(void);

/** Re-declare for a changed zone set. Declarations are keyed by zone id. */
bool events_reinit(const zone_set_t* zones);

/** Matches tracker_emit_fn, so it can be handed straight to the tracker. */
void events_emit(const dwell_event_t* ev, void* user);

/** How many declarations the event system has confirmed. */
guint events_ready_count(void);

/** How many declarations were requested. */
guint events_declared_count(void);

#endif /* EVENTS_H */
