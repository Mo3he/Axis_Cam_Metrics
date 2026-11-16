#ifndef VAPIX_H
#define VAPIX_H

#include <glib.h>

/* Obtains a service account over D-Bus. If unavailable, vapix_get returns NULL. */
void vapix_init(void);
void vapix_cleanup(void);
gboolean vapix_available(void);

/* GET a local endpoint such as "temperaturecontrol.cgi". Returns a new body, or NULL. */
gchar *vapix_get(const char *endpoint);

/* POST a JSON body to a local JSON API such as "power-settings.cgi". */
gchar *vapix_post_json(const char *endpoint, const char *json_body);

#endif /* VAPIX_H */
