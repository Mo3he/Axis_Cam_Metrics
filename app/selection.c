/*
 * Which metrics the dashboard shows, and which leave the device.
 *
 * The two are independent: a busy NVR might chart 40 series locally but publish
 * only a handful over MQTT, or collect everything for Prometheus while keeping
 * the dashboard readable.
 */

#include "selection.h"

#include <string.h>
#include <syslog.h>

#define SELECTION_FILE "/usr/local/packages/Metrics/localdata/selection.ini"

struct Selection {
    GHashTable *disabled[SELECT_SCOPES];
};

const char *selection_scope_name(SelectScope scope) {
    return scope == SELECT_TRANSMIT ? "transmit" : "display";
}

static void load(Selection *selection) {
    GKeyFile *file = g_key_file_new();
    if (g_key_file_load_from_file(file, SELECTION_FILE, G_KEY_FILE_NONE, NULL)) {
        for (int scope = 0; scope < SELECT_SCOPES; scope++) {
            gchar *csv = g_key_file_get_string(file, selection_scope_name(scope), "disabled", NULL);
            if (!csv)
                continue;
            gchar **ids = g_strsplit(csv, ",", -1);
            for (gchar **id = ids; *id; id++) {
                gchar *trimmed = g_strstrip(g_strdup(*id));
                if (*trimmed)
                    g_hash_table_add(selection->disabled[scope], trimmed);
                else
                    g_free(trimmed);
            }
            g_strfreev(ids);
            g_free(csv);
        }
    }
    g_key_file_free(file);
}

static void save(Selection *selection) {
    GKeyFile *file = g_key_file_new();
    for (int scope = 0; scope < SELECT_SCOPES; scope++) {
        GString *csv = g_string_new(NULL);
        GHashTableIter iter;
        gpointer key;
        g_hash_table_iter_init(&iter, selection->disabled[scope]);
        while (g_hash_table_iter_next(&iter, &key, NULL)) {
            if (csv->len)
                g_string_append_c(csv, ',');
            g_string_append(csv, key);
        }
        g_key_file_set_string(file, selection_scope_name(scope), "disabled", csv->str);
        g_string_free(csv, TRUE);
    }

    GError *error = NULL;
    if (!g_key_file_save_to_file(file, SELECTION_FILE, &error)) {
        syslog(LOG_WARNING, "cannot save metric selection: %s", error ? error->message : "unknown");
        g_clear_error(&error);
    }
    g_key_file_free(file);
}

Selection *selection_new(void) {
    Selection *selection = g_new0(Selection, 1);
    for (int scope = 0; scope < SELECT_SCOPES; scope++)
        selection->disabled[scope] = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    load(selection);
    return selection;
}

void selection_free(Selection *selection) {
    if (!selection)
        return;
    for (int scope = 0; scope < SELECT_SCOPES; scope++)
        g_hash_table_destroy(selection->disabled[scope]);
    g_free(selection);
}

gboolean selection_enabled(const Selection *selection, SelectScope scope, const char *id) {
    if (!selection || scope >= SELECT_SCOPES)
        return TRUE;
    return !g_hash_table_contains(selection->disabled[scope], id);
}

void selection_set_disabled(Selection *selection, SelectScope scope, const char *csv) {
    if (scope >= SELECT_SCOPES)
        return;

    g_hash_table_remove_all(selection->disabled[scope]);
    gchar **ids = g_strsplit(csv ? csv : "", ",", -1);
    for (gchar **id = ids; *id; id++) {
        gchar *trimmed = g_strstrip(g_strdup(*id));
        if (*trimmed)
            g_hash_table_add(selection->disabled[scope], trimmed);
        else
            g_free(trimmed);
    }
    g_strfreev(ids);
    save(selection);
}
