/*
 * Unit tests for store downsampling, window-to-tier selection and series
 * queries, which fail with plausible wrong numbers rather than crashes.
 * Build and run with tests/run.sh.
 */

#include "../app/metrics.h"
#include "../app/persist.h"
#include "../app/store.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

static int failures;

#define CHECK(condition, ...)                                                    \
    do {                                                                         \
        if (condition) {                                                         \
            printf("  ok   " __VA_ARGS__);                                       \
            printf("\n");                                                        \
        } else {                                                                 \
            printf("  FAIL " __VA_ARGS__);                                       \
            printf("   (%s:%d)\n", __FILE__, __LINE__);                          \
            failures++;                                                          \
        }                                                                        \
    } while (0)

static gboolean close_enough(double a, double b) {
    return fabs(a - b) < 0.001;
}

static void register_metrics(MetricRegistry *registry, const char **ids, guint count) {
    metrics_registry_init(registry);
    for (guint i = 0; i < count; i++)
        metrics_register(registry, ids[i], ids[i], "", "test");
}

/* The coarse tiers hold the mean of the fine samples in each bucket. */
static void test_downsampling(void) {
    printf("downsampling\n");

    const char *ids[] = {"a", "b"};
    MetricRegistry registry;
    register_metrics(&registry, ids, 2);

    Store *store = store_new(&registry, 1);
    gint64 base = 1000000;

    /* 15 samples of a=10 then 15 of a=20 spans one 15s bucket boundary. */
    for (int i = 0; i < 30; i++) {
        float values[2] = {i < 15 ? 10.0f : 20.0f, (float)i};
        store_push(store, values, base + i);
    }

    float row[2] = {0};
    gint64 timestamp = 0;
    CHECK(store_tier_count(store, 0) == 30, "fine tier kept every sample");
    CHECK(store_tier_count(store, 1) >= 1, "medium tier produced a bucket");

    gboolean sampled = store_tier_sample(store, 1, 0, row, &timestamp);
    CHECK(sampled, "medium tier bucket can be read");
    /* Closes at +15s: fifteen 10s plus the first 20 that triggered the flush. */
    CHECK(sampled && row[0] > 10.0 && row[0] <= 11.0, "bucket mean sits between the two levels (%.3f)", row[0]);

    Store *sparse = store_new(&registry, 1);
    for (int i = 0; i < 20; i++) {
        float values[2] = {i % 2 ? 100.0f : NAN, 0.0f};
        store_push(sparse, values, base + i);
    }
    sampled = store_tier_sample(sparse, 1, 0, row, &timestamp);
    CHECK(sampled && close_enough(row[0], 100.0), "NaN skipped rather than averaged as zero (%.3f)", row[0]);

    store_free(store);
    store_free(sparse);
    metrics_registry_clear(&registry);
}

static void test_window_selection(void) {
    printf("window to tier selection\n");

    const char *ids[] = {"a"};
    MetricRegistry registry;
    register_metrics(&registry, ids, 1);
    Store *store = store_new(&registry, 1);

    CHECK(store_tier_for_window(store, 300) == 0, "5m uses the fine tier");
    CHECK(store_tier_for_window(store, 1800) == 0, "30m uses the fine tier");
    CHECK(store_tier_for_window(store, 3600) == 1, "1h uses the medium tier");
    CHECK(store_tier_for_window(store, 21600) == 1, "6h uses the medium tier");
    CHECK(store_tier_for_window(store, 86400) == 2, "24h uses the coarse tier");
    CHECK(store_tier_for_window(store, 604800) == 2, "7d uses the coarse tier");
    CHECK(store_tier_for_window(store, 2592000) == 2, "30d uses the coarse tier");

    store_free(store);
    metrics_registry_clear(&registry);
}

static void test_series_query(void) {
    printf("series query\n");

    const char *ids[] = {"a", "b"};
    MetricRegistry registry;
    register_metrics(&registry, ids, 2);
    Store *store = store_new(&registry, 1);

    gint64 base = 2000000;
    for (int i = 0; i < 10; i++) {
        float values[2] = {(float)i, (float)(i * 2)};
        store_push(store, values, base + i);
    }

    gint64 timestamps[16];
    float values[16];
    guint n = store_series(store, 0, 1, base + 5, timestamps, values, 16);
    CHECK(n == 5, "since filter excludes older samples (%u)", n);
    CHECK(close_enough(values[0], 10.0), "first returned value matches its timestamp (%.1f)", values[0]);
    CHECK(timestamps[0] == base + 5, "timestamps line up with values");

    n = store_series(store, 0, 0, 0, timestamps, values, 3);
    CHECK(n == 3, "max_points is respected (%u)", n);
    CHECK(timestamps[n - 1] == base + 9, "capped series still ends at the newest sample");
    CHECK(close_enough(values[0], 1.5) && close_enough(values[2], 8.5),
          "capped series averages buckets (%.1f, %.1f)", values[0], values[2]);

    store_free(store);
    metrics_registry_clear(&registry);
}

/* Changing the sample interval at runtime must keep the fine tier honest about
 * its spacing, or a 30 minute chart reads from a 5 minute buffer. */
static void test_fine_interval_change(void) {
    printf("fine interval change\n");

    const char *ids[] = {"a"};
    MetricRegistry registry;
    register_metrics(&registry, ids, 1);
    Store *store = store_new(&registry, 10);

    gint64 base = 3000000;
    for (int i = 0; i < 400; i++) {
        float values[1] = {(float)i};
        store_push(store, values, base + i * 10);
    }
    guint before = store->tiers[0].capacity;
    CHECK(store_tier_count(store, 0) == before, "10s tier filled to capacity (%u)", before);

    store_set_fine_interval(store, 1);
    CHECK(store->tiers[0].interval_s == 1, "fine tier reports the new interval");
    CHECK(store->tiers[0].capacity > before, "capacity grew for the faster rate (%u)", store->tiers[0].capacity);
    CHECK(store_tier_count(store, 0) == before, "existing samples kept (%u)", store_tier_count(store, 0));
    CHECK(store_tier_for_window(store, 1800) == 0, "30m still served by the fine tier");

    float row[1];
    gint64 timestamp = 0;
    store_tier_sample(store, 0, store_tier_count(store, 0) - 1, row, &timestamp);
    CHECK(close_enough(row[0], 399.0), "newest sample survives the resize (%.1f)", row[0]);

    float next[1] = {1000.0f};
    store_push(store, next, base + 4000);
    store_tier_sample(store, 0, store_tier_count(store, 0) - 1, row, &timestamp);
    CHECK(close_enough(row[0], 1000.0) && timestamp == base + 4000, "appends continue after the resize");

    store_set_fine_interval(store, 10);
    CHECK(store_tier_count(store, 0) == store->tiers[0].capacity, "shrinking keeps only what fits (%u)",
          store_tier_count(store, 0));
    store_tier_sample(store, 0, store_tier_count(store, 0) - 1, row, &timestamp);
    CHECK(close_enough(row[0], 1000.0), "shrinking keeps the newest samples (%.1f)", row[0]);

    store_free(store);
    metrics_registry_clear(&registry);
}

/* A full ring whose header was last flushed before the newest appends (an
 * unclean stop) must still come back in time order: the slots after the
 * stale head hold the newest samples, not the oldest. */
static void test_restore_full_ring_with_stale_header(void) {
    printf("restore full ring with stale header\n");

    const char *ids[] = {"a"};
    MetricRegistry registry;
    register_metrics(&registry, ids, 1);
    Store *store = store_new(&registry, 1);
    const guint tier = 2;
    StoreTier *coarse = &store->tiers[tier];
    guint capacity = coarse->capacity;

    char directory[] = "/tmp/metrics-persist-XXXXXX";
    CHECK(mkdtemp(directory) != NULL, "temporary directory");
    char path[600];
    g_snprintf(path, sizeof(path), "%s/history.bin", directory);
    Persist *persist = persist_open_in(&registry, coarse, directory, "history.bin");
    CHECK(persist != NULL, "history file opened");
    persist_sync(persist, store, tier); /* writes the header and metric ids, as at startup */

    gint64 base = 4000000;
    guint total = capacity + 30;
    char header[4096];
    for (guint i = 0; i < total; i++) {
        float values[1] = {(float)i};
        persist_append(persist, values, base + (gint64)i * coarse->interval_s);
        if (i + 1 == capacity) {
            /* The header as flushed when the ring filled up. */
            int f = open(path, O_RDONLY);
            CHECK(pread(f, header, sizeof(header), 0) == (ssize_t)sizeof(header), "header read");
            close(f);
        }
    }
    persist_close(persist);
    /* An unclean stop: the file keeps that older header, 30 samples behind. */
    int f = open(path, O_WRONLY);
    CHECK(pwrite(f, header, sizeof(header), 0) == (ssize_t)sizeof(header), "stale header written back");
    close(f);

    Store *reloaded = store_new(&registry, 1);
    Persist *again = persist_open_in(&registry, &reloaded->tiers[tier], directory, "history.bin");
    guint restored = persist_load(again, reloaded, tier);
    CHECK(restored == capacity, "a full ring restored (%u of %u)", restored, capacity);

    gboolean ordered = TRUE;
    gint64 previous = 0, timestamp = 0;
    float row[1];
    for (guint i = 0; store_tier_sample(reloaded, tier, i, row, &timestamp); i++) {
        if (timestamp <= previous)
            ordered = FALSE;
        previous = timestamp;
    }
    CHECK(ordered, "restored samples are in time order");
    CHECK(previous == base + (gint64)(total - 1) * coarse->interval_s, "the newest sample is the last appended");

    gint64 since = previous - 12 * 3600;
    gint64 *timestamps = g_new(gint64, capacity);
    float *values = g_new(float, capacity);
    guint n = store_series(reloaded, tier, 0, since, timestamps, values, capacity);
    CHECK(n > 0 && timestamps[0] >= since, "a 12 h query starts inside the window (%u points)", n);

    g_free(timestamps);
    g_free(values);
    persist_close(again);
    unlink(path);
    rmdir(directory);
    store_free(reloaded);
    store_free(store);
    metrics_registry_clear(&registry);
}

/* The clock can step backwards (NTP correcting a wrong time); a window query
 * must still return only samples inside the window. */
static void test_series_query_after_clock_step_back(void) {
    printf("series query after the clock steps back\n");

    const char *ids[] = {"a"};
    MetricRegistry registry;
    register_metrics(&registry, ids, 1);
    Store *store = store_new(&registry, 1);

    /* Five samples at the right time, then the clock is set back a day. */
    gint64 right = 5000000, earlier = right - 86400;
    for (int i = 0; i < 5; i++) {
        float values[1] = {1.0f};
        store_push(store, values, right + i);
    }
    for (int i = 0; i < 5; i++) {
        float values[1] = {2.0f};
        store_push(store, values, earlier + i);
    }
    gint64 timestamps[16];
    float values[16];
    guint n = store_series(store, 0, 0, right, timestamps, values, 16);
    gboolean inside = TRUE;
    for (guint i = 0; i < n; i++)
        if (timestamps[i] < right)
            inside = FALSE;
    CHECK(n == 5 && inside, "only the samples inside the window (%u)", n);

    store_free(store);
    metrics_registry_clear(&registry);
}

int main(void) {
    test_downsampling();
    test_window_selection();
    test_series_query();
    test_fine_interval_change();
    test_restore_full_ring_with_stale_header();
    test_series_query_after_clock_step_back();

    printf("\n%s\n", failures ? "FAILED" : "all tests passed");
    return failures ? 1 : 0;
}
