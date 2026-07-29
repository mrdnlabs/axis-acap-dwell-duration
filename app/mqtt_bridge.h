/**
 * Device MQTT event bridge configuration.
 *
 * Copyright (c) 2026 MRDN Labs. MIT licensed.
 *
 * The application never holds broker credentials. It declares AXEvents and
 * asks the device's own MQTT client to publish them, which inherits whatever
 * host, TLS and credentials the operator configured.
 *
 * Writes are read-merge-write: `configureEventPublication` replaces the entire
 * filter list, so posting only our own filters would silently delete every
 * filter the operator had.
 */

#ifndef MQTT_BRIDGE_H
#define MQTT_BRIDGE_H

#include "dwell.h"

bool mqtt_bridge_init(void);
void mqtt_bridge_shutdown(void);

/**
 * Client state, bridge state, the resolved topic strings for each event, a
 * wildcard subscribe string and a sample payload. Caller frees with g_free().
 */
char* mqtt_bridge_state_json(const zone_set_t* zones);

/**
 * Add or remove this application's event filters, preserving every other
 * entry. Idempotent. Returns NULL on success or an error message to free.
 */
char* mqtt_bridge_configure(bool enable);

#endif /* MQTT_BRIDGE_H */
