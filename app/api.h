#ifndef API_H
#define API_H

#include "collect.h"
#include "metrics.h"
#include "store.h"

typedef struct {
    char model[64];
    char product[96];
    char type[48];
    char serial[32];
    char soc[48];
    char arch[16];
    char firmware[32];
    char hostname[64];
} DeviceInfo;

typedef struct {
    MetricRegistry *registry;
    Store *store;
    DeviceInfo device;
    const char *app_version;
    const char *persist_path; /* NULL when history is memory-only. */
    gint64 started;
} Api;

void api_read_device_info(DeviceInfo *info, void *parameter_handle);

/* Each returns a newly allocated JSON document. */
gchar *api_meta_json(const Api *api);
gchar *api_current_json(const Api *api);

/* Just the id to value object, which is what MQTT and Home Assistant
 * value_template expect. */
gchar *api_current_values_json(const Api *api);
gchar *api_series_json(const Api *api, const char *query);
gchar *api_health_json(const Api *api);

/* Prometheus text exposition format, for scrapers and Grafana. */
gchar *api_prometheus_text(const Api *api);

/* Extracts a query parameter from a request path, or NULL. Caller frees. */
gchar *api_query_param(const char *query, const char *name);

#endif /* API_H */
