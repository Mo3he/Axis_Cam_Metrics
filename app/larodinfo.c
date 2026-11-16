/*
 * Asks larod which models are loaded and on which device. Linking liblarod
 * directly would stop the app starting on devices that do not ship it, so the
 * handful of calls needed are resolved with dlopen.
 */

#include "larodinfo.h"
#include "api.h"

#include <dlfcn.h>
#include <larod.h>
#include <string.h>
#include <syslog.h>

typedef struct {
    gchar *name;
    gchar *device;
    guint64 bytes;
    gboolean accelerator;
} ModelEntry;

struct LarodInfo {
    void *lib;
    larodConnection *conn;
    GArray *models; /* ModelEntry, from the last successful read */
    __typeof__(larodConnect) *connect;
    __typeof__(larodDisconnect) *disconnect;
    __typeof__(larodClearError) *clear_error;
    __typeof__(larodListDevices) *list_devices;
    __typeof__(larodGetDeviceName) *device_name;
    __typeof__(larodGetNumSessions) *num_sessions;
    __typeof__(larodGetModels) *get_models;
    __typeof__(larodDestroyModels) *destroy_models;
    __typeof__(larodGetModelDevice) *model_device;
    __typeof__(larodGetModelSize) *model_size;
    __typeof__(larodGetModelName) *model_name;
};

static void clear_entry(gpointer data) {
    ModelEntry *entry = data;
    g_free(entry->name);
    g_free(entry->device);
}

/* Covers the DLPU backends ("a9-dlpu-native", "axis-a8-dlpu-tflite"), the
 * ARTPEC-7 GPU and Ambarella CVflow, while leaving out the CPU backends. */
static gboolean is_accelerator(const char *device) {
    return device && !strstr(device, "cpu");
}

static gboolean ensure_connected(LarodInfo *info) {
    if (info->conn)
        return TRUE;
    larodError *error = NULL;
    if (!info->connect(&info->conn, &error)) {
        info->clear_error(&error);
        info->conn = NULL;
        return FALSE;
    }
    return TRUE;
}

static void drop_connection(LarodInfo *info) {
    if (info->conn)
        info->disconnect(&info->conn, NULL);
    info->conn = NULL;
}

static gboolean has_accelerator(LarodInfo *info) {
    larodError *error = NULL;
    size_t count = 0;
    const larodDevice **devices = info->list_devices(info->conn, &count, &error);
    if (!devices) {
        info->clear_error(&error);
        return FALSE;
    }
    for (size_t i = 0; i < count; i++) {
        if (is_accelerator(info->device_name(devices[i], NULL)))
            return TRUE;
    }
    return FALSE;
}

LarodInfo *larod_info_open(void) {
    void *lib = dlopen("liblarod.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!lib)
        return NULL;

    LarodInfo *info = g_new0(LarodInfo, 1);
    info->lib = lib;
    info->models = g_array_new(FALSE, TRUE, sizeof(ModelEntry));
    g_array_set_clear_func(info->models, clear_entry);
#define RESOLVE(field, symbol)                                                   \
    if (!(info->field = (__typeof__(info->field))dlsym(lib, #symbol))) {         \
        syslog(LOG_WARNING, "larod: missing symbol %s", #symbol);                \
        larod_info_close(info);                                                  \
        return NULL;                                                             \
    }
    RESOLVE(connect, larodConnect);
    RESOLVE(disconnect, larodDisconnect);
    RESOLVE(clear_error, larodClearError);
    RESOLVE(list_devices, larodListDevices);
    RESOLVE(device_name, larodGetDeviceName);
    RESOLVE(num_sessions, larodGetNumSessions);
    RESOLVE(get_models, larodGetModels);
    RESOLVE(destroy_models, larodDestroyModels);
    RESOLVE(model_device, larodGetModelDevice);
    RESOLVE(model_size, larodGetModelSize);
    RESOLVE(model_name, larodGetModelName);
#undef RESOLVE

    if (!ensure_connected(info) || !has_accelerator(info)) {
        larod_info_close(info);
        return NULL;
    }
    return info;
}

gboolean larod_info_read(LarodInfo *info, LarodStats *stats) {
    memset(stats, 0, sizeof(*stats));
    if (!info || !ensure_connected(info))
        return FALSE;

    larodError *error = NULL;
    uint64_t sessions = 0;
    if (!info->num_sessions(info->conn, &sessions, &error)) {
        /* larod restarted under us; reconnect on the next sample. */
        info->clear_error(&error);
        drop_connection(info);
        return FALSE;
    }
    stats->sessions = sessions > 0 ? sessions - 1 : 0;

    size_t count = 0;
    larodModel **models = info->get_models(info->conn, &count, &error);
    if (!models) {
        info->clear_error(&error);
        drop_connection(info);
        return FALSE;
    }
    g_array_set_size(info->models, 0);
    for (size_t i = 0; i < count; i++) {
        const larodDevice *device = info->model_device(models[i], NULL);
        const char *device_name = device ? info->device_name(device, NULL) : NULL;
        const char *name = info->model_name(models[i], NULL);
        ModelEntry entry = {
            .name = g_strdup(name && *name ? name : "unnamed"),
            .device = g_strdup(device_name ? device_name : "unknown"),
            .bytes = info->model_size(models[i], NULL),
            .accelerator = is_accelerator(device_name),
        };
        g_array_append_val(info->models, entry);
        if (!entry.accelerator)
            continue;
        stats->models++;
        stats->model_bytes += entry.bytes;
    }
    info->destroy_models(&models, count);
    return TRUE;
}

void larod_info_models_json(const LarodInfo *info, GString *out) {
    g_string_append_c(out, '[');
    for (guint i = 0; info && i < info->models->len; i++) {
        const ModelEntry *entry = &g_array_index(info->models, ModelEntry, i);
        if (i)
            g_string_append_c(out, ',');
        g_string_append(out, "{\"name\":\"");
        api_append_json_escaped(out, entry->name);
        g_string_append(out, "\",\"device\":\"");
        api_append_json_escaped(out, entry->device);
        g_string_append_printf(out, "\",\"bytes\":%" G_GUINT64_FORMAT ",\"accelerator\":%s}",
                               entry->bytes, entry->accelerator ? "true" : "false");
    }
    g_string_append_c(out, ']');
}

void larod_info_close(LarodInfo *info) {
    if (!info)
        return;
    drop_connection(info);
    if (info->models)
        g_array_free(info->models, TRUE);
    if (info->lib)
        dlclose(info->lib);
    g_free(info);
}
