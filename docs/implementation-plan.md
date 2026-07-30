# Object Dwell Timer — implementation plan

Living document. Phase 0 is complete, and several design decisions changed as a result of what the
hardware actually did. Items marked **[verified]** were measured on-device; everything else is
still design intent.

Target device: AXIS Q3538-SLVE at `<DEVICE_IP>`, AXIS OS **12.11.77**, AOA 1.26.205, aarch64.
Build with ACAP Native SDK **12.11.0**.

---

## Context

Operators need to know how long a chosen object type (default: truck) has been sitting inside a
drawn zone, surfaced to a VMS (Genetec first, then Milestone/ACS) as enter / exit / dwell /
threshold events.

AXIS Object Analytics already detects, classifies and tracks objects, but its metadata carries no
dwell field. **[verified]** AOA does publish a `scenarioType: "TimeInArea"` event, but its payload
carries only `active`, `triggerTime`, `objectId`, `classTypes` — no elapsed value. It can tell you
*that* an object passed a time threshold, never *how long* it has been there. That gap is this
project.

This ACAP **consumes and does not detect**: zone membership, per-object timers, and event output
only. No inference, no model, no DLPU contention.

---

## Gate 0 — suitability

| Gate | Verdict | Note |
|---|---|---|
| Human safety | GO | Advisory only. Documented as *not* a safety interlock. |
| Privacy | GO | Class + normalized position only. `best-snapshot` stays disabled; the app strips `image.data` before logging. |
| Mission criticality | CAUTION | Failure loses dwell alerts; AOA and recording unaffected. Mitigated by `runMode: respawn` + a real health endpoint. |
| Regulatory | GO | No regulated workflow stated. |
| Scope | GO | "Time how long selected object types stay inside drawn zones and emit events." |
| Axis tech fit | GO | Fully on-device: Device Data Hub, AXEvent, AXParameter, axoverlay. |

---

## Metadata source **[verified]**

**Device Data Hub**, library `device-data-hub-client-c`, headers `<datahub/client.h>`,
`<datahub/subscriber.h>`, `<datahub/common.h>`. Manifest declares
`"resources": { "deviceDataHub": { "enabled": true } }`.

Topic **`com.axis.scene.frame.v1`** — frame-by-frame, Fusion Tracker, stable since AXIS OS 12.8.
`dh_client_get_topic_list()` on the target device confirms it is present, alongside
`com.axis.scene.object_track.v1` and `com.axis.scene.object_snapshot.v1`.

Not the Message Broker API: its topic is deprecated and the API is **removed in AXIS OS 13.0**.
The legacy topics remain reachable through DDH under an `mdb.` prefix
(`mdb.com.axis.analytics_scene_description.v0.beta`) if a fallback is ever needed.

Also available and useful: `com.axis.device.cpu-utilization.v1` and `memory-utilization.v1` — a
better basis for the AC-7 load check than external sampling.

### Payload **[verified]**

```json
{"channel_id":1,
 "timestamp":"2026-07-29T11:55:30.369150Z",
 "detections":[{"bounding_box":{"bottom":0.9853,"left":0.4499,"right":0.7473,"top":0.2365},
                "class":{"type":"Human","score":0.7,"upper_clothing_colors":[…]},
                "object_track_id":"8e653185-8e54-4dca-8af1-9f7202aca120"}],
 "track_events":[{"type":"TrackEnded","object_track_id":"…"}]}
```

- `detections` and `track_events` are **absent**, not empty, when there is nothing to report.
- `object_track_id` is a **UUID string**, not an integer.
- Attributes (clothing colours, etc.) live *inside* `class` alongside `type` and `score`.
- Bounding box is normalized 0–1, **origin top-left, Y increasing downward** — `bottom > top` and
  `right > left` on every observed track. Zone polygons need no transform.
- Frame rate is event-paced: ~0.5 fps idle, ~3.8 fps with a person moving.

### Classes **[partially verified]**

Full enum: `Human`, `Head`, `Vehicle`, `Car`, `Truck`, `Bus`, `Bike`, `VehicleOther`,
`LicensePlate`. Flat strings with a `score`; `Vehicle` is the fallback when a sub-type is
undetermined. Observed so far: `Human`, `Head`. Vehicle sub-types **still unconfirmed**.

> **`Head` and `LicensePlate` are attribute detections carrying their own `object_track_id`.**
> One person produces both a `Human` track and a `Head` track. They must be **excluded from dwell
> counting by default**, or the same object is timed twice and AC-6 breaks.

~10% of detections arrive with no `class` object at all — the EC-5 case, confirmed real.

---

## Architecture

```
 Fusion Tracker ──com.axis.scene.frame.v1──▶ Device Data Hub
                                                  │  (JSON, event-paced, CPU only)
                                                  ▼
   ┌──────────────────── object_dwell_timer (one process) ─────────────────────┐
   │ metadata_source → zone (point-in-polygon) → tracker (state machine)       │
   │        ┌──────────────┬───────────────┬──────────────┬──────────────┐     │
   │        ▼              ▼               ▼              ▼              ▼     │
   │   events.c       httpd.c         overlay.c       config.c     mqtt_bridge │
   └────────┼──────────────┼───────────────┼──────────────┼──────────────┼─────┘
            │ AXEvent      │ reverseProxy  │ axoverlay2   │ AXParameter  │ VAPIX
            ▼              ▼               ▼              ▼              ▼
   Device event system  /local/…      Video stream   localdata/    MQTT event bridge
            │                                                             │
            └─────────────────────────────────────────────────────────────┴─▶ Genetec
```

Single process, one `GMainLoop`. **DDH callbacks run on internal library threads** (stated in the
SDK header), so all shared state sits behind one `GMutex`, never held across I/O.

---

## Core algorithm

Time base is the frame's `timestamp` (camera NTP-synced UTC, microseconds), with a
`CLOCK_MONOTONIC` delta as a sanity cross-check (EC-6).

Per frame, in order:

1. **Apply `Rename` events first** — remap `from_id → to_id`. **[verified]** `Rename` exists in the
   schema but was **never observed** in testing, so it must not be the only re-association
   mechanism we rely on.
2. **Reference point** — default bbox bottom-centre `((left+right)/2, bottom)`; configurable to
   centroid. Origin top-left, Y down.
3. **Class filter** — match `class.type` against the selected set with `score ≥ minScore`,
   **excluding `Head` and `LicensePlate`**. Keep a sticky best class and the most recent one.
   EC-5 grace: hold an unclassified in-zone track provisionally for `classGrace` (3 s); if it later
   classifies as a selected type, backdate entry to first-inside.
4. **Point-in-polygon** — ray casting, per zone, in normalized coordinates.
5. **Advance the state machine** per `(object_track_id, zone_id)`.

### State machine

| From | Trigger | To | Emits |
|---|---|---|---|
| `ABSENT` | ref point inside | `PENDING_IN` | — (records `first_inside`) |
| `PENDING_IN` | inside ≥ `enterDebounce` | `IN` | **`Entered`**, `entry = first_inside` |
| `PENDING_IN` | leaves early | `ABSENT` | — |
| `IN` | every `updateInterval` | `IN` | **`DwellUpdate`** |
| `IN` | `elapsed ≥ threshold`, once | `IN` | **`ThresholdExceeded`**; overage reported thereafter |
| `IN` | ref point outside | `PENDING_OUT` | — (records `last_inside`) |
| `PENDING_OUT` | back inside | `IN` | — (timer never reset) |
| `PENDING_OUT` | outside ≥ `exitDebounce` | `ABSENT` | **`Exited`**, total = `last_inside − entry` |

### Absence handling — **changed by Phase 0 findings**

**[verified]** The device tracker reuses the same `object_track_id` across long absences — gaps of
**41.9 s and 49.1 s** were measured, against a normal frame interval of 0.1 s. The original plan's
5 s occlusion budget would have declared an exit while the source still considered the object
continuous.

Revised rule, which is both simpler and matches the source's own semantics:

- **Track absent from a frame, no `TrackEnded`** → the source still owns it. Keep the timer
  running; consume no gap budget. Absence is not exit.
- **`TrackEnded` received** → *now* start the gap budget:
  - moving when lost → `occlusionMaxGap`
  - stationary when lost → `stationaryHold` (reference point moved < 0.02 normalized over the
    preceding 3 s)
- **Ref point outside the polygon** → an ordinary exit governed by `exitDebounce`. Separate concern
  from absence.

Re-acquisition inside the budget resumes the same timer. A new track appearing within the budget,
within `reassocRadius` (0.05 normalized) of the last known position and class-compatible, is
adopted onto the existing dwell. Exceeding the budget is a genuine exit; a later return is a new
dwell (EC-3).

`occlusionMaxGap` and `stationaryHold` defaults are **deliberately unset** pending the stationary
lifetime measurement.

Short spurious tracks are real **[verified]** — several lived 0.1–0.5 s with `class=none`.
`enterDebounce` is what suppresses them.

---

## Interfaces

### IF-1 — AXEvent

Topic path `tnsaxis:CameraApplicationPlatform/ObjectDwellTimer/<Name>`:
`Entered`, `Exited`, `ThresholdExceeded`, `DwellUpdate` (stateless), and `ZoneOccupied`
(stateful, for "while occupied" VMS rules).

Declare `zone` (int) as a **source** key, not a data key — see below for why this now matters more
than expected.

Data keys: `objectId`, `objectType`, `objectSubType`, `state`, `elapsedSeconds`,
`thresholdExceeded`, `overageSeconds`, `utcTime`, `test`.

### IF-2 — MQTT **[verified]**

The app declares AXEvents and configures the device's own MQTT Event Bridge to publish them
(`POST /axis-cgi/mqtt/event.cgi`). The app never stores broker credentials.

**Measured topic format:**

```
axis/<SERIAL>/event/<namespaced path>[/$source/<key>/<value>]

axis/<SERIAL>/event/tns:axis/CameraApplicationPlatform/ObjectAnalytics/Device1Scenario1
axis/<SERIAL>/event/tns:onvif/Device/tns:axis/IO/Port/$source/port/1
```

- Default `deviceTopicPrefix` is `axis/<SERIAL>`; the bridge appends `/event`.
- With `includeTopicNamespaces: true` (default) namespaces render as **`tns:axis`** — with a colon.
- **Source keys appear in the topic path** as `$source/<key>/<value>`.

**This dissolves the constraint the original plan recorded.** Declaring `zone` as a source key
yields genuine per-zone topics, so a VMS can subscribe to one zone:

```
axis/<serial>/event/tns:axis/CameraApplicationPlatform/ObjectDwellTimer/Exited/$source/zone/3
```

**Measured payload:**

```json
{"topic":"onvif:Device/axis:IO/Port","timestamp":1785325150144,
 "message":{"source":{"port":"1"},"key":{},"data":{"state":"0"}}}
```

- **All values are stringified** — `"state":"0"`, not `0`. `elapsedSeconds` will arrive as
  `"42.500"`. Document this for integrators; do not promise JSON numbers or booleans.
- **`timestamp` is epoch milliseconds**, not ISO-8601 — which confirms our own `utcTime` data key
  is necessary rather than redundant (IF-4).

**Auto-configuration must be read-merge-write.** `configureEventPublication` replaces the *entire*
`eventFilterList`; a blind write deletes every filter the operator had. Read current config, drop
our own prior entries (idempotent), append ours, preserve everything else. Device-global flags
(`customTopicPrefix`, `appendEventTopic`, `includeTopicNamespaces`) are read-only unless the user
explicitly opts in. Disabling removes only our filters.

**Clean-device baseline** (restore target):
`{"topicPrefix":"default","customTopicPrefix":"","appendEventTopic":true,"includeTopicNamespaces":true,"includeSerialNumberInPayload":false,"eventFilterList":[]}`

The UI shows the resolved literal topic per event with copy buttons, a wildcard subscribe string,
and a sample payload; same data machine-readable at `GET /api/mqtt`.

### IF-3 — HTTP, behind `reverseProxy`

| Path | Access | Method |
|---|---|---|
| `/local/object_dwell_timer/status` | viewer | GET — in-zone objects, elapsed, overage |
| `…/api/config` | admin | GET / PUT — config including zone polygons |
| `…/api/test` | admin | POST `{kind}` — FR-10 test buttons |
| `…/api/mqtt` | admin | GET topics/payloads/state; POST apply or remove bridge config |
| `…/api/health` | viewer | GET — `subscribed`, `framesSeen`, `secondsSinceLastFrame` |

Apache forwards the **full** URI unchanged — register handlers on
`/local/object_dwell_timer/...`, not `/status`.

`Status="Running"` is not evidence an app works; `health` is what we and any automated check
actually verify.

### FR-10 — test buttons

`POST /api/test {kind}` builds a synthetic record with `test=true` and pushes it through the same
emit path. Real AXEvent, real MQTT, flagged as test.

---

## Config, UI, overlay

**AXParameter** for scalars, with `ax_parameter_register_callback` so edits apply live — never
snapshot at startup.

**Zone polygons** in `localdata/zones.json`, written atomically. `localdata/` survives reboot and
upgrade (NFR-5).

**UI** in `app/html/`, `settingPage: "index.html"`. **[verified]** The Apps page shows no *Open*
button unless `acapPackageConf.configuration.settingPage` is declared; the file must live in
`html/` relative to package root, and `acap-build` packages the directory automatically. Once
installed, `list.cgi` reports `ConfigurationPage=…`, which is what renders the button.

Canvas over `/axis-cgi/jpg/image.cgi` for polygon drawing; type multi-select; numeric settings;
test buttons; live status table; MQTT panel with resolved topics. HTML-encode everything rendered
from config and validate server-side too.

**Overlay** — `axoverlay2` + Cairo, zone outline plus per-object label, colour-shifted past
threshold. GPU/CPU only, no DLPU. Doubles as the OQ-2 verification tool.

---

## Build and packaging **[verified]**

- `FROM axisecp/acap-native-sdk:12.11.0-aarch64-ubuntu24.04` — **must match target firmware.**
  The DDH C API is not stable between 12.10 and 12.11 (`DHClientError`→`DHError`, listener objects
  →callback setters, `DHFilter` introduced, `dh_client_is_connected` removed). The published Axis
  GitHub example targets 12.11 and will not compile on 12.10. Read the headers in the SDK image
  you are building with.
- Manifest schema **2.2.0** (SDK 12.11 ships 2.0.0/2.1.0/2.2.0; SDK 12.10 caps at 2.0.0).
  The DDH resource key is `deviceDataHub` in 2.2.0 but `deviceDataHub_beta2` in 2.0.0.
- `setup` requires `appName, architecture, compatibleOsVersions, runMode, vendor, vendorId,
  version`. `vendorId` must match `^[A-Fa-f0-9]{10}$` — currently a placeholder.
  `compatibleOsVersions` items require **both** `min` and `max`, and **now gate installation**
  from AXIS OS 12.10 onward.
- `RUN find . -type f -exec chmod 644 {} +` — 777 source from a Windows filesystem makes the
  device silently refuse to create reverse-proxy rules.
- `.dockerignore` must list `*.eap`, or `acap-build` merges a stale manifest into the new package.
- `upload.cgi` multipart field is **`file`**. `Error: 2` = signature verification failed; unsigned
  apps are refused by default from AXIS OS 12.0.
- Under Git Bash, `docker run` mangles container paths — use PowerShell or `MSYS_NO_PATHCONV=1`.

---

## Phases

| Phase | State | Deliverable |
|---|---|---|
| **0** | **Done** | DDH subscribe + track statistics + status page + build/deploy + MQTT plumbing. OQ-1, OQ-2, IF-2 resolved on hardware. |
| **1** | **Done** | `zone` + `tracker` + `events` + `httpd` + operator page. 60 host tests pass; AXEvent → MQTT verified end-to-end to a live broker. FR-10 test triggers pulled forward — they were the only way to verify the emit path without an object in frame. |
| **2** | **Done** | Multi-zone drawing on a snapshot, AXParameter-backed settings applying live, validated `PUT /api/config` and `/api/zones`, event re-declaration on zone change. FR-11. **[verified]** Per-zone MQTT topics confirmed with two zones: `…/Entered/$source/zone/1` and `…/ThresholdExceeded/$source/zone/2`. |
| **3** | **Done** | MQTT bridge auto-config via D-Bus `GetCredentials` + loopback VAPIX, read-merge-write, resolved-topic panel with copy buttons, `GET`/`POST /api/mqtt`. IF-2. **[verified]** Resolver output diffed byte-for-byte against a live broker — identical. Merge safety proven with a pre-seeded operator filter. |
| **4** | **Done** | axoverlay2 + Cairo overlay, zone outlines and per-object timers, off by default. FR-12. **[verified]** Rendered on stream and captured, which also confirms the coordinate frame visually. |
| **5** | **Done except signing** | Security audit applied (buffer-size guard on the overlay submit path, single-threaded assumption documented, credentials zeroed on release), docs and learnings complete. Signing needs an Axis developer account and a real `vendorId`. |

## Verification

| Check | How |
|---|---|
| AC-1 | Truck enters → `Entered` within `enterDebounce`. Confirm three ways: app log, `mosquitto_sub`, device event list. |
| AC-2 | Static object in zone; poll `/status` ≥ 5 min; elapsed monotonic, never resets. |
| AC-3 | Occlude briefly → timer continues. Remove → `Exited` with correct total. |
| AC-4 | Threshold 20 s → `ThresholdExceeded` once, then `overageSeconds` on updates and exit. |
| AC-5 | Test buttons → event + MQTT with `test=true`. |
| AC-6 | Two objects in zone → two independent elapsed values. Confirm a person's `Head` track is not counted as a second object. |
| AC-7 | Subscribe to `com.axis.device.cpu-utilization.v1` with the app stopped vs running; confirm AOA still runs; grep syslog for `custodio`/`larod`/OOM. |
| IF-2 idempotency | Pre-seed a foreign event filter. Run auto-config twice. Assert ours appear once, theirs survives, global flags unchanged. Disable → only ours removed. |
| IF-2 topics | `mosquitto_sub -t '#'`, fire each test button, diff observed topic against the string the UI offers for copy. |
| NFR-5 | Reboot and restart AOA; config and zones survive, subscription recovers. |

## Design package applied (2026-07-30)

The operator page was rebuilt from a UI design package: live view as the hero with zone polygons and
timer badges, a plain-English verdict card and three tiles in place of eleven raw counters, editable
zone and class tables, and MQTT/diagnostics folded into disclosures. Reimplemented in plain
HTML/CSS/JS — the package ships React, which has no place on the device.

Four backend capabilities came with it:

1. **Class friendly names** — `ObjectTypeNames` (`Class=Name;…`) in AXParameter. Published as
   `objectType`; the raw class moved to a new `objectClass` field so existing rules can be repointed.
2. **Per-class minimum confidence** — `ClassMinScores`; falls back to the global floor.
3. **Per-zone class selection** — `classes` in `zones.json`; empty means inherit the enabled set.
4. **Per-zone threshold override** — `dwellThreshold` per zone; null means inherit.

**[verified]** All four persist across restart, validate server-side (attribute classes, unknown
classes, all-disabled, delimiter characters in names, out-of-range thresholds), and appear correctly
in real MQTT events.

## Open items

1. **Stationary track lifetime.** How long a still object keeps its track. Sets `stationaryHold`
   and decides whether AC-2 is achievable. Needs someone sitting still in frame for minutes.
2. **Vehicle sub-types.** Whether this device emits `Truck`/`Car`/`Bus`/`Bike` or only `Vehicle`.
   `Truck` is the FR-2 default. Needs vehicles in view — a traffic video on a monitor will do.
3. **Real `vendorId`** before any signed release.

## Risks

- **Vehicle sub-types may not appear** on this camera. FR-2's `Vehicle` fallback covers it.
- **`Rename` is not dependable** — never observed. Position+class re-association carries the load.
- **Clobbering operator MQTT config** is the main way this could do real damage; read-merge-write
  and the idempotency test exist specifically to prevent it.
- **Overlay + parsing CPU** on a multi-sensor device. Keep redraws throttled, overlay off by
  default until AC-7 is measured.
