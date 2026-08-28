/*
 * Three fixed-size ring buffers: a fine tier for the live view, a medium tier
 * for a day, and a coarse tier for a month. Coarse tiers are fed by averaging
 * the fine samples that land in each bucket, so a push is O(n_metrics).
 */

#include "store.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>

/* 30 minutes, 12 hours and 30 days at the tier intervals below.
 * The medium tier stops at 12 h on purpose: store_tier_for_window caps a chart
 * at ~2000 points, so nothing ever reads more than ~8 h of 15 s samples and a
 * 24 h buffer here would just be resident memory nobody queries. */
static const guint TIER_INTERVAL[STORE_TIERS] = {1, 15, 300};
static const guint TIER_CAPACITY[STORE_TIERS] = {1800, 2880, 8640};
static const guint TIER_MIN_CAPACITY[STORE_TIERS] = {300, 720, 576};

/* Cap the history at a small share of RAM so a 256 MB camera stays healthy. */
#define MEMORY_BUDGET_FRACTION 0.015

static guint64 total_memory_bytes(void) {
    FILE *f = fopen("/proc/meminfo", "re");
    if (!f)
        return 0;
    char line[128];
    guint64 kb = 0;
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "MemTotal: %" G_GUINT64_FORMAT " kB", &kb) == 1)
            break;
    }
    fclose(f);
    return kb * 1024;
}

static gsize tier_bytes(const StoreTier *tier, guint n_metrics) {
    return (gsize)tier->capacity * (n_metrics * sizeof(float) + sizeof(gint64));
}

Store *store_new(guint n_metrics, guint base_interval_s) {
    Store *store = g_new0(Store, 1);
    store->n_metrics = n_metrics;
    g_mutex_init(&store->lock);
    store->latest = g_new0(float, n_metrics);
    store->scratch = g_new0(float, n_metrics);

    guint capacity[STORE_TIERS];
    for (guint i = 0; i < STORE_TIERS; i++)
        capacity[i] = TIER_CAPACITY[i];

    /* The fine tier is sampled at the configured interval, so a slower sample
     * rate buys a longer window rather than more memory. */
    guint fine_interval = MAX(1, base_interval_s);
    capacity[0] = MAX(TIER_MIN_CAPACITY[0], TIER_CAPACITY[0] / fine_interval);

    guint64 memory = total_memory_bytes();
    if (memory > 0) {
        gsize budget = (gsize)((double)memory * MEMORY_BUDGET_FRACTION);
        gsize per_sample = n_metrics * sizeof(float) + sizeof(gint64);
        gsize wanted = 0;
        for (guint i = 0; i < STORE_TIERS; i++)
            wanted += (gsize)capacity[i] * per_sample;

        if (wanted > budget) {
            double scale = (double)budget / (double)wanted;
            for (guint i = 0; i < STORE_TIERS; i++)
                capacity[i] = MAX(TIER_MIN_CAPACITY[i], (guint)((double)capacity[i] * scale));
            syslog(LOG_WARNING,
                   "history scaled to fit %.1f MB of %.0f MB RAM: %u/%u/%u samples",
                   (double)budget / 1048576.0, (double)memory / 1048576.0,
                   capacity[0], capacity[1], capacity[2]);
        }
    }

    for (guint i = 0; i < STORE_TIERS; i++) {
        StoreTier *tier = &store->tiers[i];
        tier->interval_s = (i == 0) ? fine_interval : TIER_INTERVAL[i];
        tier->capacity = capacity[i];
        tier->timestamps = g_new0(gint64, tier->capacity);
        tier->data = g_new0(float, (gsize)tier->capacity * n_metrics);
        if (i > 0) {
            tier->acc = g_new0(double, n_metrics);
            tier->acc_n = g_new0(guint, n_metrics);
        }
    }

    syslog(LOG_INFO, "history: %u metrics, %us/%us/%us tiers, %.1f MB",
           n_metrics, store->tiers[0].interval_s, store->tiers[1].interval_s,
           store->tiers[2].interval_s, (double)store_bytes(store) / 1048576.0);

    return store;
}

void store_free(Store *store) {
    if (!store)
        return;
    for (guint i = 0; i < STORE_TIERS; i++) {
        g_free(store->tiers[i].timestamps);
        g_free(store->tiers[i].data);
        g_free(store->tiers[i].acc);
        g_free(store->tiers[i].acc_n);
    }
    g_free(store->latest);
    g_free(store->scratch);
    g_mutex_clear(&store->lock);
    g_free(store);
}

gsize store_bytes(const Store *store) {
    gsize total = 0;
    for (guint i = 0; i < STORE_TIERS; i++)
        total += tier_bytes(&store->tiers[i], store->n_metrics);
    return total;
}

guint store_tier_span_s(const Store *store, guint tier_index) {
    const StoreTier *tier = &store->tiers[tier_index];
    return tier->interval_s * tier->capacity;
}

static void tier_append(StoreTier *tier, guint n_metrics, const float *values, gint64 timestamp) {
    tier->timestamps[tier->head] = timestamp;
    memcpy(&tier->data[(gsize)tier->head * n_metrics], values, n_metrics * sizeof(float));
    tier->head = (tier->head + 1) % tier->capacity;
    if (tier->count < tier->capacity)
        tier->count++;
}

void store_push(Store *store, const float *values, gint64 timestamp) {
    g_mutex_lock(&store->lock);

    memcpy(store->latest, values, store->n_metrics * sizeof(float));
    store->latest_ts = timestamp;
    store->has_latest = TRUE;

    tier_append(&store->tiers[0], store->n_metrics, values, timestamp);

    for (guint i = 1; i < STORE_TIERS; i++) {
        StoreTier *tier = &store->tiers[i];
        if (tier->acc_start == 0)
            tier->acc_start = timestamp;

        for (guint m = 0; m < store->n_metrics; m++) {
            if (!isnan(values[m])) {
                tier->acc[m] += values[m];
                tier->acc_n[m]++;
            }
        }

        if (timestamp - tier->acc_start >= tier->interval_s) {
            for (guint m = 0; m < store->n_metrics; m++)
                store->scratch[m] = tier->acc_n[m] ? (float)(tier->acc[m] / tier->acc_n[m]) : NAN;
            tier_append(tier, store->n_metrics, store->scratch, timestamp);
            memset(tier->acc, 0, store->n_metrics * sizeof(double));
            memset(tier->acc_n, 0, store->n_metrics * sizeof(guint));
            tier->acc_start = timestamp;
        }
    }

    g_mutex_unlock(&store->lock);
}

gboolean store_latest(Store *store, float *out, gint64 *timestamp) {
    g_mutex_lock(&store->lock);
    gboolean ok = store->has_latest;
    if (ok) {
        memcpy(out, store->latest, store->n_metrics * sizeof(float));
        if (timestamp)
            *timestamp = store->latest_ts;
    }
    g_mutex_unlock(&store->lock);
    return ok;
}

guint store_tier_for_window(const Store *store, guint window_s) {
    for (guint i = 0; i < STORE_TIERS; i++) {
        /* Roughly one point per pixel on a wide chart is plenty. */
        if (window_s / store->tiers[i].interval_s <= 2000)
            return i;
    }
    return STORE_TIERS - 1;
}

guint store_series(Store *store,
                   guint tier_index,
                   guint metric_index,
                   gint64 since,
                   gint64 *timestamps,
                   float *values,
                   guint max_points) {
    if (tier_index >= STORE_TIERS || metric_index >= store->n_metrics)
        return 0;

    g_mutex_lock(&store->lock);
    StoreTier *tier = &store->tiers[tier_index];
    guint written = 0;
    guint start = (tier->head + tier->capacity - tier->count) % tier->capacity;

    for (guint i = 0; i < tier->count && written < max_points; i++) {
        guint slot = (start + i) % tier->capacity;
        if (tier->timestamps[slot] < since)
            continue;
        timestamps[written] = tier->timestamps[slot];
        values[written] = tier->data[(gsize)slot * store->n_metrics + metric_index];
        written++;
    }
    g_mutex_unlock(&store->lock);
    return written;
}
