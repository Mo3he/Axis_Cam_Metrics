/* Metrics Dashboard UI.
 *
 * Panels are derived from whatever the device actually reported in /meta, so
 * the same page renders correctly on a camera (SD card, image sensor thermals)
 * and on a recorder (SATA disk, eight PoE port VLANs).
 */
(function () {
  "use strict";

  var BASE = "data/";
  var CHART_HEIGHT = 190;
  var RANGES = [
    { label: "5m", seconds: 300 },
    { label: "15m", seconds: 900 },
    { label: "30m", seconds: 1800 },
    { label: "1h", seconds: 3600 },
    { label: "6h", seconds: 21600 },
    { label: "24h", seconds: 86400 },
    { label: "7d", seconds: 604800 },
    { label: "30d", seconds: 2592000 }
  ];

  var meta = null;
  var charts = [];
  var window_s = 1800;
  var currentTimer = null;
  var seriesTimer = null;
  var procsTimer = null;

  /* ---------------------------------------------------------------- theme */

  var THEME_KEY = "metrics.theme";

  function applyTheme(theme) {
    if (theme === "auto") document.documentElement.removeAttribute("data-theme");
    else document.documentElement.setAttribute("data-theme", theme);
    try { localStorage.setItem(THEME_KEY, theme); } catch (e) { /* private mode */ }
  }

  function currentTheme() {
    try { return localStorage.getItem(THEME_KEY) || "auto"; } catch (e) { return "auto"; }
  }

  function cssVar(name) {
    return getComputedStyle(document.documentElement).getPropertyValue(name).trim();
  }

  /* uPlot paints the axes on a canvas, so it cannot inherit the CSS colours and
   * would otherwise draw near-black text on a dark panel. */
  function chartTheme() {
    return { axis: cssVar("--muted"), grid: cssVar("--grid") };
  }

  /* ---------------------------------------------------------- formatting */

  function fmtPercent(v) {
    return v == null ? "-" : v.toFixed(1) + "%";
  }

  function fmtBytes(v) {
    if (v == null) return "-";
    var units = ["B", "KB", "MB", "GB", "TB"];
    var i = 0;
    var n = Math.abs(v);
    while (n >= 1024 && i < units.length - 1) {
      n /= 1024;
      i++;
    }
    return (v < 0 ? "-" : "") + (n < 10 ? n.toFixed(1) : Math.round(n)) + " " + units[i];
  }

  function fmtRate(v) {
    return v == null ? "-" : fmtBytes(v) + "/s";
  }

  function fmtTemp(v) {
    return v == null ? "-" : v.toFixed(1) + " \u00b0C";
  }

  function fmtCount(v) {
    if (v == null) return "-";
    return Math.abs(v) >= 1000 ? Math.round(v).toLocaleString() : String(Math.round(v * 10) / 10);
  }

  var MONTHS = ["Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"];

  function pad(n) {
    return n < 10 ? "0" + n : String(n);
  }

  /* uPlot's default time axis prints 12-hour labels and drops a second row
   * carrying the date, which reads as a stray number under the plot. These
   * labels key off the tick interval uPlot actually chose rather than the
   * requested window: a 7 day view holding one hour of data wants clock times,
   * not the same date repeated six times. Hovering gives the full timestamp,
   * so the ticks stay short enough not to collide. */
  function axisTimeLabels(splits, incr) {
    return splits.map(function (seconds) {
      var d = new Date(seconds * 1000);
      if (incr >= 86400) return d.getDate() + " " + MONTHS[d.getMonth()];
      var clock = pad(d.getHours()) + ":" + pad(d.getMinutes());
      return incr < 60 ? clock + ":" + pad(d.getSeconds()) : clock;
    });
  }

  function fmtDuration(seconds) {
    if (seconds == null) return "-";
    var d = Math.floor(seconds / 86400);
    var h = Math.floor((seconds % 86400) / 3600);
    var m = Math.floor((seconds % 3600) / 60);
    if (d > 0) return d + "d " + h + "h";
    if (h > 0) return h + "h " + m + "m";
    return m + "m";
  }

  var FORMATTERS = {
    "%": fmtPercent,
    B: fmtBytes,
    "B/s": fmtRate,
    C: fmtTemp,
    "MHz": function (v) { return v == null ? "-" : Math.round(v) + " MHz"; },
    s: fmtDuration
  };

  function formatterFor(unit) {
    return FORMATTERS[unit] || fmtCount;
  }

  /* --------------------------------------------------------------- fetch */

  function getJson(path) {
    return fetch(BASE + path, { credentials: "same-origin", headers: { Accept: "application/json" } }).then(
      function (response) {
        if (!response.ok) throw new Error(path + " returned " + response.status);
        return response.json();
      }
    );
  }

  function setStatus(text, cls) {
    var el = document.getElementById("status");
    el.textContent = text;
    el.className = "pill" + (cls ? " " + cls : "");
  }

  /* ------------------------------------------------------- chart building */

  function idsMatching(pattern) {
    return displayable().filter(function (m) { return pattern.test(m.id); });
  }

  function hasId(id) {
    return displayable().some(function (m) { return m.id === id; });
  }

  /* Metrics the user switched off are still collected and exported; they are
   * simply not charted. */
  function displayable() {
    return meta.metrics.filter(function (m) { return m.display !== false; });
  }

  function pick(ids, labels) {
    var out = [];
    ids.forEach(function (id, i) {
      if (hasId(id)) out.push({ id: id, label: labels[i] });
    });
    return out;
  }

  /* Groups "net.eth0.rx_bps" style ids by their middle component, so every
   * interface or disk the device reported gets a line. */
  function byDevice(prefix, probe, suffixes, labels) {
    var devices = [];
    idsMatching(new RegExp("^" + prefix + "\\.[^.]+\\." + probe + "$")).forEach(function (m) {
      devices.push(m.id.split(".")[1]);
    });
    var series = [];
    devices.forEach(function (device) {
      suffixes.forEach(function (suffix, i) {
        var id = prefix + "." + device + "." + suffix;
        if (hasId(id)) series.push({ id: id, label: device + " " + labels[i] });
      });
    });
    return series;
  }

  function byUnit(group, unit) {
    return displayable().filter(function (m) { return m.group === group && m.unit === unit; });
  }

  function buildChartSpecs() {
    var specs = [];

    var cpu = pick(
      ["cpu.usage", "cpu.user", "cpu.system", "cpu.iowait", "cpu.irq", "cpu.steal"],
      ["total", "user", "system", "iowait", "irq", "steal"]
    );
    if (cpu.length) specs.push({ title: "CPU", unit: "%", max: 100, series: cpu });

    var cores = idsMatching(/^cpu\.core\d+\.usage$/).map(function (m) {
      return { id: m.id, label: "core " + m.id.split(".")[1].replace("core", "") };
    });
    if (cores.length > 1) specs.push({ title: "CPU per core", unit: "%", max: 100, series: cores });

    var load = pick(["load.1", "load.5", "load.15"], ["1 min", "5 min", "15 min"]);
    if (load.length) specs.push({ title: "Load average", unit: "", series: load });

    var mem = pick(
      ["mem.used", "mem.cached", "mem.buffers", "mem.available"],
      ["used", "cached", "buffers", "available"]
    );
    if (mem.length) specs.push({ title: "Memory", unit: "B", series: mem });

    var swap = pick(["mem.swap_usage"], ["swap used"]);
    if (swap.length && hasId("mem.swap_total")) specs.push({ title: "Swap", unit: "%", max: 100, series: swap });

    /* Both the raw thermal zones and the named CGI sensors land in this group;
     * driving off the unit keeps fan RPM and heater state out of it. */
    var temps = byUnit("temperature", "C").map(function (m) {
      return { id: m.id, label: m.id.replace(/^(temp|sensor)\./, "").replace(/_/g, " ") };
    });
    if (temps.length) specs.push({ title: "Temperature", unit: "C", series: temps });

    if (hasId("sensor.fan_rpm"))
      specs.push({ title: "Fan", unit: "rpm", series: [{ id: "sensor.fan_rpm", label: "speed" }] });

    var poe = displayable()
      .filter(function (m) { return /^poe\.port\d+\.power$/.test(m.id); })
      .map(function (m) { return { id: m.id, label: "port " + m.id.match(/port(\d+)/)[1] }; });
    if (poe.length) {
      if (hasId("poe.total")) poe.push({ id: "poe.total", label: "total" });
      specs.push({ title: "PoE power", unit: "W", series: poe });
    }

    var throughput = byDevice("net", "rx_bps", ["rx_bps", "tx_bps"], ["in", "out"]);
    if (throughput.length) specs.push({ title: "Network throughput", unit: "B/s", series: throughput });

    var errors = byDevice("net", "rx_errors", ["rx_errors", "tx_errors", "rx_drops", "tx_drops"],
      ["rx err", "tx err", "rx drop", "tx drop"]);
    if (errors.length) specs.push({ title: "Network errors and drops", unit: "", series: errors });

    var diskIo = byDevice("disk", "read_bps", ["read_bps", "write_bps"], ["read", "write"]);
    if (diskIo.length) specs.push({ title: "Disk throughput", unit: "B/s", series: diskIo });

    var fs = idsMatching(/^fs\.[^.]+\.usage$/).map(function (m) {
      return { id: m.id, label: m.id.split(".")[1] };
    });
    if (fs.length) specs.push({ title: "Filesystem usage", unit: "%", max: 100, series: fs });

    var sys = pick(
      ["sys.processes", "sys.sockets", "sys.context_switches"],
      ["processes", "sockets", "ctx switches/s"]
    );
    if (sys.length) specs.push({ title: "System", unit: "", series: sys });

    return specs;
  }

  var PALETTE = [
    "#4dabf7", "#f783ac", "#69db7c", "#ffa94d", "#b197fc", "#63e6be",
    "#ffd43b", "#ff8787", "#74c0fc", "#8ce99a", "#e599f7", "#ffc078"
  ];

  function createChart(spec) {
    var container = document.createElement("div");
    container.className = "chart";
    var heading = document.createElement("h2");
    heading.textContent = spec.title;
    container.appendChild(heading);
    var plotHost = document.createElement("div");
    container.appendChild(plotHost);
    document.getElementById("charts").appendChild(container);

    var format = formatterFor(spec.unit);
    var theme = chartTheme();
    var options = {
      width: plotHost.clientWidth || 400,
      height: CHART_HEIGHT,
      cursor: { drag: { x: true, y: false } },
      scales: { x: { time: true }, y: spec.max ? { range: [0, spec.max] } : {} },
      axes: [
        {
          stroke: theme.axis,
          grid: { stroke: theme.grid },
          ticks: { stroke: theme.grid },
          values: function (self, splits, axisIdx, foundSpace, foundIncr) {
            return axisTimeLabels(splits, foundIncr);
          }
        },
        {
          /* Byte rates reach "1.32 MB/s", which clips at the default width. */
          size: 76,
          stroke: theme.axis,
          grid: { stroke: theme.grid },
          ticks: { stroke: theme.grid },
          values: function (self, ticks) {
            return ticks.map(function (v) { return format(v); });
          }
        }
      ],
      /* uPlot formats the time series legend itself and ignores a function
       * here, so it takes a date template. {HH} is 24-hour, {h} would be 12. */
      series: [{ label: "Time", value: "{YYYY}-{MM}-{DD} {HH}:{mm}:{ss}" }].concat(
        spec.series.map(function (s, i) {
          return {
            label: s.label,
            stroke: PALETTE[i % PALETTE.length],
            width: 1.5,
            spanGaps: false,
            value: function (self, v) { return format(v); }
          };
        })
      )
    };

    var plot = new uPlot(options, [[]].concat(spec.series.map(function () { return []; })), plotHost);
    var chart = { spec: spec, plot: plot, host: plotHost };

    /* The grid lays out after this returns, so the initial width is 0. */
    if (typeof ResizeObserver === "function") {
      new ResizeObserver(function () { resizeChart(chart); }).observe(plotHost);
    }
    return chart;
  }

  function resizeChart(chart) {
    var width = chart.host.clientWidth;
    if (width > 0 && width !== chart.plot.width) chart.plot.setSize({ width: width, height: CHART_HEIGHT });
  }

  function resizeCharts() {
    charts.forEach(resizeChart);
  }

  /* Axis colours are baked in at construction, so a theme change rebuilds. */
  function rebuildCharts() {
    charts.forEach(function (chart) { chart.plot.destroy(); });
    charts = [];
    document.getElementById("charts").innerHTML = "";
    buildChartSpecs().forEach(function (spec) { charts.push(createChart(spec)); });
    resizeCharts();
    refreshSeries();
  }

  /* ------------------------------------------------------------- updating */

  function wantedMetricIds() {
    var ids = {};
    charts.forEach(function (chart) {
      chart.spec.series.forEach(function (s) { ids[s.id] = true; });
    });
    return Object.keys(ids);
  }

  function refreshSeries() {
    var ids = wantedMetricIds();
    if (!ids.length) return Promise.resolve();

    return getJson("series?window=" + window_s + "&metrics=" + encodeURIComponent(ids.join(",")))
      .then(function (payload) {
        var timestamps = payload.timestamps || [];
        charts.forEach(function (chart) {
          var data = [timestamps].concat(
            chart.spec.series.map(function (s) {
              var values = payload.series[s.id] || [];
              /* uPlot wants null for gaps, which is what the API already sends. */
              return values.length === timestamps.length ? values : new Array(timestamps.length).fill(null);
            })
          );
          chart.plot.setData(data);
        });
        setStatus("live", "live");
      })
      .catch(function (error) {
        setStatus(error.message, "error");
      });
  }

  function card(label, value, sub, severity) {
    return (
      '<div class="card' + (severity ? " " + severity : "") + '">' +
      '<div class="label">' + label + "</div>" +
      '<div class="value">' + value + "</div>" +
      '<div class="sub">' + (sub || "&nbsp;") + "</div></div>"
    );
  }

  function severityFor(percent) {
    if (percent == null) return "";
    if (percent >= 90) return "bad";
    if (percent >= 75) return "warn";
    return "";
  }

  function refreshCurrent() {
    return getJson("current")
      .then(function (payload) {
        var v = payload.values || {};
        var html = "";

        html += card("CPU", fmtPercent(v["cpu.usage"]),
          v["cpu.freq"] != null ? Math.round(v["cpu.freq"]) + " MHz" : "", severityFor(v["cpu.usage"]));

        html += card("Memory", fmtPercent(v["mem.usage"]),
          fmtBytes(v["mem.used"]) + " of " + fmtBytes(v["mem.total"]), severityFor(v["mem.usage"]));

        /* Report the hottest sensor: which one is hottest varies by product. */
        var hottest = null;
        var hottestName = "";
        Object.keys(v).forEach(function (id) {
          if (id.indexOf("temp.") === 0 && (hottest == null || v[id] > hottest)) {
            hottest = v[id];
            hottestName = id.replace("temp.", "").replace(/_/g, " ");
          }
        });
        if (hottest != null) html += card("Temperature", fmtTemp(hottest), hottestName);

        /* Headline on the biggest filesystem, which is the SD card or recording
         * disk. Picking the fullest instead would point at internal flash on a
         * recorder, which is not the storage anyone means. Every filesystem is
         * still charted below. */
        var biggest = null;
        var biggestName = "";
        Object.keys(v).forEach(function (id) {
          var match = /^fs\.(.+)\.total$/.exec(id);
          if (match && (biggest == null || v[id] > biggest)) {
            biggest = v[id];
            biggestName = match[1];
          }
        });
        if (biggestName) {
          var usage = v["fs." + biggestName + ".usage"];
          html += card("Storage", fmtPercent(usage),
            fmtBytes(v["fs." + biggestName + ".free"]) + " free on " + biggestName, severityFor(usage));
        }

        var rx = 0;
        var tx = 0;
        Object.keys(v).forEach(function (id) {
          if (/^net\..+\.rx_bps$/.test(id)) rx += v[id] || 0;
          if (/^net\..+\.tx_bps$/.test(id)) tx += v[id] || 0;
        });
        html += card("Network", fmtRate(rx + tx), "in " + fmtRate(rx) + " / out " + fmtRate(tx));

        html += card("Load", fmtCount(v["load.1"]),
          fmtCount(v["load.5"]) + " / " + fmtCount(v["load.15"]) + " avg");

        html += card("Uptime", fmtDuration(v["sys.uptime"]), fmtCount(v["sys.processes"]) + " processes");

        if (v["poe.total"] != null) {
          html += card("PoE", fmtCount(v["poe.total"]) + " W",
            "of " + fmtCount(v["poe.budget"]) + " W budget",
            severityFor(v["poe.budget"] ? (100 * v["poe.total"]) / v["poe.budget"] : null));
        }

        /* Wear only moves over years, so it is a status readout rather than a
         * chart. pre_eol 1 is normal; 2 and 3 mean the card is wearing out. */
        if (v["flash.life_used"] != null) {
          var eol = v["flash.pre_eol"];
          html += card("Flash wear", fmtPercent(v["flash.life_used"]),
            eol >= 3 ? "urgent replacement" : eol === 2 ? "nearing end of life" : "healthy",
            eol >= 3 ? "bad" : eol === 2 ? "warn" : severityFor(v["flash.life_used"]));
        }

        document.getElementById("cards").innerHTML = html;
      })
      .catch(function (error) {
        setStatus(error.message, "error");
      });
  }

  /* --------------------------------------------------- metric selection UI */

  function buildMetricList() {
    var host = document.getElementById("metric-list");
    var groups = {};
    meta.metrics.forEach(function (m) { (groups[m.group] = groups[m.group] || []).push(m); });

    host.innerHTML = Object.keys(groups)
      .sort()
      .map(function (group) {
        var rows = groups[group]
          .map(function (m) {
            return (
              '<div class="metric-row"><span>' + escapeHtml(m.label) +
              "<br /><code>" + escapeHtml(m.id) + "</code></span>" +
              '<input type="checkbox" data-scope="display" data-id="' + escapeHtml(m.id) + '"' +
              (m.display !== false ? " checked" : "") + ' aria-label="Display ' + escapeHtml(m.id) + '" />' +
              '<input type="checkbox" data-scope="transmit" data-id="' + escapeHtml(m.id) + '"' +
              (m.transmit !== false ? " checked" : "") + ' aria-label="Transmit ' + escapeHtml(m.id) + '" />' +
              "</div>"
            );
          })
          .join("");
        return (
          '<details class="metric-group"><summary>' + escapeHtml(group) +
          " (" + groups[group].length + ")" +
          ' <button type="button" class="ghost" data-toggle-group="' + escapeHtml(group) +
          '" data-scope="display">display all/none</button>' +
          ' <button type="button" class="ghost" data-toggle-group="' + escapeHtml(group) +
          '" data-scope="transmit">transmit all/none</button>' +
          "</summary>" + rows + "</details>"
        );
      })
      .join("");

    host.querySelectorAll("[data-toggle-group]").forEach(function (button) {
      button.addEventListener("click", function (event) {
        event.preventDefault();
        var scope = button.dataset.scope;
        var boxes = button.closest("details").querySelectorAll('input[data-scope="' + scope + '"]');
        var allOn = Array.prototype.every.call(boxes, function (b) { return b.checked; });
        boxes.forEach(function (b) { b.checked = !allOn; });
      });
    });
  }

  function saveMetricSelection() {
    var host = document.getElementById("metric-list");
    return Promise.all(
      ["display", "transmit"].map(function (scope) {
        var disabled = Array.prototype.filter
          .call(host.querySelectorAll('input[data-scope="' + scope + '"]'), function (b) { return !b.checked; })
          .map(function (b) { return b.dataset.id; });
        return fetch("api/metrics", {
          method: "POST",
          credentials: "same-origin",
          headers: { "Content-Type": "application/x-www-form-urlencoded" },
          body: new URLSearchParams({ scope: scope, disabled: disabled.join(",") }).toString()
        });
      })
    );
  }

  /* ------------------------------------------------------------- settings */

  var CHECKBOXES = ["MqttEnabled", "MqttTls", "MqttDiscovery", "MqttDiscoveryAll"];
  var passwordIsSet = false;

  function settingsForm() {
    return document.getElementById("settings-form");
  }

  function fillSettings(values) {
    var form = settingsForm();
    Object.keys(values).forEach(function (key) {
      var field = form.elements[key];
      if (!field) return;
      if (CHECKBOXES.indexOf(key) >= 0) field.checked = values[key] === "yes";
      else field.value = values[key];
    });

    passwordIsSet = values.MqttPasswordIsSet === "yes";
    form.elements.MqttPassword.value = "";
    form.elements.MqttPassword.placeholder = passwordIsSet ? "unchanged" : "not set";

    var prefix = form.elements.MqttTopicPrefix;
    if (!prefix.value && meta && meta.device && meta.device.serial)
      prefix.placeholder = "axis/" + meta.device.serial + "/metrics";
  }

  function collectSettings() {
    var form = settingsForm();
    var body = new URLSearchParams();

    Array.prototype.forEach.call(form.elements, function (field) {
      if (!field.name) return;
      if (CHECKBOXES.indexOf(field.name) >= 0) {
        body.set(field.name, field.checked ? "yes" : "no");
      } else if (field.name === "MqttPassword") {
        /* Empty means "leave the stored password alone", since the API never
         * returns it to prefill the field. */
        if (field.value) body.set(field.name, field.value);
      } else {
        body.set(field.name, field.value);
      }
    });
    return body;
  }

  function openSettings() {
    var error = document.getElementById("settings-error");
    error.hidden = true;

    fetch("api/settings", { credentials: "same-origin" })
      .then(function (r) {
        if (!r.ok) throw new Error("settings unavailable (" + r.status + ")");
        return r.json();
      })
      .then(function (values) {
        fillSettings(values);
        applyInfluxVersion();
        buildMetricList();
        document.getElementById("theme-select").value = currentTheme();
        document.getElementById("settings").showModal();
        return buildRuleList();
      })
      .catch(function (e) {
        error.textContent = e.message;
        error.hidden = false;
        document.getElementById("settings").showModal();
      });
  }

  function saveSettings() {
    var error = document.getElementById("settings-error");
    var button = document.getElementById("settings-save");
    error.hidden = true;
    button.disabled = true;

    fetch("api/settings", {
      method: "POST",
      credentials: "same-origin",
      headers: { "Content-Type": "application/x-www-form-urlencoded" },
      body: collectSettings().toString()
    })
      .then(function (r) {
        if (!r.ok) throw new Error("save failed (" + r.status + ")");
        return saveMetricSelection();
      })
      .then(saveRules)
      .then(function () {
        document.getElementById("settings").close();
        return getJson("meta");
      })
      .then(function (fresh) {
        meta = fresh;
        rebuildCharts();
        return refreshHealth();
      })
      .catch(function (e) {
        error.textContent = e.message;
        error.hidden = false;
      })
      .finally(function () {
        button.disabled = false;
      });
  }

  function refreshHealth() {
    return getJson("health")
      .then(function (health) {
        statePill("mqtt-state", health.mqtt);
        statePill("influx-state", health.influx);
      })
      .catch(function () {});
  }

  function statePill(id, state) {
    var pill = document.getElementById(id);
    if (!pill) return;
    pill.textContent = state || "disabled";
    pill.className = "pill" + (state === "connected" ? " live" : state === "error" || state === "unauthorized" ? " error" : "");
  }

  /* 1.x authenticates with a username and password against a database, 2.x with
   * a token against an organisation's bucket. Showing both at once invites
   * filling in the pair that is ignored. */
  function applyInfluxVersion() {
    var form = settingsForm();
    var v2 = form.elements.InfluxVersion.value !== "v1";
    document.getElementById("influx-v2-fields").hidden = !v2;
    document.getElementById("influx-v1-fields").hidden = v2;
    document.getElementById("influx-database-label").textContent = v2 ? "Bucket" : "Database";
  }

  /* Settings need admin, so the button only appears for users who have it. */
  function setupSettings() {
    var open = document.getElementById("settings-open");
    fetch("api/settings", { credentials: "same-origin", method: "GET" }).then(function (r) {
      if (!r.ok) return;
      open.hidden = false;
      open.addEventListener("click", openSettings);
      document.getElementById("settings-cancel").addEventListener("click", function () {
        document.getElementById("settings").close();
      });
      document.getElementById("settings-save").addEventListener("click", saveSettings);
      document.getElementById("rule-add").addEventListener("click", addRule);
      settingsForm().elements.InfluxVersion.addEventListener("change", applyInfluxVersion);
      document.getElementById("theme-select").addEventListener("change", function (event) {
        applyTheme(event.target.value);
        rebuildCharts();
      });
      refreshHealth();
      setInterval(refreshHealth, 10000);
    });
  }

  function escapeHtml(text) {
    return String(text).replace(/[&<>"]/g, function (c) {
      return { "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" }[c];
    });
  }

  function refreshAlerts() {
    return getJson("alerts")
      .then(function (payload) {
        var host = document.getElementById("alerts");
        var firing = (payload.rules || []).filter(function (r) { return r.firing; });
        if (!firing.length) {
          host.hidden = true;
          host.innerHTML = "";
          return;
        }
        host.hidden = false;
        host.innerHTML = firing
          .map(function (r) {
            var since = r.since ? new Date(r.since * 1000).toLocaleTimeString() : "";
            return (
              '<div class="alert"><span class="alert-name">' + escapeHtml(r.name) + "</span>" +
              '<span class="alert-detail">' + escapeHtml(r.metric) + " is " + fmtCount(r.value) +
              ", " + escapeHtml(r.op) + " " + fmtCount(r.threshold) +
              (since ? " since " + since : "") + "</span></div>"
            );
          })
          .join("");
      })
      .catch(function () {});
  }

  /* ---------------------------------------------------------- rule editor */

  var deletedRules = [];

  function metricUnit(id) {
    var found = meta.metrics.filter(function (m) { return m.id === id; })[0];
    return found ? found.unit : "";
  }

  function ruleRow(rule) {
    var row = document.createElement("div");
    row.className = "rule";
    row.dataset.id = rule.id;
    row.dataset.builtin = rule.builtin ? "yes" : "no";
    row.innerHTML =
      '<input type="text" data-field="name" value="' + escapeHtml(rule.name) + '" aria-label="Rule name" />' +
      '<input type="text" list="metric-options" data-field="metric" value="' + escapeHtml(rule.metric) +
      '" aria-label="Metric"' + (rule.builtin ? " readonly" : "") + " />" +
      '<select data-field="op" aria-label="Comparison">' +
      '<option value="above"' + (rule.op === "above" ? " selected" : "") + ">rises above</option>" +
      '<option value="below"' + (rule.op === "below" ? " selected" : "") + ">falls below</option>" +
      "</select>" +
      '<span class="rule-threshold"><input type="number" step="any" data-field="threshold" value="' +
      rule.threshold + '" aria-label="Threshold" /><small data-unit>' +
      escapeHtml(metricUnit(rule.metric)) + "</small></span>" +
      '<span class="rule-threshold"><input type="number" min="0" max="86400" data-field="duration" value="' +
      rule.duration + '" aria-label="Duration" /><small>s</small></span>' +
      '<label class="check"><input type="checkbox" data-field="enabled"' +
      (rule.enabled ? " checked" : "") + ' /><span>On</span></label>' +
      '<button type="button" class="ghost" data-remove' + (rule.builtin ? " disabled" : "") +
      ' title="' + (rule.builtin ? "Built-in rules cannot be deleted" : "Delete this rule") + '">&times;</button>';

    row.querySelector('[data-field="metric"]').addEventListener("change", function (event) {
      row.querySelector("[data-unit]").textContent = metricUnit(event.target.value);
    });
    row.querySelector("[data-remove]").addEventListener("click", function () {
      deletedRules.push(rule.id);
      row.remove();
    });
    return row;
  }

  function buildRuleList() {
    deletedRules = [];
    var options = document.getElementById("metric-options");
    options.innerHTML = meta.metrics
      .map(function (m) { return '<option value="' + escapeHtml(m.id) + '">' + escapeHtml(m.label) + "</option>"; })
      .join("");

    return getJson("alerts").then(function (payload) {
      var host = document.getElementById("rule-list");
      host.innerHTML = "";
      (payload.rules || []).forEach(function (rule) { host.appendChild(ruleRow(rule)); });
    });
  }

  function addRule() {
    var id = "custom_" + Date.now().toString(36);
    document.getElementById("rule-list").appendChild(
      ruleRow({ id: id, name: "", metric: "", op: "above", threshold: 0, duration: 60, enabled: true, builtin: false })
    );
  }

  function postRule(body) {
    return fetch("api/rules", {
      method: "POST",
      credentials: "same-origin",
      headers: { "Content-Type": "application/x-www-form-urlencoded" },
      body: body.toString()
    });
  }

  /* Posted one at a time: each request rewrites the whole rules file, so
   * overlapping writes would race for it. */
  function saveRules() {
    var work = deletedRules.map(function (id) {
      return function () { return postRule(new URLSearchParams({ action: "delete", id: id })); };
    });

    Array.prototype.forEach.call(document.querySelectorAll("#rule-list .rule"), function (row) {
      var field = function (name) { return row.querySelector('[data-field="' + name + '"]'); };
      if (!field("metric").value) return; /* A row the user added but never filled in. */

      var body = new URLSearchParams({
        action: "save",
        id: row.dataset.id,
        name: field("name").value || field("metric").value,
        metric: field("metric").value,
        op: field("op").value,
        threshold: field("threshold").value || "0",
        duration: field("duration").value || "0",
        enabled: field("enabled").checked ? "yes" : "no"
      });
      work.push(function () { return postRule(body); });
    });

    return work.reduce(function (chain, step) { return chain.then(step); }, Promise.resolve());
  }

  /* -------------------------------------------------------- top processes */

  function refreshProcesses() {
    return getJson("processes?limit=12")
      .then(function (payload) {
        var panel = document.getElementById("procs-panel");
        var rows = payload.processes || [];
        if (!rows.length) {
          panel.hidden = true;
          return;
        }
        panel.hidden = false;
        document.getElementById("procs-note").textContent =
          payload.total + " running  \u00b7  share of all " + payload.cores +
          " cores, averaged over " + payload.interval + "s";
        document.getElementById("procs-body").innerHTML = rows
          .map(function (p) {
            return "<tr><td>" + escapeHtml(p.name) + "</td><td>" + p.pid +
              '</td><td class="num">' + p.cpu.toFixed(1) + '%</td><td class="num">' +
              fmtBytes(p.rss) + "</td></tr>";
          })
          .join("");
      })
      .catch(function () {
        document.getElementById("procs-panel").hidden = true;
      });
  }

  /* ------------------------------------------------------------ lifecycle */

  function seriesIntervalMs() {
    if (window_s <= 1800) return 5000;
    if (window_s <= 3600) return 15000;
    return 60000;
  }

  function restartTimers() {
    if (currentTimer) clearInterval(currentTimer);
    if (seriesTimer) clearInterval(seriesTimer);
    if (!procsTimer) procsTimer = setInterval(refreshProcesses, 10000);
    currentTimer = setInterval(function () {
      refreshCurrent();
      refreshAlerts();
    }, 2000);
    seriesTimer = setInterval(refreshSeries, seriesIntervalMs());
  }

  function updateBanner() {
    var banner = document.getElementById("banner");
    var persisted = meta.store && meta.store.persisted;
    if (!persisted && window_s > 3600) {
      banner.textContent =
        "This history is held in memory only, so it resets when the app restarts or the device reboots. " +
        "Add an SD card or disk to keep long-range history across reboots.";
      banner.hidden = false;
    } else {
      banner.hidden = true;
    }
  }

  function selectRange(seconds, button) {
    window_s = seconds;
    Array.prototype.forEach.call(document.querySelectorAll("#ranges button"), function (b) {
      b.setAttribute("aria-pressed", String(b === button));
    });
    updateBanner();
    restartTimers();
    refreshSeries();
  }

  function buildRangeButtons() {
    var host = document.getElementById("ranges");
    RANGES.forEach(function (range) {
      var button = document.createElement("button");
      button.type = "button";
      button.textContent = range.label;
      button.setAttribute("aria-pressed", String(range.seconds === window_s));
      button.addEventListener("click", function () { selectRange(range.seconds, button); });
      host.appendChild(button);
    });
  }

  function renderIdentity() {
    var device = meta.device || {};
    document.getElementById("device-model").textContent = device.model || "Metrics";
    var parts = [];
    if (device.product) parts.push(device.product);
    if (device.firmware) parts.push("AXIS OS " + device.firmware);
    if (device.soc) parts.push(device.soc);
    if (device.serial) parts.push(device.serial);
    document.getElementById("device-detail").textContent = parts.join("  \u00b7  ");

    var fine = meta.store.tiers[0];
    document.getElementById("footer-text").textContent =
      meta.metrics.length + " metrics sampled every " + fine.interval + "s  \u00b7  " +
      (meta.store.bytes / 1048576).toFixed(1) + " MB of history  \u00b7  Metrics " + meta.app.version;
  }

  function start() {
    getJson("meta")
      .then(function (payload) {
        meta = payload;
        renderIdentity();
        buildRangeButtons();
        buildChartSpecs().forEach(function (spec) { charts.push(createChart(spec)); });
        resizeCharts();
        updateBanner();
        setupSettings();
        restartTimers();
        return Promise.all([refreshCurrent(), refreshSeries(), refreshAlerts(), refreshProcesses()]);
      })
      .catch(function (error) {
        setStatus(error.message, "error");
      });
  }

  window.addEventListener("resize", resizeCharts);
  applyTheme(currentTheme());
  start();
})();
