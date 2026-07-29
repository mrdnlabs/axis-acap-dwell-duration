# Object Dwell Timer

An AXIS ACAP that measures how long selected object types stay inside user-drawn zones and emits
enter / exit / dwell / threshold events to the camera event system and MQTT.

> **Status: Phase 1. Timing works; there is no configuration UI yet.**
> Zones, per-object dwell timers, enter/exit/threshold events, MQTT output and test triggers are
> implemented and verified. Zones and settings are not yet editable from the browser. See
> [Project status](#project-status).

## How it works

AXIS Object Analytics already detects, classifies and tracks objects on the device, but its
metadata carries no dwell field — an AOA `TimeInArea` event tells you *that* an object crossed a
time threshold, never *how long* it has been there. This ACAP therefore **consumes and does not
detect**: it subscribes to the camera's existing `com.axis.scene.frame.v1` metadata over the
Device Data Hub, tests each object's reference point against user-drawn polygons, and keeps a
timer per `(object_track_id, zone_id)`.

It runs no inference and loads no model, so it adds no DLPU load and coexists with AOA. Output
goes out as AXEvents, which the device's own MQTT Event Bridge publishes to a broker — so the
application never holds broker credentials.

## Prerequisites

| | |
|---|---|
| Architecture | `aarch64` |
| AXIS OS | **12.11.72 or later** (12.11.77 validated) |
| SDK | ACAP Native SDK **12.11.0** — must match target firmware, see [caveats](#known-limitations) |
| Device features | Device Data Hub; AXIS Scene Metadata (`com.axis.scene.frame.v1`, AXIS OS 12.8+) |
| Dependency | A scene-metadata producer must be active — normally AXIS Object Analytics. This app performs no inference. |
| Build host | Docker |

Validated on: **AXIS Q3538-SLVE**, AXIS OS 12.11.77, AOA 1.26.205.

## Build

```bash
docker build --platform=linux/amd64 --build-arg ARCH=aarch64 --tag object_dwell_timer:0.1.0 .
```

Extract the package:

```bash
docker cp $(docker create --platform=linux/amd64 object_dwell_timer:0.1.0):/opt/app ./build
```

The `.eap` lands at `build/Object_Dwell_Timer_0_1_0_aarch64.eap`. Confirm the web assets were
packaged before deploying:

```bash
tar tzf build/Object_Dwell_Timer_0_1_0_aarch64.eap
```

## Install

The package is unsigned, so the device must be told to accept unsigned applications. This lowers a
security control — turn it back off when you are done.

```bash
curl --anyauth -u root:PASSWORD -X POST "http://DEVICE_IP/axis-cgi/applications/config.cgi?action=set&name=AllowUnsigned&value=true"
```

Upload and start:

```bash
curl --anyauth -u root:PASSWORD -F "file=@build/Object_Dwell_Timer_0_1_0_aarch64.eap;type=application/octet-stream" "http://DEVICE_IP/axis-cgi/applications/upload.cgi"
```

```bash
curl --anyauth -u root:PASSWORD "http://DEVICE_IP/axis-cgi/applications/control.cgi?action=start&package=object_dwell_timer"
```

Then open the app from the device's **Apps** page, or go directly to
`http://DEVICE_IP/local/object_dwell_timer/`.

Restore signing enforcement afterwards:

```bash
curl --anyauth -u root:PASSWORD -X POST "http://DEVICE_IP/axis-cgi/applications/config.cgi?action=set&name=AllowUnsigned&value=false"
```

## Endpoints

Served through the manifest's `reverseProxy`, so the device supplies TLS, authentication and access
level. Apache forwards the full URI, so handlers match the complete path.

| Path | Access | Method | Purpose |
|---|---|---|---|
| `/local/object_dwell_timer/status` | viewer | GET | in-zone objects with elapsed and overage, plus zone definitions |
| `/local/object_dwell_timer/api/health` | admin | GET | whether metadata is actually being received |
| `/local/object_dwell_timer/api/zones` | admin | GET | zone definitions |
| `/local/object_dwell_timer/api/test` | admin | POST | fire a real event flagged `test=true` |

## Verify it is working

`Status="Running"` is **not** evidence the app works — an ACAP can report running while its data
source never connects. Ask the app instead:

```bash
curl --anyauth -u root:PASSWORD "http://DEVICE_IP/local/object_dwell_timer/api/health"
```

`ok` is true only when the subscription exists *and* frames are arriving.

Prove the whole event path without waiting for an object, which also wires a VMS rule:

```bash
curl --anyauth -u root:PASSWORD -X POST "http://DEVICE_IP/local/object_dwell_timer/api/test?kind=entered"
```

Valid `kind` values are `entered`, `exited`, `threshold`, `update`. Each sends a genuine AXEvent
and, if the device MQTT client is configured, a genuine MQTT record — flagged `test=true`.

Watch them land:

```bash
mosquitto_sub -h BROKER -t '#' -v
```

The application log carries parseable prefixes (`DWELL_ENTER`, `DWELL_EXIT`, `DWELL_THRESHOLD`,
`DWELL_SUMMARY`, `DWELL_CLASSES`, `DWELL_CLOCKSTEP`):

```bash
curl --anyauth -u root:PASSWORD "http://DEVICE_IP/axis-cgi/admin/systemlog.cgi?appname=object_dwell_timer"
```

## Events

Topic path `tnsaxis:CameraApplicationPlatform/ObjectDwellTimer/<Name>` — `Entered`, `Exited`,
`ThresholdExceeded`, `DwellUpdate`. `zone` is declared as an ONVIF **source** key, which makes it a
selectable instance in a VMS and puts it in the MQTT topic path:

```
axis/<SERIAL>/event/tns:axis/CameraApplicationPlatform/ObjectDwellTimer/Exited/$source/zone/1
```

Data fields: `objectId`, `objectType`, `state`, `elapsedSeconds`, `thresholdExceeded`,
`overageSeconds`, `utcTime`, `test`.

> **For integrators:** the MQTT bridge stringifies every value — booleans arrive as `"1"`/`"0"` and
> doubles as `"112.000000"`. The bridge's own `timestamp` is epoch milliseconds; the `utcTime`
> field carries ISO-8601 UTC from the camera's NTP-synced clock.

## Tests

The timing rules are exercised on the host — `tracker.c` and `zone.c` depend only on glib, jansson
and libm, so the state machine can be driven with synthetic frames instead of a parked truck:

```bash
docker run --rm -v "$PWD":/src -w /src ubuntu:24.04 sh test/run.sh
```

## Configuration

Behaviour is active with the defaults below. They are compiled in for now — the editing UI and
AXParameter wiring arrive in Phase 2. Zones persist in `localdata/zones.json`, which survives
reboot and firmware upgrade.

| Setting | Default | Purpose |
|---|---|---|
| object types | all real classes | Classes that count as dwelling objects. `Head` and `LicensePlate` are always excluded — they are attributes of a parent object. |
| `minScore` | `0.30` | Minimum classification confidence |
| `fallbackToVehicle` | `true` | Treat an undetermined vehicle sub-type as `Vehicle` |
| reference point | bottom-centre | Point tested against the polygon; footprint rather than centroid |
| enter / exit debounce | `0.5 s` / `2.0 s` | Hysteresis on zone entry and exit |
| update interval | `10 s` | Periodic dwell-update event interval |
| dwell threshold | `100 s` | Triggers the exceeded event and overage reporting |
| `occlusionMaxGap` | `60 s` | Gap budget **after `TrackEnded`** for an object that was moving |
| `stationaryHold` | `300 s` | Gap budget **after `TrackEnded`** for an object that was still |

Two defaults deserve explanation:

- **Object types default to every real class**, not to `Truck` as FR-2 specifies. Until the config
  UI exists there is no way to change this on-device, and a `Truck`-only default makes the
  application untestable on any scene without a truck in it. This narrows to `Truck` when Phase 2
  lands.
- **`occlusionMaxGap` is 60 s, not the 5 s originally planned.** The camera's own tracker was
  measured reusing a single track id across absences of 41.9 s and 49.1 s. A shorter budget would
  declare an exit while the source still considers the object continuous. Note the budget only
  starts once `TrackEnded` arrives — mere absence never ends a dwell.

`stationaryHold` remains provisional pending the stationary-lifetime measurement.

## Project status

| Phase | State |
|---|---|
| **0 — Design spike** | **Done.** Metadata consumption, coordinate frame, class behaviour, track continuity, MQTT topic and payload format all measured on hardware. |
| **1 — Zones, timers, events** | **Done.** Point-in-polygon, per-object state machine, AXEvent output, status and health endpoints, test triggers. 60 unit tests pass; the event path is verified end-to-end to a live broker. |
| 2 — Config UI | Not started. Zone drawing on a snapshot, AXParameter, live settings. |
| 3 — MQTT | Not started. Bridge auto-configuration, copy-able resolved topics. |
| 4 — Overlay | Not started. |
| 5 — Hardening | Not started. Security audit, signing, release audit. |

**Verified:** the event and MQTT path end-to-end (AC-5, via the test triggers), and the timing
rules by unit test — enter/exit debounce, total dwell, threshold and overage, independent
per-object timers, `Rename` continuity, late-classification backdating, gap budgets, and the clock
guard.

**Not yet verified on hardware:** AC-1 to AC-4, AC-6 and AC-7 with real objects, because the test
camera views an indoor office. The logic behind each is unit-tested; what is missing is a scene.

## Known limitations

- **Not a dwell timer yet.** No zones, no timers, no events. See the phase table above.
- **The SDK version must match the target firmware.** The Device Data Hub C API changed between
  SDK 12.10 and 12.11 (`DHClientError`→`DHError`, listener objects→callback setters, `DHFilter`
  introduced). Code written against one will not compile against the other, and the published
  Axis GitHub example targets 12.11. Read the headers in the SDK image you are building with.
- **`vendorId` is a placeholder** (`4D52444E4C`). It is format-valid but not Axis-assigned, and
  must be replaced before any signed release.
- **Two measurements are still outstanding**, both needing scene activity this camera has not
  seen: how long a *stationary* object keeps its track (sets `stationaryHold` and decides AC-2),
  and whether this device emits vehicle sub-types (`Truck`/`Car`/`Bus`/`Bike`) or only the generic
  `Vehicle`. `Truck` is the default object type, so the latter matters.
- **The status page reads syslog**, which is a Phase 0 expedient. It depends on log retention and
  on the app's own verbosity, and is replaced by a real status endpoint in Phase 1.
- **Object snapshot imagery is deliberately not enabled.** The `best-snapshot` feature stays off,
  and the app strips any `image.data` before logging. Do not enable it without a privacy review.
- **Advisory only.** This is a convenience tool. It must not be used as a safety interlock or as
  the sole source of evidentiary timing.

## Repository layout

```
app/
  object_dwell_timer.c   application source
  manifest.json          ACAP manifest (schema 2.2.0)
  Makefile               build and link rules
  html/                  web UI, served at /local/object_dwell_timer/
docs/
  implementation-plan.md the full design and phase plan
learnings/               device findings and platform caveats, written as we go
tools/
  mosquitto.conf         test broker config for MQTT verification
Dockerfile               SDK toolchain and build
```

## Development notes

Device credentials live in `.env.devices`, which is git-ignored and must never be committed. Use
temporary development credentials and rotate them when finished.

Platform-specific findings are recorded in [learnings/](learnings/) as they are discovered —
including SDK/firmware API drift, manifest schema differences, MQTT topic formats, and scene
metadata behaviour.

This project follows the [AXIS ACAP guidance](https://github.com/mrdnlabs/axis-acap-guidance)
standards for project structure, security, and release readiness.

## License

MIT — see [LICENSE](LICENSE). Third-party components are listed in [app/LICENSE](app/LICENSE).
