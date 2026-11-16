#ifndef LARODINFO_H
#define LARODINFO_H

#include <glib.h>

/* Read-only view of what larod has loaded on the deep learning accelerator.
 * liblarod is opened at runtime, so devices without larod (recorders) still run. */
typedef struct LarodInfo LarodInfo;

typedef struct {
    guint models;       /* models loaded on an accelerator, not the CPU */
    guint64 model_bytes;
    guint64 sessions;   /* other clients connected to larod */
} LarodStats;

/* NULL when liblarod is missing or the device has no accelerator. */
LarodInfo *larod_info_open(void);
gboolean larod_info_read(LarodInfo *info, LarodStats *stats);
void larod_info_close(LarodInfo *info);

/* Appends the models seen by the last read as a JSON array. */
void larod_info_models_json(const LarodInfo *info, GString *out);

#endif /* LARODINFO_H */
