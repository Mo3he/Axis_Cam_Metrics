/*
 * InfluxDB writer.
 *
 * Speaks line protocol to either a 1.x server (/write?db=) or a 2.x server
 * (/api/v2/write?org=&bucket=), chosen by a setting, because both are still
 * widely deployed and the payload is identical either way.
 *
 * curl blocks, and a broker that stops answering would otherwise stall the
 * sampler and with it the whole UI, so the request runs on a worker thread. The
 * queue holds a single payload: metrics are a live view, and a stale sample is
 * worth less than the next one.
 */

#include "influx.h"

#include <curl/curl.h>
#include <math.h>
#include <string.h>
#include <syslog.h>
#include <time.h>

#define INFLUX_TIMEOUT 15L

struct Influx {
    const Api *api;
    InfluxConfig config;
    GThread *worker;
    GAsyncQueue *queue;
    gboolean stopping;

    gint64 last_write;
    GMutex state_lock;
    char state[32];
};

/* Sent through the queue to wake the worker for shutdown. */
static const char QUIT[] = "";

static void set_state(Influx *influx, const char *state) {
    g_mutex_lock(&influx->state_lock);
    g_strlcpy(influx->state, state, sizeof(influx->state));
    g_mutex_unlock(&influx->state_lock);
}

const char *influx_state(const Influx *influx) {
    return influx && influx->config.enabled ? influx->state : "disabled";
}

/* ------------------------------------------------------------ line protocol */

/* Tag keys and values, and field keys, escape commas, equals signs and spaces.
 * Metric ids never contain them, but the model and serial can. */
static void append_escaped_tag(GString *out, const char *value) {
    for (const char *c = value ? value : ""; *c; c++) {
        if (*c == ',' || *c == '=' || *c == ' ' || *c == '\\')
            g_string_append_c(out, '\\');
        g_string_append_c(out, *c);
    }
}

static void append_escaped_measurement(GString *out, const char *value) {
    for (const char *c = value ? value : ""; *c; c++) {
        if (*c == ',' || *c == ' ' || *c == '\\')
            g_string_append_c(out, '\\');
        g_string_append_c(out, *c);
    }
}

/* One line carrying every transmitted metric as a field. Splitting per metric
 * would multiply the tag set by a hundred for no gain: they all share the same
 * timestamp and device. */
static gchar *build_payload(Influx *influx) {
    const Api *api = influx->api;
    float *values = g_new(float, api->registry->count);
    gint64 timestamp = 0;

    if (!store_latest(api->store, values, &timestamp)) {
        g_free(values);
        return NULL;
    }

    GString *line = g_string_new(NULL);
    append_escaped_measurement(line, influx->config.measurement);

    g_string_append(line, ",device=");
    append_escaped_tag(line, api->device.serial[0] ? api->device.serial : "unknown");
    g_string_append(line, ",model=");
    append_escaped_tag(line, api->device.model[0] ? api->device.model : "unknown");

    guint fields = 0;
    for (guint i = 0; i < api->registry->count; i++) {
        const char *id = api->registry->defs[i].id;
        if (isnan(values[i]) || !selection_enabled(api->selection, SELECT_TRANSMIT, id))
            continue;

        char buf[G_ASCII_DTOSTR_BUF_SIZE];
        g_ascii_formatd(buf, sizeof(buf), "%.7g", values[i]);
        g_string_append_c(line, fields++ ? ',' : ' ');
        append_escaped_tag(line, id);
        g_string_append_c(line, '=');
        g_string_append(line, buf);
    }

    g_free(values);
    if (fields == 0) {
        g_string_free(line, TRUE);
        return NULL;
    }

    g_string_append_printf(line, " %" G_GINT64_FORMAT "\n", timestamp);
    return g_string_free(line, FALSE);
}

/* ----------------------------------------------------------------- worker */

static gchar *build_url(const InfluxConfig *config) {
    gchar *base = g_strdup(config->url);
    gsize length = strlen(base);
    while (length && base[length - 1] == '/')
        base[--length] = '\0';

    CURL *escaper = curl_easy_init();
    char *database = curl_easy_escape(escaper, config->database, 0);
    char *org = curl_easy_escape(escaper, config->org, 0);

    gchar *url;
    if (config->version == INFLUX_V2) {
        url = g_strdup_printf("%s/api/v2/write?org=%s&bucket=%s&precision=s", base, org ? org : "",
                              database ? database : "");
    } else {
        url = g_strdup_printf("%s/write?db=%s&precision=s", base, database ? database : "");
    }

    curl_free(database);
    curl_free(org);
    curl_easy_cleanup(escaper);
    g_free(base);
    return url;
}

static size_t discard(void *contents, size_t size, size_t count, void *user_data) {
    (void)contents;
    (void)user_data;
    return size * count;
}

static void write_payload(Influx *influx, const InfluxConfig *config, const char *payload) {
    CURL *handle = curl_easy_init();
    if (!handle) {
        set_state(influx, "error");
        return;
    }

    gchar *url = build_url(config);
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: text/plain; charset=utf-8");

    gchar *authorization = NULL;
    if (config->version == INFLUX_V2 && config->token[0]) {
        authorization = g_strdup_printf("Authorization: Token %s", config->token);
        headers = curl_slist_append(headers, authorization);
    } else if (config->version == INFLUX_V1 && config->username[0]) {
        curl_easy_setopt(handle, CURLOPT_USERNAME, config->username);
        curl_easy_setopt(handle, CURLOPT_PASSWORD, config->password);
    }

    curl_easy_setopt(handle, CURLOPT_URL, url);
    curl_easy_setopt(handle, CURLOPT_POST, 1L);
    curl_easy_setopt(handle, CURLOPT_POSTFIELDS, payload);
    curl_easy_setopt(handle, CURLOPT_POSTFIELDSIZE, (long)strlen(payload));
    curl_easy_setopt(handle, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, discard);
    curl_easy_setopt(handle, CURLOPT_TIMEOUT, INFLUX_TIMEOUT);
    curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 1L);

    CURLcode rc = curl_easy_perform(handle);
    long status = 0;
    curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &status);

    if (rc != CURLE_OK) {
        set_state(influx, "error");
        syslog(LOG_WARNING, "influx write failed: %s", curl_easy_strerror(rc));
    } else if (status == 401 || status == 403) {
        set_state(influx, "unauthorized");
        syslog(LOG_WARNING, "influx rejected our credentials (%ld)", status);
    } else if (status < 200 || status >= 300) {
        set_state(influx, "error");
        syslog(LOG_WARNING, "influx returned %ld", status);
    } else {
        set_state(influx, "connected");
    }

    curl_slist_free_all(headers);
    g_free(authorization);
    g_free(url);
    curl_easy_cleanup(handle);
}

static gpointer worker_main(gpointer user_data) {
    Influx *influx = user_data;

    for (;;) {
        gchar *payload = g_async_queue_pop(influx->queue);
        if (payload == (gchar *)QUIT)
            break;

        /* Copied under the lock because settings can change while we block on
         * the network. */
        g_mutex_lock(&influx->state_lock);
        InfluxConfig config = influx->config;
        g_mutex_unlock(&influx->state_lock);

        if (config.enabled && config.url[0])
            write_payload(influx, &config, payload);
        g_free(payload);
    }
    return NULL;
}

/* ---------------------------------------------------------------- lifecycle */

Influx *influx_new(const Api *api) {
    Influx *influx = g_new0(Influx, 1);
    influx->api = api;
    influx->queue = g_async_queue_new();
    g_mutex_init(&influx->state_lock);
    g_strlcpy(influx->state, "disabled", sizeof(influx->state));
    influx->worker = g_thread_new("influx", worker_main, influx);
    return influx;
}

void influx_free(Influx *influx) {
    if (!influx)
        return;

    influx->stopping = TRUE;
    g_async_queue_push(influx->queue, (gpointer)QUIT);
    g_thread_join(influx->worker);

    gchar *pending;
    while ((pending = g_async_queue_try_pop(influx->queue)))
        if (pending != (gchar *)QUIT)
            g_free(pending);

    g_async_queue_unref(influx->queue);
    g_mutex_clear(&influx->state_lock);
    g_free(influx);
}

void influx_apply(Influx *influx, const InfluxConfig *config) {
    if (!influx)
        return;

    gboolean was_enabled = influx->config.enabled;

    g_mutex_lock(&influx->state_lock);
    influx->config = *config;
    if (!config->enabled)
        g_strlcpy(influx->state, "disabled", sizeof(influx->state));
    else if (!was_enabled)
        g_strlcpy(influx->state, "pending", sizeof(influx->state));
    g_mutex_unlock(&influx->state_lock);

    if (config->enabled && !was_enabled)
        influx->last_write = 0; /* Write on the next tick rather than waiting out an interval. */
}

void influx_tick(Influx *influx) {
    if (!influx || !influx->config.enabled || influx->stopping || !influx->config.url[0])
        return;

    gint64 now = g_get_monotonic_time() / G_USEC_PER_SEC;
    if (influx->last_write && now - influx->last_write < (gint64)influx->config.interval_s)
        return;
    influx->last_write = now;

    /* A payload still queued means the previous write has not finished. Drop it
     * so a slow server cannot build a backlog of stale samples. */
    gchar *stale;
    while ((stale = g_async_queue_try_pop(influx->queue)))
        g_free(stale);

    gchar *payload = build_payload(influx);
    if (payload)
        g_async_queue_push(influx->queue, payload);
}
