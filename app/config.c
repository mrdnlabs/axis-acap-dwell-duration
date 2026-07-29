/**
 * Settings, backed by AXParameter.
 *
 * Copyright (c) 2026 MRDN Labs. MIT licensed.
 */

#include "config.h"

#include <axsdk/axparameter.h>
#include <jansson.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

#include "tracker.h"

#define APP_NAME "object_dwell_timer"

/* Classes this application will count. Head and LicensePlate are excluded by
 * construction — they are attributes of a parent object and carry their own
 * track id, so counting them would time the same object twice. */
static const char* const KNOWN_CLASSES[] =
    {"Human", "Vehicle", "Car", "Truck", "Bus", "Bike", "VehicleOther"};

static const char* const PARAM_NAMES[] = {"ObjectTypes",
                                          "MinScore",
                                          "FallbackToVehicle",
                                          "ReferencePoint",
                                          "EnterDebounce",
                                          "ExitDebounce",
                                          "UpdateInterval",
                                          "DwellThreshold",
                                          "OcclusionMaxGap",
                                          "StationaryHold"};

static AXParameter*      handle     = NULL;
static config_changed_fn change_cb  = NULL;
static void*             change_arg = NULL;

bool config_is_known_class(const char* cls) {
    for (size_t i = 0; i < G_N_ELEMENTS(KNOWN_CLASSES); i++) {
        if (strcmp(KNOWN_CLASSES[i], cls) == 0) {
            return true;
        }
    }
    return false;
}

/* ------------------------------------------------------------------ reading */

/** Parameter value as a newly allocated string, or NULL. */
static char* param_get(const char* name) {
    if (!handle) {
        return NULL;
    }
    gchar*  value = NULL;
    GError* err   = NULL;
    if (!ax_parameter_get(handle, name, &value, &err)) {
        syslog(LOG_WARNING, "PARAM_GET_FAIL name=%s msg=%s", name,
               err ? err->message : "unknown");
        g_clear_error(&err);
        return NULL;
    }
    return value;
}

/** Read a double, clamped into range; keeps the current value if unparseable. */
static double param_double(const char* name, double fallback, double lo, double hi) {
    char* raw = param_get(name);
    if (!raw || !*raw) {
        g_free(raw);
        return fallback;
    }

    char*        end = NULL;
    const double v   = g_ascii_strtod(raw, &end);
    const bool   ok  = end && end != raw;
    g_free(raw);

    if (!ok) {
        return fallback;
    }
    return CLAMP(v, lo, hi);
}

static bool param_bool(const char* name, bool fallback) {
    char* raw = param_get(name);
    if (!raw) {
        return fallback;
    }
    const bool v = (strcmp(raw, "yes") == 0 || strcmp(raw, "true") == 0 ||
                    strcmp(raw, "1") == 0);
    g_free(raw);
    return v;
}

/** Parse "Truck,Bus" into the type list, dropping anything unrecognised. */
static void parse_types(const char* csv, config_t* cfg) {
    cfg->n_types = 0;
    if (!csv || !*csv) {
        return;
    }

    char** parts = g_strsplit(csv, ",", MAX_TYPES + 4);
    for (int i = 0; parts[i] && cfg->n_types < MAX_TYPES; i++) {
        char* name = g_strstrip(parts[i]);
        if (!*name || !config_is_known_class(name)) {
            continue;
        }
        g_strlcpy(cfg->types[cfg->n_types], name, CLASS_LEN);
        cfg->n_types++;
    }
    g_strfreev(parts);
}

bool config_reload(config_t* cfg) {
    if (!handle) {
        return false;
    }

    char* types = param_get("ObjectTypes");
    parse_types(types, cfg);
    g_free(types);

    if (cfg->n_types == 0) {
        /* Never leave the filter empty — that would silently time nothing at
         * all, which looks identical to a broken metadata subscription. */
        syslog(LOG_WARNING, "PARAM_TYPES empty or unrecognised; keeping previous selection");
        config_t defaults;
        config_set_defaults(&defaults);
        memcpy(cfg->types, defaults.types, sizeof(cfg->types));
        cfg->n_types = defaults.n_types;
    }

    cfg->min_score           = param_double("MinScore", cfg->min_score, 0.0, 1.0);
    cfg->fallback_to_vehicle = param_bool("FallbackToVehicle", cfg->fallback_to_vehicle);

    char* ref = param_get("ReferencePoint");
    if (ref) {
        cfg->ref_point = (strcmp(ref, "centroid") == 0) ? REF_CENTROID : REF_BOTTOM_CENTER;
        g_free(ref);
    }

    cfg->enter_debounce_s  = param_double("EnterDebounce", cfg->enter_debounce_s, 0.0, 60.0);
    cfg->exit_debounce_s   = param_double("ExitDebounce", cfg->exit_debounce_s, 0.0, 300.0);
    cfg->update_interval_s = param_double("UpdateInterval", cfg->update_interval_s, 0.0, 3600.0);
    cfg->threshold_s       = param_double("DwellThreshold", cfg->threshold_s, 0.0, 86400.0);
    cfg->occlusion_max_gap_s =
        param_double("OcclusionMaxGap", cfg->occlusion_max_gap_s, 0.0, 3600.0);
    cfg->stationary_hold_s = param_double("StationaryHold", cfg->stationary_hold_s, 0.0, 86400.0);

    return true;
}

/* ----------------------------------------------------------------- callbacks */

static guint pending_reload = 0;

/**
 * Reload on a clean stack.
 *
 * Reading a parameter from inside a parameter-change callback deadlocks: the
 * synchronous get waits for a reply on the very connection that is currently
 * delivering the callback. It wedges the application *and* the param.cgi
 * request that triggered it. Deferring to an idle source lets the callback
 * return first, which is what breaks the cycle.
 */
static gboolean reload_on_idle(gpointer user_data) {
    (void)user_data;
    pending_reload = 0;
    if (change_cb) {
        change_cb(change_arg);
    }
    return G_SOURCE_REMOVE;
}

static void on_param_changed(const gchar* name, const gchar* value, gpointer data) {
    (void)data;
    syslog(LOG_INFO, "PARAM_CHANGED name=%s value=%s", name ? name : "?", value ? value : "");

    /* Coalesce — saving the settings form writes several parameters at once,
     * and one reload covers them all. */
    if (pending_reload == 0) {
        pending_reload = g_idle_add(reload_on_idle, NULL);
    }
}

bool config_init(config_t* cfg, config_changed_fn cb, void* user) {
    change_cb  = cb;
    change_arg = user;

    GError* err = NULL;
    handle      = ax_parameter_new(APP_NAME, &err);
    if (!handle) {
        syslog(LOG_ERR, "PARAM_INIT_FAIL msg=%s", err ? err->message : "unknown");
        g_clear_error(&err);
        return false;
    }

    for (size_t i = 0; i < G_N_ELEMENTS(PARAM_NAMES); i++) {
        err = NULL;
        if (!ax_parameter_register_callback(handle, PARAM_NAMES[i], on_param_changed, NULL, &err)) {
            syslog(LOG_WARNING, "PARAM_CB_FAIL name=%s msg=%s", PARAM_NAMES[i],
                   err ? err->message : "unknown");
            g_clear_error(&err);
        }
    }

    config_reload(cfg);
    syslog(LOG_INFO, "PARAM_INIT types=%d threshold_s=%.1f", cfg->n_types, cfg->threshold_s);
    return true;
}

void config_shutdown(void) {
    if (pending_reload != 0) {
        g_source_remove(pending_reload);
        pending_reload = 0;
    }
    if (!handle) {
        return;
    }
    for (size_t i = 0; i < G_N_ELEMENTS(PARAM_NAMES); i++) {
        ax_parameter_unregister_callback(handle, PARAM_NAMES[i]);
    }
    ax_parameter_free(handle);
    handle = NULL;
}

/* ------------------------------------------------------------------ writing */

static bool param_set(const char* name, const char* value) {
    if (!handle) {
        return false;
    }
    GError* err = NULL;
    if (!ax_parameter_set(handle, name, value, TRUE, &err)) {
        syslog(LOG_ERR, "PARAM_SET_FAIL name=%s msg=%s", name,
               err ? err->message : "unknown");
        g_clear_error(&err);
        return false;
    }
    return true;
}

char* config_to_json(const config_t* cfg) {
    json_t* o = json_object();

    json_t* types = json_array();
    for (int i = 0; i < cfg->n_types; i++) {
        json_array_append_new(types, json_string(cfg->types[i]));
    }
    json_object_set_new(o, "objectTypes", types);

    json_t* available = json_array();
    for (size_t i = 0; i < G_N_ELEMENTS(KNOWN_CLASSES); i++) {
        json_array_append_new(available, json_string(KNOWN_CLASSES[i]));
    }
    json_object_set_new(o, "availableTypes", available);

    json_object_set_new(o, "minScore", json_real(cfg->min_score));
    json_object_set_new(o, "fallbackToVehicle", json_boolean(cfg->fallback_to_vehicle));
    json_object_set_new(o,
                        "referencePoint",
                        json_string(cfg->ref_point == REF_CENTROID ? "centroid" : "bottomCenter"));
    json_object_set_new(o, "enterDebounce", json_real(cfg->enter_debounce_s));
    json_object_set_new(o, "exitDebounce", json_real(cfg->exit_debounce_s));
    json_object_set_new(o, "updateInterval", json_real(cfg->update_interval_s));
    json_object_set_new(o, "dwellThreshold", json_real(cfg->threshold_s));
    json_object_set_new(o, "occlusionMaxGap", json_real(cfg->occlusion_max_gap_s));
    json_object_set_new(o, "stationaryHold", json_real(cfg->stationary_hold_s));

    char* text = json_dumps(o, JSON_COMPACT | JSON_REAL_PRECISION(4));
    json_decref(o);

    char* out = g_strdup(text ? text : "{}");
    free(text);
    return out;
}

/** Validate and stage one numeric field. Returns an error string or NULL. */
static char* stage_double(json_t* root,
                          const char* json_key,
                          const char* param,
                          double lo,
                          double hi,
                          GHashTable* staged) {
    json_t* v = json_object_get(root, json_key);
    if (!v) {
        return NULL; /* absent means unchanged */
    }
    if (!json_is_number(v)) {
        return g_strdup_printf("%s must be a number", json_key);
    }

    const double d = json_number_value(v);
    if (d < lo || d > hi) {
        return g_strdup_printf("%s must be between %g and %g", json_key, lo, hi);
    }

    g_hash_table_replace(staged, g_strdup(param), g_strdup_printf("%.4f", d));
    return NULL;
}

char* config_apply_json(const char* json) {
    if (!handle) {
        return g_strdup("parameter backend unavailable");
    }

    json_error_t jerr;
    json_t*      root = json_loads(json ? json : "", 0, &jerr);
    if (!root) {
        return g_strdup_printf("invalid JSON: %s", jerr.text);
    }
    if (!json_is_object(root)) {
        json_decref(root);
        return g_strdup("expected a JSON object");
    }

    /* Validate everything before writing anything, so a rejected field cannot
     * leave the configuration half-applied. */
    GHashTable* staged = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    char*       error  = NULL;

    json_t* types = json_object_get(root, "objectTypes");
    if (types) {
        if (!json_is_array(types) || json_array_size(types) == 0) {
            error = g_strdup("objectTypes must be a non-empty array");
        } else {
            GString* csv = g_string_new(NULL);
            size_t   idx;
            json_t*  item;
            json_array_foreach(types, idx, item) {
                const char* name = json_string_value(item);
                if (!name) {
                    g_free(error);
                    error = g_strdup("objectTypes must contain strings");
                    break;
                }
                if (tracker_is_attribute_class(name)) {
                    g_free(error);
                    error = g_strdup_printf(
                        "%s is an attribute of another object, not an object that can dwell",
                        name);
                    break;
                }
                if (!config_is_known_class(name)) {
                    g_free(error);
                    error = g_strdup_printf("unknown object type: %s", name);
                    break;
                }
                g_string_append_printf(csv, "%s%s", csv->len ? "," : "", name);
            }
            if (!error) {
                g_hash_table_replace(staged, g_strdup("ObjectTypes"), g_strdup(csv->str));
            }
            g_string_free(csv, TRUE);
        }
    }

    json_t* ref = json_object_get(root, "referencePoint");
    if (!error && ref) {
        const char* s = json_string_value(ref);
        if (!s || (strcmp(s, "bottomCenter") != 0 && strcmp(s, "centroid") != 0)) {
            error = g_strdup("referencePoint must be bottomCenter or centroid");
        } else {
            g_hash_table_replace(staged, g_strdup("ReferencePoint"), g_strdup(s));
        }
    }

    json_t* fb = json_object_get(root, "fallbackToVehicle");
    if (!error && fb) {
        if (!json_is_boolean(fb)) {
            error = g_strdup("fallbackToVehicle must be true or false");
        } else {
            g_hash_table_replace(staged,
                                 g_strdup("FallbackToVehicle"),
                                 g_strdup(json_is_true(fb) ? "yes" : "no"));
        }
    }

    static const struct {
        const char* key;
        const char* param;
        double      lo, hi;
    } numbers[] = {
        {"minScore", "MinScore", 0.0, 1.0},
        {"enterDebounce", "EnterDebounce", 0.0, 60.0},
        {"exitDebounce", "ExitDebounce", 0.0, 300.0},
        {"updateInterval", "UpdateInterval", 0.0, 3600.0},
        {"dwellThreshold", "DwellThreshold", 0.0, 86400.0},
        {"occlusionMaxGap", "OcclusionMaxGap", 0.0, 3600.0},
        {"stationaryHold", "StationaryHold", 0.0, 86400.0},
    };
    for (size_t i = 0; !error && i < G_N_ELEMENTS(numbers); i++) {
        error = stage_double(root, numbers[i].key, numbers[i].param, numbers[i].lo,
                             numbers[i].hi, staged);
    }

    json_decref(root);

    if (error) {
        g_hash_table_destroy(staged);
        return error;
    }

    GHashTableIter iter;
    gpointer       key, value;
    g_hash_table_iter_init(&iter, staged);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        param_set((const char*)key, (const char*)value);
    }
    g_hash_table_destroy(staged);

    return NULL;
}
