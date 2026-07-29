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

bool zone_load(zone_set_t* zs, const char* path) {
    json_error_t err;
    json_t*      arr = json_load_file(path, 0, &err);
    if (!arr) {
        return false;
    }
    if (!json_is_array(arr)) {
        syslog(LOG_WARNING, "ZONE_LOAD path=%s err=not-an-array", path);
        json_decref(arr);
        return false;
    }

    zone_set_t parsed;
    memset(&parsed, 0, sizeof(parsed));

    size_t  idx;
    json_t* o;
    json_array_foreach(arr, idx, o) {
        if (parsed.n_zones >= MAX_ZONES) {
            syslog(LOG_WARNING, "ZONE_LOAD truncated at %d zones", MAX_ZONES);
            break;
        }
        json_t* verts = json_object_get(o, "vertices");
        if (!json_is_array(verts) || json_array_size(verts) < 3) {
            syslog(LOG_WARNING, "ZONE_LOAD skipping zone %zu: needs >= 3 vertices", idx);
            continue;
        }

        zone_t* z = &parsed.zones[parsed.n_zones];
        memset(z, 0, sizeof(*z));

        const json_int_t id = json_integer_value(json_object_get(o, "id"));
        z->id               = (int)id;
        if (z->id <= 0) {
            z->id = parsed.n_zones + 1;
        }

        const char* name = json_string_value(json_object_get(o, "name"));
        g_strlcpy(z->name, name ? name : "Zone", sizeof(z->name));

        json_t* en = json_object_get(o, "enabled");
        z->enabled = en ? json_is_true(en) : true;

        size_t  vi;
        json_t* pt;
        json_array_foreach(verts, vi, pt) {
            if (z->n_verts >= MAX_ZONE_VERTS) {
                break;
            }
            if (!json_is_array(pt) || json_array_size(pt) < 2) {
                continue;
            }
            /* Clamp rather than reject: a slightly out-of-range vertex from a
             * hand-edited file should not silently disable the whole zone. */
            double x = json_number_value(json_array_get(pt, 0));
            double y = json_number_value(json_array_get(pt, 1));
            x        = CLAMP(x, 0.0, 1.0);
            y        = CLAMP(y, 0.0, 1.0);

            z->verts[z->n_verts].x = x;
            z->verts[z->n_verts].y = y;
            z->n_verts++;
        }

        if (z->n_verts >= 3) {
            parsed.n_zones++;
        }
    }

    json_decref(arr);

    if (parsed.n_zones == 0) {
        syslog(LOG_WARNING, "ZONE_LOAD path=%s err=no-usable-zones", path);
        return false;
    }

    *zs = parsed;
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
