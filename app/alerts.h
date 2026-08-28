#ifndef ALERTS_H
#define ALERTS_H

#include "api.h"

typedef enum { ALERT_ABOVE, ALERT_BELOW } AlertOp;

typedef struct {
    char id[48];
    char name[96];
    char metric[METRIC_ID_MAX];
    AlertOp op;
    double threshold;
    guint duration_s; /* Condition must hold this long before firing. */
    gboolean enabled;
    gboolean builtin;

    /* Runtime state. */
    gboolean firing;
    gint64 breach_since;
    gint64 fired_at;
    double last_value;
    guint declaration;
} AlertRule;

typedef struct Alerts Alerts;

Alerts *alerts_new(const Api *api);
void alerts_free(Alerts *alerts);

/* Evaluates every rule against the latest sample. */
void alerts_evaluate(Alerts *alerts, const float *values);

gchar *alerts_json(Alerts *alerts);

/* Applies one form-encoded change: action=save|delete plus rule fields. */
gboolean alerts_apply(Alerts *alerts, const char *body);

/* Set to receive a one-line summary whenever a rule changes state. */
typedef void (*AlertNotify)(const AlertRule *rule, gboolean firing, gpointer user_data);
void alerts_set_notify(Alerts *alerts, AlertNotify notify, gpointer user_data);

#endif /* ALERTS_H */
