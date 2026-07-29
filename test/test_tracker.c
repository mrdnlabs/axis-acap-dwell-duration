/**
 * Host-side tests for the dwell state machine.
 *
 * Copyright (c) 2026 MRDN Labs. MIT licensed.
 *
 * tracker.c and zone.c depend only on glib, jansson and libm, so they compile
 * and run natively — the timing rules can be driven with synthetic frames
 * instead of waiting for a truck to park in front of a camera.
 *
 * Run with test/run.sh.
 */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../app/tracker.h"
#include "../app/zone.h"

#define T0 ((int64_t)1700000000 * 1000000)

static int failures = 0;
static int checks   = 0;

/* ---------------------------------------------------------- captured events */

typedef struct {
    char   kind[24];
    char   object_id[TRACK_ID_LEN];
    char   object_type[CLASS_LEN];
    int    zone_id;
    double elapsed_s;
    bool   threshold_exceeded;
    double overage_s;
} captured_t;

static captured_t captured[128];
static int        n_captured = 0;

static void capture(const dwell_event_t* ev, void* user) {
    (void)user;
    if (n_captured >= (int)(sizeof(captured) / sizeof(captured[0]))) {
        return;
    }
    captured_t* c = &captured[n_captured++];
    snprintf(c->kind, sizeof(c->kind), "%s", ev->kind);
    snprintf(c->object_id, sizeof(c->object_id), "%s", ev->object_id);
    snprintf(c->object_type, sizeof(c->object_type), "%s", ev->object_type);
    c->zone_id            = ev->zone_id;
    c->elapsed_s          = ev->elapsed_s;
    c->threshold_exceeded = ev->threshold_exceeded;
    c->overage_s          = ev->overage_s;
}

static void reset_captured(void) {
    n_captured = 0;
}

static int count_kind(const char* kind) {
    int n = 0;
    for (int i = 0; i < n_captured; i++) {
        if (strcmp(captured[i].kind, kind) == 0) {
            n++;
        }
    }
    return n;
}

static const captured_t* last_of(const char* kind) {
    for (int i = n_captured - 1; i >= 0; i--) {
        if (strcmp(captured[i].kind, kind) == 0) {
            return &captured[i];
        }
    }
    return NULL;
}

/* ------------------------------------------------------------- assertions */

static void ok(bool cond, const char* what) {
    checks++;
    if (!cond) {
        failures++;
        printf("  FAIL  %s\n", what);
    } else {
        printf("  ok    %s\n", what);
    }
}

static void ok_near(double got, double want, double tol, const char* what) {
    checks++;
    if (fabs(got - want) > tol) {
        failures++;
        printf("  FAIL  %s (got %.3f, want %.3f +/- %.3f)\n", what, got, want, tol);
    } else {
        printf("  ok    %s (%.3f)\n", what, got);
    }
}

/* ------------------------------------------------------------------ fixtures */

static void base_config(config_t* cfg) {
    config_set_defaults(cfg);
    cfg->enter_debounce_s  = 0.5;
    cfg->exit_debounce_s   = 2.0;
    cfg->update_interval_s = 10.0;
    cfg->threshold_s       = 20.0;
    cfg->min_score         = 0.30;
    /* Deliberately small so gap behaviour is testable in a short sequence. */
    cfg->occlusion_max_gap_s = 5.0;
    cfg->stationary_hold_s   = 30.0;
}

static void one_zone(zone_set_t* zs) {
    zone_set_defaults(zs); /* centred rectangle 0.2..0.8 */
}

/** A detection whose reference point (bottom-centre) is at (x, y). */
static detection_t det_at(const char* id, const char* cls, double score, double x, double y) {
    detection_t d;
    memset(&d, 0, sizeof(d));
    snprintf(d.track_id, sizeof(d.track_id), "%s", id);
    snprintf(d.cls, sizeof(d.cls), "%s", cls);
    d.score  = score;
    d.left   = x - 0.02;
    d.right  = x + 0.02;
    d.top    = y - 0.10;
    d.bottom = y;
    d.ref    = zone_ref_point(&d, REF_BOTTOM_CENTER);
    return d;
}

#define MONO0 ((int64_t)500000 * 1000000)

/** A frame where the wall clock and the monotonic clock agree — the normal case. */
static void frame(tracker_t* t, double at_s, const detection_t* d) {
    tracker_begin_frame(t, T0 + (int64_t)(at_s * 1e6), MONO0 + (int64_t)(at_s * 1e6));
    if (d) {
        tracker_detection(t, d);
    }
    tracker_end_frame(t);
}

/** A frame where the two clocks are driven independently, to model a step. */
static void frame_at(tracker_t* t, double frame_s, double mono_s, const detection_t* d) {
    tracker_begin_frame(t, T0 + (int64_t)(frame_s * 1e6), MONO0 + (int64_t)(mono_s * 1e6));
    if (d) {
        tracker_detection(t, d);
    }
    tracker_end_frame(t);
}

/* ---------------------------------------------------------------- the tests */

static void test_point_in_polygon(void) {
    printf("\npoint-in-polygon\n");
    zone_set_t zs;
    one_zone(&zs);
    const zone_t* z = &zs.zones[0];

    ok(zone_contains(z, (point_t){0.5, 0.5}), "centre is inside");
    ok(!zone_contains(z, (point_t){0.1, 0.5}), "left of zone is outside");
    ok(!zone_contains(z, (point_t){0.9, 0.5}), "right of zone is outside");
    ok(!zone_contains(z, (point_t){0.5, 0.1}), "above zone is outside");
    ok(!zone_contains(z, (point_t){0.5, 0.9}), "below zone is outside");
    ok(!zone_contains(z, (point_t){0.5, 0.5}) == false, "inside is stable");

    zone_t disabled = *z;
    disabled.enabled = false;
    ok(!zone_contains(&disabled, (point_t){0.5, 0.5}), "disabled zone contains nothing");
}

static void test_enter_debounce(void) {
    printf("\nenter debounce (FR-7)\n");
    config_t cfg;
    base_config(&cfg);
    zone_set_t zs;
    one_zone(&zs);
    reset_captured();

    tracker_t*  t = tracker_new(&cfg, &zs, capture, NULL);
    detection_t d = det_at("a", "Human", 0.9, 0.5, 0.5);

    frame(t, 0.0, &d);
    ok(count_kind("entered") == 0, "no Entered on first frame inside");

    frame(t, 0.3, &d);
    ok(count_kind("entered") == 0, "still no Entered before debounce elapses");

    frame(t, 0.6, &d);
    ok(count_kind("entered") == 1, "Entered fires once debounce is satisfied");

    frame(t, 1.0, &d);
    ok(count_kind("entered") == 1, "Entered does not repeat while inside");

    tracker_free(t);
}

static void test_brief_visit_suppressed(void) {
    printf("\nbrief visit is suppressed\n");
    config_t cfg;
    base_config(&cfg);
    zone_set_t zs;
    one_zone(&zs);
    reset_captured();

    tracker_t* t = tracker_new(&cfg, &zs, capture, NULL);
    detection_t in  = det_at("a", "Human", 0.9, 0.5, 0.5);
    detection_t out = det_at("a", "Human", 0.9, 0.05, 0.5);

    frame(t, 0.0, &in);
    frame(t, 0.2, &out); /* left before enter_debounce */
    frame(t, 3.0, &out);

    ok(count_kind("entered") == 0, "flicker through the zone emits nothing");
    ok(count_kind("exited") == 0, "and no phantom Exited");

    tracker_free(t);
}

static void test_exit_total_and_hysteresis(void) {
    printf("\nexit debounce and total dwell (FR-7)\n");
    config_t cfg;
    base_config(&cfg);
    zone_set_t zs;
    one_zone(&zs);
    reset_captured();

    tracker_t*  t   = tracker_new(&cfg, &zs, capture, NULL);
    detection_t in  = det_at("a", "Human", 0.9, 0.5, 0.5);
    detection_t out = det_at("a", "Human", 0.9, 0.05, 0.5);

    frame(t, 0.0, &in);
    frame(t, 0.6, &in); /* Entered, entry backdated to 0.0 */

    /* Step outside briefly, then return before exit_debounce expires. */
    frame(t, 5.0, &out);
    frame(t, 6.0, &in);
    ok(count_kind("exited") == 0, "brief step outside does not exit");

    frame(t, 10.0, &in);
    frame(t, 11.0, &out);
    frame(t, 12.0, &out);
    ok(count_kind("exited") == 0, "still within exit debounce");

    frame(t, 13.5, &out);
    ok(count_kind("exited") == 1, "Exited once exit debounce elapses");

    const captured_t* e = last_of("exited");
    /* Total runs from first entry (0.0) to last time inside (10.0). */
    ok_near(e ? e->elapsed_s : -1, 10.0, 0.2, "total dwell excludes the trailing absence");

    tracker_free(t);
}

static void test_threshold_and_overage(void) {
    printf("\nthreshold and overage (FR-9)\n");
    config_t cfg;
    base_config(&cfg);
    cfg.threshold_s       = 20.0;
    cfg.update_interval_s = 10.0;
    zone_set_t zs;
    one_zone(&zs);
    reset_captured();

    tracker_t*  t = tracker_new(&cfg, &zs, capture, NULL);
    detection_t d = det_at("a", "Truck", 0.9, 0.5, 0.5);

    for (double s = 0.0; s <= 45.0; s += 1.0) {
        frame(t, s, &d);
    }

    ok(count_kind("threshold") == 1, "ThresholdExceeded fires exactly once");

    const captured_t* th = last_of("threshold");
    ok_near(th ? th->elapsed_s : -1, 20.0, 1.5, "threshold fires at the configured elapsed");

    const captured_t* up = last_of("update");
    ok(up != NULL, "periodic DwellUpdate is emitted");
    ok(up && up->threshold_exceeded, "later updates report threshold exceeded");
    ok_near(up ? up->overage_s : -1,
            up ? up->elapsed_s - 20.0 : -1,
            0.01,
            "overage equals elapsed minus threshold");

    tracker_free(t);
}

static void test_absence_without_trackended_keeps_timer(void) {
    printf("\nabsence without TrackEnded keeps the timer (FR-5, Phase 0 finding)\n");
    config_t cfg;
    base_config(&cfg);
    cfg.occlusion_max_gap_s = 5.0; /* deliberately shorter than the absence */
    zone_set_t zs;
    one_zone(&zs);
    reset_captured();

    tracker_t*  t = tracker_new(&cfg, &zs, capture, NULL);
    detection_t d = det_at("a", "Human", 0.9, 0.5, 0.5);

    frame(t, 0.0, &d);
    frame(t, 0.6, &d);
    ok(count_kind("entered") == 1, "entered");

    /* 40 s of frames with no sighting, but no TrackEnded either — the device
     * tracker still owns the id, so this must not end the dwell. */
    for (double s = 1.0; s <= 40.0; s += 1.0) {
        frame(t, s, NULL);
    }

    ok(count_kind("exited") == 0, "no Exited during a long absence with no TrackEnded");

    /* Re-acquired: the timer must have kept running throughout. */
    frame(t, 41.0, &d);
    frame(t, 42.0, &d);
    char* status = tracker_status_json(t);
    ok(strstr(status, "\"elapsedSeconds\"") != NULL, "object still reported in zone");
    g_free(status);

    frame(t, 43.0, &d);
    const captured_t* up = last_of("update");
    ok(up && up->elapsed_s > 35.0, "elapsed kept accruing across the absence");

    tracker_free(t);
}

static void test_trackended_starts_gap_budget(void) {
    printf("\nTrackEnded starts the gap budget (FR-6, EC-3)\n");
    config_t cfg;
    base_config(&cfg);
    cfg.occlusion_max_gap_s = 5.0;
    cfg.stationary_hold_s   = 5.0; /* keep both short for the test */
    zone_set_t zs;
    one_zone(&zs);
    reset_captured();

    tracker_t*  t = tracker_new(&cfg, &zs, capture, NULL);
    detection_t d = det_at("a", "Human", 0.9, 0.5, 0.5);

    frame(t, 0.0, &d);
    frame(t, 0.6, &d);
    frame(t, 4.0, &d);

    tracker_begin_frame(t, T0 + (int64_t)(5.0 * 1e6), MONO0 + (int64_t)(5.0 * 1e6));
    tracker_track_ended(t, "a");
    tracker_end_frame(t);
    ok(count_kind("exited") == 0, "TrackEnded alone does not exit");

    frame(t, 8.0, NULL);
    ok(count_kind("exited") == 0, "still inside the gap budget");

    frame(t, 12.0, NULL);
    ok(count_kind("exited") == 1, "Exited once the gap budget is exceeded");

    const captured_t* e = last_of("exited");
    ok_near(e ? e->elapsed_s : -1, 4.0, 0.5, "total dwell is measured to the last sighting");

    tracker_free(t);
}

static void test_reacquisition_resumes(void) {
    printf("\nre-acquisition inside the budget resumes the same dwell\n");
    config_t cfg;
    base_config(&cfg);
    cfg.occlusion_max_gap_s = 10.0;
    zone_set_t zs;
    one_zone(&zs);
    reset_captured();

    tracker_t*  t = tracker_new(&cfg, &zs, capture, NULL);
    detection_t d = det_at("a", "Human", 0.9, 0.5, 0.5);

    frame(t, 0.0, &d);
    frame(t, 0.6, &d);

    tracker_begin_frame(t, T0 + (int64_t)(3.0 * 1e6), MONO0 + (int64_t)(3.0 * 1e6));
    tracker_track_ended(t, "a");
    tracker_end_frame(t);

    frame(t, 6.0, &d); /* back before the budget expires */
    frame(t, 30.0, &d);

    ok(count_kind("exited") == 0, "no exit after re-acquisition");
    ok(count_kind("entered") == 1, "and no second Entered — it is the same dwell");

    const captured_t* up = last_of("update");
    ok(up && up->elapsed_s > 25.0, "elapsed measured from the original entry");

    tracker_free(t);
}

static void test_head_is_not_an_object(void) {
    printf("\nHead is an attribute, not an object (AC-6)\n");
    config_t cfg;
    base_config(&cfg);
    zone_set_t zs;
    one_zone(&zs);
    reset_captured();

    ok(tracker_is_attribute_class("Head"), "Head classified as attribute");
    ok(tracker_is_attribute_class("LicensePlate"), "LicensePlate classified as attribute");
    ok(!tracker_is_attribute_class("Human"), "Human is a real object");

    tracker_t*  t    = tracker_new(&cfg, &zs, capture, NULL);
    detection_t head = det_at("h", "Head", 0.95, 0.5, 0.5);

    for (double s = 0.0; s <= 5.0; s += 0.5) {
        frame(t, s, &head);
    }
    ok(count_kind("entered") == 0, "a Head track never enters a zone");
    ok(tracker_in_zone_count(t) == 0, "and is not counted as in-zone");

    tracker_free(t);
}

static void test_late_classification_backdates(void) {
    printf("\nlate classification backdates the entry (EC-5)\n");
    config_t cfg;
    base_config(&cfg);
    zone_set_t zs;
    one_zone(&zs);
    reset_captured();

    tracker_t* t = tracker_new(&cfg, &zs, capture, NULL);

    /* Seen in the zone for 10 s with no class at all. */
    detection_t unknown = det_at("a", "", 0.0, 0.5, 0.5);
    for (double s = 0.0; s <= 10.0; s += 1.0) {
        frame(t, s, &unknown);
    }
    ok(count_kind("entered") == 0, "unclassified object does not enter");

    /* Then it classifies as a Truck. */
    detection_t truck = det_at("a", "Truck", 0.8, 0.5, 0.5);
    frame(t, 11.0, &truck);

    ok(count_kind("entered") == 1, "Entered fires once the class arrives");

    frame(t, 12.0, &truck);
    char* status = tracker_status_json(t);
    ok(strstr(status, "Truck") != NULL, "reported with its resolved class");
    g_free(status);

    /* Entry was backdated to first-inside, so elapsed already exceeds the
     * time since classification. */
    for (double s = 13.0; s <= 22.0; s += 1.0) {
        frame(t, s, &truck);
    }
    const captured_t* up = last_of("update");
    ok(up && up->elapsed_s > 20.0, "early dwell before classification is not lost");

    tracker_free(t);
}

static void test_two_objects_independent(void) {
    printf("\ntwo objects are timed independently (AC-6)\n");
    config_t cfg;
    base_config(&cfg);
    zone_set_t zs;
    one_zone(&zs);
    reset_captured();

    tracker_t* t = tracker_new(&cfg, &zs, capture, NULL);

    detection_t a = det_at("a", "Human", 0.9, 0.4, 0.5);
    detection_t b = det_at("b", "Truck", 0.9, 0.6, 0.5);

    /* a enters at 0, b enters 10 s later. */
    for (double s = 0.0; s <= 30.0; s += 1.0) {
        tracker_begin_frame(t, T0 + (int64_t)(s * 1e6), MONO0 + (int64_t)(s * 1e6));
        tracker_detection(t, &a);
        if (s >= 10.0) {
            tracker_detection(t, &b);
        }
        tracker_end_frame(t);
    }

    ok(count_kind("entered") == 2, "both objects entered");
    ok(tracker_in_zone_count(t) == 2, "both counted in zone");

    char* status = tracker_status_json(t);
    ok(strstr(status, "\"objectId\":\"a\"") != NULL, "a present in status");
    ok(strstr(status, "\"objectId\":\"b\"") != NULL, "b present in status");
    g_free(status);

    tracker_free(t);
}

static void test_rename_carries_dwell(void) {
    printf("\nRename carries the dwell across ids (FR-6)\n");
    config_t cfg;
    base_config(&cfg);
    zone_set_t zs;
    one_zone(&zs);
    reset_captured();

    tracker_t*  t  = tracker_new(&cfg, &zs, capture, NULL);
    detection_t d1 = det_at("old", "Human", 0.9, 0.5, 0.5);
    detection_t d2 = det_at("new", "Human", 0.9, 0.5, 0.5);

    frame(t, 0.0, &d1);
    frame(t, 0.6, &d1);
    ok(count_kind("entered") == 1, "entered under the original id");

    tracker_begin_frame(t, T0 + (int64_t)(10.0 * 1e6), MONO0 + (int64_t)(10.0 * 1e6));
    tracker_rename(t, "old", "new");
    tracker_detection(t, &d2);
    tracker_end_frame(t);

    for (double s = 11.0; s <= 25.0; s += 1.0) {
        frame(t, s, &d2);
    }

    ok(count_kind("entered") == 1, "no second Entered after the rename");
    ok(count_kind("exited") == 0, "and no Exited");

    const captured_t* up = last_of("update");
    ok(up && up->elapsed_s > 20.0, "elapsed continues from the original entry");
    ok(up && strcmp(up->object_id, "new") == 0, "reported under the new id");

    tracker_free(t);
}

static void test_sparse_frames_are_not_clock_steps(void) {
    printf("\nsparse metadata is not a clock step (regression)\n");
    /* The frame topic is event-paced — roughly 0.5 fps with an empty scene, and
     * arbitrarily sparse when nothing moves. An earlier implementation compared
     * frame timestamps only and rebased the whole tracker on every quiet
     * period, silently corrupting live dwell times. */
    config_t cfg;
    base_config(&cfg);
    cfg.max_clock_step_s = 5.0;
    zone_set_t zs;
    one_zone(&zs);
    reset_captured();

    tracker_t*  t = tracker_new(&cfg, &zs, capture, NULL);
    detection_t d = det_at("a", "Human", 0.9, 0.5, 0.5);

    frame(t, 0.0, &d);
    frame(t, 0.6, &d);
    ok(count_kind("entered") == 1, "entered");

    /* A 40 s gap in which both clocks advance together — no step. */
    frame(t, 40.0, &d);
    frame(t, 41.0, &d);

    const captured_t* up = last_of("update");
    ok(up != NULL, "update emitted after the sparse gap");
    ok_near(up ? up->elapsed_s : -1, 41.0, 1.0, "elapsed reflects real time, not a rebase");

    tracker_free(t);
}

static void test_clock_step_guard(void) {
    printf("\nclock step guard (EC-6)\n");
    config_t cfg;
    base_config(&cfg);
    cfg.max_clock_step_s = 5.0;
    zone_set_t zs;
    one_zone(&zs);
    reset_captured();

    tracker_t*  t = tracker_new(&cfg, &zs, capture, NULL);
    detection_t d = det_at("a", "Human", 0.9, 0.5, 0.5);

    frame(t, 0.0, &d);
    frame(t, 0.6, &d);
    frame(t, 5.0, &d);

    /* NTP yanks the wall clock forward an hour while only a second of real
     * time passes — the two clocks diverge, which is what identifies a step. */
    frame_at(t, 3605.0, 6.0, &d);

    char* status = tracker_status_json(t);
    ok(strstr(status, "\"objectId\":\"a\"") != NULL, "object survives the clock step");
    g_free(status);

    frame_at(t, 3606.0, 7.0, &d);
    frame_at(t, 3616.0, 17.0, &d);

    const captured_t* up = last_of("update");
    ok(up != NULL, "updates continue after the step");
    ok(up && up->elapsed_s < 120.0, "elapsed does not leap by the size of the step");
    ok(up && up->elapsed_s > 0.0, "elapsed stays positive");

    /* And backwards. */
    frame_at(t, 3620.0, 21.0, &d);
    frame_at(t, 100.0, 22.0, &d);

    char* s2 = tracker_status_json(t);
    ok(strstr(s2, "\"elapsedSeconds\":-") == NULL,
       "elapsed never goes negative after a backwards step");
    g_free(s2);

    tracker_free(t);
}

static void test_low_confidence_ignored(void) {
    printf("\nlow-confidence detections are ignored (FR-2)\n");
    config_t cfg;
    base_config(&cfg);
    cfg.min_score = 0.5;
    zone_set_t zs;
    one_zone(&zs);
    reset_captured();

    tracker_t*  t = tracker_new(&cfg, &zs, capture, NULL);
    detection_t weak = det_at("a", "Truck", 0.2, 0.5, 0.5);

    for (double s = 0.0; s <= 5.0; s += 0.5) {
        frame(t, s, &weak);
    }
    ok(count_kind("entered") == 0, "below min_score does not enter");

    tracker_free(t);
}

int main(void) {
    printf("dwell state machine tests\n=========================\n");

    test_point_in_polygon();
    test_enter_debounce();
    test_brief_visit_suppressed();
    test_exit_total_and_hysteresis();
    test_threshold_and_overage();
    test_absence_without_trackended_keeps_timer();
    test_trackended_starts_gap_budget();
    test_reacquisition_resumes();
    test_head_is_not_an_object();
    test_late_classification_backdates();
    test_two_objects_independent();
    test_rename_carries_dwell();
    test_sparse_frames_are_not_clock_steps();
    test_clock_step_guard();
    test_low_confidence_ignored();

    printf("\n=========================\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
