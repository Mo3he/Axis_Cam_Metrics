# Changelog

## 0.1.1

### Added

- Alert rules are editable from the settings page: thresholds, durations,
  comparison and enabled state, plus adding and deleting rules of your own.
- InfluxDB push, in both the 1.x and 2.x dialects, chosen by a setting. One
  measurement, one field per metric, tagged with the device serial and model.
  The write runs on a worker thread so an unreachable database cannot stall
  sampling.
- A top processes panel and a `data/processes` endpoint, reporting each
  process's CPU share of the whole device and its resident memory.
- `health` now reports the InfluxDB connection state alongside MQTT.

### Fixed

- MQTT sent a stored password even when the username was empty. MQTT forbids
  that, so the broker closed the connection at protocol level and the status sat
  on "connecting" forever with nothing in the log to say why.
- Changing either Home Assistant discovery setting did nothing until the
  connection happened to drop, because discovery was only published on connect.
- Discovery configs are retained, and ones no longer wanted were never
  withdrawn, so a metric dropped by an upgrade or a removed SD card left a dead
  entity in Home Assistant forever.
- Every history tier is persisted, not just the 5-minute one, so the 5m, 15m,
  30m, 1h and 6h ranges survive a restart instead of starting empty.
- Storage that mounts after the app starts is now picked up. An ACAP usually
  launches before the SD card or disk is ready at boot, which left history in
  memory for the whole session; the app now retries every 30 seconds.
- Filesystem metrics for late-mounting storage were missing for the whole
  session, because the metric set was decided before the mount existed. On a
  recorder that meant its own 3.6 TB disk had no usage metrics and no storage
  alert rule. The metric set is now rediscovered when the mount table changes.
- Closing the history file without loading it first wrote a header claiming
  zero samples, discarding the recording.
- Home Assistant showed raw bytes, so a 3.6 TB disk read as `3600000000000 B`.
  Discovery now carries `suggested_unit_of_measurement`, so sizes display as GB
  or MB and rates as MB/s while the published value stays in base units.

### Changed

- The default set of Home Assistant entities is now the metrics worth putting on
  a dashboard. Named sensors such as Optics and ImageSensor replace the raw
  kernel thermal zones, SD card wear and PoE totals are included, and constants
  like `mem.total`, the internal `/mnt/flash` and `/mnt/persistent` partitions
  and VLAN sub-interfaces are not. A recorder gained its disk and PoE readings
  and lost sixteen per-VLAN throughput entities.
- The theme control moved out of the settings dialog into a button in the
  header, so it no longer needs admin rights to reach.
- The history file header is flushed periodically rather than on every sample.
  Rewriting 4 KB once a second would have cost more card wear than the samples.
  An unclean shutdown now loses the last few samples of a tier rather than the
  whole recording.

## 0.1.0

First release.

### Dashboard

- Charts CPU, memory, temperature, network, disk, filesystem and system metrics
  over 5m, 15m, 30m, 1h, 6h, 24h, 7d and 30d.
- Panels are built from what the device actually reports, so one package suits
  both a camera and a recorder.
- Light, dark and system themes.
- Firing alerts appear at the top of the page.

### Collection

- Samples `/proc` and `/sys` once a second, configurable up to ten seconds.
- Named temperature sensors, fan speed and heater state through a VAPIX service
  account obtained over D-Bus.
- Per-port PoE power, allocation and class on recorders.
- SD and eMMC wear from the JEDEC life-time bands.
- CPU cores, network interfaces, block devices and mounts are discovered at
  startup; read-only filesystems are skipped.

### History

- Three ring buffers at 1s, 15s and 5min, sized against the device's RAM.
- The 5 minute tier is written to a circular file on the SD card or disk, so the
  long ranges survive a restart. It is never written to flash.
- Samples are remapped by metric id on load, so a changed metric set does not
  invalidate saved history.

### Interfaces

- JSON API: `meta`, `current`, `series`, `alerts`, `health`.
- Server-sent event stream.
- Prometheus text exposition with per-device labels and unit suffixes.
- MQTT publishing with a retained availability topic and Home Assistant
  discovery, using a statically linked Eclipse Paho client.
- Display and transmit can be chosen per metric, independently.

### Alerts

- Rules fire only once a condition has held for their duration.
- Published as stateful Axis events, so they appear in the device's own action
  rules, plus MQTT and syslog.
- Defaults are generated from what the device reports and stay in step with it.

### Notes

- Requires AXIS OS 12.10.68 or later, which is the minimum for packages built
  with the ACAP SDK 12.10.
- The armv7hf package builds but has not been tested on hardware.
