/**
 * Zone geometry and persistence.
 *
 * Copyright (c) 2026 MRDN Labs. MIT licensed.
 */

#ifndef ZONE_H
#define ZONE_H

#include "dwell.h"

/** A single centred rectangle, used on first run until the UI draws real zones. */
void zone_set_defaults(zone_set_t* zs);

/** Load zones from JSON. Returns false if the file is missing or unusable. */
bool zone_load(zone_set_t* zs, const char* path);

/** Write zones atomically (temp file + rename) so a crash cannot truncate them. */
bool zone_save(const zone_set_t* zs, const char* path);

/** Ray-cast point-in-polygon. Points exactly on an edge are not guaranteed. */
bool zone_contains(const zone_t* z, point_t p);

/** Reference point of a detection under the configured rule. */
point_t zone_ref_point(const detection_t* d, ref_point_t mode);

/** Serialize zones to a JSON array string. Caller frees with g_free(). */
char* zone_to_json(const zone_set_t* zs);

#endif /* ZONE_H */
