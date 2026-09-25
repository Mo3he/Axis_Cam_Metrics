#ifndef PERSIST_H
#define PERSIST_H

#include "metrics.h"
#include "store.h"

typedef struct Persist Persist;

/* Opens one tier's history file on SD card or disk. Returns NULL when there is
 * no such storage, in which case history stays memory-only. */
Persist *persist_open(const MetricRegistry *registry, const StoreTier *tier, const char *filename);
void persist_close(Persist *persist);

/* Restores saved samples, remapped by metric id so a changed metric set does not
 * invalidate the file. Returns the number restored. */
guint persist_load(Persist *persist, Store *store, guint tier_index);

/* Rewrites the file for the current metric set and replays the tier. Call once
 * after persist_load so later appends line up. */
void persist_sync(Persist *persist, Store *store, guint tier_index);

/* One record write, plus a header write every HEADER_FLUSH_EVERY appends. */
void persist_append(Persist *persist, const float *values, gint64 timestamp);

const char *persist_path(const Persist *persist);

#endif /* PERSIST_H */
