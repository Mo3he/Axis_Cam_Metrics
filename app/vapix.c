/*
 * Minimal local VAPIX client.
 *
 * A few metrics have no /proc equivalent: named temperature sensors, fan RPM
 * and per-port PoE power all come from CGIs. AXIS OS hands ACAPs a service
 * account over D-Bus for exactly this, which avoids storing any credentials.
 */

#include "vapix.h"

#include <curl/curl.h>
#include <gio/gio.h>
#include <string.h>
#include <syslog.h>

/* The service account is only accepted on this loopback alias. */
#define VAPIX_HOST    "127.0.0.12"
#define VAPIX_TIMEOUT 8L

static CURL *handle;
static char *credentials;

typedef struct {
    char *data;
    gsize length;
} Buffer;

static size_t on_data(void *contents, size_t size, size_t count, void *user_data) {
    Buffer *buffer = user_data;
    gsize chunk = size * count;
    buffer->data = g_realloc(buffer->data, buffer->length + chunk + 1);
    memcpy(buffer->data + buffer->length, contents, chunk);
    buffer->length += chunk;
    buffer->data[buffer->length] = '\0';
    return chunk;
}

static char *fetch_credentials(void) {
    GError *error = NULL;
    GDBusConnection *connection = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
    if (!connection) {
        syslog(LOG_WARNING, "vapix: no system bus: %s", error ? error->message : "unknown");
        g_clear_error(&error);
        return NULL;
    }

    GVariant *result = g_dbus_connection_call_sync(
        connection, "com.axis.HTTPConf1", "/com/axis/HTTPConf1/VAPIXServiceAccounts1",
        "com.axis.HTTPConf1.VAPIXServiceAccounts1", "GetCredentials",
        g_variant_new("(s)", "acap"), NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);

    if (!result) {
        syslog(LOG_WARNING, "vapix: GetCredentials failed: %s", error ? error->message : "unknown");
        g_clear_error(&error);
        g_object_unref(connection);
        return NULL;
    }

    gchar *value = NULL;
    g_variant_get(result, "(s)", &value);
    g_variant_unref(result);
    g_object_unref(connection);
    return value;
}

void vapix_init(void) {
    credentials = fetch_credentials();
    if (!credentials) {
        syslog(LOG_INFO, "vapix unavailable, CGI-backed metrics disabled");
        return;
    }
    handle = curl_easy_init();
    if (handle)
        syslog(LOG_INFO, "vapix service account acquired");
}

gboolean vapix_available(void) {
    return handle != NULL && credentials != NULL;
}

gchar *vapix_get(const char *endpoint) {
    if (!vapix_available())
        return NULL;

    char url[512];
    g_snprintf(url, sizeof(url), "http://%s/axis-cgi/%s", VAPIX_HOST, endpoint);

    Buffer buffer = {NULL, 0};
    curl_easy_reset(handle);
    curl_easy_setopt(handle, CURLOPT_URL, url);
    curl_easy_setopt(handle, CURLOPT_USERPWD, credentials);
    curl_easy_setopt(handle, CURLOPT_HTTPAUTH, (long)CURLAUTH_ANY);
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, on_data);
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, &buffer);
    curl_easy_setopt(handle, CURLOPT_TIMEOUT, VAPIX_TIMEOUT);
    curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);

    CURLcode rc = curl_easy_perform(handle);
    long status = 0;
    curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &status);

    if (rc != CURLE_OK || status != 200) {
        g_free(buffer.data);
        return NULL;
    }
    return buffer.data;
}

void vapix_cleanup(void) {
    if (handle)
        curl_easy_cleanup(handle);
    handle = NULL;
    g_free(credentials);
    credentials = NULL;
}
