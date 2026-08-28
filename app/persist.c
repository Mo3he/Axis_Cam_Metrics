/*
 * Persistence for the coarse history tier.
 *
 * The file is a fixed-size circular buffer, so a new sample costs one record
 * write plus a header write rather than rewriting the whole history. At one
 * sample per five minutes that is a few hundred bytes every five minutes,
 * which an SD card or disk absorbs without complaint.
 *
 * It deliberately never lands on flash. /mnt/flash has ~144 MB free on a
 * recorder, and even this modest write rate is wear that belongs on removable
 * storage instead. If no card or disk is present, history stays in memory and
 * the UI says so.
 */

#include "persist.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <syslog.h>
#include <unistd.h>

#define PERSIST_MAGIC   "AXMETRIC"
#define PERSIST_VERSION 1u
#define HEADER_BYTES    4096u
#define APP_SUBDIR      "Metrics"
#define HISTORY_FILE    "history.bin"

typedef struct {
    char magic[8];
    guint32 version;
    guint32 n_metrics;
    guint32 capacity;
    guint32 interval_s;
    guint32 head;
    guint32 count;
    guint32 reserved;
} PersistHeader;

struct Persist {
    int fd;
    char path[512];
    guint n_metrics;
    guint capacity;
    guint interval_s;
    guint head;
    guint count;
    gsize records_offset;
    gsize record_bytes;
    float *record;
};

/* ------------------------------------------------------- storage discovery */

typedef struct {
    char mountpoint[256];
    guint64 total;
    gboolean preferred;
} Candidate;

static gboolean fstype_is_volatile(const char *fstype) {
    return strcmp(fstype, "tmpfs") == 0 || strcmp(fstype, "ramfs") == 0 ||
           strcmp(fstype, "devtmpfs") == 0 || strcmp(fstype, "vcrfs") == 0;
}

/* Each area is mounted several times. On the devices tested every mount exposes
 * the same subtree, so the paths are interchangeable, but "areas/<AREA>/root"
 * is the documented area root and is what the rest of the portfolio uses, so it
 * wins ties rather than leaving the choice to /proc/mounts ordering. */
static gboolean is_area_root(const char *mountpoint) {
    return g_str_has_prefix(mountpoint, "/var/spool/storage/areas/") &&
           g_str_has_suffix(mountpoint, "/root");
}

/* Picks the largest real filesystem that looks like edge storage. */
static gboolean find_storage(char *out, gsize out_len) {
    FILE *f = fopen("/proc/mounts", "re");
    if (!f)
        return FALSE;

    Candidate best = {{0}, 0, FALSE};
    char device[128], mountpoint[256], fstype[32], options[256];

    while (fscanf(f, "%127s %255s %31s %255s %*d %*d\n", device, mountpoint, fstype, options) == 4) {
        if (fstype_is_volatile(fstype))
            continue;
        if (g_str_has_prefix(options, "ro,") || strcmp(options, "ro") == 0)
            continue;
        if (g_str_has_prefix(mountpoint, "/var/volatile"))
            continue;
        if (!g_str_has_prefix(mountpoint, "/var/spool/storage/"))
            continue;

        struct statvfs st;
        if (statvfs(mountpoint, &st) != 0 || st.f_blocks == 0)
            continue;

        guint64 total = (guint64)st.f_blocks * st.f_frsize;
        gboolean preferred = is_area_root(mountpoint);
        gboolean better = total > best.total ||
                          (total == best.total && preferred && !best.preferred);
        if (better) {
            best.total = total;
            best.preferred = preferred;
            g_strlcpy(best.mountpoint, mountpoint, sizeof(best.mountpoint));
        }
    }
    fclose(f);

    if (best.total == 0)
        return FALSE;
    g_strlcpy(out, best.mountpoint, out_len);
    return TRUE;
}

/* ------------------------------------------------------------ file layout */

static gsize id_table_bytes(guint n_metrics) {
    return (gsize)n_metrics * METRIC_ID_MAX;
}

static gboolean read_at(int fd, void *buffer, gsize length, off_t offset) {
    gsize done = 0;
    while (done < length) {
        ssize_t got = pread(fd, (char *)buffer + done, length - done, offset + (off_t)done);
        if (got <= 0)
            return FALSE;
        done += (gsize)got;
    }
    return TRUE;
}

static gboolean write_at(int fd, const void *buffer, gsize length, off_t offset) {
    gsize done = 0;
    while (done < length) {
        ssize_t put = pwrite(fd, (const char *)buffer + done, length - done, offset + (off_t)done);
        if (put <= 0)
            return FALSE;
        done += (gsize)put;
    }
    return TRUE;
}

static void write_header(Persist *persist) {
    PersistHeader header;
    memset(&header, 0, sizeof(header));
    memcpy(header.magic, PERSIST_MAGIC, sizeof(header.magic));
    header.version = PERSIST_VERSION;
    header.n_metrics = persist->n_metrics;
    header.capacity = persist->capacity;
    header.interval_s = persist->interval_s;
    header.head = persist->head;
    header.count = persist->count;
    write_at(persist->fd, &header, sizeof(header), 0);
}

Persist *persist_open(const MetricRegistry *registry, const StoreTier *tier) {
    char base[256];
    if (!find_storage(base, sizeof(base))) {
        syslog(LOG_INFO, "no SD card or disk found, history stays in memory");
        return NULL;
    }

    char directory[512];
    g_snprintf(directory, sizeof(directory), "%s/%s", base, APP_SUBDIR);
    if (g_mkdir_with_parents(directory, 0750) != 0) {
        syslog(LOG_WARNING, "cannot create %s: %s, history stays in memory", directory, g_strerror(errno));
        return NULL;
    }

    Persist *persist = g_new0(Persist, 1);
    persist->n_metrics = registry->count;
    persist->capacity = tier->capacity;
    persist->interval_s = tier->interval_s;
    persist->record_bytes = sizeof(gint64) + (gsize)registry->count * sizeof(float);
    persist->records_offset = HEADER_BYTES + id_table_bytes(registry->count);
    persist->record = g_new0(float, registry->count);
    g_snprintf(persist->path, sizeof(persist->path), "%s/%s", directory, HISTORY_FILE);

    persist->fd = open(persist->path, O_RDWR | O_CREAT | O_CLOEXEC, 0640);
    if (persist->fd < 0) {
        syslog(LOG_WARNING, "cannot open %s: %s, history stays in memory", persist->path,
               g_strerror(errno));
        g_free(persist->record);
        g_free(persist);
        return NULL;
    }

    syslog(LOG_INFO, "history persisted to %s", persist->path);
    return persist;
}

void persist_close(Persist *persist) {
    if (!persist)
        return;
    if (persist->fd >= 0) {
        write_header(persist);
        close(persist->fd);
    }
    g_free(persist->record);
    g_free(persist);
}

const char *persist_path(const Persist *persist) {
    return persist ? persist->path : NULL;
}

/* ------------------------------------------------------------------ load */

static void initialise_file(Persist *persist, const MetricRegistry *registry) {
    persist->head = 0;
    persist->count = 0;

    char *table = g_malloc0(id_table_bytes(registry->count));
    for (guint i = 0; i < registry->count; i++)
        g_strlcpy(table + (gsize)i * METRIC_ID_MAX, registry->defs[i].id, METRIC_ID_MAX);
    write_at(persist->fd, table, id_table_bytes(registry->count), HEADER_BYTES);
    g_free(table);

    write_header(persist);
    if (ftruncate(persist->fd, (off_t)(persist->records_offset +
                                       (gsize)persist->capacity * persist->record_bytes)) != 0)
        syslog(LOG_WARNING, "cannot size %s: %s", persist->path, g_strerror(errno));
}

guint persist_load(Persist *persist, Store *store, guint tier_index) {
    if (!persist)
        return 0;

    PersistHeader header;
    if (!read_at(persist->fd, &header, sizeof(header), 0) ||
        memcmp(header.magic, PERSIST_MAGIC, sizeof(header.magic)) != 0 ||
        header.version != PERSIST_VERSION) {
        return 0; /* New or foreign file. */
    }

    if (header.interval_s != persist->interval_s || header.n_metrics == 0 ||
        header.n_metrics > 100000 || header.capacity == 0) {
        syslog(LOG_INFO, "history file layout changed, starting fresh");
        return 0;
    }

    /* The metric set can differ between runs, so samples are remapped by id
     * rather than by position. */
    gsize saved_table_bytes = id_table_bytes(header.n_metrics);
    char *saved_ids = g_malloc0(saved_table_bytes);
    if (!read_at(persist->fd, saved_ids, saved_table_bytes, HEADER_BYTES)) {
        g_free(saved_ids);
        return 0;
    }

    int *mapping = g_new(int, header.n_metrics);
    guint matched = 0;
    for (guint i = 0; i < header.n_metrics; i++) {
        mapping[i] = metrics_find(store_registry(store), saved_ids + (gsize)i * METRIC_ID_MAX);
        if (mapping[i] >= 0)
            matched++;
    }
    g_free(saved_ids);

    gsize saved_record_bytes = sizeof(gint64) + (gsize)header.n_metrics * sizeof(float);
    gsize saved_records_offset = HEADER_BYTES + saved_table_bytes;
    char *raw = g_malloc0(saved_record_bytes);
    float *values = g_new(float, store->n_metrics);

    guint restored = 0;
    guint count = MIN(header.count, header.capacity);
    guint start = (header.head + header.capacity - count) % header.capacity;

    for (guint i = 0; i < count; i++) {
        guint slot = (start + i) % header.capacity;
        off_t offset = (off_t)(saved_records_offset + (gsize)slot * saved_record_bytes);
        if (!read_at(persist->fd, raw, saved_record_bytes, offset))
            break;

        gint64 timestamp;
        memcpy(&timestamp, raw, sizeof(timestamp));
        if (timestamp <= 0)
            continue;

        for (guint m = 0; m < store->n_metrics; m++)
            values[m] = NAN;
        const float *saved_values = (const float *)(raw + sizeof(gint64));
        for (guint m = 0; m < header.n_metrics; m++) {
            if (mapping[m] >= 0)
                values[mapping[m]] = saved_values[m];
        }

        store_restore(store, tier_index, values, timestamp);
        restored++;
    }

    g_free(raw);
    g_free(values);
    g_free(mapping);

    if (restored) {
        syslog(LOG_INFO, "restored %u samples from %s, %u of %u metric ids still present", restored,
               persist->path, matched, header.n_metrics);
    }
    return restored;
}

void persist_sync(Persist *persist, Store *store, guint tier_index) {
    if (!persist)
        return;

    initialise_file(persist, store_registry(store));

    float *values = g_new(float, store->n_metrics);
    gint64 timestamp = 0;
    for (guint i = 0; store_tier_sample(store, tier_index, i, values, &timestamp); i++)
        persist_append(persist, values, timestamp);
    g_free(values);
}

/* ---------------------------------------------------------------- append */

void persist_append(Persist *persist, const float *values, gint64 timestamp) {
    if (!persist || persist->fd < 0)
        return;

    char *record = g_malloc(persist->record_bytes);
    memcpy(record, &timestamp, sizeof(timestamp));
    memcpy(record + sizeof(timestamp), values, (gsize)persist->n_metrics * sizeof(float));

    off_t offset = (off_t)(persist->records_offset + (gsize)persist->head * persist->record_bytes);
    if (write_at(persist->fd, record, persist->record_bytes, offset)) {
        persist->head = (persist->head + 1) % persist->capacity;
        if (persist->count < persist->capacity)
            persist->count++;
        write_header(persist);
    }
    g_free(record);
}
