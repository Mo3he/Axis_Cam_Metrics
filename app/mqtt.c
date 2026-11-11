/*
 * MQTT publishing, including Home Assistant discovery.
 *
 * paho.mqtt.c is static: OS 13 does not ship libpaho and recorders lack the
 * device MQTT client API. The async client keeps a down broker from blocking
 * the sampler; paho owns the reconnect loop.
 *
 * paho calls back on its own thread, while the registry, selection and config
 * belong to the main loop (a new disk even rebuilds the registry). Callbacks
 * therefore only set flags, and mqtt_tick does the work on the main loop.
 */

#include "mqtt.h"

#include <MQTTAsync.h>
#include <math.h>
#include <string.h>
#include <syslog.h>

#define CONNECT_RETRY_MIN_S 2
#define CONNECT_RETRY_MAX_S 60

struct Mqtt {
    const Api *api;
    MqttConfig config;
    MQTTAsync client;
    char address[224];
    char client_id[64];
    gint connected;      /* Atomic: written by paho callbacks. */
    gint just_connected; /* Atomic: set on (re)connect, consumed by mqtt_tick. */
    gboolean discovery_sent;
    gint64 last_publish;
};

/* Metric ids are sanitized, so VLAN eth1.3 shows up as "net.eth1_3.rx_bps". */
static gboolean interface_is_vlan(const char *id) {
    const char *start = id + strlen("net.");
    const char *end = strchr(start, '.');
    if (!end)
        return FALSE;
    return memchr(start, '_', (gsize)(end - start)) != NULL;
}

/* Default Home Assistant entities: what somebody would put on a dashboard or
 * alert on. Publishing everything would bury the useful ones. */
static gboolean is_essential(const char *id, gboolean named_sensors) {
    static const char *exact[] = {"cpu.usage",       "mem.usage",     "mem.used",
                                  "load.1",          "sys.uptime",    "flash.life_used",
                                  "flash.pre_eol",   "poe.total",     "poe.budget",
                                  NULL};
    for (int i = 0; exact[i]; i++) {
        if (strcmp(id, exact[i]) == 0)
            return TRUE;
    }

    /* Raw kernel zones ("rsp_thermal") only stand in when there are no named sensors. */
    if (g_str_has_prefix(id, "sensor."))
        return TRUE;
    if (g_str_has_prefix(id, "temp."))
        return !named_sensors;

    /* Internal flash and config partitions barely move; removable storage fills up. */
    if (g_str_has_prefix(id, "fs.") && g_str_has_suffix(id, ".usage"))
        return !g_str_has_prefix(id, "fs.mnt_");

    /* Skipping VLANs keeps an eight-port recorder from adding sixteen entities. */
    if (g_str_has_prefix(id, "net.") &&
        (g_str_has_suffix(id, ".rx_bps") || g_str_has_suffix(id, ".tx_bps")))
        return !interface_is_vlan(id);

    return FALSE;
}

/* A device without a VAPIX service account reports no named sensors, so the raw
 * thermal zones are all it has. */
static gboolean has_named_sensors(const Mqtt *mqtt) {
    for (guint i = 0; i < mqtt->api->registry->count; i++) {
        if (g_str_has_prefix(mqtt->api->registry->defs[i].id, "sensor."))
            return TRUE;
    }
    return FALSE;
}

/* Home Assistant infers history and units from these. */
static const char *device_class_for(const char *unit) {
    if (strcmp(unit, "C") == 0) return "temperature";
    if (strcmp(unit, "B") == 0) return "data_size";
    if (strcmp(unit, "B/s") == 0) return "data_rate";
    if (strcmp(unit, "s") == 0) return "duration";
    if (strcmp(unit, "MHz") == 0) return "frequency";
    return NULL;
}

static const char *ha_unit_for(const char *unit) {
    if (strcmp(unit, "C") == 0) return "\u00b0C";
    if (strcmp(unit, "B") == 0) return "B";
    if (strcmp(unit, "B/s") == 0) return "B/s";
    if (strcmp(unit, "s") == 0) return "s";
    if (strcmp(unit, "MHz") == 0) return "MHz";
    if (strcmp(unit, "Mb/s") == 0) return "Mbit/s";
    return unit;
}

/* Values stay in base units so consumers can do arithmetic; this tells Home
 * Assistant how to display them (otherwise a 3.6 TB disk reads in bytes). */
static const char *ha_suggested_unit(const MetricDef *def) {
    if (strcmp(def->unit, "B") == 0)
        return g_str_has_prefix(def->id, "fs.") ? "GB" : "MB";
    if (strcmp(def->unit, "B/s") == 0)
        return "MB/s";
    return NULL;
}

static void topic(const Mqtt *mqtt, char *out, gsize len, const char *leaf) {
    g_snprintf(out, len, "%s/%s", mqtt->config.topic_prefix, leaf);
}

/* wait_ms > 0 blocks until the broker has the message, which matters only for
 * the farewell publish during shutdown. */
static int publish(Mqtt *mqtt, const char *topic_name, const char *payload, int qos, int retained,
                   int wait_ms) {
    if (!mqtt->client)
        return MQTTASYNC_FAILURE;

    MQTTAsync_message message = MQTTAsync_message_initializer;
    message.payload = (void *)payload;
    message.payloadlen = (int)strlen(payload);
    message.qos = qos;
    message.retained = retained;

    MQTTAsync_responseOptions options = MQTTAsync_responseOptions_initializer;
    int rc = MQTTAsync_sendMessage(mqtt->client, topic_name, &message, &options);
    if (rc == MQTTASYNC_SUCCESS && wait_ms > 0)
        MQTTAsync_waitForCompletion(mqtt->client, options.token, (unsigned long)wait_ms);
    return rc;
}

/* ------------------------------------------------------ discovery payloads */

/* Appends "key":"value", with the value escaped and a trailing comma. */
static void append_field(GString *out, const char *key, const char *value) {
    g_string_append_printf(out, "\"%s\":\"", key);
    api_append_json_escaped(out, value);
    g_string_append(out, "\",");
}

static void publish_discovery(Mqtt *mqtt, gboolean withdraw_stale) {
    char state_topic[256];
    char availability_topic[256];
    topic(mqtt, state_topic, sizeof(state_topic), "state");
    topic(mqtt, availability_topic, sizeof(availability_topic), "status");

    const DeviceInfo *device = &mqtt->api->device;
    gboolean named_sensors = has_named_sensors(mqtt);
    guint published = 0;
    guint retracted = 0;

    for (guint i = 0; i < mqtt->api->registry->count; i++) {
        const MetricDef *def = &mqtt->api->registry->defs[i];

        /* An untransmitted metric would give an entity that never updates. */
        gboolean wanted = selection_enabled(mqtt->api->selection, SELECT_TRANSMIT, def->id) &&
                          (mqtt->config.discovery_all || is_essential(def->id, named_sensors));

        char object_id[128];
        g_snprintf(object_id, sizeof(object_id), "%s_%s", device->serial, def->id);
        for (char *c = object_id; *c; c++) {
            if (!g_ascii_isalnum(*c))
                *c = '_';
        }

        char config_topic[256];
        g_snprintf(config_topic, sizeof(config_topic), "homeassistant/sensor/%s/config", object_id);

        /* Retained configs would leave dead entities; an empty retained payload
         * removes them. Only once per broker, so a flapping link cannot spam. */
        if (!wanted) {
            if (withdraw_stale) {
                publish(mqtt, config_topic, "", 0, 1, 0);
                retracted++;
            }
            continue;
        }

        gchar *unique_id = g_strdup_printf("axis_%s", object_id);
        /* Every entity reads its own field out of the one retained state object. */
        gchar *value_template = g_strdup_printf("{{ value_json['%s'] }}", def->id);

        GString *payload = g_string_new("{");
        append_field(payload, "name", def->label);
        append_field(payload, "unique_id", unique_id);
        append_field(payload, "state_topic", state_topic);
        append_field(payload, "availability_topic", availability_topic);
        append_field(payload, "value_template", value_template);
        if (def->unit[0])
            append_field(payload, "unit_of_measurement", ha_unit_for(def->unit));
        const char *device_class = device_class_for(def->unit);
        if (device_class)
            append_field(payload, "device_class", device_class);
        const char *suggested = ha_suggested_unit(def);
        if (suggested) {
            append_field(payload, "suggested_unit_of_measurement", suggested);
            g_string_append(payload, "\"suggested_display_precision\":1,");
        }
        append_field(payload, "state_class", "measurement");
        g_string_append(payload, "\"device\":{\"identifiers\":[\"axis_");
        api_append_json_escaped(payload, device->serial);
        g_string_append(payload, "\"],");
        append_field(payload, "name", device->model[0] ? device->model : "Axis device");
        append_field(payload, "manufacturer", "Axis");
        append_field(payload, "model", device->model);
        g_string_append(payload, "\"sw_version\":\"");
        api_append_json_escaped(payload, device->firmware);
        g_string_append(payload, "\"}}");
        g_free(unique_id);
        g_free(value_template);

        publish(mqtt, config_topic, payload->str, 0, 1, 0);
        g_string_free(payload, TRUE);
        published++;
    }

    syslog(LOG_INFO, "published %u Home Assistant discovery configs, withdrew %u", published,
           retracted);
    mqtt->discovery_sent = TRUE;
}

/* --------------------------------------------------------------- callbacks */

/* paho thread: flags only, see the top of the file. */
static void on_connected(void *context, char *cause) {
    (void)cause;
    Mqtt *mqtt = context;
    g_atomic_int_set(&mqtt->connected, TRUE);
    g_atomic_int_set(&mqtt->just_connected, TRUE);
}

static void on_connection_lost(void *context, char *cause) {
    Mqtt *mqtt = context;
    g_atomic_int_set(&mqtt->connected, FALSE);
    syslog(LOG_WARNING, "mqtt connection lost: %s", cause ? cause : "unknown");
}

/* Main loop: what a (re)connect needs, deferred from on_connected. */
static void handle_connected(Mqtt *mqtt) {
    syslog(LOG_INFO, "mqtt connected to %s", mqtt->address);

    char status_topic[256];
    topic(mqtt, status_topic, sizeof(status_topic), "status");
    publish(mqtt, status_topic, "online", 1, 1, 0);

    if (mqtt->config.discovery)
        publish_discovery(mqtt, !mqtt->discovery_sent);
    mqtt->last_publish = 0; /* Publish state immediately rather than after a full interval. */
}

static int on_message(void *context, char *topic_name, int topic_len, MQTTAsync_message *message) {
    (void)context;
    (void)topic_len;
    MQTTAsync_freeMessage(&message);
    MQTTAsync_free(topic_name);
    return 1;
}

/* ------------------------------------------------------------- lifecycle */

Mqtt *mqtt_new(const Api *api) {
    Mqtt *mqtt = g_new0(Mqtt, 1);
    mqtt->api = api;
    return mqtt;
}

static void disconnect(Mqtt *mqtt) {
    if (!mqtt->client)
        return;

    if (g_atomic_int_get(&mqtt->connected)) {
        char status_topic[256];
        topic(mqtt, status_topic, sizeof(status_topic), "status");
        /* The will only covers an ungraceful exit, so a clean stop has to
         * retract "online" itself, and the send must land before DISCONNECT. */
        publish(mqtt, status_topic, "offline", 1, 1, 1000);

        MQTTAsync_disconnectOptions options = MQTTAsync_disconnectOptions_initializer;
        options.timeout = 1000;
        MQTTAsync_disconnect(mqtt->client, &options);
    }

    MQTTAsync_destroy(&mqtt->client);
    mqtt->client = NULL;
    g_atomic_int_set(&mqtt->connected, FALSE);
    g_atomic_int_set(&mqtt->just_connected, FALSE);
    mqtt->discovery_sent = FALSE;
}

static gboolean connection_differs(const MqttConfig *a, const MqttConfig *b) {
    return a->enabled != b->enabled || a->port != b->port || a->tls != b->tls ||
           strcmp(a->host, b->host) != 0 || strcmp(a->username, b->username) != 0 ||
           strcmp(a->password, b->password) != 0 || strcmp(a->topic_prefix, b->topic_prefix) != 0;
}

static void connect_now(Mqtt *mqtt) {
    g_snprintf(mqtt->address, sizeof(mqtt->address), "%s://%s:%u",
               mqtt->config.tls ? "ssl" : "tcp", mqtt->config.host, mqtt->config.port);
    g_snprintf(mqtt->client_id, sizeof(mqtt->client_id), "axis-metrics-%s",
               mqtt->api->device.serial[0] ? mqtt->api->device.serial : "unknown");

    MQTTAsync_createOptions create = MQTTAsync_createOptions_initializer;
    create.MQTTVersion = MQTTVERSION_3_1_1;
    if (MQTTAsync_createWithOptions(&mqtt->client, mqtt->address, mqtt->client_id,
                                    MQTTCLIENT_PERSISTENCE_NONE, NULL, &create) != MQTTASYNC_SUCCESS) {
        syslog(LOG_ERR, "mqtt client creation failed for %s", mqtt->address);
        mqtt->client = NULL;
        return;
    }

    MQTTAsync_setCallbacks(mqtt->client, mqtt, on_connection_lost, on_message, NULL);
    MQTTAsync_setConnected(mqtt->client, mqtt, on_connected);

    char status_topic[256];
    topic(mqtt, status_topic, sizeof(status_topic), "status");

    MQTTAsync_connectOptions options = MQTTAsync_connectOptions_initializer;
    options.keepAliveInterval = 60;
    options.cleansession = 1;
    options.automaticReconnect = 1;
    options.minRetryInterval = CONNECT_RETRY_MIN_S;
    options.maxRetryInterval = CONNECT_RETRY_MAX_S;
    options.context = mqtt;
    /* MQTT forbids a password without a username, and brokers answer it with a
     * disconnect that looks like an unreachable host. */
    if (mqtt->config.username[0]) {
        options.username = mqtt->config.username;
        if (mqtt->config.password[0])
            options.password = mqtt->config.password;
    } else if (mqtt->config.password[0]) {
        syslog(LOG_WARNING, "mqtt password ignored: the broker also needs a username");
    }

    /* Makes the Home Assistant availability topic meaningful on an ungraceful drop. */
    MQTTAsync_willOptions will = MQTTAsync_willOptions_initializer;
    will.topicName = status_topic;
    will.message = "offline";
    will.retained = 1;
    will.qos = 1;
    options.will = &will;

    MQTTAsync_SSLOptions ssl = MQTTAsync_SSLOptions_initializer;
    if (mqtt->config.tls) {
        ssl.enableServerCertAuth = 1;
        ssl.verify = 1;
        options.ssl = &ssl;
    }

    int rc = MQTTAsync_connect(mqtt->client, &options);
    if (rc != MQTTASYNC_SUCCESS)
        syslog(LOG_WARNING, "mqtt connect to %s failed (%d), retrying in background", mqtt->address, rc);
}

void mqtt_apply(Mqtt *mqtt, const MqttConfig *config) {
    gboolean reconnect = connection_differs(&mqtt->config, config) || !mqtt->client;
    /* Discovery is otherwise only published on connect. */
    gboolean rediscover = !reconnect && g_atomic_int_get(&mqtt->connected) && config->discovery &&
                          (config->discovery != mqtt->config.discovery ||
                           config->discovery_all != mqtt->config.discovery_all);
    mqtt->config = *config;

    if (!config->enabled) {
        disconnect(mqtt);
        return;
    }
    if (!config->host[0]) {
        syslog(LOG_WARNING, "mqtt enabled but no broker host configured");
        disconnect(mqtt);
        return;
    }
    if (reconnect) {
        disconnect(mqtt);
        connect_now(mqtt);
    } else if (rediscover) {
        publish_discovery(mqtt, TRUE);
    }
}

void mqtt_tick(Mqtt *mqtt) {
    if (!mqtt->client || !mqtt->config.enabled)
        return;
    if (g_atomic_int_compare_and_exchange(&mqtt->just_connected, TRUE, FALSE))
        handle_connected(mqtt);
    if (!g_atomic_int_get(&mqtt->connected))
        return;

    gint64 now = g_get_monotonic_time() / G_USEC_PER_SEC;
    if (mqtt->last_publish && now - mqtt->last_publish < mqtt->config.interval_s)
        return;
    mqtt->last_publish = now;

    char state_topic[256];
    topic(mqtt, state_topic, sizeof(state_topic), "state");
    gchar *json = api_current_values_json(mqtt->api);
    publish(mqtt, state_topic, json, 0, 1, 0);
    g_free(json);
}

void mqtt_publish_alert(Mqtt *mqtt,
                        const char *rule_id,
                        const char *name,
                        const char *metric,
                        double value,
                        gboolean firing) {
    if (!mqtt || !g_atomic_int_get(&mqtt->connected))
        return;

    char leaf[128];
    char alert_topic[256];
    g_snprintf(leaf, sizeof(leaf), "alert/%s", rule_id);
    topic(mqtt, alert_topic, sizeof(alert_topic), leaf);

    GString *payload = g_string_new("{");
    append_field(payload, "rule", rule_id);
    append_field(payload, "name", name);
    append_field(payload, "metric", metric);
    g_string_append(payload, "\"value\":");
    api_append_json_number(payload, value);
    g_string_append_printf(payload, ",\"state\":\"%s\"}", firing ? "firing" : "cleared");
    publish(mqtt, alert_topic, payload->str, 1, 1, 0);
    g_string_free(payload, TRUE);
}

const char *mqtt_state(const Mqtt *mqtt) {
    if (!mqtt->config.enabled)
        return "disabled";
    if (!mqtt->client)
        return "error";
    return g_atomic_int_get(&mqtt->connected) ? "connected" : "connecting";
}

void mqtt_free(Mqtt *mqtt) {
    if (!mqtt)
        return;
    disconnect(mqtt);
    g_free(mqtt);
}
