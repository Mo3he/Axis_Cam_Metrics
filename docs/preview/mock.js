/* Sample data for the public preview: there is no camera behind this page.
 * This file stands in for the app's data/ and api/ endpoints with made-up
 * readings from one imaginary camera. Every other file here is a copy of
 * app/html; to refresh them, copy app/html/* over again and re-add
 * <script src="mock.js"></script> as the first script in index.html's <head>.
 */
(function () {
  "use strict";

  var realFetch = window.fetch ? window.fetch.bind(window) : null;
  var MiB = 1048576;
  var GiB = 1073741824;
  var CORES = 4;

  function nowSec() {
    return Math.floor(Date.now() / 1000);
  }

  var loadedAt = nowSec();
  var bootAt = loadedAt - (12 * 86400 + 5 * 3600 + 17 * 60);
  var appStartedAt = bootAt + 74;

  /* ------------------------------------------------------- deterministic noise */

  function hash(n, seed) {
    var x = Math.sin(n * 127.1 + seed * 311.7) * 43758.5453;
    return (x - Math.floor(x)) * 2 - 1;
  }

  /* Smooth value noise in [-1, 1] that changes over `scale` seconds. Detail
   * finer than `res` is averaged away, as the coarse history tiers do. */
  function wave(t, scale, seed, res) {
    var amp = res && res > scale ? scale / res : 1;
    var x = t / scale;
    var i = Math.floor(x);
    var f = x - i;
    f = f * f * (3 - 2 * f);
    return amp * (hash(i, seed) * (1 - f) + hash(i + 1, seed) * f);
  }

  /* 0 at night, 1 in the afternoon, in the viewer's local time. */
  function daylight(t) {
    var d = new Date(t * 1000);
    var hour = d.getHours() + d.getMinutes() / 60;
    return 0.5 - 0.5 * Math.cos((2 * Math.PI * (hour - 3)) / 24);
  }

  function clamp(v, lo, hi) {
    return Math.max(lo, Math.min(hi, v));
  }

  /* ------------------------------------------------------------- the device */

  var device = {
    model: "AXIS P3288-LV",
    product: "AXIS P3288-LV Dome Camera",
    type: "Dome Camera",
    serial: "B8A44F000000",
    soc: "Axis Artpec-9",
    architecture: "aarch64",
    firmware: "12.11.77",
    hostname: "axis-b8a44f000000"
  };

  var MEM_TOTAL = 1994940 * 1024;
  var VIDEO_TOTAL = 448 * MiB;
  var SD_TOTAL = 62333722624;
  var LOCAL_TOTAL = 943718400;

  function cpuUsage(t, res) {
    return clamp(
      21 + 9 * daylight(t) + 9 * wave(t, 420, 1, res) + 4 * wave(t, 45, 2, res) + 2.5 * wave(t, 4, 3, res),
      3, 100);
  }

  function txBps(t, res) {
    return 1.15 * MiB + 0.55 * MiB * daylight(t) + 0.22 * MiB * wave(t, 90, 11, res) + 0.12 * MiB * wave(t, 3, 12, res);
  }

  function sdUsedFraction(t) {
    var cycle = 3 * 86400;
    return 0.66 + 0.11 * ((t % cycle) / cycle);
  }

  function uptime(t) {
    return t >= bootAt ? t - bootAt : t - (bootAt - 41 * 86400);
  }

  function socTemp(t, res) {
    return 50.5 + 4 * daylight(t) + 0.08 * cpuUsage(t, res) + 1.2 * wave(t, 900, 21, res) + 0.3 * wave(t, 8, 22, res);
  }

  var defs = [];
  var gens = {};

  function metric(id, label, unit, group, gen) {
    defs.push({ id: id, label: label, unit: unit, group: group, display: true, transmit: true });
    gens[id] = gen;
  }

  metric("cpu.usage", "CPU usage", "%", "cpu", cpuUsage);
  metric("cpu.user", "CPU user", "%", "cpu", function (t, r) { return cpuUsage(t, r) * 0.66; });
  metric("cpu.system", "CPU system", "%", "cpu", function (t, r) { return cpuUsage(t, r) * 0.25; });
  metric("cpu.iowait", "CPU iowait", "%", "cpu", function (t, r) { return clamp(1.1 + 0.8 * wave(t, 20, 4, r), 0, 100); });
  metric("cpu.irq", "CPU irq", "%", "cpu", function (t, r) { return cpuUsage(t, r) * 0.05; });
  metric("cpu.steal", "CPU steal", "%", "cpu", function () { return 0; });
  metric("cpu.freq", "CPU frequency", "MHz", "cpu", function (t, r) { return cpuUsage(t, r) > 12 ? 1704 : 1296; });
  for (var core = 0; core < CORES; core++) {
    (function (core) {
      var bias = [6, -2, 1, -5][core];
      metric("cpu.core" + core + ".usage", "Core " + core + " usage", "%", "cpu", function (t, r) {
        return clamp(cpuUsage(t, r) + bias + 7 * wave(t, 30, 40 + core, r) + 3 * wave(t, 3, 50 + core, r), 0, 100);
      });
      metric("cpu.core" + core + ".freq", "Core " + core + " frequency", "MHz", "cpu", function (t, r) {
        return cpuUsage(t, r) > 12 ? 1704 : 1296;
      });
    })(core);
  }

  function load(t, r, lag) {
    return clamp((cpuUsage(t - lag, Math.max(r, lag)) / 100) * CORES * 0.85 + 0.25 + 0.15 * wave(t, lag, 60, r), 0, 20);
  }
  metric("load.1", "Load average 1m", "", "system", function (t, r) { return load(t, r, 60); });
  metric("load.5", "Load average 5m", "", "system", function (t, r) { return load(t, r, 300); });
  metric("load.15", "Load average 15m", "", "system", function (t, r) { return load(t, r, 900); });
  metric("sys.uptime", "Uptime", "s", "system", uptime);
  metric("sys.processes", "Processes", "", "system", function (t, r) { return Math.round(143 + 2 * wave(t, 300, 61, r)); });
  metric("sys.processes_running", "Runnable processes", "", "system", function (t, r) { return Math.round(clamp(1.5 + 1.5 * wave(t, 2, 62, r), 1, 6)); });
  metric("sys.context_switches", "Context switches", "/s", "system", function (t, r) { return 7200 + 95 * cpuUsage(t, r) + 400 * wave(t, 5, 63, r); });
  metric("sys.sockets", "Sockets in use", "", "system", function (t, r) { return Math.round(312 + 6 * wave(t, 600, 64, r)); });
  metric("sys.tcp_inuse", "TCP sockets", "", "system", function (t, r) { return Math.round(47 + 3 * wave(t, 600, 65, r)); });
  metric("sys.udp_inuse", "UDP sockets", "", "system", function () { return 21; });

  function memAvailable(t, r) {
    return MEM_TOTAL * (0.53 - 0.025 * wave(t, 3600, 71, r) - 0.006 * wave(t, 15, 72, r));
  }
  function memCached(t, r) { return MEM_TOTAL * (0.22 + 0.01 * wave(t, 7200, 73, r)); }
  function memBuffers() { return MEM_TOTAL * 0.011; }
  metric("mem.total", "Memory total", "B", "memory", function () { return MEM_TOTAL; });
  metric("mem.used", "Memory used", "B", "memory", function (t, r) { return MEM_TOTAL - memAvailable(t, r); });
  metric("mem.free", "Memory free", "B", "memory", function (t, r) {
    return memAvailable(t, r) - memCached(t, r) - memBuffers();
  });
  metric("mem.available", "Memory available", "B", "memory", memAvailable);
  metric("mem.buffers", "Buffers", "B", "memory", memBuffers);
  metric("mem.cached", "Cached", "B", "memory", memCached);
  metric("mem.usage", "Memory usage", "%", "memory", function (t, r) { return 100 * (1 - memAvailable(t, r) / MEM_TOTAL); });
  /* Cameras run without swap, so usage is unavailable; hidden from the charts
   * the way a user would switch an always-empty panel off. */
  metric("mem.swap_total", "Swap total", "B", "memory", function () { return 0; });
  metric("mem.swap_used", "Swap used", "B", "memory", function () { return 0; });
  metric("mem.swap_usage", "Swap usage", "%", "memory", function () { return null; });

  metric("temp.cpu_thermal", "cpu-thermal temperature", "C", "temperature", socTemp);
  metric("temp.ddr_thermal", "ddr-thermal temperature", "C", "temperature", function (t, r) {
    return socTemp(t, r) - 6.5 + 0.4 * wave(t, 60, 23, r);
  });

  metric("net.eth0.rx_bps", "eth0 receive", "B/s", "network", function (t, r) {
    return 21000 + 9000 * wave(t, 40, 31, r) + 4000 * wave(t, 3, 32, r);
  });
  metric("net.eth0.tx_bps", "eth0 transmit", "B/s", "network", txBps);
  metric("net.eth0.rx_pps", "eth0 receive packets", "/s", "network", function (t, r) { return 260 + 60 * wave(t, 20, 33, r); });
  metric("net.eth0.tx_pps", "eth0 transmit packets", "/s", "network", function (t, r) { return txBps(t, r) / 1380; });
  metric("net.eth0.rx_errors", "eth0 receive errors", "", "network", function () { return 0; });
  metric("net.eth0.tx_errors", "eth0 transmit errors", "", "network", function () { return 0; });
  metric("net.eth0.rx_drops", "eth0 receive drops", "", "network", function (t) { return 3 + Math.floor(uptime(t) / 43200); });
  metric("net.eth0.tx_drops", "eth0 transmit drops", "", "network", function () { return 0; });
  metric("net.eth0.link", "eth0 link speed", "Mb/s", "network", function () { return 1000; });
  metric("net.eth0.up", "eth0 link up", "", "network", function () { return 1; });

  metric("disk.mmcblk0.read_bps", "mmcblk0 read", "B/s", "disk", function (t, r) { return clamp(1800 + 2400 * wave(t, 15, 81, r), 0, 1e9); });
  metric("disk.mmcblk0.write_bps", "mmcblk0 write", "B/s", "disk", function (t, r) { return clamp(26000 + 30000 * wave(t, 10, 82, r), 0, 1e9); });
  metric("disk.mmcblk0.read_iops", "mmcblk0 read ops", "/s", "disk", function (t, r) { return clamp(0.6 + 0.6 * wave(t, 15, 83, r), 0, 1e6); });
  metric("disk.mmcblk0.write_iops", "mmcblk0 write ops", "/s", "disk", function (t, r) { return clamp(4 + 3 * wave(t, 10, 84, r), 0, 1e6); });
  metric("disk.mmcblk0.util", "mmcblk0 utilisation", "%", "disk", function (t, r) { return clamp(0.8 + 0.6 * wave(t, 10, 85, r), 0, 100); });
  metric("disk.mmcblk1.read_bps", "mmcblk1 read", "B/s", "disk", function (t, r) { return clamp(4000 + 5000 * wave(t, 30, 86, r), 0, 1e9); });
  metric("disk.mmcblk1.write_bps", "mmcblk1 write", "B/s", "disk", function (t, r) { return txBps(t - 2, r) * 0.93; });
  metric("disk.mmcblk1.read_iops", "mmcblk1 read ops", "/s", "disk", function (t, r) { return clamp(1 + wave(t, 30, 87, r), 0, 1e6); });
  metric("disk.mmcblk1.write_iops", "mmcblk1 write ops", "/s", "disk", function (t, r) { return txBps(t - 2, r) / 131072; });
  metric("disk.mmcblk1.util", "mmcblk1 utilisation", "%", "disk", function (t, r) { return clamp(9 + 3 * wave(t, 20, 88, r), 0, 100); });

  metric("fs.usr_local.total", "/usr/local total", "B", "storage", function () { return LOCAL_TOTAL; });
  metric("fs.usr_local.used", "/usr/local used", "B", "storage", function (t, r) { return LOCAL_TOTAL * (0.231 + 0.002 * wave(t, 86400, 91, r)); });
  metric("fs.usr_local.free", "/usr/local free", "B", "storage", function (t, r) { return LOCAL_TOTAL * (0.769 - 0.002 * wave(t, 86400, 91, r)); });
  metric("fs.usr_local.usage", "/usr/local usage", "%", "storage", function (t, r) { return 23.1 + 0.2 * wave(t, 86400, 91, r); });
  metric("fs.SD_DISK.total", "/var/spool/storage/SD_DISK total", "B", "storage", function () { return SD_TOTAL; });
  metric("fs.SD_DISK.used", "/var/spool/storage/SD_DISK used", "B", "storage", function (t) { return SD_TOTAL * sdUsedFraction(t); });
  metric("fs.SD_DISK.free", "/var/spool/storage/SD_DISK free", "B", "storage", function (t) { return SD_TOTAL * (1 - sdUsedFraction(t)); });
  metric("fs.SD_DISK.usage", "/var/spool/storage/SD_DISK usage", "%", "storage", function (t) { return 100 * sdUsedFraction(t); });

  metric("sensor.CPU", "CPU temperature", "C", "temperature", function (t, r) { return Math.round((socTemp(t, Math.max(r, 30)) - 1.5) * 4) / 4; });

  metric("power.draw", "Power draw", "W", "power", function (t, r) { return 5.6 + 0.025 * cpuUsage(t, r) + 0.15 * wave(t, 30, 101, r); });
  metric("power.requested", "Power requested", "W", "power", function () { return 12.95; });
  metric("power.pse_class", "PoE class (switch)", "", "power", function () { return 3; });

  metric("flash.life_used", "Flash life used", "%", "storage", function () { return 10; });
  metric("flash.pre_eol", "Flash pre-EOL state", "", "storage", function () { return 1; });

  metric("mem.video_total", "Video memory total", "B", "memory", function () { return VIDEO_TOTAL; });
  function videoUsed(t, r) { return VIDEO_TOTAL * (0.62 + 0.03 * wave(t, 1200, 111, r) + 0.004 * wave(t, 5, 112, r)); }
  metric("mem.video_used", "Video memory used", "B", "memory", videoUsed);
  metric("mem.video_usage", "Video memory usage", "%", "memory", function (t, r) { return (100 * videoUsed(t, r)) / VIDEO_TOTAL; });

  var MODELS = [
    { name: "object_analytics_detector", device: "a9-dlpu-tflite", bytes: 9437184, accelerator: true },
    { name: "object_analytics_classifier", device: "a9-dlpu-tflite", bytes: 4782080, accelerator: true },
    { name: "image_preprocessing", device: "cpu-proc", bytes: 0, accelerator: false }
  ];
  metric("dlpu.inferences", "DLPU inferences", "/s", "dlpu", function (t, r) {
    return clamp(19.6 + 0.6 * daylight(t) + 0.5 * wave(t, 6, 121, r), 0, 1000);
  });
  metric("dlpu.models", "DLPU models loaded", "", "dlpu", function () { return 2; });
  metric("dlpu.model_bytes", "DLPU model size", "B", "dlpu", function () { return 9437184 + 4782080; });
  metric("dlpu.sessions", "larod clients", "", "dlpu", function () { return 3; });

  defs.forEach(function (m) {
    if (/^mem\.swap_/.test(m.id)) m.display = false;
  });

  /* ------------------------------------------------------------- settings */

  var SECRETS = ["MqttPassword", "InfluxToken", "InfluxPassword"];
  var settings = {
    SampleInterval: "1",
    MqttEnabled: "yes",
    MqttHost: "homeassistant.local",
    MqttPort: "1883",
    MqttTls: "no",
    MqttUsername: "metrics",
    MqttPassword: "sample",
    MqttTopicPrefix: "",
    MqttInterval: "30",
    MqttDiscovery: "yes",
    MqttDiscoveryAll: "no",
    InfluxEnabled: "no",
    InfluxVersion: "v2",
    InfluxUrl: "",
    InfluxDatabase: "",
    InfluxOrg: "",
    InfluxToken: "",
    InfluxUsername: "",
    InfluxPassword: "",
    InfluxMeasurement: "axis_metrics",
    InfluxInterval: "30"
  };

  function settingsJson() {
    var out = {};
    Object.keys(settings).forEach(function (key) {
      if (SECRETS.indexOf(key) >= 0) {
        out[key] = "";
        out[key + "IsSet"] = settings[key] ? "yes" : "no";
      } else {
        out[key] = settings[key];
      }
    });
    return out;
  }

  /* --------------------------------------------------------------- history */

  var TIER_INTERVAL = [1, 15, 300];
  var TIER_CAPACITY = [1800, 2880, 8640];
  var TIER_MIN_CAPACITY = [300, 720, 576];
  var MAX_SERIES_POINTS = 4000;

  function tiers() {
    var fine = clamp(parseInt(settings.SampleInterval, 10) || 1, 1, 10);
    return TIER_INTERVAL.map(function (interval, i) {
      var iv = i === 0 ? fine : interval;
      var capacity = i === 0 ? Math.max(TIER_MIN_CAPACITY[0], Math.floor(TIER_CAPACITY[0] / fine)) : TIER_CAPACITY[i];
      return { interval: iv, capacity: capacity };
    });
  }

  function storeBytes() {
    return tiers().reduce(function (sum, tier) { return sum + tier.capacity * (defs.length * 4 + 8); }, 0);
  }

  /* JSON carries what a float32 printed with %.7g would. */
  function num(v) {
    if (v == null || !isFinite(v)) return null;
    return Number(Number(v).toPrecision(7));
  }

  function valueAt(id, t, res) {
    return num(gens[id](t, res || 1));
  }

  function latestTimestamp() {
    var fine = tiers()[0].interval;
    return Math.floor(nowSec() / fine) * fine;
  }

  function findDef(id) {
    for (var i = 0; i < defs.length; i++) if (defs[i].id === id) return defs[i];
    return null;
  }

  /* --------------------------------------------------------------- alerts */

  var rules = [
    { id: "storage_full_usr_local", name: "usr_local almost full", metric: "fs.usr_local.usage", op: "above", threshold: 90, duration: 300, enabled: true, builtin: true },
    { id: "storage_full_SD_DISK", name: "SD_DISK almost full", metric: "fs.SD_DISK.usage", op: "above", threshold: 90, duration: 300, enabled: true, builtin: true },
    { id: "cpu_high", name: "CPU usage high", metric: "cpu.usage", op: "above", threshold: 90, duration: 300, enabled: true, builtin: true },
    { id: "memory_high", name: "Memory usage high", metric: "mem.usage", op: "above", threshold: 90, duration: 300, enabled: true, builtin: true },
    { id: "temperature_high", name: "Temperature high", metric: "sensor.CPU", op: "above", threshold: 85, duration: 120, enabled: true, builtin: true },
    { id: "custom_cpu_busy", name: "CPU busier than usual", metric: "cpu.usage", op: "above", threshold: 36, duration: 20, enabled: true, builtin: false },
    { id: "custom_sd_stalled", name: "SD card stopped recording", metric: "disk.mmcblk1.write_bps", op: "below", threshold: 100000, duration: 60, enabled: true, builtin: false }
  ];

  function holds(rule, t) {
    if (!gens[rule.metric]) return false;
    var v = valueAt(rule.metric, t, tiers()[0].interval);
    if (v == null) return false;
    return rule.op === "below" ? v < rule.threshold : v > rule.threshold;
  }

  /* Firing once the condition held for the whole duration, judged on the
   * generated samples, so rules a visitor adds behave like real ones. */
  function evaluate(rule, now) {
    var step = tiers()[0].interval;
    var since = now;
    /* A rule added or edited here starts watching from that moment. */
    var limit = Math.max(rule.armedAt || 0, now - Math.max(rule.duration, 0) - 3600);
    while (since - step >= limit && holds(rule, since - step)) since -= step;
    var firing = rule.enabled && holds(rule, now) && now - since >= rule.duration;
    return { firing: firing, since: firing ? since + rule.duration : 0 };
  }

  function alertsJson() {
    var now = latestTimestamp();
    var firing = 0;
    var out = rules.map(function (rule) {
      var state = evaluate(rule, now);
      if (state.firing) firing++;
      return {
        id: rule.id, name: rule.name, metric: rule.metric, op: rule.op,
        threshold: num(rule.threshold), duration: rule.duration, enabled: rule.enabled,
        builtin: rule.builtin, firing: state.firing,
        value: gens[rule.metric] ? valueAt(rule.metric, now) : null,
        since: state.since
      };
    });
    return { rules: out, firing: firing };
  }

  function applyRule(form) {
    var action = form.get("action");
    var id = form.get("id");
    if (!action || !id || !/^[A-Za-z0-9_-]{1,63}$/.test(id)) return;
    var index = -1;
    rules.forEach(function (r, i) { if (r.id === id) index = i; });

    if (action === "delete") {
      if (index >= 0 && !rules[index].builtin) rules.splice(index, 1);
    } else if (action === "save") {
      var rule = index >= 0 ? rules[index] : null;
      if (!rule) {
        rule = { id: id, name: "", metric: "", op: "above", threshold: 0, duration: 0, enabled: false, builtin: false };
        rules.push(rule);
      }
      if (form.has("name")) rule.name = form.get("name");
      if (form.has("metric") && !rule.builtin) rule.metric = form.get("metric");
      if (form.has("op")) rule.op = form.get("op") === "below" ? "below" : "above";
      if (form.has("threshold")) {
        var parsed = parseFloat(form.get("threshold"));
        if (isFinite(parsed)) rule.threshold = parsed;
      }
      if (form.has("duration")) rule.duration = parseInt(form.get("duration"), 10) || 0;
      if (form.has("enabled")) rule.enabled = form.get("enabled") === "yes";
      if (!rule.name) rule.name = rule.id;
      rule.armedAt = latestTimestamp();
    }
  }

  /* ------------------------------------------------------------ processes */

  var PROCS = [
    ["vdod", 412, 5.1, 41], ["objectanalytics", 2215, 3.8, 63], ["larod", 1890, 2.2, 24],
    ["imaged", 388, 1.9, 19], ["rtspd", 655, 1.3, 9.5], ["httpd", 702, 0.9, 15],
    ["Metrics", 2471, 0.45, 6.2], ["storaged", 530, 0.35, 5.1], ["systemd-journald", 211, 0.3, 8.4],
    ["parhandd", 302, 0.22, 7.3], ["eventd", 344, 0.18, 6.6], ["dbus-broker", 276, 0.15, 3.9],
    ["avahi-daemon", 610, 0.06, 2.8], ["systemd", 1, 0.05, 9.7], ["sshd", 815, 0.0, 3.1]
  ];

  function processesJson(limit) {
    var bucket = Math.floor(nowSec() / 10);
    var rows = PROCS.map(function (p, i) {
      var cpu = Math.max(0, p[2] * (1 + 0.25 * hash(bucket, 200 + i)));
      return { pid: p[1], name: p[0], cpu: Number(cpu.toFixed(2)), rss: Math.round(p[3] * MiB * (1 + 0.01 * hash(bucket, 300 + i))) };
    });
    rows.sort(function (a, b) { return b.cpu - a.cpu; });
    return {
      interval: 10,
      cores: CORES,
      processes: rows.slice(0, limit),
      total: Math.round(gens["sys.processes"](nowSec(), 1))
    };
  }

  /* -------------------------------------------------------------- routing */

  function metaJson() {
    var t = tiers();
    return {
      app: { name: "Metrics", version: "0.1.7" },
      device: device,
      store: {
        persisted: true,
        persistPath: "/var/spool/storage/SD_DISK/Metrics/history.bin",
        tiers: t.map(function (tier) {
          return { interval: tier.interval, capacity: tier.capacity, span: tier.interval * tier.capacity, points: tier.capacity, persisted: true };
        }),
        bytes: storeBytes()
      },
      windows: [300, 900, 1800, 3600, 21600, 86400, 604800, 2592000],
      metrics: defs.map(function (m) {
        return { id: m.id, label: m.label, unit: m.unit, group: m.group, display: m.display, transmit: m.transmit };
      })
    };
  }

  function currentJson() {
    var ts = latestTimestamp();
    var values = {};
    defs.forEach(function (m) {
      var v = valueAt(m.id, ts);
      if (v != null) values[m.id] = v;
    });
    return { timestamp: ts, values: values };
  }

  function seriesJson(params) {
    var window_s = parseInt(params.get("window") || "1800", 10) || 1800;
    var t = tiers();
    var tierIndex = t.length - 1;
    for (var i = 0; i < t.length; i++) {
      if (Math.floor(window_s / t[i].interval) <= 2000) { tierIndex = i; break; }
    }
    var tier = t[tierIndex];
    var since = nowSec() - window_s;
    var newest = tierIndex === 0 ? latestTimestamp() : Math.floor(nowSec() / tier.interval) * tier.interval;
    var oldest = newest - (tier.capacity - 1) * tier.interval;
    var first = Math.max(oldest, Math.ceil(since / tier.interval) * tier.interval);
    var matched = first > newest ? 0 : (newest - first) / tier.interval + 1;
    var stride = Math.max(1, Math.ceil(matched / MAX_SERIES_POINTS));

    var timestamps = [];
    var mids = [];
    for (var n = 0; n < matched; n += stride) {
      var end = Math.min(n + stride, matched) - 1;
      timestamps.push(first + end * tier.interval);
      mids.push(first + ((n + end) / 2) * tier.interval);
    }

    var wanted = params.get("metrics");
    var ids = wanted ? wanted.split(",") : null;
    var series = {};
    defs.forEach(function (m) {
      if (ids && ids.indexOf(m.id) < 0) return;
      series[m.id] = mids.map(function (mid) { return valueAt(m.id, Math.round(mid), tier.interval * stride); });
    });
    return { window: window_s, tier: tierIndex, interval: tier.interval, series: series, timestamps: timestamps };
  }

  function healthJson() {
    var mqtt = settings.MqttEnabled === "yes" && settings.MqttHost ? "connected" : "disabled";
    var influx = settings.InfluxEnabled === "yes" && settings.InfluxUrl ? "connected" : "disabled";
    return {
      ok: true, metrics: defs.length, uptime: nowSec() - appStartedAt, samples: tiers()[0].capacity,
      bytes: storeBytes(), persisted: true, mqtt: mqtt, influx: influx
    };
  }

  function respond(status, body) {
    return Promise.resolve(new Response(JSON.stringify(body), {
      status: status,
      headers: { "Content-Type": "application/json", "Cache-Control": "no-store" }
    }));
  }

  function bodyText(init, request) {
    if (init && init.body != null) return Promise.resolve(String(init.body));
    if (request) return request.text();
    return Promise.resolve("");
  }

  function handle(scope, route, params, method, body) {
    var isGet = method === "GET";
    if (scope === "data" && (!isGet || route === "settings")) return respond(403, { error: "forbidden" });

    if (isGet && route === "settings") return respond(200, settingsJson());
    if (method === "POST" && route === "settings") {
      var form = new URLSearchParams(body);
      form.forEach(function (value, key) {
        if (Object.prototype.hasOwnProperty.call(settings, key)) settings[key] = value;
      });
      return respond(200, { status: "ok" });
    }
    if (isGet && route === "meta") return respond(200, metaJson());
    if (isGet && route === "current") return respond(200, currentJson());
    if (isGet && route === "series") return respond(200, seriesJson(params));
    if (isGet && route === "health") return respond(200, healthJson());
    if (isGet && route === "processes") {
      var limit = parseInt(params.get("limit") || "12", 10) || 12;
      return respond(200, processesJson(clamp(limit, 1, 100)));
    }
    if (isGet && route === "dlpu") return respond(200, { available: true, models: MODELS });
    if (isGet && route === "alerts") return respond(200, alertsJson());
    if (method === "POST" && route === "rules") {
      applyRule(new URLSearchParams(body));
      return respond(200, alertsJson());
    }
    if (method === "POST" && route === "metrics") {
      var sel = new URLSearchParams(body);
      var which = sel.get("scope") === "transmit" ? "transmit" : "display";
      if (sel.has("scope") && sel.has("disabled")) {
        var off = sel.get("disabled").split(",");
        defs.forEach(function (m) { m[which] = off.indexOf(m.id) < 0; });
      }
      return respond(200, { status: "ok" });
    }
    return respond(404, { error: "not found" });
  }

  /* Only data/ and api/ beside this page are answered here; everything else
   * goes to the network untouched. */
  var baseDir = new URL(".", document.baseURI || location.href);

  function match(url) {
    var u;
    try { u = new URL(url, document.baseURI || location.href); } catch (e) { return null; }
    if (u.origin !== baseDir.origin || u.pathname.indexOf(baseDir.pathname) !== 0) return null;
    var rest = u.pathname.slice(baseDir.pathname.length);
    var m = /^(data|api)\/([^/]+)\/?$/.exec(rest);
    if (!m) return null;
    return { scope: m[1], route: m[2], params: u.searchParams };
  }

  window.fetch = function (input, init) {
    var request = typeof Request !== "undefined" && input instanceof Request ? input : null;
    var hit = match(request ? request.url : String(input));
    if (!hit) {
      if (realFetch) return realFetch(input, init);
      return Promise.reject(new TypeError("fetch unavailable"));
    }
    var method = ((init && init.method) || (request && request.method) || "GET").toUpperCase();
    return bodyText(init, request).then(function (body) {
      return handle(hit.scope, hit.route, hit.params, method, body);
    });
  };
})();
