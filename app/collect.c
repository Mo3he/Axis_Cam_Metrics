/*
 * Reads metrics straight from /proc and /sys. Nothing here talks to VAPIX, so
 * a sample costs a handful of small reads and no round trips.
 *
 * Availability differs a lot between products (a camera has 6 thermal zones and
 * an SD card, a recorder has 5 zones, a SATA disk and 8 PoE port VLANs), so
 * everything is discovered at startup rather than assumed.
 */

#include "collect.h"

#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/statvfs.h>

#define MAX_CORES  32
#define MAX_IFACES 32
#define MAX_DISKS  24
#define MAX_MOUNTS 16
#define MAX_ZONES  16

typedef struct {
    guint64 total;
    guint64 idle;
} CpuTimes;

typedef struct {
    char name[32];
    guint idx_rx_bps, idx_tx_bps, idx_rx_pps, idx_tx_pps;
    guint idx_rx_err, idx_tx_err, idx_rx_drop, idx_tx_drop;
    guint idx_link, idx_up;
    guint64 prev_rx_bytes, prev_tx_bytes, prev_rx_pkts, prev_tx_pkts;
} Iface;

typedef struct {
    char name[32];
    guint idx_read_bps, idx_write_bps, idx_read_iops, idx_write_iops, idx_util;
    guint64 prev_read_sectors, prev_write_sectors;
    guint64 prev_reads, prev_writes, prev_io_ms;
} Disk;

typedef struct {
    char mountpoint[128];
    guint idx_total, idx_used, idx_free, idx_usage;
} Mount;

typedef struct {
    char path[128];
    guint idx;
} Zone;

struct Collector {
    MetricRegistry *registry;

    guint n_cores;
    guint idx_cpu_usage, idx_cpu_user, idx_cpu_system, idx_cpu_iowait;
    guint idx_cpu_irq, idx_cpu_steal, idx_cpu_freq;
    guint idx_core_usage[MAX_CORES];
    guint idx_core_freq[MAX_CORES];
    CpuTimes prev_total;
    CpuTimes prev_core[MAX_CORES];
    guint64 prev_cpu_field[8]; /* user, nice, system, idle, iowait, irq, softirq, steal */

    guint idx_load1, idx_load5, idx_load15;
    guint idx_mem_total, idx_mem_used, idx_mem_free, idx_mem_available;
    guint idx_mem_buffers, idx_mem_cached, idx_mem_usage;
    guint idx_swap_total, idx_swap_used, idx_swap_usage;
    guint idx_uptime, idx_procs, idx_procs_running, idx_ctxt;
    guint idx_sockets, idx_tcp_inuse, idx_udp_inuse;
    guint64 prev_ctxt;

    Iface ifaces[MAX_IFACES];
    guint n_ifaces;
    Disk disks[MAX_DISKS];
    guint n_disks;
    Mount mounts[MAX_MOUNTS];
    guint n_mounts;
    Zone zones[MAX_ZONES];
    guint n_zones;

    gint64 prev_us;
};

/* ------------------------------------------------------------------ helpers */

static gboolean read_text(const char *path, char *buf, gsize len) {
    FILE *f = fopen(path, "re");
    if (!f)
        return FALSE;
    gsize n = fread(buf, 1, len - 1, f);
    fclose(f);
    buf[n] = '\0';
    return n > 0;
}

static double read_double(const char *path, double fallback) {
    char buf[64];
    if (!read_text(path, buf, sizeof(buf)))
        return fallback;
    return g_ascii_strtod(buf, NULL);
}

/* Metric ids use '.' as a separator, so device names must not contain one. */
static void sanitize(const char *in, char *out, gsize len) {
    gsize j = 0;
    for (gsize i = 0; in[i] && j + 1 < len; i++) {
        char c = in[i];
        out[j++] = (g_ascii_isalnum(c) || c == '_') ? c : '_';
    }
    out[j] = '\0';
}

static guint reg(Collector *c, const char *id, const char *label, const char *unit, const char *group) {
    return metrics_register(c->registry, id, label, unit, group);
}

static void set(float *values, guint idx, double v) {
    values[idx] = (float)v;
}

static double rate(guint64 now, guint64 prev, double seconds) {
    if (seconds <= 0 || now < prev)
        return NAN; /* Counter wrapped or reset. */
    return (double)(now - prev) / seconds;
}

/* --------------------------------------------------------------- discovery */

static void discover_cpu(Collector *c) {
    c->idx_cpu_usage = reg(c, "cpu.usage", "CPU usage", "%", "cpu");
    c->idx_cpu_user = reg(c, "cpu.user", "CPU user", "%", "cpu");
    c->idx_cpu_system = reg(c, "cpu.system", "CPU system", "%", "cpu");
    c->idx_cpu_iowait = reg(c, "cpu.iowait", "CPU iowait", "%", "cpu");
    c->idx_cpu_irq = reg(c, "cpu.irq", "CPU irq", "%", "cpu");
    c->idx_cpu_steal = reg(c, "cpu.steal", "CPU steal", "%", "cpu");
    c->idx_cpu_freq = reg(c, "cpu.freq", "CPU frequency", "MHz", "cpu");

    FILE *f = fopen("/proc/stat", "re");
    if (!f)
        return;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        guint core;
        if (sscanf(line, "cpu%u ", &core) != 1 || core >= MAX_CORES)
            continue;
        char id[METRIC_ID_MAX], label[METRIC_LABEL_MAX];
        g_snprintf(id, sizeof(id), "cpu.core%u.usage", core);
        g_snprintf(label, sizeof(label), "Core %u usage", core);
        c->idx_core_usage[core] = reg(c, id, label, "%", "cpu");
        g_snprintf(id, sizeof(id), "cpu.core%u.freq", core);
        g_snprintf(label, sizeof(label), "Core %u frequency", core);
        c->idx_core_freq[core] = reg(c, id, label, "MHz", "cpu");
        if (core + 1 > c->n_cores)
            c->n_cores = core + 1;
    }
    fclose(f);
}

static void discover_system(Collector *c) {
    c->idx_load1 = reg(c, "load.1", "Load average 1m", "", "system");
    c->idx_load5 = reg(c, "load.5", "Load average 5m", "", "system");
    c->idx_load15 = reg(c, "load.15", "Load average 15m", "", "system");
    c->idx_uptime = reg(c, "sys.uptime", "Uptime", "s", "system");
    c->idx_procs = reg(c, "sys.processes", "Processes", "", "system");
    c->idx_procs_running = reg(c, "sys.processes_running", "Runnable processes", "", "system");
    c->idx_ctxt = reg(c, "sys.context_switches", "Context switches", "/s", "system");
    c->idx_sockets = reg(c, "sys.sockets", "Sockets in use", "", "system");
    c->idx_tcp_inuse = reg(c, "sys.tcp_inuse", "TCP sockets", "", "system");
    c->idx_udp_inuse = reg(c, "sys.udp_inuse", "UDP sockets", "", "system");

    c->idx_mem_total = reg(c, "mem.total", "Memory total", "B", "memory");
    c->idx_mem_used = reg(c, "mem.used", "Memory used", "B", "memory");
    c->idx_mem_free = reg(c, "mem.free", "Memory free", "B", "memory");
    c->idx_mem_available = reg(c, "mem.available", "Memory available", "B", "memory");
    c->idx_mem_buffers = reg(c, "mem.buffers", "Buffers", "B", "memory");
    c->idx_mem_cached = reg(c, "mem.cached", "Cached", "B", "memory");
    c->idx_mem_usage = reg(c, "mem.usage", "Memory usage", "%", "memory");
    c->idx_swap_total = reg(c, "mem.swap_total", "Swap total", "B", "memory");
    c->idx_swap_used = reg(c, "mem.swap_used", "Swap used", "B", "memory");
    c->idx_swap_usage = reg(c, "mem.swap_usage", "Swap usage", "%", "memory");
}

static void discover_zones(Collector *c) {
    for (guint i = 0; i < MAX_ZONES; i++) {
        char path[160], type[64];
        g_snprintf(path, sizeof(path), "/sys/class/thermal/thermal_zone%u/type", i);
        if (!read_text(path, type, sizeof(type)))
            break;
        g_strstrip(type);

        char safe[64], id[METRIC_ID_MAX], label[METRIC_LABEL_MAX];
        sanitize(type, safe, sizeof(safe));
        g_snprintf(id, sizeof(id), "temp.%s", safe);
        g_snprintf(label, sizeof(label), "%s temperature", type);

        Zone *zone = &c->zones[c->n_zones++];
        g_snprintf(zone->path, sizeof(zone->path),
                   "/sys/class/thermal/thermal_zone%u/temp", i);
        zone->idx = reg(c, id, label, "C", "temperature");
    }
}

static void discover_ifaces(Collector *c) {
    FILE *f = fopen("/proc/net/dev", "re");
    if (!f)
        return;
    char line[512];
    (void)!fgets(line, sizeof(line), f);
    (void)!fgets(line, sizeof(line), f);
    while (fgets(line, sizeof(line), f) && c->n_ifaces < MAX_IFACES) {
        char *colon = strchr(line, ':');
        if (!colon)
            continue;
        *colon = '\0';
        char *name = g_strstrip(line);
        if (strcmp(name, "lo") == 0)
            continue;

        Iface *iface = &c->ifaces[c->n_ifaces++];
        g_strlcpy(iface->name, name, sizeof(iface->name));

        char safe[32], id[METRIC_ID_MAX], label[METRIC_LABEL_MAX];
        sanitize(name, safe, sizeof(safe));

#define IFACE_METRIC(field, suffix, text, unit)                                  \
    g_snprintf(id, sizeof(id), "net.%s." suffix, safe);                          \
    g_snprintf(label, sizeof(label), "%s " text, name);                          \
    iface->field = reg(c, id, label, unit, "network")

        IFACE_METRIC(idx_rx_bps, "rx_bps", "receive", "B/s");
        IFACE_METRIC(idx_tx_bps, "tx_bps", "transmit", "B/s");
        IFACE_METRIC(idx_rx_pps, "rx_pps", "receive packets", "/s");
        IFACE_METRIC(idx_tx_pps, "tx_pps", "transmit packets", "/s");
        IFACE_METRIC(idx_rx_err, "rx_errors", "receive errors", "/s");
        IFACE_METRIC(idx_tx_err, "tx_errors", "transmit errors", "/s");
        IFACE_METRIC(idx_rx_drop, "rx_drops", "receive drops", "/s");
        IFACE_METRIC(idx_tx_drop, "tx_drops", "transmit drops", "/s");
        IFACE_METRIC(idx_link, "link", "link speed", "Mb/s");
        IFACE_METRIC(idx_up, "up", "link up", "");
#undef IFACE_METRIC
    }
    fclose(f);
}

static void discover_disks(Collector *c) {
    FILE *f = fopen("/proc/diskstats", "re");
    if (!f)
        return;
    char line[512];
    while (fgets(line, sizeof(line), f) && c->n_disks < MAX_DISKS) {
        char name[32];
        if (sscanf(line, " %*u %*u %31s", name) != 1)
            continue;
        if (g_str_has_prefix(name, "loop") || g_str_has_prefix(name, "ram"))
            continue;

        /* Whole devices only: partitions have no /sys/block entry. */
        char sys_path[128];
        g_snprintf(sys_path, sizeof(sys_path), "/sys/block/%s", name);
        if (!g_file_test(sys_path, G_FILE_TEST_IS_DIR))
            continue;

        Disk *disk = &c->disks[c->n_disks++];
        g_strlcpy(disk->name, name, sizeof(disk->name));

        char safe[32], id[METRIC_ID_MAX], label[METRIC_LABEL_MAX];
        sanitize(name, safe, sizeof(safe));

#define DISK_METRIC(field, suffix, text, unit)                                   \
    g_snprintf(id, sizeof(id), "disk.%s." suffix, safe);                         \
    g_snprintf(label, sizeof(label), "%s " text, name);                          \
    disk->field = reg(c, id, label, unit, "disk")

        DISK_METRIC(idx_read_bps, "read_bps", "read", "B/s");
        DISK_METRIC(idx_write_bps, "write_bps", "write", "B/s");
        DISK_METRIC(idx_read_iops, "read_iops", "read ops", "/s");
        DISK_METRIC(idx_write_iops, "write_iops", "write ops", "/s");
        DISK_METRIC(idx_util, "util", "utilisation", "%");
#undef DISK_METRIC
    }
    fclose(f);
}

/* Each storage area is mounted several times (plain, areas/<x>/root and the
 * /var/volatile duplicates). Keep the first, shortest, non-volatile one.
 * Read-only mounts are skipped: the root filesystem sits at 100% by design on
 * every Axis device, so charting it is noise. */
static gboolean mount_is_interesting(const char *device,
                                     const char *mountpoint,
                                     const char *fstype,
                                     const char *options) {
    static const char *keep[] = {"ext2", "ext3", "ext4", "xfs", "f2fs", "vfat",
                                 "exfat", "ubifs", "cifs", "nfs", NULL};
    if (!g_str_has_prefix(device, "/dev/") && strcmp(fstype, "cifs") != 0 && strcmp(fstype, "nfs") != 0)
        return FALSE;
    if (g_str_has_prefix(mountpoint, "/var/volatile"))
        return FALSE;
    if (g_str_has_prefix(options, "ro,") || strcmp(options, "ro") == 0)
        return FALSE;
    for (int i = 0; keep[i]; i++) {
        if (strcmp(fstype, keep[i]) == 0)
            return TRUE;
    }
    return FALSE;
}

/* Storage areas are what users recognise, so "/var/spool/storage/SD_DISK"
 * becomes "SD_DISK" rather than the whole path. */
static const char *mount_short_name(const char *mountpoint) {
    static const char *prefixes[] = {"/var/spool/storage/areas/", "/var/spool/storage/", NULL};
    if (strcmp(mountpoint, "/") == 0)
        return "root";
    for (int i = 0; prefixes[i]; i++) {
        if (g_str_has_prefix(mountpoint, prefixes[i]))
            return mountpoint + strlen(prefixes[i]);
    }
    return mountpoint + 1;
}

static void discover_mounts(Collector *c) {
    FILE *f = fopen("/proc/mounts", "re");
    if (!f)
        return;
    char device[128], mountpoint[256], fstype[32], options[256];
    char seen[MAX_MOUNTS][128];
    guint n_seen = 0;

    while (fscanf(f, "%127s %255s %31s %255s %*d %*d\n", device, mountpoint, fstype, options) == 4 &&
           c->n_mounts < MAX_MOUNTS) {
        if (!mount_is_interesting(device, mountpoint, fstype, options))
            continue;

        gboolean duplicate = FALSE;
        for (guint i = 0; i < n_seen; i++) {
            if (strcmp(seen[i], device) == 0)
                duplicate = TRUE;
        }
        if (duplicate)
            continue;
        g_strlcpy(seen[n_seen++], device, sizeof(seen[0]));

        Mount *mount = &c->mounts[c->n_mounts++];
        g_strlcpy(mount->mountpoint, mountpoint, sizeof(mount->mountpoint));

        char safe[128], id[METRIC_ID_MAX], label[METRIC_LABEL_MAX];
        sanitize(mount_short_name(mountpoint), safe, sizeof(safe));

#define FS_METRIC(field, suffix, text, unit)                                     \
    g_snprintf(id, sizeof(id), "fs.%s." suffix, safe);                           \
    g_snprintf(label, sizeof(label), "%s " text, mountpoint);                    \
    mount->field = reg(c, id, label, unit, "storage")

        FS_METRIC(idx_total, "total", "total", "B");
        FS_METRIC(idx_used, "used", "used", "B");
        FS_METRIC(idx_free, "free", "free", "B");
        FS_METRIC(idx_usage, "usage", "usage", "%");
#undef FS_METRIC
    }
    fclose(f);
}

Collector *collector_new(MetricRegistry *registry) {
    Collector *c = g_new0(Collector, 1);
    c->registry = registry;
    discover_cpu(c);
    discover_system(c);
    discover_zones(c);
    discover_ifaces(c);
    discover_disks(c);
    discover_mounts(c);
    return c;
}

void collector_free(Collector *collector) {
    g_free(collector);
}

/* ---------------------------------------------------------------- sampling */

static void sample_cpu(Collector *c, float *values, double seconds, gboolean first) {
    FILE *f = fopen("/proc/stat", "re");
    if (!f)
        return;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        if (g_str_has_prefix(line, "ctxt ")) {
            guint64 ctxt = g_ascii_strtoull(line + 5, NULL, 10);
            if (!first)
                set(values, c->idx_ctxt, rate(ctxt, c->prev_ctxt, seconds));
            c->prev_ctxt = ctxt;
            continue;
        }
        if (g_str_has_prefix(line, "procs_running ")) {
            set(values, c->idx_procs_running, g_ascii_strtoull(line + 14, NULL, 10));
            continue;
        }
        if (!g_str_has_prefix(line, "cpu"))
            continue;

        guint64 v[8] = {0};
        guint core = 0;
        gboolean aggregate = (line[3] == ' ');
        const char *rest;
        if (aggregate) {
            rest = line + 4;
        } else {
            if (sscanf(line, "cpu%u ", &core) != 1 || core >= c->n_cores)
                continue;
            rest = strchr(line, ' ');
            if (!rest)
                continue;
        }
        sscanf(rest, "%" G_GUINT64_FORMAT " %" G_GUINT64_FORMAT " %" G_GUINT64_FORMAT
                     " %" G_GUINT64_FORMAT " %" G_GUINT64_FORMAT " %" G_GUINT64_FORMAT
                     " %" G_GUINT64_FORMAT " %" G_GUINT64_FORMAT,
               &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]);

        guint64 total = 0;
        for (int i = 0; i < 8; i++)
            total += v[i];
        guint64 idle = v[3] + v[4];

        CpuTimes *prev = aggregate ? &c->prev_total : &c->prev_core[core];
        guint64 dt = total - prev->total;
        guint64 didle = idle - prev->idle;
        double usage = (!first && total > prev->total && dt >= didle)
                           ? 100.0 * (double)(dt - didle) / (double)dt
                           : NAN;

        if (aggregate) {
            set(values, c->idx_cpu_usage, usage);
            if (!first && dt > 0) {
                double scale = 100.0 / (double)dt;
                set(values, c->idx_cpu_user, (double)(v[0] + v[1] - c->prev_cpu_field[0] - c->prev_cpu_field[1]) * scale);
                set(values, c->idx_cpu_system, (double)(v[2] + v[6] - c->prev_cpu_field[2] - c->prev_cpu_field[6]) * scale);
                set(values, c->idx_cpu_iowait, (double)(v[4] - c->prev_cpu_field[4]) * scale);
                set(values, c->idx_cpu_irq, (double)(v[5] - c->prev_cpu_field[5]) * scale);
                set(values, c->idx_cpu_steal, (double)(v[7] - c->prev_cpu_field[7]) * scale);
            }
            memcpy(c->prev_cpu_field, v, sizeof(v));
        } else {
            set(values, c->idx_core_usage[core], usage);
        }
        prev->total = total;
        prev->idle = idle;
    }
    fclose(f);

    double freq_sum = 0;
    guint freq_n = 0;
    for (guint core = 0; core < c->n_cores; core++) {
        char path[128];
        g_snprintf(path, sizeof(path),
                   "/sys/devices/system/cpu/cpu%u/cpufreq/scaling_cur_freq", core);
        double khz = read_double(path, NAN);
        if (!isnan(khz)) {
            set(values, c->idx_core_freq[core], khz / 1000.0);
            freq_sum += khz / 1000.0;
            freq_n++;
        }
    }
    set(values, c->idx_cpu_freq, freq_n ? freq_sum / freq_n : NAN);
}

static void sample_memory(Collector *c, float *values) {
    FILE *f = fopen("/proc/meminfo", "re");
    if (!f)
        return;
    char line[128];
    guint64 total = 0, available = 0, free_kb = 0, buffers = 0, cached = 0;
    guint64 swap_total = 0, swap_free = 0;
    while (fgets(line, sizeof(line), f)) {
        guint64 v;
        if (sscanf(line, "MemTotal: %" G_GUINT64_FORMAT " kB", &v) == 1) total = v;
        else if (sscanf(line, "MemFree: %" G_GUINT64_FORMAT " kB", &v) == 1) free_kb = v;
        else if (sscanf(line, "MemAvailable: %" G_GUINT64_FORMAT " kB", &v) == 1) available = v;
        else if (sscanf(line, "Buffers: %" G_GUINT64_FORMAT " kB", &v) == 1) buffers = v;
        else if (sscanf(line, "Cached: %" G_GUINT64_FORMAT " kB", &v) == 1) cached = v;
        else if (sscanf(line, "SwapTotal: %" G_GUINT64_FORMAT " kB", &v) == 1) swap_total = v;
        else if (sscanf(line, "SwapFree: %" G_GUINT64_FORMAT " kB", &v) == 1) swap_free = v;
    }
    fclose(f);

    set(values, c->idx_mem_total, (double)total * 1024.0);
    set(values, c->idx_mem_free, (double)free_kb * 1024.0);
    set(values, c->idx_mem_available, (double)available * 1024.0);
    set(values, c->idx_mem_buffers, (double)buffers * 1024.0);
    set(values, c->idx_mem_cached, (double)cached * 1024.0);
    set(values, c->idx_mem_used, (double)(total - available) * 1024.0);
    set(values, c->idx_mem_usage, total ? 100.0 * (double)(total - available) / (double)total : NAN);
    set(values, c->idx_swap_total, (double)swap_total * 1024.0);
    set(values, c->idx_swap_used, (double)(swap_total - swap_free) * 1024.0);
    set(values, c->idx_swap_usage,
        swap_total ? 100.0 * (double)(swap_total - swap_free) / (double)swap_total : NAN);
}

static void sample_system(Collector *c, float *values) {
    char buf[256];
    if (read_text("/proc/loadavg", buf, sizeof(buf))) {
        double l1 = 0, l5 = 0, l15 = 0;
        guint procs = 0;
        sscanf(buf, "%lf %lf %lf %*u/%u", &l1, &l5, &l15, &procs);
        set(values, c->idx_load1, l1);
        set(values, c->idx_load5, l5);
        set(values, c->idx_load15, l15);
        set(values, c->idx_procs, procs);
    }
    set(values, c->idx_uptime, read_double("/proc/uptime", NAN));

    FILE *f = fopen("/proc/net/sockstat", "re");
    if (f) {
        char line[128];
        while (fgets(line, sizeof(line), f)) {
            guint v;
            if (sscanf(line, "sockets: used %u", &v) == 1)
                set(values, c->idx_sockets, v);
            else if (sscanf(line, "TCP: inuse %u", &v) == 1)
                set(values, c->idx_tcp_inuse, v);
            else if (sscanf(line, "UDP: inuse %u", &v) == 1)
                set(values, c->idx_udp_inuse, v);
        }
        fclose(f);
    }
}

static void sample_zones(Collector *c, float *values) {
    for (guint i = 0; i < c->n_zones; i++)
        set(values, c->zones[i].idx, read_double(c->zones[i].path, NAN) / 1000.0);
}

static void sample_ifaces(Collector *c, float *values, double seconds, gboolean first) {
    FILE *f = fopen("/proc/net/dev", "re");
    if (!f)
        return;
    char line[512];
    (void)!fgets(line, sizeof(line), f);
    (void)!fgets(line, sizeof(line), f);
    while (fgets(line, sizeof(line), f)) {
        char *colon = strchr(line, ':');
        if (!colon)
            continue;
        *colon = '\0';
        char *name = g_strstrip(line);

        Iface *iface = NULL;
        for (guint i = 0; i < c->n_ifaces; i++) {
            if (strcmp(c->ifaces[i].name, name) == 0)
                iface = &c->ifaces[i];
        }
        if (!iface)
            continue;

        guint64 rxb = 0, rxp = 0, rxe = 0, rxd = 0, txb = 0, txp = 0, txe = 0, txd = 0;
        sscanf(colon + 1,
               "%" G_GUINT64_FORMAT " %" G_GUINT64_FORMAT " %" G_GUINT64_FORMAT
               " %" G_GUINT64_FORMAT " %*u %*u %*u %*u"
               " %" G_GUINT64_FORMAT " %" G_GUINT64_FORMAT " %" G_GUINT64_FORMAT
               " %" G_GUINT64_FORMAT,
               &rxb, &rxp, &rxe, &rxd, &txb, &txp, &txe, &txd);

        if (!first) {
            set(values, iface->idx_rx_bps, rate(rxb, iface->prev_rx_bytes, seconds));
            set(values, iface->idx_tx_bps, rate(txb, iface->prev_tx_bytes, seconds));
            set(values, iface->idx_rx_pps, rate(rxp, iface->prev_rx_pkts, seconds));
            set(values, iface->idx_tx_pps, rate(txp, iface->prev_tx_pkts, seconds));
        }
        iface->prev_rx_bytes = rxb;
        iface->prev_tx_bytes = txb;
        iface->prev_rx_pkts = rxp;
        iface->prev_tx_pkts = txp;

        /* Error and drop counters are reported as absolute totals: they are rare
         * enough that the running total is more useful than a rate. */
        set(values, iface->idx_rx_err, (double)rxe);
        set(values, iface->idx_tx_err, (double)txe);
        set(values, iface->idx_rx_drop, (double)rxd);
        set(values, iface->idx_tx_drop, (double)txd);

        char path[160], state[32];
        g_snprintf(path, sizeof(path), "/sys/class/net/%s/speed", iface->name);
        double speed = read_double(path, NAN);
        set(values, iface->idx_link, speed < 0 ? NAN : speed);

        g_snprintf(path, sizeof(path), "/sys/class/net/%s/operstate", iface->name);
        if (read_text(path, state, sizeof(state))) {
            g_strstrip(state);
            set(values, iface->idx_up, strcmp(state, "up") == 0 ? 1 : 0);
        }
    }
    fclose(f);
}

static void sample_disks(Collector *c, float *values, double seconds, gboolean first) {
    FILE *f = fopen("/proc/diskstats", "re");
    if (!f)
        return;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char name[32];
        guint64 reads = 0, read_sectors = 0, writes = 0, write_sectors = 0, io_ms = 0;
        if (sscanf(line,
                   " %*u %*u %31s %" G_GUINT64_FORMAT " %*u %" G_GUINT64_FORMAT
                   " %*u %" G_GUINT64_FORMAT " %*u %" G_GUINT64_FORMAT
                   " %*u %*u %" G_GUINT64_FORMAT,
                   name, &reads, &read_sectors, &writes, &write_sectors, &io_ms) != 6)
            continue;

        Disk *disk = NULL;
        for (guint i = 0; i < c->n_disks; i++) {
            if (strcmp(c->disks[i].name, name) == 0)
                disk = &c->disks[i];
        }
        if (!disk)
            continue;

        if (!first) {
            set(values, disk->idx_read_bps, rate(read_sectors, disk->prev_read_sectors, seconds) * 512.0);
            set(values, disk->idx_write_bps, rate(write_sectors, disk->prev_write_sectors, seconds) * 512.0);
            set(values, disk->idx_read_iops, rate(reads, disk->prev_reads, seconds));
            set(values, disk->idx_write_iops, rate(writes, disk->prev_writes, seconds));
            double busy_ms = rate(io_ms, disk->prev_io_ms, 1.0);
            set(values, disk->idx_util, isnan(busy_ms) ? NAN : MIN(100.0, busy_ms / (seconds * 10.0)));
        }
        disk->prev_read_sectors = read_sectors;
        disk->prev_write_sectors = write_sectors;
        disk->prev_reads = reads;
        disk->prev_writes = writes;
        disk->prev_io_ms = io_ms;
    }
    fclose(f);
}

static void sample_mounts(Collector *c, float *values) {
    for (guint i = 0; i < c->n_mounts; i++) {
        Mount *mount = &c->mounts[i];
        struct statvfs st;
        if (statvfs(mount->mountpoint, &st) != 0 || st.f_blocks == 0) {
            set(values, mount->idx_total, NAN);
            set(values, mount->idx_used, NAN);
            set(values, mount->idx_free, NAN);
            set(values, mount->idx_usage, NAN);
            continue;
        }
        double total = (double)st.f_blocks * (double)st.f_frsize;
        double avail = (double)st.f_bavail * (double)st.f_frsize;
        double used = total - (double)st.f_bfree * (double)st.f_frsize;
        set(values, mount->idx_total, total);
        set(values, mount->idx_used, used);
        set(values, mount->idx_free, avail);
        set(values, mount->idx_usage, total > 0 ? 100.0 * used / total : NAN);
    }
}

void collector_sample(Collector *c, float *values) {
    gint64 now = g_get_monotonic_time();
    gboolean first = (c->prev_us == 0);
    double seconds = first ? 0 : (double)(now - c->prev_us) / 1e6;

    for (guint i = 0; i < c->registry->count; i++)
        values[i] = NAN;

    sample_cpu(c, values, seconds, first);
    sample_memory(c, values);
    sample_system(c, values);
    sample_zones(c, values);
    sample_ifaces(c, values, seconds, first);
    sample_disks(c, values, seconds, first);
    sample_mounts(c, values);

    c->prev_us = now;
}
