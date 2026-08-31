#ifndef PROCS_H
#define PROCS_H

#include <glib.h>

typedef struct Procs Procs;

Procs *procs_new(void);
void procs_free(Procs *procs);

/* Rescans /proc when the sampling window has elapsed. Cheap enough to call on
 * every sample tick. */
void procs_tick(Procs *procs);

/* Top consumers by CPU, then by memory. Caller frees. */
gchar *procs_json(Procs *procs, guint limit);

#endif /* PROCS_H */
