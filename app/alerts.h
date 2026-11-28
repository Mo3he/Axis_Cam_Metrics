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

/* Applies one form-encoded change: action=save|delete plus rule fields. A
 * rejected request sets *error (static text); deleting a missing rule is not
 * an error. */
gboolean alerts_apply(Alerts *alerts, const char *body, const char **error);

/* Called whenever a rule starts or stops firing, or is deleted. */
typedef enum { ALERT_CLEARED, ALERT_FIRING, ALERT_REMOVED } AlertChange;
typedef void (*AlertNotify)(const AlertRule *rule, AlertChange change, gpointer user_data);
void alerts_set_notify(Alerts *alerts, AlertNotify notify, gpointer user_data);

#endif /* ALERTS_H */
