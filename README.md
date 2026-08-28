# Metrics ACAP for Axis Cameras

[![Release](https://img.shields.io/github/v/release/Mo3he/Axis_Cam_Metrics?style=flat)](https://github.com/Mo3he/Axis_Cam_Metrics/releases)
[![Build](https://github.com/Mo3he/Axis_Cam_Metrics/actions/workflows/build.yml/badge.svg)](https://github.com/Mo3he/Axis_Cam_Metrics/actions/workflows/build.yml)
[![License](https://img.shields.io/github/license/Mo3he/Axis_Cam_Metrics?style=flat)](LICENSE)
[![Super-Linter](https://github.com/Mo3he/Axis_Cam_Metrics/actions/workflows/super-linter.yml/badge.svg)](https://github.com/Mo3he/Axis_Cam_Metrics/actions/workflows/super-linter.yml)
[![Sponsor](https://img.shields.io/badge/Sponsor%20My%20Work-EA4AAA?style=flat&logo=github&logoColor=white)](https://github.com/sponsors/Mo3he)
[![Buy Me A Coffee](https://img.shields.io/badge/Buy%20Me%20A%20Coffee-FFDD00?style=flat&logo=buy-me-a-coffee&logoColor=black)](https://www.buymeacoffee.com/mo3he)

Real-time device metrics dashboard for AXIS cameras and recorders, with REST, Prometheus and MQTT export

> **Disclaimer:** Independent, community-developed ACAP package. Not an official
> Axis product and not affiliated with, endorsed by, or supported by Axis
> Communications AB. Use at your own risk.

## Overview

A self-contained metrics dashboard that runs on the device it monitors. It
samples the kernel directly (`/proc`, `/sys`) once a second, keeps a tiered
in-memory history, and serves both a chart UI and a read-only JSON API through
the device's own authenticated reverse proxy.

- **Discovers what the device has.** CPU cores, thermal zones, network
  interfaces, block devices and mounted filesystems are enumerated at startup,
  so the same package renders correctly on a camera with an SD card and image
  sensor thermals and on a recorder with a SATA disk and eight PoE port VLANs.
- **Charts over 5m, 15m, 30m, 1h, 6h, 24h, 7d and 30d**, backed by three ring
  buffers (1s, 15s and 5min resolution) that are sized against the device's RAM.
- **No extra ports.** Everything is served on loopback and exposed only through
  the reverse proxy, so it inherits the device's authentication.
- **No flash writes.** History is held in memory.

### Collected metrics

| Group | Examples |
|---|---|
| CPU | total and per-core usage, user/system/iowait/irq/steal split, per-core frequency |
| Memory | used, free, available, buffers, cached, swap |
| Temperature | every thermal zone the device exposes |
| Network | per-interface throughput, packet rate, errors, drops, link speed, link state |
| Disk | per-device read/write throughput, IOPS and utilisation |
| Storage | per-filesystem total, used, free and usage percent |
| System | load average, uptime, process counts, context switches, socket counts |

Read-only filesystems are skipped: the Axis root filesystem sits at 100% by
design, so charting it would be noise.

## Compatibility

| Package | AXIS OS | Architecture | Status |
|---|---|---|---|
| ACAP 4 | 11.11 - 13 | aarch64 | Supported |
| ACAP 4 | 11.11 - 13 | armv7hf | Supported |

The app runs as the unprivileged `sdk` user. It does not require root access.
Device model, serial and firmware are read from the parameter store rather than
`param.cgi`, so identity also works on OS 13 recorders where `param.cgi` is
absent.

## Installation

Download the EAP matching the device architecture from the
[Releases page](https://github.com/Mo3he/Axis_Cam_Metrics/releases), then:

1. Open the device web interface.
2. Go to **Apps -> Add app**.
3. Upload the EAP and start the app.
4. Open the app to see the dashboard.

## Configuration

Settings are stored in the device's parameter store and are available from
**Apps -> Metrics Dashboard -> Settings**.

| Parameter | Default | Description |
|---|---|---|
| `SampleInterval` | `1` | Seconds between samples, 1 to 10. A slower interval buys a longer fine-grained window rather than less memory. |

The settings page reads and writes through the app's own endpoint at
`/local/Metrics/api/settings`, served by the app itself and reachable only
through the device's authenticated reverse proxy. Nothing depends on
`/axis-cgi/param.cgi`, which recorder, NVR, and access-control class devices do
not expose, so settings persist on every device class.

## API

All endpoints are read-only, served under `/local/Metrics/data/` and require at
least `viewer` access.

| Endpoint | Returns |
|---|---|
| `meta` | Device identity, store layout, and every metric's id, label, unit and group |
| `current` | The latest sample as an id to value map |
| `series?window=<seconds>&metrics=<id,id,...>` | History for the named metrics, with a shared timestamp array |
| `health` | Liveness, metric count, sample count and memory use |

The tier is chosen from the requested window, so a 30 day request is answered
from the 5-minute buffer rather than by returning millions of points.

```sh
curl -k --anyauth -u user:password \
  'https://<device>/local/Metrics/data/series?window=3600&metrics=cpu.usage,mem.usage'
```

## Ports & security

- The app opens no ports on the device's external interfaces.
- The HTTP server binds `127.0.0.1:2207` and is reachable only through the
  device's authenticated reverse proxy.
- `api/...` requires `admin`; the read-only `data/...` endpoints require `viewer`.

## Build from source

Docker or Podman is required; the Axis ACAP Native SDK image does the build.

```sh
./build.sh                 # aarch64 and armv7hf into ./releases
ARCHES=aarch64 ./build.sh  # a single architecture
```

### CI

Every push builds both architectures and uploads the packages as workflow
artifacts. Pushing a `v*` tag creates a release with the packages attached.

## Links

- Releases: <https://github.com/Mo3he/Axis_Cam_Metrics/releases>
- Issues: <https://github.com/Mo3he/Axis_Cam_Metrics/issues>
- ACAP documentation: <https://developer.axis.com/acap/>
- Axis Communications: <https://www.axis.com/>

## License

The packaging and app code in this repository is licensed under BSD 3-Clause
(see [LICENSE](LICENSE)). Bundled upstream components are listed in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
