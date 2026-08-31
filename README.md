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
history, and serves both a chart UI and a read-only JSON API through the
device's own authenticated reverse proxy.

- **Discovers what the device has.** CPU cores, thermal zones, network
  interfaces, block devices and mounted filesystems are enumerated at startup,
  so the same package renders correctly on a camera with an SD card and image
  sensor thermals and on a recorder with a SATA disk and eight PoE port VLANs.
  A few readings with no kernel equivalent, such as named temperature sensors,
  fan speed and per-port PoE, come from VAPIX using a service account the device
  issues over D-Bus, so no credentials are stored.
- **Charts over 5m, 15m, 30m, 1h, 6h, 24h, 7d and 30d**, backed by three ring
  buffers (1s, 15s and 5min resolution) that are sized against the device's RAM
  and written to an SD card or disk so they survive a reboot.
- **No extra ports.** Everything is served on loopback and exposed only through
  the reverse proxy, so it inherits the device's authentication.
- **Alerts on any metric**, raised as device events so they can drive the
  camera's own action rules, and mirrored to MQTT.
- **Exports to whatever you already run:** JSON, server-sent events, a
  Prometheus endpoint, MQTT with Home Assistant discovery, and InfluxDB 1.x
  or 2.x.

### Collected metrics

| Group | Examples |
|---|---|
| CPU | total and per-core usage, user/system/iowait/irq/steal split, per-core frequency |
| Memory | used, free, available, buffers, cached, swap |
| Temperature | every thermal zone, plus named sensors such as Optics, ImageSensor, IR, heater and fan speed where the product reports them |
| Network | per-interface throughput, packet rate, errors, drops, link speed, link state |
| Disk | per-device read/write throughput, IOPS and utilisation |
| Storage | per-filesystem total, used, free and usage percent, plus SD card wear and pre-EOL state |
| PoE | per-port power, allocation, class and connection state, with totals against the budget |
| System | load average, uptime, process counts, context switches, socket counts |

A device with no PoE ports or no SD card simply reports neither group; the
panels are built from what was found.

Read-only filesystems are skipped: the Axis root filesystem sits at 100% by
design, so charting it would be noise.

## Compatibility

| Package | AXIS OS | Architecture | Status |
|---|---|---|---|
| ACAP 4 | 12.10.68 - 13 | aarch64 | Supported |
| ACAP 4 | 12.10.68 - 13 | armv7hf | Built, not yet tested on hardware |

The minimum is set by the SDK the packages are built with, which is what the
device enforces at install time.

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
| `MqttEnabled` | `no` | Publish metrics to an MQTT broker. |
| `MqttHost` | empty | Broker hostname or address. |
| `MqttPort` | `1883` | Broker port. Defaults to 8883 when TLS is on and the port is unset. |
| `MqttTls` | `no` | Connect with TLS, verifying against the device's CA store. |
| `MqttUsername` | empty | Broker username. |
| `MqttPassword` | empty | Broker password. Stored write-only. |
| `MqttTopicPrefix` | empty | Defaults to `axis/<serial>/metrics`. |
| `MqttInterval` | `30` | Seconds between state publishes. |
| `MqttDiscovery` | `yes` | Publish Home Assistant discovery configs. |
| `MqttDiscoveryAll` | `no` | Publish a config for every metric instead of a curated subset. |
| `InfluxEnabled` | `no` | Write metrics to InfluxDB. |
| `InfluxVersion` | `v2` | `v1` for 1.x, `v2` for 2.x. Selects the endpoint and the authentication. |
| `InfluxUrl` | empty | Server URL, for example `http://influx.example.com:8086`. |
| `InfluxDatabase` | empty | Bucket on 2.x, database on 1.x. |
| `InfluxOrg` | empty | Organisation. 2.x only. |
| `InfluxToken` | empty | API token. 2.x only. Stored write-only. |
| `InfluxUsername` | empty | Username. 1.x only, optional. |
| `InfluxPassword` | empty | Password. 1.x only. Stored write-only. |
| `InfluxMeasurement` | `axis_metrics` | Measurement name to write into. |
| `InfluxInterval` | `30` | Seconds between writes. |

Alert rules, the metrics to display and transmit, and the theme are all set from
the dashboard rather than the parameter store.

## MQTT

With MQTT enabled the app publishes:

| Topic | Payload |
|---|---|
| `<prefix>/status` | `online` or `offline`, retained. Set as the last will, and retracted on a clean shutdown. |
| `<prefix>/state` | Retained JSON object of every current metric, keyed by id. |

The client is Eclipse Paho, linked statically: AXIS OS 12 ships `libpaho` but
OS 13 does not, and recorders have no device MQTT client API at all, so neither
can be relied on. TLS uses the device's own OpenSSL 3 and CA trust store.

### Home Assistant

Discovery configs are published to `homeassistant/sensor/<serial>_<metric>/config`,
grouped under one device per camera, with the availability topic wired up so
entities go unavailable when the device does.

By default only the metrics worth putting on a dashboard or alerting on become
entities, which is around 17 on a camera:

| Included | Deliberately not included |
|---|---|
| CPU usage, memory usage and used, load average, uptime | `mem.total` and other constants |
| Named sensors: CPU, Optics, ImageSensor, IR, heater, fan | The raw kernel thermal zones, unless a device reports no named sensors |
| Usage of removable storage (SD card, disk) | `/mnt/flash` and `/mnt/persistent`, which barely move and cannot be acted on |
| SD card wear and pre-EOL state | |
| PoE total and budget | Per-port PoE power, which is 32 entities on an eight-port recorder |
| Throughput of real interfaces | VLAN sub-interfaces such as `eth1_3` |

`MqttDiscoveryAll` publishes every metric instead. Changing either discovery
setting republishes immediately rather than waiting for a reconnect.

Discovery configs are retained, so a config this device published before but no
longer wants is withdrawn with an empty payload. Without that, a metric dropped
by an upgrade or a removed SD card would leave a dead entity in Home Assistant
forever.

Values are always published in base units, so a size is bytes and a rate is
bytes per second. A unit that changed with magnitude would break arithmetic and
split a history graph at the crossover point. The discovery config carries
`suggested_unit_of_measurement`, so Home Assistant displays GB for filesystems,
MB for memory and MB/s for throughput while still storing the raw value.

Home Assistant only applies a suggested unit when it first creates an entity.
Sensors created by an earlier version keep showing bytes; change the unit in the
entity's settings, or delete the device and let discovery recreate it.

A password is only sent when a username is set too. MQTT forbids a password on
its own, and a broker answers one with a protocol-level disconnect that is
indistinguishable from an unreachable host.

## InfluxDB

The app can push to InfluxDB as well, in either dialect:

| Version | Endpoint | Authentication |
|---|---|---|
| 2.x | `<url>/api/v2/write?org=&bucket=` | `Authorization: Token <token>` |
| 1.x | `<url>/write?db=` | optional username and password |

Everything lands in one measurement (`axis_metrics` by default), one field per
metric id, tagged with the device serial and model. Only metrics enabled for
transmit are sent.

```text
axis_metrics,device=B8A44F123456,model=AXIS\ P3288-LV cpu.usage=23.15,mem.used=525074400 1788163510
```

The write runs on a worker thread, because a broker or database that stops
answering would otherwise block the sampler and with it the whole dashboard. The
queue holds a single payload: metrics are a live view, so a stale sample is
dropped in favour of the next one rather than building a backlog.

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
| `processes?limit=<n>` | Top processes by CPU, with pid, name, CPU share and resident memory |
| `stream` | Server-sent events, one `data:` frame per sample |
| `prometheus` | Prometheus text exposition format |
| `health` | Liveness, metric count, sample count, memory use and the MQTT and InfluxDB connection states |

The tier is chosen from the requested window, so a 30 day request is answered
from the 5-minute buffer rather than by returning millions of points.

```sh
curl -k --anyauth -u user:password \
  'https://<device>/local/Metrics/data/series?window=3600&metrics=cpu.usage,mem.usage'
```

### Scraping with Prometheus

The endpoints sit behind the device's own reverse proxy, which authenticates
every request before the app sees it. HTTP basic auth is accepted, so Prometheus
needs no special handling:

```yaml
scrape_configs:
  - job_name: axis
    scheme: https
    tls_config: { insecure_skip_verify: true }
    basic_auth: { username: metrics, password: <password> }
    metrics_path: /local/Metrics/data/prometheus
    static_configs:
      - targets: ['192.168.0.10']
```

Create a dedicated device account with the **viewer** role for this. Viewer is
enough to read `data/...` and cannot reach `api/...`, so a scraper credential
cannot change any setting. Revoke it by deleting the account.

## History and persistence

Every tier is written to its own fixed-size circular file on the SD card or
disk, so all ranges from 5 minutes to 30 days survive a restart or reboot.

| File | Interval | Span |
|---|---|---|
| `history-fine.bin` | sample interval, 1s by default | 30 minutes |
| `history-medium.bin` | 15s | 12 hours |
| `history.bin` | 5 minutes | 30 days |

The index in each file header is flushed periodically rather than on every
sample, because rewriting a 4 KB header once a second would cost far more wear
than the sample itself. An unclean shutdown therefore loses the last few samples
of a tier rather than its whole recording.

It is deliberately never written to flash. A recorder has around 144 MB free on
`/mnt/flash`, and that wear belongs on removable storage. If the device has no
card or disk, history stays in memory, `meta` reports `persisted: false`, and the
dashboard shows a banner on the long ranges.

Storage is often not mounted yet when an ACAP starts at boot, so the app keeps
looking for it every 30 seconds. If a filesystem appears after startup, the
metric set is rediscovered as well, which is what gives a recorder's disk its
usage metrics and its own storage alert rule.

Samples are remapped by metric id when loaded, so adding an interface or
inserting an SD card does not invalidate the saved history.

## Choosing metrics

Two independent switches per metric, both on the settings page:

- **Display** decides what the dashboard charts.
- **Transmit** decides what leaves the device over MQTT, including which Home
  Assistant entities are created.

They are deliberately separate: a recorder with eight cameras can chart a
readable handful locally while publishing only what a broker needs, or collect
everything for Prometheus while keeping the page uncluttered.

Nothing is ever stopped from being collected. Every metric stays in `current`,
`series` and the Prometheus endpoint regardless, so a scraper is unaffected by a
display choice. The selection stores the *disabled* ids, so a metric that
appears after a firmware or app upgrade is on by default rather than silently
missing.

| Endpoint | Purpose |
|---|---|
| `api/metrics` | POST `scope=display\|transmit` and `disabled=<id,id,...>` |

## Appearance

The dashboard follows the system light or dark preference by default. The theme
button in the header cycles auto, light and dark, and the choice is stored per
browser rather than on the device, so it does not change what other operators
see. It needs no admin rights, unlike the settings dialog.

## Top processes

The dashboard lists the busiest processes with their pid, CPU share and resident
memory, refreshed every ten seconds. CPU is reported as a share of the whole
device, matching `cpu.usage`, so a process cannot read 53% on a device the rest
of the page calls 27% busy.

A percentage only exists as a difference between two readings, so a process that
has appeared since the last scan reports zero rather than a misleading spike.

## Alerts

Each rule watches one metric and fires once the condition has held for its
duration, so a single noisy sample cannot raise an alarm. Firing and clearing
are published three ways:

- As **stateful Axis events** under `CameraApplicationPlatform/Metrics/<rule>`,
  so they appear in the device's own action rules next to motion and tampering
  and can trigger a recording or notification with no involvement from this app.
- As a retained MQTT message on `<prefix>/alert/<rule>`.
- To the system log.

A default set is created on first run and kept in step with the code and the
device: CPU usage, memory usage, temperature, and one rule per mounted
filesystem. Rules for metrics a product does not report are skipped, and
built-ins that no longer apply are dropped on upgrade. Built-ins can be disabled
or retuned but not deleted, so a product's default cover cannot be lost by
accident.

The settings page edits all of this: thresholds, durations, comparison and
enabled state, plus adding and deleting rules of your own against any metric the
device reports. The threshold field shows the metric's unit so a byte rate is
not mistaken for a percentage.

| Endpoint | Purpose |
|---|---|
| `data/alerts` | Every rule with its current value and firing state |
| `api/rules` | POST `action=save` or `action=delete` with the rule fields |

```sh
curl -k --anyauth -u user:password -X POST \
  'https://<device>/local/Metrics/api/rules' \
  --data-urlencode action=save --data-urlencode id=fan_stopped \
  --data-urlencode 'name=Fan stopped' --data-urlencode metric=sensor.fan_rpm \
  --data-urlencode op=below --data-urlencode threshold=100 \
  --data-urlencode duration=60 --data-urlencode enabled=yes
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

## Tests

```sh
sh tests/run.sh                                   # unit tests, no device needed
ACAP_PASSWORD=... sh tests/smoke.sh <device> <user>  # every endpoint on a device
```

The unit tests cover tier downsampling and the window-to-tier mapping, which
are the places a bug produces plausible but wrong numbers instead of a crash.
The smoke test asks for every metric in one series request and checks the array
lengths line up, which is what catches truncation and encoding faults in the
request path. It reads the password from the environment so it never lands in
shell history.

### CI

Every push builds both architectures and runs the unit tests. Releases are cut
with a `workflow_dispatch` on `build.yml`, which produces a **draft** release
with the packages attached. The packages are then signed by Axis and the draft
is published with the signed EAPs in place of the unsigned ones, which is why
the release assets are named `signed_*.eap`.

## Links

- Releases: <https://github.com/Mo3he/Axis_Cam_Metrics/releases>
- Issues: <https://github.com/Mo3he/Axis_Cam_Metrics/issues>
- ACAP documentation: <https://developer.axis.com/acap/>
- Axis Communications: <https://www.axis.com/>

## License

The packaging and app code in this repository is licensed under BSD 3-Clause
(see [LICENSE](LICENSE)). Bundled upstream components are listed in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
