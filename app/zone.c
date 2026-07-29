/**
 * Zone geometry and persistence.
 *
 * Copyright (c) 2026 MRDN Labs. MIT licensed.
 */

#include "zone.h"

#include <jansson.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

void config_set_defaults(config_t* cfg) {
    memset(cfg, 0, sizeof(*cfg));

    /* FR-2 specifies Truck as the shipped default. Until the Phase 2 config UI
     * exists there is no way to change this on-device, so the build ships with
     * every real object class enabled — otherwise the app is untestable on any
     * scene without a truck in it. Narrow this to Truck when the UI lands. */
    static const char* const initial_types[] =
        {"Human", "Vehicle", "Car", "Truck", "Bus", "Bike", "VehicleOther"};
    for (size_t i = 0; i < G_N_ELEMENTS(initial_types); i++) {
        g_strlcpy(cfg->types[i], initial_types[i], CLASS_LEN);
    }
    cfg->n_types = (int)G_N_ELEMENTS(initial_types);

    cfg->min_score           = 0.30;
    cfg->fallback_to_vehicle = true;
    cfg->ref_point           = REF_BOTTOM_CENTER;

    cfg->enter_debounce_s  = 0.5;
    cfg->exit_debounce_s   = 2.0;
    cfg->update_interval_s = 10.0;
    cfg->threshold_s       = 100.0;

    /* Provisional. The measured ceiling for source-side gap bridging was 49.1 s,
     * so the occlusion budget starts above it; the stationary hold is a guess
     * until a genuinely still object has been observed for several minutes. */
    cfg->occlusion_max_gap_s = 60.0;
    cfg->stationary_hold_s   = 300.0;
    cfg->stationary_eps      = 0.02;
    cfg->stationary_window_s = 3.0;

    cfg->max_clock_step_s = 5.0;

    cfg->mqtt_auto_configure = true;
    cfg->overlay_enabled     = false;
}

void zone_set_defaults(zone_set_t* zs) {
    memset(zs, 0, sizeof(*zs));
    zone_t* z = &zs->zones[0];

    z->id      = 1;
    z->enabled = true;
    g_strlcpy(z->name, "Zone 1", sizeof(z->name));

    /* Centred rectangle covering the middle 60% of the frame. Deliberately
     * large so something crosses it before anyone has drawn a real zone. */
    const double v[4][2] = {{0.20, 0.20}, {0.80, 0.20}, {0.80, 0.80}, {0.20, 0.80}};
    for (int i = 0; i < 4; i++) {
        z->verts[i].x = v[i][0];
        z->verts[i].y = v[i][1];
    }
    z->n_verts = 4;
    zs->n_zones = 1;
}

point_t zone_ref_point(const detection_t* d, ref_point_t mode) {
    point_t p;
    p.x = (d->left + d->right) / 2.0;
    p.y = (mode == REF_CENTROID) ? (d->top + d->bottom) / 2.0 : d->bottom;
    return p;
}

bool zone_contains(const zone_t* z, point_t p) {
    if (!z->enabled || z->n_verts < 3) {
        return false;
    }

    /* Standard ray casting: count edge crossings of a ray heading +x from p. */
    bool inside = false;
    for (int i = 0, j = z->n_verts - 1; i < z->n_verts; j = i++) {
        const double xi = z->verts[i].x, yi = z->verts[i].y;
        const double xj = z->verts[j].x, yj = z->verts[j].y;

        const bool straddles = (yi > p.y) != (yj > p.y);
        if (!straddles) {
            continue;
        }
        const double dy = yj - yi;
        if (dy > -1e-12 && dy < 1e-12) {
            continue; /* horizontal edge, cannot be crossed by a horizontal ray */
        }
        const double x_at_p = (xj - xi) * (p.y - yi) / dy + xi;
        if (p.x < x_at_p) {
            inside = !inside;
        }
    }
    return inside;
}

char* zone_to_json(const zone_set_t* zs) {
    json_t* arr = json_array();

    for (int i = 0; i < zs->n_zones; i++) {
        const zone_t* z = &zs->zones[i];
        json_t*       o = json_object();
        json_object_set_new(o, "id", json_integer(z->id));
        json_object_set_new(o, "name", json_string(z->name));
        json_object_set_new(o, "enabled", json_boolean(z->enabled));

        json_t* verts = json_array();
        for (int v = 0; v < z->n_verts; v++) {
            json_t* pt = json_array();
            json_array_append_new(pt, json_real(z->verts[v].x));
            json_array_append_new(pt, json_real(z->verts[v].y));
            json_array_append_new(verts, pt);
        }
        json_object_set_new(o, "vertices", verts);
        json_array_append_new(arr, o);
    }

    char* text = json_dumps(arr, JSON_COMPACT | JSON_REAL_PRECISION(6));
    json_decref(arr);

    char* out = g_strdup(text ? text : "[]");
    free(text);
    return out;
}

char* zone_parse_json(const char* text, zone_set_t* out) {
    json_error_t jerr;
    json_t*      arr = json_loads(text ? text : "", 0, &jerr);
    if (!arr) {
        return g_strdup_printf("invalid JSON: %s", jerr.text);
    }
    if (!json_is_array(arr)) {
        json_decref(arr);
        return g_strdup("expected a JSON array of zones");
    }
    if (json_array_size(arr) == 0) {
        json_decref(arr);
        return g_strdup("at least one zone is required");
    }
    if (json_array_size(arr) > MAX_ZONES) {
        json_decref(arr);
        return g_strdup_printf("at most %d zones are supported", MAX_ZONES);
    }

    zone_set_t parsed;
    memset(&parsed, 0, sizeof(parsed));
    char* error = NULL;

    size_t  idx;
    json_t* o;
    json_array_foreach(arr, idx, o) {
        json_t* verts = json_object_get(o, "vertices");
        if (!json_is_array(verts)) {
            error = g_strdup_printf("zone %zu: vertices must be an array", idx + 1);
            break;
        }
        if (json_array_size(verts) < 3) {
            error = g_strdup_printf("zone %zu: a polygon needs at least 3 points", idx + 1);
            break;
        }
        if (json_array_size(verts) > MAX_ZONE_VERTS) {
            error = g_strdup_printf("zone %zu: at most %d points", idx + 1, MAX_ZONE_VERTS);
            break;
        }

        zone_t* z = &parsed.zones[parsed.n_zones];
        memset(z, 0, sizeof(*z));

        const json_int_t id = json_integer_value(json_object_get(o, "id"));
        z->id               = (id > 0) ? (int)id : (int)(parsed.n_zones + 1);

        for (int prev = 0; prev < parsed.n_zones; prev++) {
            if (parsed.zones[prev].id == z->id) {
                error = g_strdup_printf("duplicate zone id %d", z->id);
                break;
            }
        }
        if (error) {
            break;
        }

        const char* name = json_string_value(json_object_get(o, "name"));
        g_strlcpy(z->name, (name && *name) ? name : "Zone", sizeof(z->name));

        json_t* en = json_object_get(o, "enabled");
        z->enabled = en ? json_is_true(en) : true;

        size_t  vi;
        json_t* pt;
        json_array_foreach(verts, vi, pt) {
            if (!json_is_array(pt) || json_array_size(pt) < 2) {
                error = g_strdup_printf("zone %zu: point %zu must be [x, y]", idx + 1, vi + 1);
                break;
            }
            /* Clamp rather than reject — a vertex a hair outside the frame,
             * from a hand-edited file or a drag past the edge, is an obvious
             * intent rather than an error worth refusing the whole zone for. */
            z->verts[z->n_verts].x = CLAMP(json_number_value(json_array_get(pt, 0)), 0.0, 1.0);
            z->verts[z->n_verts].y = CLAMP(json_number_value(json_array_get(pt, 1)), 0.0, 1.0);
            z->n_verts++;
        }
        if (error) {
            break;
        }

        parsed.n_zones++;
    }

    json_decref(arr);

    if (error) {
        return error;
    }
    if (parsed.n_zones == 0) {
        return g_strdup("no usable zones");
    }

    *out = parsed;
    return NULL;
}

bool zone_load(zone_set_t* zs, const char* path) {
    char*   text = NULL;
    gsize   len  = 0;
    GError* gerr = NULL;

    if (!g_file_get_contents(path, &text, &len, &gerr)) {
        g_clear_error(&gerr);
        return false;
    }

    char* err = zone_parse_json(text, zs);
    g_free(text);

    if (err) {
        syslog(LOG_WARNING, "ZONE_LOAD path=%s err=%s", path, err);
        g_free(err);
        return false;
    }
    return true;
}

bool zone_save(const zone_set_t* zs, const char* path) {
    char* text = zone_to_json(zs);
    char* tmp  = g_strdup_printf("%s.tmp", path);

    bool  ok = false;
    FILE* f  = fopen(tmp, "w");
    if (f) {
        const size_t len = strlen(text);
        if (fwrite(text, 1, len, f) == len && fflush(f) == 0) {
            /* Durability before rename, so a power cut cannot leave an empty file. */
            fsync(fileno(f));
            ok = true;
        }
        fclose(f);
    }

    if (ok) {
        ok = (rename(tmp, path) == 0);
    }
    if (!ok) {
        syslog(LOG_ERR, "ZONE_SAVE path=%s failed", path);
        unlink(tmp);
    }

    g_free(tmp);
    g_free(text);
    return ok;
}
