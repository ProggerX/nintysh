#ifndef NINTYSH_BAR_H
#define NINTYSH_BAR_H

#include <gtk/gtk.h>

typedef struct NintyBar NintyBar;

NintyBar *ninty_bar_new(GtkApplication *app);
void ninty_bar_free(NintyBar *bar);
void ninty_bar_command(NintyBar *bar, const char *command);

#endif
