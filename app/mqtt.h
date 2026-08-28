#ifndef MQTT_H
#define MQTT_H

#include "api.h"

typedef struct {
    gboolean enabled;
    char host[128];
    guint port;
    gboolean tls;
    char username[64];
    char password[128];
    char topic_prefix[128];
    guint interval_s;
    gboolean discovery;      /* Publish Home Assistant MQTT discovery configs. */
    gboolean discovery_all;  /* Every metric rather than a curated subset. */
} MqttConfig;

typedef struct Mqtt Mqtt;

Mqtt *mqtt_new(const Api *api);
void mqtt_free(Mqtt *mqtt);

/* Reconnects only when something that affects the connection changed. */
void mqtt_apply(Mqtt *mqtt, const MqttConfig *config);

/* Called on every sample; publishes at the configured interval. */
void mqtt_tick(Mqtt *mqtt);

const char *mqtt_state(const Mqtt *mqtt);

#endif /* MQTT_H */
