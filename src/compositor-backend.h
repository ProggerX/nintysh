#ifndef NINTYSH_COMPOSITOR_BACKEND_H
#define NINTYSH_COMPOSITOR_BACKEND_H

#include "compositor.h"

typedef struct CompositorOps {
    const char *name;
    gboolean (*init)(CompositorClient *client);
    void (*destroy)(CompositorClient *client);
    gboolean (*is_connected)(CompositorClient *client);
    guint (*workspace_count)(CompositorClient *client);
    CompositorWorkspace *(*workspace)(CompositorClient *client, guint index);
    guint (*window_count)(CompositorClient *client);
    CompositorWindow *(*window)(CompositorClient *client, guint index);
    const char *(*keyboard_layout)(CompositorClient *client);
    void (*focus_workspace)(CompositorClient *client, gint64 idx);
    void (*focus_window)(CompositorClient *client, gint64 id);
    void (*next_layout)(CompositorClient *client);
    void (*close_window)(CompositorClient *client, gint64 id);
    void (*toggle_floating)(CompositorClient *client, gint64 id);
    void (*fullscreen)(CompositorClient *client, gint64 id);
} CompositorOps;

struct CompositorClient {
    const CompositorOps *ops;
    gpointer impl;
    CompositorChangedFunc changed;
    gpointer user_data;
};

extern const CompositorOps niri_compositor_ops;
extern const CompositorOps sway_compositor_ops;
extern const CompositorOps hyprland_compositor_ops;

void compositor_client_notify(CompositorClient *client);

CompositorWorkspace *compositor_workspace_new(void);
void compositor_workspace_free(CompositorWorkspace *workspace);
CompositorWindow *compositor_window_new(void);
void compositor_window_free(CompositorWindow *window);
void compositor_clear_workspaces(GPtrArray *workspaces);
void compositor_clear_windows(GPtrArray *windows);
void compositor_set_keyboard_layout(char *dest, gsize size, const char *name);

#endif
