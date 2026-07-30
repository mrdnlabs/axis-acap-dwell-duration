# Phase 0 — device findings

Device: AXIS Q3538-SLVE, serial `<SERIAL>`, AXIS OS **12.11.77** (build Jul 08 2026),
aarch64. AOA **1.26.205** installed and Running. Date: 2026-07-29.

---

## Build / SDK

**SDK version must match device firmware.** From ACAP 12.x the SDK version tracks the AXIS OS
version 1:1 (SDK 12.11 ↔ AXIS OS 12.11). Earlier it was offset (SDK 1.13 ↔ OS 11.9,
SDK 1.15 ↔ OS 11.11), realigned at 12.0.

**The Device Data Hub C API is NOT stable between 12.10 and 12.11.** This bit us and would bite
anyone copying the published GitHub example onto a 12.10 device:

| Concern | SDK 12.10 | SDK 12.11 (and the GitHub example) |
|---|---|---|
| Error type | `DHClientError`, `dh_client_error_to_string`, `dh_client_destroy_error` | `DHError`, `dh_error_to_string`, `dh_error_destroy` |
| Callbacks | `dh_subscriber_listener_create()` + `dh_subscriber_set_listener()` | `dh_subscriber_set_data_callback()` / `..._topic_update_callback()` |
| Filtering | none — topics passed straight to `dh_subscriber_subscribe()` | `DHFilter` + `DHSubscribeOptions` |
| Sample accessors | `dh_topic_sample_get_topic_data`, `dh_topic_data_get_json_str` | `dh_topic_sample_get_data`, `dh_topic_data_get_json_data` |
| Data callback arg | `DHTopicSample*` (non-const) | `const DHTopicSample*` |
| Topic list free | `dh_client_destroy_topic_list` | `dh_topic_list_destroy` |
| `dh_client_create` | `(name, options)` — no error out-param | `(name, DHError**)` |
| `dh_client_is_connected` | present | **absent** — use `dh_client_get_connection_state()` |

Always read the headers in the SDK image you are actually building with:

```bash
docker run --rm --entrypoint /bin/bash axisecp/acap-native-sdk:<ver>-aarch64-ubuntu24.04 \
  -c "cat /opt/axis/acapsdk/sysroots/aarch64*/usr/include/datahub/subscriber.h"
```

### Manifest schema

- SDK 12.10 ships schema **v2.0.0 max**; SDK 12.11 ships **v2.0.0, v2.1.0, v2.2.0**.
- The Device Data Hub resource key was renamed: **`deviceDataHub_beta2`** in v2.0.0,
  **`deviceDataHub`** in v2.2.0 (v2.2.0 still accepts the `_beta2` spelling).
- In schema v2.0.0/2.2.0 `setup.required` = `appName, architecture, compatibleOsVersions,
  runMode, vendor, vendorId, version`. `vendorId` must match `^[A-Fa-f0-9]{10}$`.
- `compatibleOsVersions` items require **both** `min` and `max`.
- **`compatibleOsVersions` now gates installation** — "Starting with AXIS OS 12.10, the
  application will be prevented from being installed on devices with an AXIS OS version not
  within any of these ranges." Get it wrong and install fails outright.
- SDK 12.11.0's own minimum AXIS OS is **12.11.72**; setting `min` below that only warns.

### The "Open" button on the Apps page

There is no Open button unless the manifest declares a settings page. Add to
`acapPackageConf`:

```json
"configuration": { "settingPage": "index.html" }
```

Per the schema: *"Must be located in directory 'html' relative to application package root."*
So the file lives at `app/html/index.html` and is served at `/local/<appName>/`. `acap-build`
packages the whole `html/` directory automatically — verify with
`tar tzf <pkg>.eap` before deploying.

Once installed, `list.cgi` reports `ConfigurationPage="local/object_dwell_timer/index.html"`, and
that attribute is what renders the Open button. The key is **`settingPage`**, singular — the
plural spelling silently does nothing.

### Install

- `upload.cgi` accepts either multipart field name — **both `file` and `packfil` work.** An earlier
  note here claimed `packfil` was rejected; that was wrong. The `Error: 2` seen at the time was the
  signature check, and the field name was changed in the same step, so the field got the blame.
  Re-tested on AXIS OS 12.11.77 with `AllowUnsigned=true`: both return `OK`.
- `Error: 2` from `upload.cgi` = signature verification failed. Unsigned apps are refused by
  default from AXIS OS 12.0 onward (was allowed up to 11.11). Enable with:
  `POST /axis-cgi/applications/config.cgi?action=set&name=AllowUnsigned&value=true`
- Building on Windows: `docker run` under Git Bash mangles container paths
  (`/bin/bash` → `C:/Program Files/Git/usr/bin/bash`). Use PowerShell or `MSYS_NO_PATHCONV=1`.

---

## OQ-1 — metadata source: **ANSWERED, frame.v1 is present**

`dh_client_get_topic_list()` on this device returns 16 topics:

```
com.axis.scene.frame.v1                          <-- our source
com.axis.scene.object_track.v1
com.axis.scene.object_snapshot.v1
com.axis.objectanalytics.object_in_area
com.axis.objectanalytics.motion_in_area
com.axis.device.cpu-utilization.v1               <-- useful for AC-7
com.axis.device.memory-utilization.v1            <-- useful for AC-7
com.axis.device.boot-info.v1
com.axis.device.sdcard-status.v1
com.axis.device.stream-count.v1
com.axis.device.flash-status.v1
com.axis.team_internal.scene.frame.v1
mdb.com.axis.analytics_scene_description.v0.beta
mdb.com.axis.consolidated_track.v1.beta
mdb.com.axis.video_object_detection.time.analyzing.v1
mdb.analytics_scene_description
```

Notes:

- The **legacy Message Broker topics are bridged into DDH under an `mdb.` prefix** — so the
  documented fallback is reachable through the same client, just under a different name. An
  exact-string check for `com.axis.analytics_scene_description.v0.beta` reports absent; the real
  name is `mdb.com.axis.analytics_scene_description.v0.beta`.
- `com.axis.device.cpu-utilization.v1` / `memory-utilization.v1` are a much better basis for the
  AC-7 "no measurable load added" check than external sampling.

**Frame rate with an empty scene: ~0.53 fps** (15 frames in 28.1 s). The topic is sparse when
nothing is happening, which is good news for NFR-4. Frames with no detections look like:

```json
{"channel_id":1,"timestamp":"2026-07-29T11:51:43.602249Z"}
```

`detections` and `track_events` are simply **absent** when empty — not present-but-empty. Parsing
must treat them as optional (jansson's `json_array_foreach` over NULL is a no-op, so this is safe).

---

## IF-2 — MQTT topic + payload format: **RESOLVED ON HARDWARE**

This was flagged in the plan as the one thing not pinned down from docs. Measured with a local
Mosquitto broker and a catch-all `//.` event filter.

**Topic format:**

```
axis/<SERIAL>/event/<namespaced topic path>[/$source/<key>/<value>]
```

Real examples:

```
axis/<SERIAL>/event/tns:axis/CameraApplicationPlatform/ObjectAnalytics/Device1Scenario1
axis/<SERIAL>/event/tns:onvif/Device/tns:axis/IO/Port/$source/port/1
```

- Default `deviceTopicPrefix` is **`axis/<SERIAL>`**, and the bridge appends `/event`.
- With `includeTopicNamespaces: true` (the default) namespaces render as **`tns:axis`** —
  note the colon, not `tnsaxis`.
- **Source keys appear in the topic path** as `$source/<key>/<value>`.

**This is better than the plan assumed.** The plan recorded a constraint that `zoneId` would have
to live in the payload rather than the topic. Because source keys are rendered into the path,
declaring `zone` as an AXEvent **source** key gives genuine per-zone topics:

```
axis/<SERIAL>/event/tns:axis/CameraApplicationPlatform/ObjectDwellTimer/Exited/$source/zone/3
```

So a VMS can subscribe to one zone. The IF-2 constraint is largely dissolved — keep declaring
`zone` as a source key, not a data key.

**Payload format:**

```json
{
  "topic": "onvif:Device/axis:IO/Port",
  "timestamp": 1785325150144,
  "message": {
    "source": {"port": "1"},
    "key": {},
    "data": {"state": "0"}
  }
}
```

Two consequences for our event design:

1. **Every value is stringified.** `"state":"0"`, not `0`. So `elapsedSeconds` will arrive as
   `"42.500"` and `thresholdExceeded` as `"1"`/`"0"`. Document this for the Genetec integrator;
   do not promise JSON numbers or booleans.
2. **`timestamp` is epoch milliseconds**, not ISO-8601. IF-4 wants UTC from the NTP-synced clock,
   so keep emitting our own ISO-8601 `utcTime` in `data` — the plan already specifies this and
   this confirms it is necessary rather than redundant.

**Default event publication config on a clean device** (restore target):

```json
{"topicPrefix":"default","customTopicPrefix":"","appendEventTopic":true,
 "includeTopicNamespaces":true,"includeSerialNumberInPayload":false,"eventFilterList":[]}
```

---

## Our own events, verified end-to-end (Phase 1)

`POST api/test` → AXEvent → device MQTT bridge → broker, with nothing in the scene. The Phase 0
prediction about source keys held exactly:

```
axis/<SERIAL>/event/tns:axis/CameraApplicationPlatform/ObjectDwellTimer/Entered/$source/zone/1
```

```json
{"topic":"axis:CameraApplicationPlatform/ObjectDwellTimer/Entered",
 "timestamp":1785332731817,
 "message":{"source":{"zone":"1"},"key":{},
            "data":{"objectType":"Truck","elapsedSeconds":"0.000000",
                    "thresholdExceeded":"0","utcTime":"2026-07-29T13:45:31.817493Z",
                    "objectId":"00000000-0000-0000-0000-000000000000",
                    "test":"1","overageSeconds":"0.000000","state":"in"}}}
```

Confirms, on our own events rather than by analogy with someone else's:

- Declaring `zone` as an AXEvent **source** key puts it in the MQTT topic path, so a VMS really can
  subscribe to one zone.
- Booleans arrive as `"1"` / `"0"` strings and doubles as fixed-point strings
  (`"112.000000"`). Do not promise integrators JSON numbers or booleans.
- The bridge's own `timestamp` is epoch-ms, so the ISO-8601 `utcTime` data field is what carries
  IF-4's NTP-synced UTC.

## Testing an overlay: the JPEG snapshot will not show it

`/axis-cgi/jpg/image.cgi` opens its **own** VDO stream, so an overlay attached to some other
stream is simply not in that image. Grabbing a snapshot is the obvious way to check an overlay and
it produces a convincing-looking negative result.

An overlay also cannot be created until a stream exists to attach to — on an idle camera there may
be none, and `axo_start()` succeeding tells you nothing about whether anything was drawn.

What actually works:

```bash
# hold a stream open, which is what triggers OVERLAY_CREATED
curl -s --anyauth -u root:PASS "http://<ip>/axis-cgi/mjpg/video.cgi?resolution=1280x720&fps=5" -o stream.mjpg &
sleep 10
# then pull a frame out of that same stream — JPEG frames are FFD8...FFD9
```

Watch the log for `OVERLAY_CREATED stream=<id>` before concluding anything. Rendering to a stream
nobody is watching is also why the VDO `"filter": "overlay"` map matters — it keeps the app from
drawing for streams that will never display it.

## AXParameter callbacks deadlock if you read a parameter inside them

Calling `ax_parameter_get()` from inside an `ax_parameter_register_callback` handler **deadlocks
the application and the request that triggered it**. The synchronous get waits for a reply on the
same connection that is currently delivering the callback.

The symptom is nasty because it does not look like an application bug: a `param.cgi?action=update`
request simply never returns, and the ACAP stops answering everything — including its own health
endpoint. Nothing is logged. Recovery needs an application stop/start.

The fix is to do no parameter work inside the callback. Defer it:

```c
static gboolean reload_on_idle(gpointer d) { pending = 0; reload(); return G_SOURCE_REMOVE; }

static void on_param_changed(const gchar* name, const gchar* value, gpointer d) {
    if (pending == 0) pending = g_idle_add(reload_on_idle, NULL);   /* also coalesces */
}
```

Coalescing matters as well: saving a settings form writes several parameters, and each one fires
its own callback.

Related, and worth stating: `ax_parameter_get()` is D-Bus I/O. Do not hold an application mutex
across it. Read into a local copy first, then take the lock only to install the result.

### Parameter group capitalization

Also confirmed on device — the group name is **not** the manifest `appName` verbatim. `appName`
`object_dwell_timer` is stored as `root.Object_dwell_timer.*` (leading capital, underscores kept).
`ax_parameter_get()` uses the short key and is unaffected; external `param.cgi` callers need the
real name:

```bash
curl --anyauth -u root:PASS "http://<ip>/axis-cgi/param.cgi?action=list" | grep -i <appname>
```

## The frame topic is sparse — and that breaks a naive clock guard

Worth stating separately because it caused a real bug. The metadata topic is **event-paced, not
video-paced**: ~0.5 fps with an empty scene and arbitrarily long quiet periods.

A clock-step guard that compares consecutive *frame timestamps* therefore cannot distinguish "the
scene was quiet for 40 s" from "NTP moved the clock 40 s". The first implementation did exactly
that and silently rebased every dwell timer on every quiet period.

The fix is to carry a monotonic reading alongside each frame and compare how much **each** clock
advanced. Only divergence between them is a real step. Anything on this platform that reasons about
elapsed time from metadata timestamps needs the same treatment.

## AOA already has a "TimeInArea" scenario — and it confirms our premise

The catch-all capture shows AOA publishing:

```json
{"topic":"axis:CameraApplicationPlatform/ObjectAnalytics/Device1Scenario1",
 "message":{"data":{"active":"0","triggerTime":"","scenarioType":"TimeInArea",
                    "objectId":"","classTypes":""}}}
```

So AOA does have a **TimeInArea** scenario type that fires a boolean once an object has been in an
area for a configured time. Its payload carries `active`, `triggerTime`, `objectId`, `classTypes`
— and **no elapsed / dwell value**. That is exactly the gap this ACAP fills: a running elapsed
timer, overage past a threshold, periodic dwell updates, and per-object totals on exit.

Worth telling any stakeholder who asks "can't AOA already do this?" — it can tell you *that* an
object passed a time threshold, not *how long* it has been there.

---

## OQ-2 — coordinate frame: **ANSWERED, top-left origin, Y down**

Every observed track logs `sanity_bottom_gt_top=yes` and `sanity_right_gt_left=yes`. Real bbox:

```json
"bounding_box":{"bottom":0.9853,"left":0.4499,"right":0.7473,"top":0.2365}
```

Normalized 0–1, origin **top-left**, X right, **Y increasing downward**. Zone polygons can be
stored in the same frame with no transform. Bottom-centre reference point = `((l+r)/2, bottom)`
is the lowest point of the box, i.e. the footprint. Confirmed.

## Real detection payload (richer than the docs showed)

```json
{"channel_id":1,
 "detections":[{"bounding_box":{"bottom":0.9853,"left":0.4499,"right":0.7473,"top":0.2365},
                "class":{"type":"Human","score":0.7,
                         "upper_clothing_colors":[{"name":"black","score":0.66}, ...],
                         "lower_clothing_colors":[{"name":"black","score":0.36}, ...]},
                "object_track_id":"8e653185-8e54-4dca-8af1-9f7202aca120"}],
 "timestamp":"2026-07-29T11:55:30.369150Z"}
```

Attributes live **inside** `class` alongside `type` and `score`. Parse `class.type` and
`class.score` and ignore the rest.

## OQ-3 — classes seen so far: `Human`, `Head`

Over ~4.5 min of an indoor scene: `Human:834, Head:221`. No vehicles yet (indoor office).

**Important design consequence: `Head` is a separate detection with its own `object_track_id`.**
One person therefore produces **two** tracks — a `Human` and a `Head`. If the type filter naively
matched "any human-ish class" the same person would be counted twice and timed twice, breaking
FR-4/AC-6. **`Head` (and `LicensePlate`) must be excluded from dwell counting by default** — they
are attribute detections of a parent object, not independent objects.

**~10% of detections are unclassified** (`unclassified=116` of 1171), arriving with no `class`
object at all. This is exactly the EC-5 case and confirms the class-grace design is needed rather
than theoretical.

## OQ-4 — track continuity: **the source bridges far longer gaps than assumed**

Measured over 268 s / 1013 frames / 7 ended tracks:

| Track | lifetime | frames | max gap | excursion | class |
|---|---|---|---|---|---|
| 6dcb1674… | 52.7 s | 110 | **41.9 s** | 0.185 | Head 0.91 |
| others (6) | 0.1–0.5 s | 2–5 | 0.1 s | <0.01 | none |

A longer session (51 ended tracks) later showed a **max gap of 49.1 s**, so 41.9 s was not the
ceiling. Treat "the source bridges tens of seconds" as the rule, not the exception.

Two findings:

1. **The device tracker held the same `object_track_id` across a 41.9 s absence.** Every other
   track shows 0.1 s (the normal frame interval). So the source itself performs long-gap
   re-association *without* emitting a `Rename` — it simply resumes the same id.

   **This changes the FR-6 design.** The plan's `occlusion_max_gap_s` default of 5 s would declare
   an exit while the source still considers the object continuous. Better rule, and simpler:

   - Track absent from a frame but **no `TrackEnded`** → the source still owns it → keep the timer
     running, no budget consumed.
   - **`TrackEnded` received** → *now* start the gap budget (occlusion vs stationary hold) for
     possible re-association.
   - Ref point outside the polygon → that is a normal exit, governed by `exit_debounce`, and is a
     separate concern from absence.

   This makes our bridging a genuine second layer over the source rather than a competing one.

2. **`renames=0`** across the whole run. `Rename` exists in the schema but was not exercised in
   this scene, so it must not be the *only* re-association mechanism we rely on — handle it when
   it appears, but do not depend on it.

**Spurious short tracks are real**: six tracks lived 0.1–0.5 s with `class=none` and near-zero
excursion. `enter_debounce` (FR-7) is what suppresses these; without it every flicker would emit
an `Entered` event.

### Revised after a full day of running

A ~7-hour sample on the same indoor scene: **30,508 frames, 26,057 detections, 385 track ends,
0 renames**, averaging 1.25 fps.

Two numbers change earlier conclusions:

- **85% of detections carry no class at all** — 22,280 of 26,057, against the ~10% seen in the
  first few minutes. Only `Human` (3,078) and `Head` (699) were ever classified. So an
  unclassified detection is the *normal* case on this scene, not an edge case. The EC-5 class-grace
  design (record `first_inside` regardless of class, backdate entry when a class finally arrives)
  matters far more than the short sample suggested, and a high `minScore` would discard most of
  what the camera reports.
- **`Rename` was emitted zero times across 385 completed tracks.** The short run showed the same,
  but at this volume it is conclusive: on this device re-association shows up as a reused
  `object_track_id`, never as a `Rename` event. Handle `Rename` when it appears, but nothing may
  depend on it.

**OQ-3 remains unanswered even at this volume** — 26k detections produced no vehicle class of any
kind. An indoor scene simply cannot answer it; it needs vehicles in view.

## Load (NFR-4 / AC-7 early read)

`fps=3.78` with a person moving in frame, `0.53` with an empty scene. The frame topic is
event-paced, not video-paced. No Larod/DLPU use by this app at all.

---

## Still open — needs objects in the scene

The camera views an indoor office. Two things still cannot be measured from this scene:

- **OQ-3, vehicle sub-types.** Only `Human` and `Head` have appeared. Whether this camera emits
  `Truck` / `Car` / `Bus` / `Bike` or falls back to generic `Vehicle` is still unverified, and
  `Truck` is the *default* object type in FR-2. Needs vehicles in view — a traffic video played
  on one of the monitors in frame would do it.
- **FR-5 sizing, stationary track lifetime.** `longest_stationary_s=0.5` so far; the person was
  moving throughout. The number that matters is how long a genuinely still object keeps its track
  before the tracker gives up — that sets the `stationary_hold_s` default and decides AC-2.
  Needs someone to sit still in frame for several minutes, or a static object placed in view.

The spike already logs both: `SPIKE_CLASSES` accumulates every distinct `class.type`, and
`SPIKE_STATIONARY` reports live still-tracks every 30 s with their running age.
