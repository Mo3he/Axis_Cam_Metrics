# Changelog


## Unreleased

- Fix the 12 hour, 7 day and 30 day ranges on devices that have kept history
  for 30 days: after a restart they returned the whole 30-day buffer out of
  order. The history file's index is written only periodically, so after an
  unclean stop the newest samples were read as the oldest. History is now
  restored in time order from every record on the card (which also keeps the
  last samples before an unclean stop), and range queries select samples by
  time, so a clock set backwards cannot break them either.
## 0.1.9 - 2026-11-26

- Release 0.1.9.
- **MQTT:** failed broker connections now show an error instead of staying
  stuck on "connecting". The app keeps retrying and returns to "connected"
  after a successful connection.

## 0.1.8 - 2026-11-26

- Release 0.1.8.
- **InfluxDB settings:** the "Write to InfluxDB" checkbox now saves, so
  InfluxDB output can be switched on from the settings page. Saving settings
  no longer clears a stored InfluxDB API token or password; leave the field
  empty to keep it.
- **Alerts:** disabling or deleting a rule while it is firing now clears it.
  Before, a disabled rule stayed firing for good, and its Axis event stayed
  active, so action rules kept running. Deleting a rule also removes its
  retained MQTT alert topic instead of leaving it on the broker.
- **Home Assistant:** changing which metrics are transmitted now updates
  discovery right away. Before, the change only reached Home Assistant after
  the next MQTT reconnect.

## 0.1.7 - 2026-11-16

- Release 0.1.7.
- **Video memory:** cameras now report video memory, the reserved pool that
  video buffers, the encoder and analytics share (Cmax on ARTPEC, DSP DRAM on
  Ambarella). When it fills up, new streams and analytics fail to start even
  though ordinary memory looks fine, so it is shown as a chart and a card and
  is a default Home Assistant entity.
- **DLPU:** devices with a deep learning processor now show its work rate, how
  many neural network models are loaded on it and their size, and how many
  clients are connected to larod. A new **Loaded models** panel lists each
  model larod has loaded with its device and size. Together they tell you which
  analytics hold the DLPU and whether they are actually running. On ARTPEC-9
  the rate is inferences per second; on Ambarella it is hardware jobs per
  second, and one image preprocessing job alone makes about three, so it shows
  load rather than inferences. The DLPU does not expose a busy
  percentage. larod is loaded at runtime, so recorders and other devices
  without it are unaffected.
- **History on the SD card:** a history folder left by an earlier install can
  end up owned by a different app, because AXIS OS reuses app users. The app
  could then not open it and kept history in memory only, losing it on every
  restart. It now moves the unreadable folder aside as `Metrics.stale-<time>`
  and starts a new history.

## 0.1.6 - 2026-11-16

- Release 0.1.6.
- **Power on cameras:** cameras now report their own power draw in watts, on a
  new Power chart and card. PoE was only read on recorders, which supply power
  to their ports; a camera draws PoE instead, so it never showed any power
  readings. The PoE class granted by the switch and over LLDP, and the power the
  camera requested, are added where the link reports them. Power draw is also a
  default Home Assistant entity, and watt values now use Home Assistant's power
  device class.

## 0.1.5 - 2026-11-16

- Release 0.1.5.
- **30 day chart:** the 30d view now shows the last 30 days up to now.
  Before, it stopped about two weeks short of today (in mid-November it ended
  at the end of October), while the 7d view was correct. A chart request can
  return at most 4000 points, and 30 days of 5 minute history is up to 8640,
  so the oldest points filled the chart and the newest were left out. Long
  windows are now averaged down to fit, so they always reach the latest sample.

## 0.1.4 - 2026-11-12

- Release 0.1.4.
- **Sample interval:** changing `SampleInterval` now resizes the 30 minute
  window straight away. Before, the fine history tier kept the old interval
  until a restart, so going from 10s to 1s left the 30 minute chart showing
  only the last 5 minutes.
- **MQTT:** connect handling (online status and Home Assistant discovery) now
  runs on the main loop instead of paho's network thread, where it could read
  the metric list while a newly mounted disk was rebuilding it.
- **Escaping:** alert names, Home Assistant discovery fields and alert MQTT
  payloads are JSON-escaped, so a quote or newline in a rule name no longer
  produces invalid JSON. Prometheus label values and help text are escaped too.
  A non-numeric alert threshold such as `nan` is ignored instead of breaking
  `data/alerts`.
- **Alert rules:** ids are limited to letters, digits, `_` and `-`, because
  they also name the rules file section and the device event. Deleting a rule
  removes its device event, and renaming one updates the event's name.
- **Settings:** saving returns an error when the device rejects a value,
  instead of reporting success.
- **HTTP:** a request body over the 16 KB limit gets `413` instead of being
  cut off silently, and a client that stops sending is dropped after 5 seconds
  rather than holding up sampling. A slow `data/stream` reader no longer gets a
  frame cut in half.
- **`meta`:** every history tier now reports `persisted: true` when history is
  on a card or disk, since all three tiers are saved.

## 0.1.3

### Breaking change

- **Prometheus metric names:** the network error and drop counters were
  labelled as per-second rates, but their values are running totals since the
  interface came up. They now carry no unit, so the Prometheus names lose the
  `_per_second` suffix: `axis_net_rx_errors`, `axis_net_tx_errors`,
  `axis_net_rx_drops` and `axis_net_tx_drops` (previously
  `axis_net_rx_errors_per_second` and so on). Update any dashboards or alerts
  that use the old names; use `rate()` or `increase()` for a per-second view.
  Metric ids (`net.<interface>.rx_errors` ...) and the MQTT and InfluxDB field
  names are unchanged.

## 0.1.2

### Security

- A viewer-level account could change settings. The `data/...` paths meant for
  viewers reached the same server as the admin `api/...` paths, so a viewer
  could POST to `data/settings`, `data/rules` or `data/metrics` and change the
  MQTT and InfluxDB configuration, the alert rules and which metrics are sent.
  Viewers could also read the non-secret settings, such as broker and database
  addresses. `data/...` is now served from a separate read-only port that
  refuses settings and anything other than a GET. The dashboard is unchanged
  for viewers and admins.

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

- The dashboard and `meta` reported the version the source was written with
  rather than the version of the installed package, because the release tooling
  only rewrites HTML files. The binary now takes it from the manifest at build
  time, so there is one place it is recorded.
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
