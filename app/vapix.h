#ifndef VAPIX_H
#define VAPIX_H

#include <glib.h>

/* Obtains a service account over D-Bus. If unavailable, vapix_get returns NULL. */
void vapix_init(void);
void vapix_cleanup(void);
gboolean vapix_available(void);

/* GET a local endpoint such as "temperaturecontrol.cgi". Returns a new body, or NULL. */
gchar *vapix_get(const char *endpoint);

#endif /* VAPIX_H */
