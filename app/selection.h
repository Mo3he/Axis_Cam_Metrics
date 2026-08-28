#ifndef SELECTION_H
#define SELECTION_H

#include <glib.h>

typedef enum { SELECT_DISPLAY, SELECT_TRANSMIT, SELECT_SCOPES } SelectScope;

typedef struct Selection Selection;

Selection *selection_new(void);
void selection_free(Selection *selection);

gboolean selection_enabled(const Selection *selection, SelectScope scope, const char *id);

/* Stores the DISABLED ids, so a metric that appears after an upgrade is on by
 * default rather than silently missing. */
void selection_set_disabled(Selection *selection, SelectScope scope, const char *csv);

const char *selection_scope_name(SelectScope scope);

#endif /* SELECTION_H */
