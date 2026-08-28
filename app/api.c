/*
 * JSON rendering for the read-only metric endpoints.
 *
 * Everything is hand-rolled rather than pulled from a JSON library: the app
 * only ever writes JSON, and the shapes are small and fixed.
 */

#include "api.h"

#include <axsdk/axparameter.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define MAX_SERIES_POINTS 4000

static void append_escaped(GString *out, const char *value) {
    for (const char *c = value ? value : ""; *c; c++) {
        switch (*c) {
        case '"':  g_string_append(out, "\\\""); break;
        case '\\': g_string_append(out, "\\\\"); break;
        case '\n': g_string_append(out, "\\n"); break;
        case '\r': g_string_append(out, "\\r"); break;
        case '\t': g_string_append(out, "\\t"); break;
        default:
            if ((unsigned char)*c < 0x20)
                g_string_append_printf(out, "\\u%04x", (unsigned char)*c);
            else
                g_string_append_c(out, *c);
        }
    }
}

static void append_kv(GString *out, const char *key, const char *value, gboolean comma) {
    if (comma)
        g_string_append_c(out, ',');
    g_string_append_c(out, '"');
    append_escaped(out, key);
    g_string_append(out, "\":\"");
    append_escaped(out, value);
    g_string_append_c(out, '"');
}

/* JSON has no NaN, so unavailable readings become null. */
static void append_number(GString *out, float value) {
    if (isnan(value) || isinf(value)) {
        g_string_append(out, "null");
        return;
    }
    char buf[G_ASCII_DTOSTR_BUF_SIZE];
    /* 7 digits is about all a float32 carries, and byte counts need them all. */
    g_ascii_formatd(buf, sizeof(buf), "%.7g", value);
    g_string_append(out, buf);
}

static gchar *take(GString *out) {
    /* g_string_free(out, FALSE) inlines to g_string_free_and_steal() on newer
     * headers and breaks on AXIS OS 11.x runtimes; copy the buffer instead. */
    gchar *result = g_strdup(out->str);
    g_string_free(out, TRUE);
    return result;
}

/* ------------------------------------------------------------ device info */

static void read_param(AXParameter *handle, const char *name, char *out, gsize len) {
    GError *error = NULL;
    gchar *value = NULL;
    out[0] = '\0';
    if (ax_parameter_get(handle, name, &value, &error) && value) {
        g_strlcpy(out, g_strstrip(value), len);
        g_free(value);
    }
    g_clear_error(&error);
}

/* Reads through parhand rather than VAPIX, which is the only way that also
 * works on OS 13 recorders where param.cgi is gone. */
void api_read_device_info(DeviceInfo *info, void *parameter_handle) {
    AXParameter *handle = parameter_handle;
    memset(info, 0, sizeof(*info));

    if (handle) {
        read_param(handle, "root.Brand.ProdShortName", info->model, sizeof(info->model));
        read_param(handle, "root.Brand.ProdFullName", info->product, sizeof(info->product));
        read_param(handle, "root.Brand.ProdType", info->type, sizeof(info->type));
        read_param(handle, "root.Properties.System.SerialNumber", info->serial, sizeof(info->serial));
        read_param(handle, "root.Properties.System.Soc", info->soc, sizeof(info->soc));
        read_param(handle, "root.Properties.System.Architecture", info->arch, sizeof(info->arch));
        read_param(handle, "root.Properties.Firmware.Version", info->firmware, sizeof(info->firmware));
    }

    gchar *hostname = NULL;
    if (g_file_get_contents("/etc/hostname", &hostname, NULL, NULL)) {
        g_strlcpy(info->hostname, g_strstrip(hostname), sizeof(info->hostname));
        g_free(hostname);
    }
    if (!info->firmware[0]) {
        gchar *release = NULL;
        if (g_file_get_contents("/etc/os-release", &release, NULL, NULL)) {
            const char *v = strstr(release, "VERSION_ID=");
            if (v)
                sscanf(v + 11, "%31s", info->firmware);
            g_free(release);
        }
    }
}

/* ------------------------------------------------------------- endpoints */

gchar *api_meta_json(const Api *api) {
    GString *out = g_string_new("{\"app\":{");
    append_kv(out, "name", "Metrics", FALSE);
    append_kv(out, "version", api->app_version, TRUE);
    g_string_append(out, "},\"device\":{");
    append_kv(out, "model", api->device.model, FALSE);
    append_kv(out, "product", api->device.product, TRUE);
    append_kv(out, "type", api->device.type, TRUE);
    append_kv(out, "serial", api->device.serial, TRUE);
    append_kv(out, "soc", api->device.soc, TRUE);
    append_kv(out, "architecture", api->device.arch, TRUE);
    append_kv(out, "firmware", api->device.firmware, TRUE);
    append_kv(out, "hostname", api->device.hostname, TRUE);

    g_string_append(out, "},\"store\":{\"persisted\":false,\"tiers\":[");
    for (guint i = 0; i < STORE_TIERS; i++) {
        const StoreTier *tier = &api->store->tiers[i];
        g_string_append_printf(out, "%s{\"interval\":%u,\"capacity\":%u,\"span\":%u,\"points\":%u}",
                               i ? "," : "", tier->interval_s, tier->capacity,
                               store_tier_span_s(api->store, i), tier->count);
    }
    g_string_append_printf(out,
                           "],\"bytes\":%zu},\"windows\":[300,900,1800,3600,21600,86400,604800,2592000],",
                           store_bytes(api->store));

    g_string_append(out, "\"metrics\":[");
    for (guint i = 0; i < api->registry->count; i++) {
        const MetricDef *def = &api->registry->defs[i];
        g_string_append(out, i ? ",{" : "{");
        append_kv(out, "id", def->id, FALSE);
        append_kv(out, "label", def->label, TRUE);
        append_kv(out, "unit", def->unit, TRUE);
        append_kv(out, "group", def->group, TRUE);
        g_string_append_c(out, '}');
    }
    g_string_append(out, "]}");
    return take(out);
}

gchar *api_current_json(const Api *api) {
    float *values = g_new(float, api->registry->count);
    gint64 timestamp = 0;

    if (!store_latest(api->store, values, &timestamp)) {
        g_free(values);
        return g_strdup("{\"timestamp\":0,\"values\":{}}");
    }

    GString *out = g_string_new(NULL);
    g_string_append_printf(out, "{\"timestamp\":%" G_GINT64_FORMAT ",\"values\":{", timestamp);

    gboolean first = TRUE;
    for (guint i = 0; i < api->registry->count; i++) {
        if (isnan(values[i]))
            continue; /* Omit rather than emit null: the current view wants facts. */
        if (!first)
            g_string_append_c(out, ',');
        first = FALSE;
        g_string_append_c(out, '"');
        append_escaped(out, api->registry->defs[i].id);
        g_string_append(out, "\":");
        append_number(out, values[i]);
    }
    g_string_append(out, "}}");

    g_free(values);
    return take(out);
}

static gchar *url_decode(const char *value, gsize length) {
    GString *out = g_string_sized_new(length);

    for (gsize i = 0; i < length; i++) {
        if (value[i] == '+') {
            g_string_append_c(out, ' ');
        } else if (value[i] == '%' && i + 2 < length && g_ascii_isxdigit(value[i + 1]) &&
                   g_ascii_isxdigit(value[i + 2])) {
            g_string_append_c(out, (gchar)(g_ascii_xdigit_value(value[i + 1]) * 16 +
                                           g_ascii_xdigit_value(value[i + 2])));
            i += 2;
        } else {
            g_string_append_c(out, value[i]);
        }
    }
    return take(out);
}

gchar *api_query_param(const char *query, const char *name) {
    if (!query)
        return NULL;
    const char *cursor = strchr(query, '?');
    cursor = cursor ? cursor + 1 : query;

    gsize name_len = strlen(name);
    while (*cursor) {
        const char *end = strchr(cursor, '&');
        gsize len = end ? (gsize)(end - cursor) : strlen(cursor);
        if (len > name_len && strncmp(cursor, name, name_len) == 0 && cursor[name_len] == '=')
            return url_decode(cursor + name_len + 1, len - name_len - 1);
        if (!end)
            break;
        cursor = end + 1;
    }
    return NULL;
}

gchar *api_series_json(const Api *api, const char *query) {
    gchar *window_arg = api_query_param(query, "window");
    gchar *metrics_arg = api_query_param(query, "metrics");

    guint window = window_arg ? (guint)g_ascii_strtoull(window_arg, NULL, 10) : 1800;
    if (window == 0)
        window = 1800;
    g_free(window_arg);

    guint tier_index = store_tier_for_window(api->store, window);
    gint64 since = (gint64)time(NULL) - window;

    gchar **wanted = metrics_arg ? g_strsplit(metrics_arg, ",", -1) : NULL;
    g_free(metrics_arg);

    gint64 *timestamps = g_new(gint64, MAX_SERIES_POINTS);
    float *values = g_new(float, MAX_SERIES_POINTS);

    GString *out = g_string_new(NULL);
    g_string_append_printf(out,
                           "{\"window\":%u,\"tier\":%u,\"interval\":%u,\"series\":{",
                           window, tier_index, api->store->tiers[tier_index].interval_s);

    gboolean first = TRUE;
    gint64 *shared_timestamps = NULL;
    guint shared_count = 0;

    for (guint i = 0; i < api->registry->count; i++) {
        const char *id = api->registry->defs[i].id;
        if (wanted) {
            gboolean match = FALSE;
            for (gchar **w = wanted; *w; w++) {
                if (strcmp(*w, id) == 0)
                    match = TRUE;
            }
            if (!match)
                continue;
        }

        guint n = store_series(api->store, tier_index, i, since, timestamps, values, MAX_SERIES_POINTS);

        /* Every metric shares one sample clock, so the timestamps are emitted
         * once instead of per series. */
        if (!shared_timestamps && n > 0) {
            shared_timestamps = g_memdup2(timestamps, n * sizeof(gint64));
            shared_count = n;
        }

        if (!first)
            g_string_append_c(out, ',');
        first = FALSE;
        g_string_append_c(out, '"');
        append_escaped(out, id);
        g_string_append(out, "\":[");
        for (guint p = 0; p < n; p++) {
            if (p)
                g_string_append_c(out, ',');
            append_number(out, values[p]);
        }
        g_string_append_c(out, ']');
    }

    g_string_append(out, "},\"timestamps\":[");
    for (guint p = 0; p < shared_count; p++)
        g_string_append_printf(out, "%s%" G_GINT64_FORMAT, p ? "," : "", shared_timestamps[p]);
    g_string_append(out, "]}");

    g_free(shared_timestamps);
    g_free(timestamps);
    g_free(values);
    if (wanted)
        g_strfreev(wanted);
    return take(out);
}

gchar *api_health_json(const Api *api) {
    GString *out = g_string_new(NULL);
    g_string_append_printf(out,
                           "{\"ok\":true,\"metrics\":%u,\"uptime\":%" G_GINT64_FORMAT
                           ",\"samples\":%u,\"bytes\":%zu}",
                           api->registry->count, (gint64)time(NULL) - api->started,
                           api->store->tiers[0].count, store_bytes(api->store));
    return take(out);
}
