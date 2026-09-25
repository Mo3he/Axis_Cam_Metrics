#ifndef COLLECT_H
#define COLLECT_H

#include "metrics.h"

typedef struct Collector Collector;

/* Discovers every available source and registers a metric for each. */
Collector *collector_new(MetricRegistry *registry);
void collector_free(Collector *collector);

/* Writes registry->count floats. Rates need two samples, so the first call
 * leaves every rate metric at NAN. */
void collector_sample(Collector *collector, float *values);

/* True when the number of mounted filesystems changed since discovery. Storage
 * often mounts after boot, and new metrics can only be registered by rebuilding. */
gboolean collector_mounts_changed(Collector *collector);

#endif /* COLLECT_H */
