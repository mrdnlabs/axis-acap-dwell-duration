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

static const char* const PARAM_NAMES[] = {"ObjectTypes",
                                          "ObjectTypeNames",
                                          "ClassMinScores",
                                          "MinScore",
                                          "FallbackToVehicle",
                                          "ReferencePoint",
                                          "EnterDebounce",
                                          "ExitDebounce",
                                          "UpdateInterval",
                                          "DwellThreshold",
                                          "OcclusionMaxGap",
                                          "StationaryHold",
                                          "MqttAutoConfigure",
                                          "OverlayEnabled"};

static AXParameter*      handle     = NULL;
static config_changed_fn change_cb  = NULL;
static void*             change_arg = NULL;

bool config_is_known_class(const char* cls) {
    /* The canonical list lives with the defaults, so there is only one place
     * that decides which classes this application will ever time. */
    config_t defaults;
    config_set_defaults(&defaults);
    return config_find_class(&defaults, cls) != NULL;
}

/* ------------------------------------------------------------------ reading */

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

/**
 * Rebuild the class table.
 *
 * The table always contains every class this application knows about, so the
 * UI can show them all. Three parameters layer on top of the defaults:
 * which classes are enabled, an operator-facing name per class, and an
 * optional per-class confidence floor.
 */
static void load_classes(config_t* cfg) {
    config_t defaults;
    config_set_defaults(&defaults);
    memcpy(cfg->classes, defaults.classes, sizeof(cfg->classes));
    cfg->n_classes = defaults.n_classes;

    /* Enabled set: "Truck,Bus". An unreadable or empty value leaves the
     * defaults alone rather than silently timing nothing. */
    char* enabled = param_get("ObjectTypes");
    if (enabled && *enabled) {
        for (int i = 0; i < cfg->n_classes; i++) {
            cfg->classes[i].enabled = false;
        }
        char** parts = g_strsplit(enabled, ",", MAX_CLASSES + 4);
        int    hits  = 0;
        for (int i = 0; parts[i]; i++) {
            char* name = g_strstrip(parts[i]);
            for (int c = 0; c < cfg->n_classes; c++) {
                if (strcmp(cfg->classes[c].cls, name) == 0) {
                    cfg->classes[c].enabled = true;
                    hits++;
                }
            }
        }
        g_strfreev(parts);

        if (hits == 0) {
            syslog(LOG_WARNING,
                   "PARAM_CLASSES none of the stored classes are recognised; keeping defaults");
            for (int i = 0; i < cfg->n_classes; i++) {
                cfg->classes[i].enabled = defaults.classes[i].enabled;
            }
        }
    }
    g_free(enabled);

    /* Friendly names: "Truck=Delivery lorry;Human=Staff on foot". */
    char* names = param_get("ObjectTypeNames");
    if (names && *names) {
        char** pairs = g_strsplit(names, ";", MAX_CLASSES + 4);
        for (int i = 0; pairs[i]; i++) {
            char* eq = strchr(pairs[i], '=');
            if (!eq) {
                continue;
            }
            *eq = '\0';
            char* cls   = g_strstrip(pairs[i]);
            char* label = g_strstrip(eq + 1);
            if (!*label) {
                continue;
            }
            for (int c = 0; c < cfg->n_classes; c++) {
                if (strcmp(cfg->classes[c].cls, cls) == 0) {
                    g_strlcpy(cfg->classes[c].name, label, NAME_LEN);
                }
            }
        }
        g_strfreev(pairs);
    }
    g_free(names);

    /* Per-class confidence floors: "Truck=0.45;Car=0.6". */
    char* scores = param_get("ClassMinScores");
    if (scores && *scores) {
        char** pairs = g_strsplit(scores, ";", MAX_CLASSES + 4);
        for (int i = 0; pairs[i]; i++) {
            char* eq = strchr(pairs[i], '=');
            if (!eq) {
                continue;
            }
            *eq = '\0';
            char*        cls = g_strstrip(pairs[i]);
            char*        end = NULL;
            const double v   = g_ascii_strtod(g_strstrip(eq + 1), &end);
            if (!end || end == eq + 1) {
                continue;
            }
            for (int c = 0; c < cfg->n_classes; c++) {
                if (strcmp(cfg->classes[c].cls, cls) == 0) {
                    cfg->classes[c].min_score = CLAMP(v, 0.0, 1.0);
                }
            }
        }
        g_strfreev(pairs);
    }
    g_free(scores);
}

bool config_reload(config_t* cfg) {
    if (!handle) {
        return false;
    }

    load_classes(cfg);

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

    cfg->mqtt_auto_configure = param_bool("MqttAutoConfigure", cfg->mqtt_auto_configure);
    cfg->overlay_enabled     = param_bool("OverlayEnabled", cfg->overlay_enabled);

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
    syslog(LOG_INFO, "PARAM_INIT classes=%d threshold_s=%.1f", cfg->n_classes, cfg->threshold_s);
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

    json_t* classes = json_array();
    for (int i = 0; i < cfg->n_classes; i++) {
        const class_cfg_t* c    = &cfg->classes[i];
        json_t*            item = json_object();
        json_object_set_new(item, "class", json_string(c->cls));
        json_object_set_new(item, "name", json_string(c->name));
        json_object_set_new(item, "enabled", json_boolean(c->enabled));
        /* null means "inherit the global floor" — distinct from an explicit 0. */
        if (c->min_score >= 0.0) {
            json_object_set_new(item, "minScore", json_real(c->min_score));
        } else {
            json_object_set_new(item, "minScore", json_null());
        }
        json_array_append_new(classes, item);
    }
    json_object_set_new(o, "classes", classes);

    /* Shown as permanently excluded, with the reason, rather than silently
     * missing from the list. */
    json_t* excluded = json_array();
    json_array_append_new(excluded, json_string("Head"));
    json_array_append_new(excluded, json_string("LicensePlate"));
    json_object_set_new(o, "excludedClasses", excluded);
    json_object_set_new(o,
                        "excludedReason",
                        json_string("attribute of a parent object — timing it would count the "
                                    "same object twice"));

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
    json_object_set_new(o, "mqttAutoConfigure", json_boolean(cfg->mqtt_auto_configure));
    json_object_set_new(o, "overlayEnabled", json_boolean(cfg->overlay_enabled));

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

/** Validate the classes array and stage the three parameters it maps onto. */
static char* stage_classes(json_t* root, GHashTable* staged) {
    json_t* classes = json_object_get(root, "classes");
    if (!classes) {
        return NULL;
    }
    if (!json_is_array(classes) || json_array_size(classes) == 0) {
        return g_strdup("classes must be a non-empty array");
    }

    GString* enabled = g_string_new(NULL);
    GString* names   = g_string_new(NULL);
    GString* scores  = g_string_new(NULL);
    char*    error   = NULL;
    int      n_on    = 0;

    size_t  idx;
    json_t* item;
    json_array_foreach(classes, idx, item) {
        const char* cls = json_string_value(json_object_get(item, "class"));
        if (!cls) {
            error = g_strdup("each class entry needs a \"class\"");
            break;
        }
        if (tracker_is_attribute_class(cls)) {
            error = g_strdup_printf(
                "%s is an attribute of another object, not an object that can dwell", cls);
            break;
        }
        if (!config_is_known_class(cls)) {
            error = g_strdup_printf("unknown object class: %s", cls);
            break;
        }

        json_t* en = json_object_get(item, "enabled");
        if (en && json_is_true(en)) {
            g_string_append_printf(enabled, "%s%s", enabled->len ? "," : "", cls);
            n_on++;
        }

        const char* label = json_string_value(json_object_get(item, "name"));
        if (label) {
            /* Semicolons and equals signs are the encoding's delimiters. */
            if (strchr(label, ';') || strchr(label, '=')) {
                error = g_strdup_printf("name for %s cannot contain ';' or '='", cls);
                break;
            }
            if (!*label) {
                error = g_strdup_printf("name for %s cannot be empty", cls);
                break;
            }
            g_string_append_printf(names, "%s%s=%s", names->len ? ";" : "", cls, label);
        }

        json_t* ms = json_object_get(item, "minScore");
        if (ms && !json_is_null(ms)) {
            if (!json_is_number(ms)) {
                error = g_strdup_printf("minScore for %s must be a number or null", cls);
                break;
            }
            const double v = json_number_value(ms);
            if (v < 0.0 || v > 1.0) {
                error = g_strdup_printf("minScore for %s must be between 0 and 1", cls);
                break;
            }
            g_string_append_printf(scores, "%s%s=%.4f", scores->len ? ";" : "", cls, v);
        }
    }

    if (!error && n_on == 0) {
        error = g_strdup("at least one class must be enabled, or nothing will ever be timed");
    }

    if (!error) {
        g_hash_table_replace(staged, g_strdup("ObjectTypes"), g_strdup(enabled->str));
        g_hash_table_replace(staged, g_strdup("ObjectTypeNames"), g_strdup(names->str));
        g_hash_table_replace(staged, g_strdup("ClassMinScores"), g_strdup(scores->str));
    }

    g_string_free(enabled, TRUE);
    g_string_free(names, TRUE);
    g_string_free(scores, TRUE);
    return error;
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
    char*       error  = stage_classes(root, staged);

    json_t* ref = json_object_get(root, "referencePoint");
    if (!error && ref) {
        const char* s = json_string_value(ref);
        if (!s || (strcmp(s, "bottomCenter") != 0 && strcmp(s, "centroid") != 0)) {
            error = g_strdup("referencePoint must be bottomCenter or centroid");
        } else {
            g_hash_table_replace(staged, g_strdup("ReferencePoint"), g_strdup(s));
        }
    }

    static const struct {
        const char* key;
        const char* param;
    } booleans[] = {
        {"fallbackToVehicle", "FallbackToVehicle"},
        {"mqttAutoConfigure", "MqttAutoConfigure"},
        {"overlayEnabled", "OverlayEnabled"},
    };
    for (size_t i = 0; !error && i < G_N_ELEMENTS(booleans); i++) {
        json_t* b = json_object_get(root, booleans[i].key);
        if (!b) {
            continue;
        }
        if (!json_is_boolean(b)) {
            error = g_strdup_printf("%s must be true or false", booleans[i].key);
            break;
        }
        g_hash_table_replace(staged,
                             g_strdup(booleans[i].param),
                             g_strdup(json_is_true(b) ? "yes" : "no"));
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
