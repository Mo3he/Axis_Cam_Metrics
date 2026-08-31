/*
 * Metrics Dashboard ACAP entry binary.
 *
 * Owns the parameter store entries, samples the device on a timer into the
 * tiered history store, and serves both the settings endpoint and the
 * read-only metric API on 127.0.0.1:2207. The camera's reverse proxy exposes
 * those as /local/Metrics/api/... (admin) and /local/Metrics/data/... (viewer),
 * so no additional port is opened on the network.
 *
 * The loopback port must be unique across all ACAPs that may run on the same
 * device; see PARAM_CGI_FALLBACK.md for the registry.
 */

#include <axsdk/axparameter.h>
#include <gio/gio.h>
#include <glib-unix.h>
#include <glib/gstdio.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include "api.h"
#include "alerts.h"
#include "collect.h"
#include "influx.h"
#include "metrics.h"
#include "mqtt.h"
#include "persist.h"
#include "procs.h"
#include "selection.h"
#include "store.h"
#include "vapix.h"

#define APP_NAME           "Metrics"
#define APP_VERSION        "0.1.0" /* acap:installed-version */
#define SETTINGS_HTTP_PORT 2207
#define MAX_REQUEST        16384
/* A series request names every metric it wants, so the path is far longer than
 * a typical URL. */
#define MAX_PATH_LENGTH    8192

typedef struct {
    const char *name;
    const char *fallback;
    gboolean secret;
    char *value;
} Parameter;

/* Must stay in sync with paramConfig in manifest.json. */
static Parameter parameters[] = {
    {"SampleInterval", "1", FALSE, NULL},
    {"MqttEnabled", "no", FALSE, NULL},
    {"MqttHost", "", FALSE, NULL},
    {"MqttPort", "1883", FALSE, NULL},
    {"MqttTls", "no", FALSE, NULL},
    {"MqttUsername", "", FALSE, NULL},
    {"MqttPassword", "", TRUE, NULL},
    {"MqttTopicPrefix", "", FALSE, NULL},
    {"MqttInterval", "30", FALSE, NULL},
    {"MqttDiscovery", "yes", FALSE, NULL},
    {"MqttDiscoveryAll", "no", FALSE, NULL},
    {"InfluxEnabled", "no", FALSE, NULL},
    {"InfluxVersion", "v2", FALSE, NULL},
    {"InfluxUrl", "", FALSE, NULL},
    {"InfluxDatabase", "", FALSE, NULL},
    {"InfluxOrg", "", FALSE, NULL},
    {"InfluxToken", "", TRUE, NULL},
    {"InfluxUsername", "", FALSE, NULL},
    {"InfluxPassword", "", TRUE, NULL},
    {"InfluxMeasurement", "axis_metrics", FALSE, NULL},
    {"InfluxInterval", "30", FALSE, NULL},
};

static AXParameter *parameter_handle;
static GMainLoop *main_loop;

static void sse_broadcast(void);

static MetricRegistry registry;
static Collector *collector;
static Store *store;
static Persist *persists[STORE_TIERS];
static Mqtt *mqtt;
static Influx *influx;
static Procs *procs;
static Alerts *alerts;
static Api api;
static float *sample_buffer;
static guint sample_timer;
static guint sample_interval_s;

static Parameter *find_parameter(const char *name) {
    for (gsize i = 0; i < G_N_ELEMENTS(parameters); i++) {
        if (g_strcmp0(parameters[i].name, name) == 0)
            return &parameters[i];
    }
    return NULL;
}

/* Parameter callbacks report a qualified name such as root.Metrics.Enabled. */
static Parameter *find_parameter_suffix(const char *qualified_name) {
    const char *last = strrchr(qualified_name, '.');
    return find_parameter(last ? last + 1 : qualified_name);
}

static const char *parameter_value(const char *name) {
    Parameter *parameter = find_parameter(name);
    if (!parameter)
        return "";
    return parameter->value ? parameter->value : parameter->fallback;
}

static void load_parameter(Parameter *parameter) {
    GError *error = NULL;
    gchar *value = NULL;

    if (ax_parameter_get(parameter_handle, parameter->name, &value, &error)) {
        g_free(parameter->value);
        parameter->value = g_strdup(value ? value : "");
        g_free(value);
    } else {
        syslog(LOG_WARNING, "read parameter %s failed: %s", parameter->name,
               error ? error->message : "unknown error");
        if (!parameter->value)
            parameter->value = g_strdup(parameter->fallback);
    }
    g_clear_error(&error);
}

/* ----------------------------------------------------------------- sampler */

static void on_tier_sample(const float *values, gint64 timestamp, gpointer user_data) {
    persist_append(persists[GPOINTER_TO_UINT(user_data)], values, timestamp);
}

/* Alerts already reach the device event system; this mirrors them to MQTT so a
 * broker-side consumer does not have to poll. */
static void on_alert_changed(const AlertRule *rule, gboolean firing, gpointer user_data) {
    (void)user_data;
    mqtt_publish_alert(mqtt, rule->id, rule->name, rule->metric, rule->last_value, firing);
}

/* Storage is not necessarily mounted when an ACAP starts at boot: on a recorder
 * the app came up more than an hour before the disk appeared, and a one-shot
 * probe left history in memory for the whole run. Retrying also means an SD
 * card inserted later starts being used without a restart. */
#define PERSIST_RETRY_S 30

/* One file per tier. The coarse tier keeps the original name so an existing
 * recording survives the upgrade that added the other two. */
G_STATIC_ASSERT(STORE_TIERS == 3);
static const char *PERSIST_FILES[STORE_TIERS] = {"history-fine.bin", "history-medium.bin",
                                                "history.bin"};

static guint persist_retry_timer;

static void rebuild_for_new_storage(void);

static void close_persistence(void) {
    for (guint i = 0; i < STORE_TIERS; i++) {
        if (!persists[i])
            continue;
        store_set_tier_callback(store, i, NULL, NULL);
        persist_close(persists[i]);
        persists[i] = NULL;
    }
    api.persist_path = NULL;
}

static gboolean try_open_persistence(gpointer user_data) {
    (void)user_data;
    if (persists[STORE_TIERS - 1])
        return G_SOURCE_REMOVE;

    /* Checked before opening: a disk appearing usually means filesystem metrics
     * are missing too, and opening the history only to close it again during
     * the rebuild would rewrite its header. */
    if (collector_mounts_changed(collector)) {
        persist_retry_timer = 0;
        rebuild_for_new_storage();
        return G_SOURCE_REMOVE;
    }

    for (guint i = 0; i < STORE_TIERS; i++) {
        persists[i] = persist_open(&registry, &store->tiers[i], PERSIST_FILES[i]);
        if (!persists[i]) {
            close_persistence();
            return G_SOURCE_CONTINUE;
        }

        /* Only adopt the saved history when nothing has been recorded yet.
         * Replaying older samples on top of newer ones would put the tier out
         * of order, and after a late mount a tier is usually still empty. */
        if (store_tier_count(store, i) == 0)
            persist_load(persists[i], store, i);

        persist_sync(persists[i], store, i);
        store_set_tier_callback(store, i, on_tier_sample, GUINT_TO_POINTER(i));
    }

    api.persist_path = persist_path(persists[STORE_TIERS - 1]);
    persist_retry_timer = 0;
    return G_SOURCE_REMOVE;
}

/* Rebuilding is the only way to pick up a filesystem that mounted after the
 * metric registry was built, and the registry's size is baked into the store
 * and the sample buffer. It is rare: only when the mount set actually changes,
 * which in practice means a disk appearing at boot or a card being inserted.
 * The coarse history is reloaded from disk afterwards and remaps by metric id,
 * so nothing durable is lost. */
static void rebuild_for_new_storage(void) {
    syslog(LOG_INFO, "storage changed, rediscovering metrics");

    close_persistence();
    alerts_free(alerts);
    store_free(store);
    collector_free(collector);
    metrics_registry_clear(&registry);
    g_free(sample_buffer);

    metrics_registry_init(&registry);
    collector = collector_new(&registry);
    sample_buffer = g_new0(float, registry.count);
    store = store_new(&registry, sample_interval_s);

    api.registry = &registry;
    api.store = store;
    api.persist_path = NULL;

    alerts = alerts_new(&api);
    alerts_set_notify(alerts, on_alert_changed, NULL);

    if (try_open_persistence(NULL) == G_SOURCE_CONTINUE && !persist_retry_timer)
        persist_retry_timer = g_timeout_add_seconds(PERSIST_RETRY_S, try_open_persistence, NULL);

    syslog(LOG_INFO, "now tracking %u metrics", registry.count);
}

static gboolean on_sample_tick(gpointer user_data) {
    (void)user_data;
    collector_sample(collector, sample_buffer);
    store_push(store, sample_buffer, (gint64)time(NULL));
    alerts_evaluate(alerts, sample_buffer);
    sse_broadcast();
    mqtt_tick(mqtt);
    influx_tick(influx);
    procs_tick(procs);
    g_strlcpy(api.mqtt_status, mqtt_state(mqtt), sizeof(api.mqtt_status));
    g_strlcpy(api.influx_status, influx_state(influx), sizeof(api.influx_status));
    return G_SOURCE_CONTINUE;
}

static gboolean parameter_is_yes(const char *name) {
    return g_strcmp0(parameter_value(name), "yes") == 0;
}

static void apply_mqtt_settings(void) {
    MqttConfig config;
    memset(&config, 0, sizeof(config));

    config.enabled = parameter_is_yes("MqttEnabled");
    config.tls = parameter_is_yes("MqttTls");
    config.discovery = parameter_is_yes("MqttDiscovery");
    config.discovery_all = parameter_is_yes("MqttDiscoveryAll");
    g_strlcpy(config.host, parameter_value("MqttHost"), sizeof(config.host));
    g_strlcpy(config.username, parameter_value("MqttUsername"), sizeof(config.username));
    g_strlcpy(config.password, parameter_value("MqttPassword"), sizeof(config.password));

    config.port = (guint)g_ascii_strtoull(parameter_value("MqttPort"), NULL, 10);
    if (config.port == 0 || config.port > 65535)
        config.port = config.tls ? 8883 : 1883;

    config.interval_s = (guint)g_ascii_strtoull(parameter_value("MqttInterval"), NULL, 10);
    config.interval_s = CLAMP(config.interval_s, 1, 3600);

    /* Default to the same shape as the device's own MQTT client so topics look
     * familiar next to it. */
    const char *prefix = parameter_value("MqttTopicPrefix");
    if (prefix && *prefix)
        g_strlcpy(config.topic_prefix, prefix, sizeof(config.topic_prefix));
    else
        g_snprintf(config.topic_prefix, sizeof(config.topic_prefix), "axis/%s/metrics",
                   api.device.serial[0] ? api.device.serial : "device");

    mqtt_apply(mqtt, &config);
}

static void apply_influx_settings(void) {
    InfluxConfig config;
    memset(&config, 0, sizeof(config));

    config.enabled = parameter_is_yes("InfluxEnabled");
    config.version = g_strcmp0(parameter_value("InfluxVersion"), "v1") == 0 ? INFLUX_V1 : INFLUX_V2;
    g_strlcpy(config.url, parameter_value("InfluxUrl"), sizeof(config.url));
    g_strlcpy(config.database, parameter_value("InfluxDatabase"), sizeof(config.database));
    g_strlcpy(config.org, parameter_value("InfluxOrg"), sizeof(config.org));
    g_strlcpy(config.token, parameter_value("InfluxToken"), sizeof(config.token));
    g_strlcpy(config.username, parameter_value("InfluxUsername"), sizeof(config.username));
    g_strlcpy(config.password, parameter_value("InfluxPassword"), sizeof(config.password));

    const char *measurement = parameter_value("InfluxMeasurement");
    g_strlcpy(config.measurement, measurement && *measurement ? measurement : "axis_metrics",
              sizeof(config.measurement));

    config.interval_s = (guint)g_ascii_strtoull(parameter_value("InfluxInterval"), NULL, 10);
    config.interval_s = CLAMP(config.interval_s, 1, 3600);

    influx_apply(influx, &config);
}

static void restart_sampler(void) {
    guint interval = (guint)g_ascii_strtoull(parameter_value("SampleInterval"), NULL, 10);
    interval = CLAMP(interval, 1, 10);
    if (interval == sample_interval_s && sample_timer)
        return;

    sample_interval_s = interval;
    if (sample_timer)
        g_source_remove(sample_timer);
    sample_timer = g_timeout_add_seconds(sample_interval_s, on_sample_tick, NULL);
    syslog(LOG_INFO, "sampling every %u s", sample_interval_s);
}

static void on_parameter_changed(const gchar *name, const gchar *value, gpointer user_data) {
    (void)user_data;
    Parameter *parameter = find_parameter_suffix(name);
    if (!parameter || g_strcmp0(parameter->value, value) == 0)
        return;

    g_free(parameter->value);
    parameter->value = g_strdup(value ? value : "");
    syslog(LOG_INFO, "parameter %s changed", parameter->name);
    restart_sampler();
    apply_mqtt_settings();
    apply_influx_settings();
}

/* ---------------------------------------------------------------- settings */

static void json_append_escaped(GString *out, const char *value) {
    for (const char *cursor = value ? value : ""; *cursor; cursor++) {
        switch (*cursor) {
        case '"':  g_string_append(out, "\\\""); break;
        case '\\': g_string_append(out, "\\\\"); break;
        case '\n': g_string_append(out, "\\n"); break;
        case '\r': g_string_append(out, "\\r"); break;
        case '\t': g_string_append(out, "\\t"); break;
        default:
            if ((unsigned char)*cursor < 0x20)
                g_string_append_printf(out, "\\u%04x", (unsigned char)*cursor);
            else
                g_string_append_c(out, *cursor);
        }
    }
}

/* Secrets are never returned; the UI only learns whether one is stored. */
static gchar *settings_json(void) {
    GString *out = g_string_new("{");

    for (gsize i = 0; i < G_N_ELEMENTS(parameters); i++) {
        const char *value = parameter_value(parameters[i].name);
        if (i)
            g_string_append_c(out, ',');
        g_string_append_c(out, '"');
        json_append_escaped(out, parameters[i].name);
        g_string_append(out, "\":\"");
        if (!parameters[i].secret)
            json_append_escaped(out, value);
        g_string_append_c(out, '"');
        if (parameters[i].secret) {
            g_string_append(out, ",\"");
            json_append_escaped(out, parameters[i].name);
            g_string_append_printf(out, "IsSet\":\"%s\"", *value ? "yes" : "no");
        }
    }

    g_string_append_c(out, '}');
    /* g_string_free(out, FALSE) inlines to g_string_free_and_steal() on newer
     * headers and breaks on AXIS OS 11.x runtimes; copy the buffer instead. */
    gchar *json = g_strdup(out->str);
    g_string_free(out, TRUE);
    return json;
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

    gchar *decoded = g_strdup(out->str);
    g_string_free(out, TRUE);
    return decoded;
}

/* Applies an application/x-www-form-urlencoded body, ignoring unknown keys. */
static gboolean apply_settings_body(const char *body) {
    gchar **pairs = g_strsplit(body ? body : "", "&", -1);
    gboolean changed = FALSE;

    for (gchar **pair = pairs; *pair; pair++) {
        gchar *separator = strchr(*pair, '=');
        if (!separator)
            continue;

        gchar *name = url_decode(*pair, (gsize)(separator - *pair));
        gchar *value = url_decode(separator + 1, strlen(separator + 1));
        Parameter *parameter = find_parameter(name);
        GError *error = NULL;

        if (!parameter) {
            syslog(LOG_WARNING, "rejected unknown parameter %s", name);
        } else if (g_strcmp0(parameter->value, value) == 0) {
            /* No change. */
        } else if (ax_parameter_set(parameter_handle, parameter->name, value, TRUE, &error)) {
            g_free(parameter->value);
            parameter->value = g_strdup(value);
            changed = TRUE;
        } else {
            syslog(LOG_ERR, "write parameter %s failed: %s", parameter->name,
                   error ? error->message : "unknown error");
        }

        g_clear_error(&error);
        g_free(name);
        g_free(value);
    }

    g_strfreev(pairs);
    return changed;
}

/* ------------------------------------------------------------- http server */

static void send_response(GOutputStream *out, const char *status, const char *content_type, const char *body) {
    gchar *response = g_strdup_printf("HTTP/1.1 %s\r\n"
                                      "Content-Type: %s\r\n"
                                      "Content-Length: %zu\r\n"
                                      "Cache-Control: no-store\r\n"
                                      "X-Content-Type-Options: nosniff\r\n"
                                      "Connection: close\r\n"
                                      "\r\n"
                                      "%s",
                                      status, content_type, strlen(body), body);
    g_output_stream_write_all(out, response, strlen(response), NULL, NULL, NULL);
    g_free(response);
}

/* The reverse proxy may or may not strip the apiPath prefix, so routes are
 * matched on the trailing segment rather than the full path. */
static gboolean route_is(const char *path, const char *name) {
    const char *found = strstr(path, name);
    if (!found)
        return FALSE;
    char next = found[strlen(name)];
    return next == '\0' || next == '?' || next == '/';
}

static void send_json(GOutputStream *out, gchar *json) {
    send_response(out, "200 OK", "application/json", json);
    g_free(json);
}

/* ---------------------------------------------------------- event stream */

/* Connections held open for server-sent events. The listener is synchronous,
 * so these sockets are non-blocking: a client that cannot keep up misses
 * frames rather than stalling the sampler. */
typedef struct {
    GSocketConnection *connection;
    guint stalled; /* Consecutive frames the client had no room for. */
} SseClient;

static GList *sse_clients;

/* A reader that never drains its socket would otherwise sit here forever,
 * since a full buffer looks the same as a slow client. */
#define SSE_MAX_STALLED 30

static void sse_drop(SseClient *client) {
    sse_clients = g_list_remove(sse_clients, client);
    g_io_stream_close(G_IO_STREAM(client->connection), NULL, NULL);
    g_object_unref(client->connection);
    g_free(client);
}

static void sse_add(GSocketConnection *connection) {
    GOutputStream *out = g_io_stream_get_output_stream(G_IO_STREAM(connection));
    static const char headers[] = "HTTP/1.1 200 OK\r\n"
                                  "Content-Type: text/event-stream\r\n"
                                  "Cache-Control: no-store\r\n"
                                  "X-Content-Type-Options: nosniff\r\n"
                                  "Connection: keep-alive\r\n\r\n";
    if (!g_output_stream_write_all(out, headers, sizeof(headers) - 1, NULL, NULL, NULL))
        return;

    g_socket_set_blocking(g_socket_connection_get_socket(connection), FALSE);

    SseClient *client = g_new0(SseClient, 1);
    client->connection = g_object_ref(connection);
    sse_clients = g_list_prepend(sse_clients, client);
}

static void sse_broadcast(void) {
    if (!sse_clients)
        return;

    gchar *json = api_current_json(&api);
    gchar *frame = g_strdup_printf("data: %s\n\n", json);
    gsize length = strlen(frame);
    g_free(json);

    for (GList *node = sse_clients; node;) {
        GList *next = node->next;
        SseClient *client = node->data;
        GOutputStream *out = g_io_stream_get_output_stream(G_IO_STREAM(client->connection));
        GError *error = NULL;
        gssize written = g_output_stream_write(out, frame, length, NULL, &error);

        if (written > 0) {
            client->stalled = 0;
        } else if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK)) {
            if (++client->stalled >= SSE_MAX_STALLED)
                sse_drop(client);
        } else {
            sse_drop(client);
        }

        g_clear_error(&error);
        node = next;
    }

    g_free(frame);
}

/* Returns TRUE when the connection must stay open, which only the event stream
 * needs. */
static gboolean handle_request(GSocketConnection *connection,
                              const char *method,
                              const char *path,
                              const char *body) {
    GOutputStream *out = g_io_stream_get_output_stream(G_IO_STREAM(connection));
    gboolean is_get = strcmp(method, "GET") == 0;

    if (is_get && route_is(path, "settings")) {
        send_json(out, settings_json());
    } else if (strcmp(method, "POST") == 0 && route_is(path, "settings")) {
        gboolean changed = apply_settings_body(body);
        send_response(out, "200 OK", "application/json", "{\"status\":\"ok\"}");
        if (changed) {
            restart_sampler();
            apply_mqtt_settings();
            apply_influx_settings();
        }
    } else if (is_get && route_is(path, "meta")) {
        send_json(out, api_meta_json(&api));
    } else if (is_get && route_is(path, "current")) {
        send_json(out, api_current_json(&api));
    } else if (is_get && route_is(path, "series")) {
        send_json(out, api_series_json(&api, path));
    } else if (is_get && route_is(path, "health")) {
        send_json(out, api_health_json(&api));
    } else if (is_get && route_is(path, "prometheus")) {
        gchar *text = api_prometheus_text(&api);
        send_response(out, "200 OK", "text/plain; version=0.0.4; charset=utf-8", text);
        g_free(text);
    } else if (is_get && route_is(path, "processes")) {
        gchar *limit = api_query_param(path, "limit");
        guint count = limit ? (guint)g_ascii_strtoull(limit, NULL, 10) : 12;
        g_free(limit);
        send_json(out, procs_json(procs, CLAMP(count, 1, 100)));
    } else if (is_get && route_is(path, "alerts")) {
        send_json(out, alerts_json(alerts));
    } else if (strcmp(method, "POST") == 0 && route_is(path, "rules")) {
        alerts_apply(alerts, body);
        send_json(out, alerts_json(alerts));
    } else if (strcmp(method, "POST") == 0 && route_is(path, "metrics")) {
        gchar *scope = api_query_param(body, "scope");
        gchar *disabled = api_query_param(body, "disabled");
        if (scope && disabled) {
            selection_set_disabled(api.selection,
                                   g_strcmp0(scope, "transmit") == 0 ? SELECT_TRANSMIT : SELECT_DISPLAY,
                                   disabled);
        }
        g_free(scope);
        g_free(disabled);
        send_response(out, "200 OK", "application/json", "{\"status\":\"ok\"}");
    } else if (is_get && route_is(path, "stream")) {
        sse_add(connection);
        return TRUE;
    } else {
        send_response(out, "404 Not Found", "application/json", "{\"error\":\"not found\"}");
    }
    return FALSE;
}

static gboolean on_http_request(GSocketService *service, GSocketConnection *connection, GObject *source,
                                gpointer user_data) {
    (void)service;
    (void)source;
    (void)user_data;

    GInputStream *in = g_io_stream_get_input_stream(G_IO_STREAM(connection));
    GOutputStream *out = g_io_stream_get_output_stream(G_IO_STREAM(connection));
    gchar buffer[MAX_REQUEST];
    gsize total = 0;
    gsize header_length = 0;
    glong content_length = 0;
    gboolean keep_open = FALSE;

    while (total < sizeof(buffer) - 1) {
        gssize got = g_input_stream_read(in, buffer + total, sizeof(buffer) - 1 - total, NULL, NULL);
        if (got <= 0)
            break;
        total += (gsize)got;
        buffer[total] = '\0';

        gchar *header_end = strstr(buffer, "\r\n\r\n");
        if (!header_end)
            continue;
        header_length = (gsize)(header_end - buffer) + 4;

        if (content_length == 0) {
            gchar *headers = g_ascii_strdown(buffer, (gssize)header_length);
            gchar *field = strstr(headers, "content-length:");
            if (field)
                content_length = strtol(field + strlen("content-length:"), NULL, 10);
            g_free(headers);
        }
        if (total - header_length >= (gsize)content_length)
            break;
    }

    if (header_length == 0) {
        send_response(out, "400 Bad Request", "application/json", "{\"error\":\"bad request\"}");
    } else {
        gchar method[8] = {0};
        gchar *path = g_malloc0(MAX_PATH_LENGTH);
        if (sscanf(buffer, "%7s %8191s", method, path) == 2) {
            gchar *request_body = g_strndup(buffer + header_length, total - header_length);
            keep_open = handle_request(connection, method, path, request_body);
            g_free(request_body);
        } else {
            send_response(out, "400 Bad Request", "application/json", "{\"error\":\"bad request\"}");
        }
        g_free(path);
    }

    if (!keep_open)
        g_io_stream_close(G_IO_STREAM(connection), NULL, NULL);
    return TRUE;
}

static gboolean start_http_server(void) {
    GError *error = NULL;
    GSocketService *service = g_socket_service_new();
    GInetAddress *address = g_inet_address_new_loopback(G_SOCKET_FAMILY_IPV4);
    GSocketAddress *socket_address = g_inet_socket_address_new(address, SETTINGS_HTTP_PORT);
    gboolean added = g_socket_listener_add_address(G_SOCKET_LISTENER(service), socket_address,
                                                   G_SOCKET_TYPE_STREAM, G_SOCKET_PROTOCOL_TCP, NULL, NULL, &error);

    g_object_unref(socket_address);
    g_object_unref(address);

    if (!added) {
        /* A failure here usually means another ACAP already claimed the port,
         * which would silently forward this app's API to that app. */
        syslog(LOG_ERR, "http server cannot bind 127.0.0.1:%d: %s", SETTINGS_HTTP_PORT,
               error ? error->message : "unknown error");
        g_clear_error(&error);
        g_object_unref(service);
        return FALSE;
    }

    g_signal_connect(service, "incoming", G_CALLBACK(on_http_request), NULL);
    g_socket_service_start(service);
    syslog(LOG_INFO, "http server listening on 127.0.0.1:%d", SETTINGS_HTTP_PORT);
    return TRUE;
}

/* -------------------------------------------------------------------- main */

static gboolean on_terminate(gpointer user_data) {
    (void)user_data;
    g_main_loop_quit(main_loop);
    return G_SOURCE_REMOVE;
}

int main(void) {
    GError *error = NULL;

    openlog(APP_NAME, LOG_PID, LOG_USER);

    parameter_handle = ax_parameter_new(APP_NAME, &error);
    if (!parameter_handle) {
        syslog(LOG_ERR, "ax_parameter_new failed: %s", error ? error->message : "unknown error");
        g_clear_error(&error);
        return EXIT_FAILURE;
    }

    for (gsize i = 0; i < G_N_ELEMENTS(parameters); i++) {
        load_parameter(&parameters[i]);
        if (!ax_parameter_register_callback(parameter_handle, parameters[i].name, on_parameter_changed, NULL,
                                            &error)) {
            syslog(LOG_WARNING, "callback for %s failed: %s", parameters[i].name,
                   error ? error->message : "unknown error");
        }
        g_clear_error(&error);
    }

    metrics_registry_init(&registry);
    vapix_init();
    collector = collector_new(&registry);
    sample_buffer = g_new0(float, registry.count);

    guint interval = (guint)g_ascii_strtoull(parameter_value("SampleInterval"), NULL, 10);
    store = store_new(&registry, CLAMP(interval, 1, 10));

    api.registry = &registry;
    api.store = store;
    api.selection = selection_new();
    api.app_version = APP_VERSION;
    api.started = (gint64)time(NULL);
    api_read_device_info(&api.device, parameter_handle);

    mqtt = mqtt_new(&api);
    influx = influx_new(&api);
    procs = procs_new();
    alerts = alerts_new(&api);
    alerts_set_notify(alerts, on_alert_changed, NULL);

    syslog(LOG_INFO, "%s %s on %s (%s), %u metrics", APP_NAME, APP_VERSION,
           api.device.model[0] ? api.device.model : "unknown device", api.device.firmware, registry.count);

    main_loop = g_main_loop_new(NULL, FALSE);
    g_unix_signal_add(SIGTERM, on_terminate, NULL);
    g_unix_signal_add(SIGINT, on_terminate, NULL);

    start_http_server();
    restart_sampler();
    apply_mqtt_settings();
    apply_influx_settings();

    /* Every tier is persisted, so the short windows survive a restart too. */
    if (try_open_persistence(NULL) == G_SOURCE_CONTINUE)
        persist_retry_timer = g_timeout_add_seconds(PERSIST_RETRY_S, try_open_persistence, NULL);

    on_sample_tick(NULL); /* Seed the counters so the first tick yields real rates. */

    g_main_loop_run(main_loop);

    if (sample_timer)
        g_source_remove(sample_timer);
    if (persist_retry_timer)
        g_source_remove(persist_retry_timer);
    for (gsize i = 0; i < G_N_ELEMENTS(parameters); i++)
        g_free(parameters[i].value);
    g_free(sample_buffer);
    alerts_free(alerts);
    mqtt_free(mqtt);
    influx_free(influx);
    procs_free(procs);
    selection_free(api.selection);
    close_persistence();
    store_free(store);
    collector_free(collector);
    metrics_registry_clear(&registry);
    vapix_cleanup();
    ax_parameter_free(parameter_handle);
    g_main_loop_unref(main_loop);
    closelog();
    return EXIT_SUCCESS;
}
