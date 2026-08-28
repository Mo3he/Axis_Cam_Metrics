#include "metrics.h"

#include <string.h>

void metrics_registry_init(MetricRegistry *registry) {
    registry->capacity = 64;
    registry->count = 0;
    registry->defs = g_new0(MetricDef, registry->capacity);
}

void metrics_registry_clear(MetricRegistry *registry) {
    g_free(registry->defs);
    registry->defs = NULL;
    registry->count = 0;
    registry->capacity = 0;
}

int metrics_find(const MetricRegistry *registry, const char *id) {
    for (guint i = 0; i < registry->count; i++) {
        if (strcmp(registry->defs[i].id, id) == 0)
            return (int)i;
    }
    return -1;
}

guint metrics_register(MetricRegistry *registry,
                       const char *id,
                       const char *label,
                       const char *unit,
                       const char *group) {
    int existing = metrics_find(registry, id);
    if (existing >= 0)
        return (guint)existing;

    if (registry->count == registry->capacity) {
        registry->capacity *= 2;
        registry->defs = g_renew(MetricDef, registry->defs, registry->capacity);
        memset(&registry->defs[registry->count], 0,
               sizeof(MetricDef) * (registry->capacity - registry->count));
    }

    MetricDef *def = &registry->defs[registry->count];
    g_strlcpy(def->id, id, sizeof(def->id));
    g_strlcpy(def->label, label ? label : id, sizeof(def->label));
    g_strlcpy(def->unit, unit ? unit : "", sizeof(def->unit));
    g_strlcpy(def->group, group ? group : "other", sizeof(def->group));

    return registry->count++;
}
