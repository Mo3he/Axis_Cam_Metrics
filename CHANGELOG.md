# Changelog

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
