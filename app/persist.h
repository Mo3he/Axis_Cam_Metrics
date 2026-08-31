#ifndef PERSIST_H
#define PERSIST_H

#include "metrics.h"
#include "store.h"

typedef struct Persist Persist;

/* Resolves a real, writable, non-volatile filesystem (SD card or disk) and
 * opens one tier's history file there. Returns NULL when the device has no such
 * storage, in which case history stays memory-only. */
Persist *persist_open(const MetricRegistry *registry, const StoreTier *tier, const char *filename);
void persist_close(Persist *persist);

/* Restores previously saved samples into the tier, remapping by metric id so a
 * changed metric set (new interface, SD card inserted) does not invalidate the
 * file. Returns the number of samples restored. */
guint persist_load(Persist *persist, Store *store, guint tier_index);

/* Rewrites the file for the store's current metric set and replays whatever is
 * in the tier. Call once after persist_load so later appends line up. */
void persist_sync(Persist *persist, Store *store, guint tier_index);

/* Appends one sample. One record write, plus a header write now and then. */
void persist_append(Persist *persist, const float *values, gint64 timestamp);

const char *persist_path(const Persist *persist);

#endif /* PERSIST_H */
