/*
 * Shared metric model.
 *
 * Metrics are discovered at startup (cores, interfaces, mounts, thermal zones)
 * and registered into a flat array. Every sample is then just a float vector
 * indexed by metric id, which is what makes the ring buffers cheap.
 */

#ifndef METRICS_H
#define METRICS_H

#include <glib.h>
#include <stdint.h>

#define METRIC_ID_MAX    64
#define METRIC_LABEL_MAX 80
#define METRIC_UNIT_MAX  16
#define METRIC_GROUP_MAX 24

/* NAN marks "not available in this sample" and is skipped by consumers. */
typedef struct {
    char id[METRIC_ID_MAX];
    char label[METRIC_LABEL_MAX];
    char unit[METRIC_UNIT_MAX];
    char group[METRIC_GROUP_MAX];
} MetricDef;

typedef struct {
    MetricDef *defs;
    guint count;
    guint capacity;
} MetricRegistry;

void metrics_registry_init(MetricRegistry *registry);
void metrics_registry_clear(MetricRegistry *registry);

/* Returns the index of the new metric, or the existing one if id is a dup. */
guint metrics_register(MetricRegistry *registry,
                       const char *id,
                       const char *label,
                       const char *unit,
                       const char *group);

int metrics_find(const MetricRegistry *registry, const char *id);

#endif /* METRICS_H */
