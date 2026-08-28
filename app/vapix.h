#ifndef VAPIX_H
#define VAPIX_H

#include <glib.h>

/* Obtains a service account over D-Bus. Safe to call when unavailable; the
 * getters then simply return NULL. */
void vapix_init(void);
void vapix_cleanup(void);
gboolean vapix_available(void);

/* GET a local VAPIX endpoint, for example "temperaturecontrol.cgi".
 * Returns a newly allocated body, or NULL. */
gchar *vapix_get(const char *endpoint);

#endif /* VAPIX_H */
