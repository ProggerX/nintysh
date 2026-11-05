#ifndef NINTYSH_NIRI_H
#define NINTYSH_NIRI_H

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
} NiriWorkspace;

typedef struct {
    gint64 id;
    char *title;
    char *app_id;
    gint64 workspace_id;
    gboolean is_focused;
    gboolean is_urgent;
} NiriWindow;

typedef struct NiriClient NiriClient;
typedef void (*NiriChangedFunc)(NiriClient *client, gpointer user_data);

NiriClient *niri_client_new(NiriChangedFunc changed, gpointer user_data);
void niri_client_free(NiriClient *client);

gboolean niri_client_is_connected(NiriClient *client);
guint niri_client_workspace_count(NiriClient *client);
NiriWorkspace *niri_client_workspace(NiriClient *client, guint index);
guint niri_client_window_count(NiriClient *client);
NiriWindow *niri_client_window(NiriClient *client, guint index);
const char *niri_client_keyboard_layout(NiriClient *client);

void niri_client_focus_workspace(NiriClient *client, gint64 idx);
void niri_client_focus_window(NiriClient *client, gint64 id);
void niri_client_next_layout(NiriClient *client);
void niri_client_close_window(NiriClient *client, gint64 id);
void niri_client_toggle_floating(NiriClient *client, gint64 id);
void niri_client_fullscreen(NiriClient *client, gint64 id);

#endif
