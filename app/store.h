#ifndef STORE_H
#define STORE_H

#include "metrics.h"

#include <glib.h>

#define STORE_TIERS 3

/* Fires when a tier emits a downsampled sample. Invoked outside the store lock
 * so a slow listener (a disk write) cannot stall the sampler. */
typedef void (*StoreTierCallback)(const float *values, gint64 timestamp, gpointer user_data);

typedef struct {
    guint interval_s;
    guint capacity;
    guint count;
    guint head; /* Next slot to write. */
    gint64 *timestamps;
    float *data; /* capacity * n_metrics, row-major by sample. */

    /* Coarse tiers are means of the samples that fell in the bucket. */
    double *acc;
    guint *acc_n;
    gint64 acc_start;

    StoreTierCallback callback;
    gpointer callback_data;
} StoreTier;

typedef struct {
    const MetricRegistry *registry;
    guint n_metrics;
    StoreTier tiers[STORE_TIERS];
    GMutex lock;
    float *latest;
    float *scratch;
    gint64 latest_ts;
    gboolean has_latest;
} Store;

/* Capacities are scaled down on low-memory devices; the resulting sizes are
 * logged at startup. */
Store *store_new(const MetricRegistry *registry, guint base_interval_s);
void store_free(Store *store);

const MetricRegistry *store_registry(const Store *store);

/* Appends straight into one tier, bypassing the downsampling accumulator.
 * Used to replay saved history at startup. */
void store_restore(Store *store, guint tier_index, const float *values, gint64 timestamp);

/* Reads one whole sample row, oldest first. Returns FALSE past the end. */
gboolean store_tier_sample(Store *store, guint tier_index, guint index, float *out, gint64 *timestamp);
guint store_tier_count(Store *store, guint tier_index);

void store_set_tier_callback(Store *store, guint tier_index, StoreTierCallback callback, gpointer user_data);

void store_push(Store *store, const float *values, gint64 timestamp);

/* Copies the most recent sample. Returns FALSE before the first push. */
gboolean store_latest(Store *store, float *out, gint64 *timestamp);

/* Picks the coarsest tier that still resolves the window. */
guint store_tier_for_window(const Store *store, guint window_s);

/* Writes up to max_points samples for one metric, oldest first. Returns the
 * number written. */
guint store_series(Store *store,
                   guint tier_index,
                   guint metric_index,
                   gint64 since,
                   gint64 *timestamps,
                   float *values,
                   guint max_points);

guint store_tier_span_s(const Store *store, guint tier_index);
gsize store_bytes(const Store *store);

#endif /* STORE_H */
