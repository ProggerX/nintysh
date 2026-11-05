#ifndef NINTYSH_CONTROL_H
#define NINTYSH_CONTROL_H

#include <glib.h>

typedef struct NintyControl NintyControl;
typedef void (*NintyControlCallback)(const char *command, gpointer data);

NintyControl *ninty_control_new(NintyControlCallback callback, gpointer data);
void ninty_control_free(NintyControl *control);
int ninty_control_send(int argc, char **argv);

#endif
