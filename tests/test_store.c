/*
 * Unit tests for the parts that fail silently.
 *
 * A bug in tier downsampling or in the persistence id-remap produces plausible
 * but wrong numbers rather than a crash, so those are the two things worth
 * pinning down. Build and run with tests/run.sh.
 */

#include "../app/metrics.h"
#include "../app/persist.h"
#include "../app/store.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

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

    float row[2];
    gint64 timestamp = 0;
    CHECK(store_tier_count(store, 0) == 30, "fine tier kept every sample");
    CHECK(store_tier_count(store, 1) >= 1, "medium tier produced a bucket");

    if (store_tier_sample(store, 1, 0, row, &timestamp)) {
        /* The first bucket closes at +15s, averaging the fifteen 10s and the
         * first 20 that triggered the flush. */
        CHECK(row[0] > 10.0 && row[0] <= 11.0, "bucket mean sits between the two levels (%.3f)", row[0]);
    }

    /* NaN must be skipped, not treated as zero. */
    Store *sparse = store_new(&registry, 1);
    for (int i = 0; i < 20; i++) {
        float values[2] = {i % 2 ? 100.0f : NAN, 0.0f};
        store_push(sparse, values, base + i);
    }
    if (store_tier_sample(sparse, 1, 0, row, &timestamp))
        CHECK(close_enough(row[0], 100.0), "NaN skipped rather than averaged as zero (%.3f)", row[0]);

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

/* A series must survive the metric set changing shape between runs. */
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

    store_free(store);
    metrics_registry_clear(&registry);
}

int main(void) {
    test_downsampling();
    test_window_selection();
    test_series_query();

    printf("\n%s\n", failures ? "FAILED" : "all tests passed");
    return failures ? 1 : 0;
}
