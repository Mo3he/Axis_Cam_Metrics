/*
 * Persistence for the history tiers: one fixed-size circular file per tier, so a
 * sample costs one record write. The header index is only flushed every
 * HEADER_FLUSH_EVERY appends, so an unclean shutdown loses the last few samples.
 *
 * Never written to flash (~144 MB free on a recorder, and wear belongs on
 * removable storage); without a card or disk, history stays in memory.
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

#define PERSIST_MAGIC       "AXMETRIC"
#define PERSIST_VERSION     1u
#define HEADER_BYTES        4096u
#define APP_SUBDIR          "Metrics"
#define HEADER_FLUSH_EVERY  60u

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
    guint appends_since_header;
    /* Until persist_sync has laid the file out, head and count are still zero
     * and writing them would destroy the saved index. */
    gboolean owns_file;
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

/* Each area is mounted several times with the same subtree; prefer the
 * documented "areas/<AREA>/root" on ties rather than /proc/mounts order. */
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
    persist->appends_since_header = 0;
}

/* Renames path to "<path>.stale-<time>" so a fresh one can be created. */
static gboolean move_aside(const char *path) {
    GDateTime *now = g_date_time_new_now_local();
    gchar *stamp = g_date_time_format(now, "%Y%m%d%H%M%S");
    gchar *target = g_strdup_printf("%s.stale-%s", path, stamp);
    gboolean ok = rename(path, target) == 0;
    if (ok)
        syslog(LOG_WARNING, "%s belonged to another user (left by an earlier install), moved to %s",
               path, target);
    else
        syslog(LOG_WARNING, "cannot move unusable %s aside: %s", path, g_strerror(errno));
    g_free(target);
    g_free(stamp);
    g_date_time_unref(now);
    return ok;
}

/* AXIS OS gives each app a fresh uid on install and may hand an old one to
 * another app, so history left by an earlier install can belong to somebody
 * else. Same-directory renames only need write access to the storage root,
 * which the storage group has. */
static gboolean ensure_own_directory(const char *directory) {
    struct stat st;
    if (stat(directory, &st) == 0 && st.st_uid != geteuid() &&
        access(directory, R_OK | W_OK | X_OK) != 0) {
        if (!move_aside(directory))
            return FALSE;
    }
    if (g_mkdir_with_parents(directory, 0750) != 0) {
        syslog(LOG_WARNING, "cannot create %s: %s, history stays in memory", directory, g_strerror(errno));
        return FALSE;
    }
    return TRUE;
}

Persist *persist_open(const MetricRegistry *registry, const StoreTier *tier, const char *filename) {
    static gboolean absence_logged;

    char base[256];
    if (!find_storage(base, sizeof(base))) {
        /* Storage is retried on a timer, so this must not repeat every go. */
        if (!absence_logged) {
            syslog(LOG_INFO, "no SD card or disk mounted yet, history stays in memory for now");
            absence_logged = TRUE;
        }
        return NULL;
    }

    char directory[512];
    g_snprintf(directory, sizeof(directory), "%s/%s", base, APP_SUBDIR);
    if (!ensure_own_directory(directory))
        return NULL;
    return persist_open_in(registry, tier, directory, filename);
}

Persist *persist_open_in(const MetricRegistry *registry, const StoreTier *tier, const char *directory,
                         const char *filename) {
    Persist *persist = g_new0(Persist, 1);
    persist->n_metrics = registry->count;
    persist->capacity = tier->capacity;
    persist->interval_s = tier->interval_s;
    persist->record_bytes = sizeof(gint64) + (gsize)registry->count * sizeof(float);
    persist->records_offset = HEADER_BYTES + id_table_bytes(registry->count);
    persist->record = g_new0(float, registry->count);
    g_snprintf(persist->path, sizeof(persist->path), "%s/%s", directory, filename);

    persist->fd = open(persist->path, O_RDWR | O_CREAT | O_CLOEXEC, 0640);
    if (persist->fd < 0 && errno == EACCES && move_aside(persist->path))
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
        if (persist->owns_file)
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
    persist->owns_file = TRUE;

    char *table = g_malloc0(id_table_bytes(registry->count));
    for (guint i = 0; i < registry->count; i++)
        g_strlcpy(table + (gsize)i * METRIC_ID_MAX, registry->defs[i].id, METRIC_ID_MAX);
    write_at(persist->fd, table, id_table_bytes(registry->count), HEADER_BYTES);
    g_free(table);

    write_header(persist);
    /* Truncating to the records first zeroes them, so samples in an older
     * layout cannot be read back as records of this one. */
    if (ftruncate(persist->fd, (off_t)persist->records_offset) != 0 ||
        ftruncate(persist->fd, (off_t)(persist->records_offset +
                                       (gsize)persist->capacity * persist->record_bytes)) != 0)
        syslog(LOG_WARNING, "cannot size %s: %s", persist->path, g_strerror(errno));
}

/* Orders history slots by timestamp (each record starts with its gint64). */
static gint compare_slots(gconstpointer a, gconstpointer b) {
    gint64 x = *(const gint64 *)a, y = *(const gint64 *)b;
    return x < y ? -1 : x > y;
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

    /* The header index is flushed only every HEADER_FLUSH_EVERY appends, so
     * after an unclean stop it can be behind: in a full ring the slots after
     * it then hold the newest samples, not the oldest. Every written slot is
     * read and the samples restored in time order instead.
     *
     * A ring that is not full holds samples only up to the saved count plus
     * at most one flush interval. Files from before the records were zeroed
     * on layout changes can hold older-layout bytes after that, so those
     * unflushed slots are only taken while they continue the sequence. */
    typedef struct {
        gint64 timestamp;
        guint slot;
    } Slot;
    gboolean full = header.count >= header.capacity;
    guint scan = full ? header.capacity : MIN(header.capacity, header.count + HEADER_FLUSH_EVERY);
    gint64 latest = g_get_real_time() / G_USEC_PER_SEC + 86400;
    gint64 previous = 0;
    GArray *slots = g_array_sized_new(FALSE, FALSE, sizeof(Slot), scan);
    for (guint slot = 0; slot < scan; slot++) {
        gint64 timestamp;
        off_t offset = (off_t)(saved_records_offset + (gsize)slot * saved_record_bytes);
        if (!read_at(persist->fd, &timestamp, sizeof(timestamp), offset))
            break;
        if (!full && slot >= header.count &&
            (timestamp <= 0 || timestamp < previous || timestamp > latest))
            break;
        if (timestamp > 0) {
            Slot entry = {timestamp, slot};
            g_array_append_val(slots, entry);
        }
        previous = timestamp;
    }
    g_array_sort(slots, compare_slots);

    guint restored = 0;
    for (guint i = 0; i < slots->len; i++) {
        const Slot *entry = &g_array_index(slots, Slot, i);
        off_t offset = (off_t)(saved_records_offset + (gsize)entry->slot * saved_record_bytes);
        if (!read_at(persist->fd, raw, saved_record_bytes, offset))
            break;

        for (guint m = 0; m < store->n_metrics; m++)
            values[m] = NAN;
        const float *saved_values = (const float *)(raw + sizeof(gint64));
        for (guint m = 0; m < header.n_metrics; m++) {
            if (mapping[m] >= 0)
                values[mapping[m]] = saved_values[m];
        }

        store_restore(store, tier_index, values, entry->timestamp);
        restored++;
    }
    g_array_free(slots, TRUE);

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
        if (++persist->appends_since_header >= HEADER_FLUSH_EVERY)
            write_header(persist);
    }
    g_free(record);
}
