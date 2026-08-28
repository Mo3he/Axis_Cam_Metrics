/*
 * Threshold alerting.
 *
 * Each rule watches one metric and fires once the condition has held for its
 * duration, which keeps a single noisy sample from raising an alarm. Firing and
 * clearing are published as stateful Axis events, so they show up in the
 * device's own action rules alongside motion and tampering, and can drive a
 * recording or a notification without this app knowing anything about it.
 */

#include "alerts.h"

#include <axsdk/axevent.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>

#define RULES_FILE   "/usr/local/packages/Metrics/localdata/alerts.ini"
#define EVENT_TOPIC  "Metrics"
#define MAX_RULES    64

struct Alerts {
    const Api *api;
    AlertRule rules[MAX_RULES];
    guint count;
    AXEventHandler *events;
    AlertNotify notify;
    gpointer notify_data;
};

/* Sensible defaults so the app is useful before anyone configures anything.
 * Metric ids that do not exist on this device are skipped at load. Filesystem
 * space is not listed here; a rule is generated per mount below. */
static const AlertRule BUILTIN[] = {
    {"cpu_high", "CPU usage high", "cpu.usage", ALERT_ABOVE, 90, 300, TRUE, TRUE, 0, 0, 0, 0, 0},
    {"memory_high", "Memory usage high", "mem.usage", ALERT_ABOVE, 90, 300, TRUE, TRUE, 0, 0, 0, 0, 0},
    {"temperature_high", "Temperature high", "sensor.CPU", ALERT_ABOVE, 85, 120, TRUE, TRUE, 0, 0, 0, 0, 0},
};

/* ------------------------------------------------------------- persistence */

static const char *op_name(AlertOp op) {
    return op == ALERT_BELOW ? "below" : "above";
}

static AlertOp op_from(const char *name) {
    return g_strcmp0(name, "below") == 0 ? ALERT_BELOW : ALERT_ABOVE;
}

static AlertRule *find_rule(Alerts *alerts, const char *id) {
    for (guint i = 0; i < alerts->count; i++) {
        if (g_strcmp0(alerts->rules[i].id, id) == 0)
            return &alerts->rules[i];
    }
    return NULL;
}

static void save_rules(Alerts *alerts) {
    GKeyFile *file = g_key_file_new();
    for (guint i = 0; i < alerts->count; i++) {
        AlertRule *rule = &alerts->rules[i];
        g_key_file_set_string(file, rule->id, "name", rule->name);
        g_key_file_set_string(file, rule->id, "metric", rule->metric);
        g_key_file_set_string(file, rule->id, "op", op_name(rule->op));
        g_key_file_set_double(file, rule->id, "threshold", rule->threshold);
        g_key_file_set_integer(file, rule->id, "duration", (gint)rule->duration_s);
        g_key_file_set_boolean(file, rule->id, "enabled", rule->enabled);
        g_key_file_set_boolean(file, rule->id, "builtin", rule->builtin);
    }
    GError *error = NULL;
    if (!g_key_file_save_to_file(file, RULES_FILE, &error)) {
        syslog(LOG_WARNING, "cannot save alert rules: %s", error ? error->message : "unknown");
        g_clear_error(&error);
    }
    g_key_file_free(file);
}

static gboolean add_rule(Alerts *alerts, const AlertRule *source) {
    if (alerts->count >= MAX_RULES || find_rule(alerts, source->id))
        return FALSE;
    alerts->rules[alerts->count++] = *source;
    return TRUE;
}

static void load_rules(Alerts *alerts) {
    GKeyFile *file = g_key_file_new();
    if (g_key_file_load_from_file(file, RULES_FILE, G_KEY_FILE_NONE, NULL)) {
        gsize n = 0;
        gchar **groups = g_key_file_get_groups(file, &n);
        for (gsize i = 0; i < n && alerts->count < MAX_RULES; i++) {
            AlertRule rule;
            memset(&rule, 0, sizeof(rule));
            g_strlcpy(rule.id, groups[i], sizeof(rule.id));

            gchar *name = g_key_file_get_string(file, groups[i], "name", NULL);
            gchar *metric = g_key_file_get_string(file, groups[i], "metric", NULL);
            gchar *op = g_key_file_get_string(file, groups[i], "op", NULL);
            g_strlcpy(rule.name, name ? name : groups[i], sizeof(rule.name));
            g_strlcpy(rule.metric, metric ? metric : "", sizeof(rule.metric));
            rule.op = op_from(op);
            rule.threshold = g_key_file_get_double(file, groups[i], "threshold", NULL);
            rule.duration_s = (guint)g_key_file_get_integer(file, groups[i], "duration", NULL);
            rule.enabled = g_key_file_get_boolean(file, groups[i], "enabled", NULL);
            rule.builtin = g_key_file_get_boolean(file, groups[i], "builtin", NULL);
            g_free(name);
            g_free(metric);
            g_free(op);

            if (rule.metric[0])
                add_rule(alerts, &rule);
        }
        g_strfreev(groups);
    }
    g_key_file_free(file);
}

/* ------------------------------------------------------------------ events */

/* Declared stateful so the device shows one active/inactive condition per rule
 * rather than a stream of pulses. */
static void declare_rule(Alerts *alerts, AlertRule *rule) {
    if (!alerts->events || rule->declaration)
        return;

    AXEventKeyValueSet *set = ax_event_key_value_set_new();
    gint dummy = 0;
    ax_event_key_value_set_add_key_value(set, "topic0", "tnsaxis", "CameraApplicationPlatform",
                                         AX_VALUE_TYPE_STRING, NULL);
    ax_event_key_value_set_add_key_value(set, "topic1", "tnsaxis", EVENT_TOPIC,
                                         AX_VALUE_TYPE_STRING, NULL);
    ax_event_key_value_set_add_nice_names(set, "topic1", "tnsaxis", EVENT_TOPIC, "Metrics", NULL);
    ax_event_key_value_set_add_key_value(set, "topic2", "tnsaxis", rule->id, AX_VALUE_TYPE_STRING, NULL);
    ax_event_key_value_set_add_nice_names(set, "topic2", "tnsaxis", rule->id, rule->name, NULL);
    ax_event_key_value_set_add_key_value(set, "state", NULL, &dummy, AX_VALUE_TYPE_BOOL, NULL);
    ax_event_key_value_set_mark_as_data(set, "state", NULL, NULL);

    guint declaration = 0;
    if (ax_event_handler_declare(alerts->events, set, 0, &declaration, NULL, NULL, NULL))
        rule->declaration = declaration;
    else
        syslog(LOG_WARNING, "could not declare event for rule %s", rule->id);

    ax_event_key_value_set_free(set);
}

static void send_state(Alerts *alerts, AlertRule *rule) {
    if (!alerts->events || !rule->declaration)
        return;

    AXEventKeyValueSet *set = ax_event_key_value_set_new();
    gint state = rule->firing ? 1 : 0;
    ax_event_key_value_set_add_key_value(set, "state", NULL, &state, AX_VALUE_TYPE_BOOL, NULL);
    AXEvent *event = ax_event_new2(set, NULL);
    ax_event_handler_send_event(alerts->events, rule->declaration, event, NULL);
    ax_event_free(event);
    ax_event_key_value_set_free(set);
}

/* --------------------------------------------------------------- lifecycle */

/* Built-ins are code-owned: the saved file may still hold ones from an older
 * version, or ones for a filesystem that has since gone away. Anything marked
 * builtin that the current build would not generate is dropped, so the default
 * set stays in step with the code and the device. */
static void prune_stale_builtins(Alerts *alerts, GHashTable *expected) {
    guint kept = 0;
    for (guint i = 0; i < alerts->count; i++) {
        AlertRule *rule = &alerts->rules[i];
        if (rule->builtin && !g_hash_table_contains(expected, rule->id)) {
            syslog(LOG_INFO, "dropping stale built-in rule %s", rule->id);
            continue;
        }
        alerts->rules[kept++] = *rule;
    }
    alerts->count = kept;
}

Alerts *alerts_new(const Api *api) {
    Alerts *alerts = g_new0(Alerts, 1);
    alerts->api = api;
    alerts->events = ax_event_handler_new();

    load_rules(alerts);

    GHashTable *expected = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    for (gsize i = 0; i < G_N_ELEMENTS(BUILTIN); i++) {
        if (metrics_find(api->registry, BUILTIN[i].metric) >= 0)
            g_hash_table_add(expected, g_strdup(BUILTIN[i].id));
    }

    /* Every filesystem gets a space rule; which ones exist is product specific. */
    for (guint i = 0; i < api->registry->count; i++) {
        const char *id = api->registry->defs[i].id;
        if (!g_str_has_prefix(id, "fs.") || !g_str_has_suffix(id, ".usage"))
            continue;
        gchar *area = g_strndup(id + 3, strlen(id) - 3 - strlen(".usage"));
        gchar *rule_id = g_strdup_printf("storage_full_%s", area);
        g_hash_table_add(expected, g_strdup(rule_id));

        if (!find_rule(alerts, rule_id)) {
            AlertRule rule;
            memset(&rule, 0, sizeof(rule));
            g_strlcpy(rule.id, rule_id, sizeof(rule.id));
            g_snprintf(rule.name, sizeof(rule.name), "%s almost full", area);
            g_strlcpy(rule.metric, id, sizeof(rule.metric));
            rule.op = ALERT_ABOVE;
            rule.threshold = 90;
            rule.duration_s = 300;
            rule.enabled = TRUE;
            rule.builtin = TRUE;
            add_rule(alerts, &rule);
        }
        g_free(rule_id);
        g_free(area);
    }

    for (gsize i = 0; i < G_N_ELEMENTS(BUILTIN); i++) {
        if (find_rule(alerts, BUILTIN[i].id))
            continue;
        if (metrics_find(api->registry, BUILTIN[i].metric) < 0)
            continue;
        add_rule(alerts, &BUILTIN[i]);
    }

    prune_stale_builtins(alerts, expected);
    g_hash_table_destroy(expected);

    for (guint i = 0; i < alerts->count; i++)
        declare_rule(alerts, &alerts->rules[i]);

    save_rules(alerts);
    syslog(LOG_INFO, "%u alert rules active", alerts->count);
    return alerts;
}

void alerts_free(Alerts *alerts) {
    if (!alerts)
        return;
    if (alerts->events)
        ax_event_handler_free(alerts->events);
    g_free(alerts);
}

void alerts_set_notify(Alerts *alerts, AlertNotify notify, gpointer user_data) {
    alerts->notify = notify;
    alerts->notify_data = user_data;
}

/* -------------------------------------------------------------- evaluation */

void alerts_evaluate(Alerts *alerts, const float *values) {
    gint64 now = (gint64)time(NULL);

    for (guint i = 0; i < alerts->count; i++) {
        AlertRule *rule = &alerts->rules[i];
        int index = metrics_find(alerts->api->registry, rule->metric);
        if (index < 0 || !rule->enabled) {
            rule->breach_since = 0;
            continue;
        }

        float value = values[index];
        if (isnan(value)) {
            rule->breach_since = 0;
            continue; /* Absent readings must not clear a firing rule. */
        }
        rule->last_value = value;

        gboolean breached = rule->op == ALERT_ABOVE ? value > rule->threshold : value < rule->threshold;
        if (!breached) {
            rule->breach_since = 0;
            if (rule->firing) {
                rule->firing = FALSE;
                send_state(alerts, rule);
                syslog(LOG_INFO, "alert cleared: %s (%s = %.2f)", rule->name, rule->metric, value);
                if (alerts->notify)
                    alerts->notify(rule, FALSE, alerts->notify_data);
            }
            continue;
        }

        if (!rule->breach_since)
            rule->breach_since = now;
        if (!rule->firing && now - rule->breach_since >= (gint64)rule->duration_s) {
            rule->firing = TRUE;
            rule->fired_at = now;
            send_state(alerts, rule);
            syslog(LOG_WARNING, "alert firing: %s (%s = %.2f, threshold %s %.2f)", rule->name,
                   rule->metric, value, op_name(rule->op), rule->threshold);
            if (alerts->notify)
                alerts->notify(rule, TRUE, alerts->notify_data);
        }
    }
}

/* --------------------------------------------------------------------- api */

static void append_escaped(GString *out, const char *value) {
    for (const char *c = value ? value : ""; *c; c++) {
        if (*c == '"' || *c == '\\')
            g_string_append_c(out, '\\');
        g_string_append_c(out, *c);
    }
}

gchar *alerts_json(Alerts *alerts) {
    GString *out = g_string_new("{\"rules\":[");
    guint firing = 0;

    for (guint i = 0; i < alerts->count; i++) {
        AlertRule *rule = &alerts->rules[i];
        if (rule->firing)
            firing++;
        g_string_append(out, i ? ",{" : "{");
        g_string_append(out, "\"id\":\"");
        append_escaped(out, rule->id);
        g_string_append(out, "\",\"name\":\"");
        append_escaped(out, rule->name);
        g_string_append(out, "\",\"metric\":\"");
        append_escaped(out, rule->metric);
        g_string_append_printf(out,
                               "\",\"op\":\"%s\",\"threshold\":%g,\"duration\":%u,"
                               "\"enabled\":%s,\"builtin\":%s,\"firing\":%s,"
                               "\"value\":%g,\"since\":%" G_GINT64_FORMAT "}",
                               op_name(rule->op), rule->threshold, rule->duration_s,
                               rule->enabled ? "true" : "false", rule->builtin ? "true" : "false",
                               rule->firing ? "true" : "false", rule->last_value, rule->fired_at);
    }
    g_string_append_printf(out, "],\"firing\":%u}", firing);

    gchar *json = g_strdup(out->str);
    g_string_free(out, TRUE);
    return json;
}

static gchar *field(const char *body, const char *name) {
    return api_query_param(body, name);
}

gboolean alerts_apply(Alerts *alerts, const char *body) {
    gchar *action = field(body, "action");
    gchar *id = field(body, "id");
    gboolean changed = FALSE;

    if (!action || !id) {
        g_free(action);
        g_free(id);
        return FALSE;
    }

    if (g_strcmp0(action, "delete") == 0) {
        AlertRule *rule = find_rule(alerts, id);
        /* Built-ins can be disabled but not removed, so a product's default
         * cover cannot be lost by accident. */
        if (rule && !rule->builtin) {
            guint index = (guint)(rule - alerts->rules);
            memmove(&alerts->rules[index], &alerts->rules[index + 1],
                    (alerts->count - index - 1) * sizeof(AlertRule));
            alerts->count--;
            changed = TRUE;
        }
    } else if (g_strcmp0(action, "save") == 0) {
        AlertRule *rule = find_rule(alerts, id);
        AlertRule fresh;
        if (!rule) {
            memset(&fresh, 0, sizeof(fresh));
            g_strlcpy(fresh.id, id, sizeof(fresh.id));
            if (!add_rule(alerts, &fresh)) {
                g_free(action);
                g_free(id);
                return FALSE;
            }
            rule = find_rule(alerts, id);
        }

        gchar *name = field(body, "name");
        gchar *metric = field(body, "metric");
        gchar *op = field(body, "op");
        gchar *threshold = field(body, "threshold");
        gchar *duration = field(body, "duration");
        gchar *enabled = field(body, "enabled");

        if (name) g_strlcpy(rule->name, name, sizeof(rule->name));
        if (metric && !rule->builtin) g_strlcpy(rule->metric, metric, sizeof(rule->metric));
        if (op) rule->op = op_from(op);
        if (threshold) rule->threshold = g_ascii_strtod(threshold, NULL);
        if (duration) rule->duration_s = (guint)g_ascii_strtoull(duration, NULL, 10);
        if (enabled) rule->enabled = g_strcmp0(enabled, "yes") == 0;
        if (!rule->name[0]) g_strlcpy(rule->name, rule->id, sizeof(rule->name));

        declare_rule(alerts, rule);
        changed = TRUE;

        g_free(name);
        g_free(metric);
        g_free(op);
        g_free(threshold);
        g_free(duration);
        g_free(enabled);
    }

    if (changed)
        save_rules(alerts);

    g_free(action);
    g_free(id);
    return changed;
}
