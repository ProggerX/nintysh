#ifndef NINTYSH_COMPOSITOR_H
#define NINTYSH_COMPOSITOR_H

#include <glib.h>

typedef struct {
    gint64 id;
    gint64 idx;
    char *name;
    char *output;
    gboolean is_active;
    gboolean is_focused;
    gboolean is_urgent;
    gint64 active_window_id;
} CompositorWorkspace;

typedef struct {
    gint64 id;
    char *title;
    char *app_id;
    gint64 workspace_id;
    gboolean is_focused;
    gboolean is_urgent;
} CompositorWindow;

typedef struct CompositorClient CompositorClient;
typedef void (*CompositorChangedFunc)(CompositorClient *client,
                                     gpointer user_data);

CompositorClient *compositor_client_new(CompositorChangedFunc changed,
                                        gpointer user_data);
void compositor_client_free(CompositorClient *client);

const char *compositor_client_name(CompositorClient *client);
gboolean compositor_client_is_connected(CompositorClient *client);
guint compositor_client_workspace_count(CompositorClient *client);
CompositorWorkspace *compositor_client_workspace(CompositorClient *client,
                                                 guint index);
guint compositor_client_window_count(CompositorClient *client);
CompositorWindow *compositor_client_window(CompositorClient *client,
                                           guint index);
const char *compositor_client_keyboard_layout(CompositorClient *client);

void compositor_client_focus_workspace(CompositorClient *client, gint64 idx);
void compositor_client_focus_window(CompositorClient *client, gint64 id);
void compositor_client_next_layout(CompositorClient *client);
void compositor_client_close_window(CompositorClient *client, gint64 id);
void compositor_client_toggle_floating(CompositorClient *client, gint64 id);
void compositor_client_fullscreen(CompositorClient *client, gint64 id);

#endif
