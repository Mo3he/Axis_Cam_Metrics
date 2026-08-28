#ifndef COLLECT_H
#define COLLECT_H

#include "metrics.h"

typedef struct Collector Collector;

/* Discovers cores, interfaces, disks, mounts and thermal zones, and registers
 * a metric for each. Must be called before any sampling. */
Collector *collector_new(MetricRegistry *registry);
void collector_free(Collector *collector);

/* Writes registry->count floats into values. Rates need two samples, so the
 * first call leaves every rate metric at NAN. */
void collector_sample(Collector *collector, float *values);

#endif /* COLLECT_H */
