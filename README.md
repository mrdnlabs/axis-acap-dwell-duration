# Object Dwell Timer

An AXIS ACAP that measures how long selected object types stay inside user-drawn zones and emits
enter / exit / dwell / threshold events to the camera event system and MQTT.

> **Status: Phase 0 (design spike). Not yet functional as a dwell timer.**
> The current build consumes scene metadata and reports what the hardware actually does, so the
> timer logic is built on measurements rather than assumptions. See
> [Project status](#project-status) for exactly what is and is not implemented.

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

## Verify it is working

`Status="Running"` is **not** evidence the app works — an ACAP can report running while its data
source never connects. Check the application log instead:

```bash
curl --anyauth -u root:PASSWORD "http://DEVICE_IP/axis-cgi/admin/systemlog.cgi?appname=object_dwell_timer"
```

Every line is prefixed for machine parsing. The ones that matter:

| Prefix | Meaning |
|---|---|
| `SPIKE_OQ1` | whether `com.axis.scene.frame.v1` is present on this device |
| `SPIKE_TOPIC` | each Device Data Hub topic offered |
| `SPIKE_SUMMARY` | frame/detection counters and frame rate, every 30 s |
| `SPIKE_CLASSES` | every distinct `class.type` seen so far |
| `SPIKE_NEWTRACK` | first sight of a track, including coordinate-frame sanity checks |
| `SPIKE_TRACKEND` | lifetime, max gap, movement and class of a completed track |
| `SPIKE_STATIONARY` | live still-tracks and their running age |

The web page renders the same data, refreshed every 5 s.

## Configuration parameters

None yet — the spike has no configurable behaviour. The parameters below are the planned set
(see [docs/implementation-plan.md](docs/implementation-plan.md)); they are **not implemented**.

| Parameter | Default | Purpose |
|---|---|---|
| `objectTypes` | `Truck` | Classes that count as dwelling objects |
| `minScore` | `0.5` | Minimum classification confidence |
| `fallbackToVehicle` | `true` | Treat an undetermined vehicle sub-type as `Vehicle` |
| `referencePoint` | `bottomCenter` | Point tested against the polygon; or `centroid` |
| `enterDebounce` / `exitDebounce` | `0.5 s` / `2.0 s` | Hysteresis on zone entry and exit |
| `updateInterval` | `10 s` | Periodic dwell-update event interval |
| `dwellThreshold` | `100 s` | Threshold that triggers the exceeded event and overage reporting |
| `occlusionMaxGap` | TBD | Gap budget after `TrackEnded` for a moving object |
| `stationaryHold` | TBD | Gap budget after `TrackEnded` for an object that was still |
| `mqttAutoConfigure` | `true` | Let the app configure the device MQTT event bridge |
| `overlayEnabled` | `false` | Draw zones and elapsed times on the video stream |

The two `TBD` defaults are deliberately unset — they depend on a measurement not yet taken
(see [Known limitations](#known-limitations)).

## Project status

| Phase | State |
|---|---|
| **0 — Design spike** | **Done.** Metadata consumption, track statistics, status page, build and deploy pipeline, MQTT plumbing verified. |
| 1 — Zones, timers, events | Not started. Point-in-polygon, per-object state machine, AXEvent output, status endpoint. |
| 2 — Config UI | Not started. Zone drawing, AXParameter, persistence, test buttons. |
| 3 — MQTT | Not started. Bridge auto-configuration, copy-able resolved topics. |
| 4 — Overlay | Not started. |
| 5 — Hardening | Not started. Security audit, signing, release audit. |

No acceptance criterion (AC-1 … AC-7) has been verified yet.

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
