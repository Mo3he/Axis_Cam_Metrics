#ifndef INFLUX_H
#define INFLUX_H

#include "api.h"

typedef enum { INFLUX_V1, INFLUX_V2 } InfluxVersion;

typedef struct {
    gboolean enabled;
    InfluxVersion version;
    char url[256];      /* Base server URL, no path. */
    char database[96];  /* v1 database, v2 bucket. */
    char org[96];       /* v2 only. */
    char token[256];    /* v2 only. */
    char username[64];  /* v1 only, optional. */
    char password[128]; /* v1 only, optional. */
    char measurement[64];
    guint interval_s;
} InfluxConfig;

typedef struct Influx Influx;

Influx *influx_new(const Api *api);
void influx_free(Influx *influx);

void influx_apply(Influx *influx, const InfluxConfig *config);

/* Called on every sample; writes at the configured interval. */
void influx_tick(Influx *influx);

const char *influx_state(const Influx *influx);

#endif /* INFLUX_H */
