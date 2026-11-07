#ifndef NINTYSH_AUTH_H
#define NINTYSH_AUTH_H

#include <glib.h>

gboolean ninty_auth_password(const char *password, char **message);

#endif
