/*
 * Per-process CPU and memory, for answering "what is using the camera".
 *
 * CPU percentage only exists as a difference between two readings, so the
 * table is refreshed on a fixed cadence and each entry reports its share of the
 * whole device over that interval. That matches cpu.usage, which is also an
 * average across cores: a process cannot read 53% on a box the dashboard calls
 * 27% busy. A process that appeared since the last scan has no baseline and
 * reports zero rather than a misleading spike.
 */

#include "procs.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PROCS_INTERVAL_S 10
#define PROCS_MAX        512

typedef struct {
    int pid;
    char name[24];
    gulong jiffies;   /* utime + stime. */
    gsize rss_bytes;
    double cpu_percent;
    gboolean seen;
} Proc;

struct Procs {
    Proc entries[PROCS_MAX];
    guint count;
    gint64 last_scan;
    long ticks_per_second;
    long page_size;
    guint cores;
};

Procs *procs_new(void) {
    Procs *procs = g_new0(Procs, 1);
    procs->ticks_per_second = sysconf(_SC_CLK_TCK);
    if (procs->ticks_per_second <= 0)
        procs->ticks_per_second = 100;
    procs->page_size = sysconf(_SC_PAGESIZE);
    if (procs->page_size <= 0)
        procs->page_size = 4096;
    long cores = sysconf(_SC_NPROCESSORS_ONLN);
    procs->cores = cores > 0 ? (guint)cores : 1;
    return procs;
}

void procs_free(Procs *procs) {
    g_free(procs);
}

static Proc *find(Procs *procs, int pid) {
    for (guint i = 0; i < procs->count; i++) {
        if (procs->entries[i].pid == pid)
            return &procs->entries[i];
    }
    return NULL;
}

/* /proc/<pid>/stat cannot be split on spaces because the second field is a
 * command name in parentheses that may itself contain spaces and brackets. The
 * numeric fields all follow the last ')'. */
static gboolean read_stat(const char *path, char *name, gsize name_len, gulong *jiffies,
                          gulong *rss_pages) {
    gchar *contents = NULL;
    if (!g_file_get_contents(path, &contents, NULL, NULL))
        return FALSE;

    char *open_paren = strchr(contents, '(');
    char *close_paren = strrchr(contents, ')');
    if (!open_paren || !close_paren || close_paren < open_paren) {
        g_free(contents);
        return FALSE;
    }

    gsize length = (gsize)(close_paren - open_paren - 1);
    if (length >= name_len)
        length = name_len - 1;
    memcpy(name, open_paren + 1, length);
    name[length] = '\0';

    gulong utime = 0, stime = 0, rss = 0;
    /* Counting from the state in field 3: utime and stime are 14 and 15, rss is
     * 24. Skipped fields are matched as plain tokens because gcc rejects a
     * length modifier on a suppressed conversion. */
    int matched = sscanf(close_paren + 2,
                         "%*s %*s %*s %*s %*s %*s %*s %*s %*s %*s %*s %lu %lu "
                         "%*s %*s %*s %*s %*s %*s %*s %*s %lu",
                         &utime, &stime, &rss);
    g_free(contents);
    if (matched != 3)
        return FALSE;

    *jiffies = utime + stime;
    *rss_pages = rss;
    return TRUE;
}

static void scan(Procs *procs, double elapsed_s) {
    for (guint i = 0; i < procs->count; i++)
        procs->entries[i].seen = FALSE;

    GDir *dir = g_dir_open("/proc", 0, NULL);
    if (!dir)
        return;

    const char *entry;
    while ((entry = g_dir_read_name(dir))) {
        if (!g_ascii_isdigit(entry[0]))
            continue;

        int pid = atoi(entry);
        char path[64];
        char name[24];
        gulong jiffies = 0, rss_pages = 0;
        g_snprintf(path, sizeof(path), "/proc/%d/stat", pid);
        if (!read_stat(path, name, sizeof(name), &jiffies, &rss_pages))
            continue;

        Proc *proc = find(procs, pid);
        if (!proc) {
            if (procs->count >= PROCS_MAX)
                continue;
            proc = &procs->entries[procs->count++];
            memset(proc, 0, sizeof(*proc));
            proc->pid = pid;
            proc->jiffies = jiffies; /* No baseline yet, so no CPU figure. */
        }

        if (elapsed_s > 0 && jiffies >= proc->jiffies) {
            double seconds = (double)(jiffies - proc->jiffies) / (double)procs->ticks_per_second;
            proc->cpu_percent = 100.0 * seconds / elapsed_s / (double)procs->cores;
        }
        proc->jiffies = jiffies;
        proc->rss_bytes = (gsize)rss_pages * (gsize)procs->page_size;
        g_strlcpy(proc->name, name, sizeof(proc->name));
        proc->seen = TRUE;
    }
    g_dir_close(dir);

    /* Drop the processes that exited, so their pids can be reused without
     * inheriting a stale baseline. */
    guint kept = 0;
    for (guint i = 0; i < procs->count; i++) {
        if (procs->entries[i].seen)
            procs->entries[kept++] = procs->entries[i];
    }
    procs->count = kept;
}

void procs_tick(Procs *procs) {
    if (!procs)
        return;

    gint64 now = g_get_monotonic_time();
    if (procs->last_scan && now - procs->last_scan < PROCS_INTERVAL_S * G_USEC_PER_SEC)
        return;

    double elapsed = procs->last_scan ? (double)(now - procs->last_scan) / G_USEC_PER_SEC : 0;
    procs->last_scan = now;
    scan(procs, elapsed);
}

static int by_cpu(const void *a, const void *b) {
    const Proc *left = a;
    const Proc *right = b;
    if (right->cpu_percent > left->cpu_percent) return 1;
    if (right->cpu_percent < left->cpu_percent) return -1;
    if (right->rss_bytes > left->rss_bytes) return 1;
    if (right->rss_bytes < left->rss_bytes) return -1;
    return 0;
}

static void append_escaped(GString *out, const char *value) {
    for (const char *c = value ? value : ""; *c; c++) {
        if (*c == '"' || *c == '\\')
            g_string_append_c(out, '\\');
        if ((unsigned char)*c < 0x20)
            g_string_append_printf(out, "\\u%04x", (unsigned char)*c);
        else
            g_string_append_c(out, *c);
    }
}

gchar *procs_json(Procs *procs, guint limit) {
    GString *out = g_string_new("{\"interval\":");
    g_string_append_printf(out, "%d,\"cores\":%u,\"processes\":[", PROCS_INTERVAL_S, procs->cores);

    Proc *sorted = g_new(Proc, procs->count ? procs->count : 1);
    memcpy(sorted, procs->entries, procs->count * sizeof(Proc));
    qsort(sorted, procs->count, sizeof(Proc), by_cpu);

    guint shown = MIN(limit, procs->count);
    for (guint i = 0; i < shown; i++) {
        g_string_append(out, i ? ",{\"pid\":" : "{\"pid\":");
        g_string_append_printf(out, "%d,\"name\":\"", sorted[i].pid);
        append_escaped(out, sorted[i].name);
        char buf[G_ASCII_DTOSTR_BUF_SIZE];
        g_ascii_formatd(buf, sizeof(buf), "%.2f", sorted[i].cpu_percent);
        g_string_append_printf(out, "\",\"cpu\":%s,\"rss\":%" G_GSIZE_FORMAT "}", buf,
                               sorted[i].rss_bytes);
    }

    g_free(sorted);
    g_string_append_printf(out, "],\"total\":%u}", procs->count);
    return g_string_free(out, FALSE);
}
