/**
 * Video overlay: zone outlines and per-object elapsed time.
 *
 * Copyright (c) 2026 MRDN Labs. MIT licensed.
 */

#include "overlay.h"

#include <axoverlay2.h>
#include <cairo/cairo.h>
#include <glib.h>
#include <math.h>
#include <string.h>
#include <syslog.h>
#include <vdo-error.h>
#include <vdo-stream.h>

/* Two redraws a second. Dwell times change by the second, so anything faster
 * spends CPU to no visible effect. */
#define REDRAW_MS 500

/* Beyond this the overlay is drawn at half size and upscaled by the hardware,
 * which keeps memory and draw time sane on high-resolution streams. */
#define UPSCALE_PIXEL_THRESHOLD 4000000

typedef struct {
    int              overlay_id;
    unsigned         stream_id;
    unsigned         full_width, full_height;
    cairo_surface_t* surface;
} stream_overlay_t;

typedef struct {
    zone_set_t    zones;
    dwell_label_t labels[OVERLAY_MAX_LABELS];
    int           n_labels;
} model_t;

static GMutex     model_lock;
static model_t    model;
static GHashTable* overlays;        /* stream_id -> stream_overlay_t* */
static VdoStream*  event_stream = NULL;
static GIOChannel* vdo_channel  = NULL;
static guint       vdo_watch    = 0;
static guint       redraw_timer = 0;
static bool        running      = false;

static void overlay_free_record(void* p);
static gboolean on_stream_event(GIOChannel* channel, GIOCondition condition, void* user);
static gboolean on_redraw(gpointer user);
static void create_for_stream(unsigned stream_id, unsigned w, unsigned h);
static void remove_for_stream(unsigned stream_id);
static void draw_overlay(stream_overlay_t* ov);
static void render(stream_overlay_t* ov, char* target, size_t target_bytes);

/* ------------------------------------------------------------------- model */

void overlay_set_model(const zone_set_t* zones, const dwell_label_t* labels, int n_labels) {
    g_mutex_lock(&model_lock);
    model.zones    = *zones;
    model.n_labels = CLAMP(n_labels, 0, OVERLAY_MAX_LABELS);
    if (model.n_labels > 0) {
        memcpy(model.labels, labels, sizeof(dwell_label_t) * (size_t)model.n_labels);
    }
    g_mutex_unlock(&model_lock);
}

/* ------------------------------------------------------------------ render */

static void render(stream_overlay_t* ov, char* target, size_t target_bytes) {
    const size_t needed = (size_t)ov->full_width * ov->full_height * 4u;
    if (target_bytes < needed) {
        /* Never write past a buffer the overlay system owns. Sizes are derived
         * from axo_get_aligned_size and should always agree; if they ever do
         * not, skip the frame rather than corrupt memory. */
        syslog(LOG_ERR, "OVERLAY_BUFFER_TOO_SMALL have=%zu need=%zu", target_bytes, needed);
        return;
    }

    cairo_t* cr = cairo_create(ov->surface);

    /* Fully transparent everywhere we do not draw. */
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    const double w = ov->full_width;
    const double h = ov->full_height;

    model_t snapshot;
    g_mutex_lock(&model_lock);
    snapshot = model;
    g_mutex_unlock(&model_lock);

    /* --- zones -------------------------------------------------------- */
    for (int z = 0; z < snapshot.zones.n_zones; z++) {
        const zone_t* zone = &snapshot.zones.zones[z];
        if (zone->n_verts < 3) {
            continue;
        }

        cairo_new_path(cr);
        for (int v = 0; v < zone->n_verts; v++) {
            const double x = zone->verts[v].x * w;
            const double y = zone->verts[v].y * h;
            if (v == 0) {
                cairo_move_to(cr, x, y);
            } else {
                cairo_line_to(cr, x, y);
            }
        }
        cairo_close_path(cr);

        cairo_set_source_rgba(cr, 0.04, 0.55, 0.90, zone->enabled ? 0.16 : 0.06);
        cairo_fill_preserve(cr);

        cairo_set_line_width(cr, 3.0);
        cairo_set_source_rgba(cr, 0.20, 0.70, 1.00, 0.95);
        if (!zone->enabled) {
            const double dash[] = {10.0, 8.0};
            cairo_set_dash(cr, dash, 2, 0);
        }
        cairo_stroke(cr);
        cairo_set_dash(cr, NULL, 0, 0);

        /* Zone name at the topmost vertex. */
        double lx = zone->verts[0].x * w, ly = zone->verts[0].y * h;
        for (int v = 1; v < zone->n_verts; v++) {
            if (zone->verts[v].y * h < ly) {
                lx = zone->verts[v].x * w;
                ly = zone->verts[v].y * h;
            }
        }
        cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, 20.0);
        cairo_move_to(cr, lx + 8.0, MAX(24.0, ly - 8.0));
        cairo_set_source_rgba(cr, 0, 0, 0, 0.65);
        cairo_show_text(cr, zone->name);
        cairo_move_to(cr, lx + 7.0, MAX(23.0, ly - 9.0));
        cairo_set_source_rgba(cr, 0.20, 0.70, 1.00, 1.0);
        cairo_show_text(cr, zone->name);
    }

    /* --- object labels ------------------------------------------------- */
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 24.0);

    for (int i = 0; i < snapshot.n_labels; i++) {
        const dwell_label_t* lab = &snapshot.labels[i];

        cairo_text_extents_t ext;
        cairo_text_extents(cr, lab->text, &ext);

        const double pad = 7.0;
        double       bx  = lab->x * w - (ext.width / 2.0) - pad;
        double       by  = lab->y * h - ext.height - 3.0 * pad;

        /* Keep the badge on screen even for an object at the frame edge. */
        bx = CLAMP(bx, 2.0, w - ext.width - 2.0 * pad - 2.0);
        by = CLAMP(by, 2.0, h - ext.height - 2.0 * pad - 2.0);

        const double bw = ext.width + 2.0 * pad;
        const double bh = ext.height + 2.0 * pad;

        if (lab->over) {
            cairo_set_source_rgba(cr, 0.75, 0.10, 0.08, 0.85);
        } else {
            cairo_set_source_rgba(cr, 0.05, 0.08, 0.11, 0.78);
        }
        cairo_rectangle(cr, bx, by, bw, bh);
        cairo_fill(cr);

        cairo_set_source_rgba(cr, 1, 1, 1, 0.97);
        cairo_move_to(cr, bx + pad - ext.x_bearing, by + pad - ext.y_bearing);
        cairo_show_text(cr, lab->text);

        /* A small tick down to the object's actual reference point. */
        cairo_set_line_width(cr, 2.0);
        cairo_move_to(cr, bx + bw / 2.0, by + bh);
        cairo_line_to(cr, lab->x * w, lab->y * h);
        cairo_stroke(cr);
    }

    cairo_destroy(cr);
    cairo_surface_flush(ov->surface);

    memcpy(target,
           cairo_image_surface_get_data(ov->surface),
           (size_t)ov->full_width * ov->full_height * 4u);
}

static void draw_overlay(stream_overlay_t* ov) {
    axo_err*    err    = NULL;
    axo_buffer* buffer = axo_get_buffer(ov->overlay_id, NULL, &err);

    if (!buffer) {
        const axo_err_code code = axo_err_get_code(err);
        /* Both are normal during ordinary camera use, not failures. */
        if (code != AXO_ERR_NO_STREAM && code != AXO_ERR_WAIT) {
            syslog(LOG_ERR, "OVERLAY_BUFFER_FAIL id=%d msg=%s", ov->overlay_id,
                   axo_err_get_message(err));
        }
        axo_err_clear(&err);
        return;
    }

    char* target = axo_buffer_get_data(buffer, &err);
    if (!target) {
        syslog(LOG_ERR, "OVERLAY_DATA_FAIL msg=%s", axo_err_get_message(err));
        axo_err_clear(&err);
        return;
    }

    render(ov, target, axo_buffer_get_byte_size(buffer));

    if (!axo_submit_buffer(buffer, NULL, &err)) {
        syslog(LOG_ERR, "OVERLAY_SUBMIT_FAIL id=%d msg=%s", ov->overlay_id,
               axo_err_get_message(err));
    }
    axo_err_clear(&err);
}

static gboolean on_redraw(gpointer user) {
    (void)user;
    GHashTableIter iter;
    gpointer       key, value;

    g_hash_table_iter_init(&iter, overlays);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        draw_overlay(value);
    }
    return G_SOURCE_CONTINUE;
}

/* ------------------------------------------------------------ stream wiring */

static void create_for_stream(unsigned stream_id, unsigned w, unsigned h) {
    axo_err*   err   = NULL;
    axo_props* props = NULL;
    axo_match* match = NULL;

    /* Full-stream overlay: zone polygons are normalized to the whole frame, so
     * anything smaller would put them in the wrong place. */
    const bool upscale = ((unsigned long)w * h) > UPSCALE_PIXEL_THRESHOLD;
    unsigned   draw_w  = upscale ? w / 2 : w;
    unsigned   draw_h  = upscale ? h / 2 : h;

    unsigned full_w = 0, full_h = 0;
    if (!axo_get_aligned_size(AXO_FORMAT_ARGB32, draw_w, draw_h, &full_w, &full_h, &err)) {
        syslog(LOG_ERR, "OVERLAY_ALIGN_FAIL msg=%s", axo_err_get_message(err));
        goto out;
    }

    props = axo_props_new();
    axo_props_set_format(props, AXO_FORMAT_ARGB32);
    axo_props_set_size(props, full_w, full_h);
    axo_props_set_upscale_x2(props, upscale);

    match = axo_match_new();
    axo_match_stream_id(match, (int)stream_id);

    const int id = axo_create_overlay(props, match, &err);
    if (id < 0) {
        /* The stream can close before we get here; that is not an error. */
        if (axo_err_get_code(err) != AXO_ERR_NO_STREAM) {
            syslog(LOG_ERR, "OVERLAY_CREATE_FAIL stream=%u msg=%s", stream_id,
                   axo_err_get_message(err));
        }
        goto out;
    }

    cairo_surface_t* surface =
        cairo_image_surface_create(CAIRO_FORMAT_ARGB32, (int)full_w, (int)full_h);
    if (cairo_image_surface_get_stride(surface) != (int)(full_w * 4u)) {
        /* The straight memcpy in render() assumes no extra Cairo padding. */
        syslog(LOG_ERR, "OVERLAY_STRIDE_MISMATCH stream=%u — overlay disabled", stream_id);
        cairo_surface_destroy(surface);
        axo_remove_overlay(id, NULL);
        goto out;
    }

    stream_overlay_t* ov = g_new0(stream_overlay_t, 1);
    ov->overlay_id       = id;
    ov->stream_id        = stream_id;
    ov->full_width       = full_w;
    ov->full_height      = full_h;
    ov->surface          = surface;

    g_hash_table_insert(overlays, GINT_TO_POINTER(stream_id), ov);
    syslog(LOG_INFO, "OVERLAY_CREATED stream=%u size=%ux%u upscale=%s", stream_id, full_w, full_h,
           upscale ? "yes" : "no");

    draw_overlay(ov);

out:
    axo_err_clear(&err);
    if (props) {
        axo_props_free(props);
    }
    if (match) {
        axo_match_free(match);
    }
}

static void remove_for_stream(unsigned stream_id) {
    const stream_overlay_t* ov = g_hash_table_lookup(overlays, GINT_TO_POINTER(stream_id));
    if (ov) {
        axo_remove_overlay(ov->overlay_id, NULL);
        syslog(LOG_INFO, "OVERLAY_REMOVED stream=%u", stream_id);
    }
    g_hash_table_remove(overlays, GINT_TO_POINTER(stream_id));
}

static void overlay_free_record(void* p) {
    stream_overlay_t* ov = p;
    cairo_surface_destroy(ov->surface);
    g_free(ov);
}

static gboolean on_stream_event(GIOChannel* channel, GIOCondition condition, void* user) {
    (void)channel;
    (void)user;

    if (condition & (G_IO_ERR | G_IO_HUP)) {
        syslog(LOG_ERR, "OVERLAY_VDO_BROKEN condition=0x%04x", condition);
        return G_SOURCE_REMOVE;
    }

    GError*    error  = NULL;
    VdoMap*    ev     = vdo_stream_get_event(event_stream, &error);
    VdoStream* stream = NULL;
    VdoMap*    info   = NULL;

    if (!ev) {
        if (!g_error_matches(error, VDO_ERROR, VDO_ERROR_NO_EVENT)) {
            syslog(LOG_ERR, "OVERLAY_VDO_EVENT_FAIL msg=%s", error->message);
        }
        g_clear_error(&error);
        return G_SOURCE_CONTINUE;
    }

    const unsigned type      = vdo_map_get_uint32(ev, "event", 0);
    const unsigned stream_id = vdo_map_get_uint32(ev, "id", 0);

    if (type == VDO_STREAM_EVENT_EXISTING || type == VDO_STREAM_EVENT_CREATED) {
        stream = vdo_stream_get(stream_id, &error);
        info   = stream ? vdo_stream_get_info(stream, NULL) : NULL;

        const unsigned w = info ? vdo_map_get_uint32(info, "width", 0) : 0;
        const unsigned h = info ? vdo_map_get_uint32(info, "height", 0) : 0;
        if (w && h) {
            create_for_stream(stream_id, w, h);
        }
    } else if (type == VDO_STREAM_EVENT_CLOSED) {
        remove_for_stream(stream_id);
    }

    g_clear_error(&error);
    g_clear_object(&ev);
    g_clear_object(&stream);
    g_clear_object(&info);
    return G_SOURCE_CONTINUE;
}

/* --------------------------------------------------------------- lifecycle */

bool overlay_is_running(void) {
    return running;
}

/**
 * Give fontconfig somewhere writable to cache.
 *
 * Cairo's text API pulls in fontconfig, which has no writable cache directory
 * under an ACAP's restricted home. It then logs "No writable cache
 * directories" on *every* text draw — twice a second here, which floods the
 * device syslog. Pointing XDG_CACHE_HOME at our own localdata silences it and
 * lets the font cache actually work.
 */
static void ensure_font_cache(void) {
    static bool done = false;
    if (done) {
        return;
    }
    done = true;

    char* cwd   = g_get_current_dir();
    char* cache = g_build_filename(cwd, "localdata", "cache", NULL);

    if (g_mkdir_with_parents(cache, 0755) == 0) {
        g_setenv("XDG_CACHE_HOME", cache, TRUE);
    } else {
        syslog(LOG_WARNING, "OVERLAY_FONTCACHE_UNAVAILABLE dir=%s", cache);
    }

    g_free(cache);
    g_free(cwd);
}

bool overlay_start(void) {
    if (running) {
        return true;
    }

    ensure_font_cache();

    axo_err* err = NULL;
    if (!axo_start(NULL, &err)) {
        syslog(LOG_ERR, "OVERLAY_START_FAIL msg=%s", axo_err_get_message(err));
        axo_err_clear(&err);
        return false;
    }

    overlays = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, overlay_free_record);

    GError* error = NULL;
    /* Stream 0 is a pseudo-stream that reports events about all other streams. */
    event_stream = vdo_stream_get(0, &error);
    if (!event_stream) {
        syslog(LOG_ERR, "OVERLAY_VDO_FAIL msg=%s", error ? error->message : "unknown");
        g_clear_error(&error);
        overlay_stop();
        return false;
    }

    /* Ignore streams that do not want overlays — drawing for them is waste. */
    VdoMap* filter = vdo_map_new();
    vdo_map_set_string(filter, "filter", "overlay");
    if (!vdo_stream_attach(event_stream, filter, &error)) {
        syslog(LOG_ERR, "OVERLAY_VDO_ATTACH_FAIL msg=%s", error ? error->message : "unknown");
        g_clear_error(&error);
        g_clear_object(&filter);
        overlay_stop();
        return false;
    }
    g_clear_object(&filter);

    const int fd = vdo_stream_get_event_fd(event_stream, &error);
    if (fd < 0) {
        syslog(LOG_ERR, "OVERLAY_VDO_FD_FAIL msg=%s", error ? error->message : "unknown");
        g_clear_error(&error);
        overlay_stop();
        return false;
    }

    vdo_channel = g_io_channel_unix_new(fd);
    vdo_watch   = g_io_add_watch(vdo_channel, G_IO_IN | G_IO_PRI | G_IO_ERR | G_IO_HUP,
                                 on_stream_event, NULL);

    redraw_timer = g_timeout_add(REDRAW_MS, on_redraw, NULL);
    running      = true;

    syslog(LOG_INFO, "OVERLAY_STARTED redraw_ms=%d", REDRAW_MS);
    return true;
}

void overlay_stop(void) {
    if (redraw_timer) {
        g_source_remove(redraw_timer);
        redraw_timer = 0;
    }
    if (vdo_watch) {
        g_source_remove(vdo_watch);
        vdo_watch = 0;
    }
    if (vdo_channel) {
        g_io_channel_unref(vdo_channel);
        vdo_channel = NULL;
    }
    if (overlays) {
        /* Release each overlay before tearing the system down. */
        GHashTableIter iter;
        gpointer       key, value;
        g_hash_table_iter_init(&iter, overlays);
        while (g_hash_table_iter_next(&iter, &key, &value)) {
            axo_remove_overlay(((stream_overlay_t*)value)->overlay_id, NULL);
        }
        g_hash_table_destroy(overlays);
        overlays = NULL;
    }
    g_clear_object(&event_stream);

    if (running) {
        axo_stop(NULL);
        syslog(LOG_INFO, "OVERLAY_STOPPED");
    }
    running = false;
}
