/**
 * Settings, backed by AXParameter.
 *
 * Copyright (c) 2026 MRDN Labs. MIT licensed.
 *
 * Values live in AXParameter rather than a private file so they are visible and
 * scriptable through param.cgi and the device's own tooling, and so they
 * survive a firmware upgrade. Zone polygons are too structured for a parameter
 * and stay in localdata/zones.json.
 *
 * Parameters are read on demand, never snapshotted at startup: a change made
 * through param.cgi or the device UI must take effect without a restart.
 */

#ifndef CONFIG_H
#define CONFIG_H

#include "dwell.h"

/** Invoked when any parameter changes, from any source. */
typedef void (*config_changed_fn)(void* user);

bool config_init(config_t* cfg, config_changed_fn cb, void* user);
void config_shutdown(void);

/** Re-read every parameter into cfg. */
bool config_reload(config_t* cfg);

/** Settings as JSON for the UI. Caller frees with g_free(). */
char* config_to_json(const config_t* cfg);

/**
 * Validate a JSON settings object and write accepted values to AXParameter.
 * Returns NULL on success, or a newly allocated human-readable error.
 */
char* config_apply_json(const char* json);

/** True when cls is a class this application will ever count. */
bool config_is_known_class(const char* cls);

#endif /* CONFIG_H */
