#include <gtk/gtk.h>

#include "bar.h"
#include "control.h"

static void activate(GtkApplication *app, gpointer user_data) {
    (void)user_data;
    if (g_object_get_data(G_OBJECT(app), "nintysh-bar") != NULL) {
        return;
    }

    NintyBar *bar = ninty_bar_new(app);
    g_object_set_data_full(G_OBJECT(app), "nintysh-bar", bar,
                           (GDestroyNotify)ninty_bar_free);
}

int main(int argc, char **argv) {
    if (argc > 1 && g_strcmp0(argv[1], "msg") == 0) {
        return ninty_control_send(argc - 2, argv + 2);
    }

    if (g_getenv("GSK_RENDERER") == NULL) {
        g_setenv("GSK_RENDERER", "cairo", TRUE);
    }

    GtkApplication *app = gtk_application_new(
        "org.nintysh.Shell", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);

    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}
