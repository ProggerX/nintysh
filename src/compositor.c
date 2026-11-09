#include "compositor-backend.h"

#include <string.h>

static const CompositorOps *detect_compositor(void) {
    const char *forced = g_getenv("NINTYSH_COMPOSITOR");

    if (forced != NULL && forced[0] != '\0') {
        if (g_strcmp0(forced, "niri") == 0) {
            return &niri_compositor_ops;
        }
        if (g_strcmp0(forced, "sway") == 0) {
            return &sway_compositor_ops;
        }
        if (g_strcmp0(forced, "hyprland") == 0) {
            return &hyprland_compositor_ops;
        }
        g_warning("nintysh: unknown NINTYSH_COMPOSITOR=%s", forced);
    }

    const char *niri_socket = g_getenv("NIRI_SOCKET");
    if (niri_socket != NULL && niri_socket[0] != '\0') {
        return &niri_compositor_ops;
    }

    const char *hyprland =
        g_getenv("HYPRLAND_INSTANCE_SIGNATURE");
    if (hyprland != NULL && hyprland[0] != '\0') {
        return &hyprland_compositor_ops;
    }

    const char *sway_socket = g_getenv("SWAYSOCK");
    if (sway_socket != NULL && sway_socket[0] != '\0') {
        return &sway_compositor_ops;
    }

    return &niri_compositor_ops;
}

CompositorWorkspace *compositor_workspace_new(void) {
    CompositorWorkspace *workspace = g_new0(CompositorWorkspace, 1);
    workspace->active_window_id = -1;
    return workspace;
}

void compositor_workspace_free(CompositorWorkspace *workspace) {
    if (workspace == NULL) {
        return;
    }
    g_free(workspace->name);
    g_free(workspace->output);
    g_free(workspace);
}

CompositorWindow *compositor_window_new(void) {
    CompositorWindow *window = g_new0(CompositorWindow, 1);
    window->workspace_id = -1;
    return window;
}

void compositor_window_free(CompositorWindow *window) {
    if (window == NULL) {
        return;
    }
    g_free(window->title);
    g_free(window->app_id);
    g_free(window);
}

void compositor_clear_workspaces(GPtrArray *workspaces) {
    if (workspaces == NULL) {
        return;
    }
    while (workspaces->len > 0) {
        g_ptr_array_remove_index(workspaces, workspaces->len - 1);
    }
}

void compositor_clear_windows(GPtrArray *windows) {
    if (windows == NULL) {
        return;
    }
    while (windows->len > 0) {
        g_ptr_array_remove_index(windows, windows->len - 1);
    }
}

void compositor_set_keyboard_layout(char *dest, gsize size, const char *name) {
    if (dest == NULL || size == 0) {
        return;
    }
    dest[0] = '\0';
    if (name == NULL) {
        return;
    }

    char code[8] = { 0 };
    int out = 0;

    for (const char *p = name; *p != '\0' && out < 2; p++) {
        if (*p == ' ' || *p == '(' || *p == ',' || *p == '-') {
            break;
        }
        if (g_ascii_isalnum(*p)) {
            code[out++] = g_ascii_toupper(*p);
        }
    }
    g_strlcpy(dest, code, size);
}

void compositor_client_notify(CompositorClient *client) {
    if (client != NULL && client->changed != NULL) {
        client->changed(client, client->user_data);
    }
}

CompositorClient *compositor_client_new(CompositorChangedFunc changed,
                                        gpointer user_data) {
    CompositorClient *client = g_new0(CompositorClient, 1);

    client->ops = detect_compositor();
    client->changed = changed;
    client->user_data = user_data;

    if (client->ops->init != NULL) {
        client->ops->init(client);
    }
    return client;
}

void compositor_client_free(CompositorClient *client) {
    if (client == NULL) {
        return;
    }
    if (client->ops != NULL && client->ops->destroy != NULL) {
        client->ops->destroy(client);
    }
    g_free(client);
}

const char *compositor_client_name(CompositorClient *client) {
    if (client == NULL || client->ops == NULL) {
        return "unknown";
    }
    return client->ops->name != NULL ? client->ops->name : "unknown";
}

gboolean compositor_client_is_connected(CompositorClient *client) {
    if (client == NULL || client->ops == NULL ||
        client->ops->is_connected == NULL) {
        return FALSE;
    }
    return client->ops->is_connected(client);
}

guint compositor_client_workspace_count(CompositorClient *client) {
    if (client == NULL || client->ops == NULL ||
        client->ops->workspace_count == NULL) {
        return 0;
    }
    return client->ops->workspace_count(client);
}

CompositorWorkspace *compositor_client_workspace(CompositorClient *client,
                                                 guint index) {
    if (client == NULL || client->ops == NULL ||
        client->ops->workspace == NULL) {
        return NULL;
    }
    return client->ops->workspace(client, index);
}

guint compositor_client_window_count(CompositorClient *client) {
    if (client == NULL || client->ops == NULL ||
        client->ops->window_count == NULL) {
        return 0;
    }
    return client->ops->window_count(client);
}

CompositorWindow *compositor_client_window(CompositorClient *client,
                                           guint index) {
    if (client == NULL || client->ops == NULL || client->ops->window == NULL) {
        return NULL;
    }
    return client->ops->window(client, index);
}

const char *compositor_client_keyboard_layout(CompositorClient *client) {
    if (client == NULL || client->ops == NULL ||
        client->ops->keyboard_layout == NULL) {
        return "";
    }
    return client->ops->keyboard_layout(client);
}

void compositor_client_focus_workspace(CompositorClient *client, gint64 idx) {
    if (client != NULL && client->ops != NULL &&
        client->ops->focus_workspace != NULL) {
        client->ops->focus_workspace(client, idx);
    }
}

void compositor_client_focus_window(CompositorClient *client, gint64 id) {
    if (client != NULL && client->ops != NULL &&
        client->ops->focus_window != NULL) {
        client->ops->focus_window(client, id);
    }
}

void compositor_client_next_layout(CompositorClient *client) {
    if (client != NULL && client->ops != NULL &&
        client->ops->next_layout != NULL) {
        client->ops->next_layout(client);
    }
}

void compositor_client_close_window(CompositorClient *client, gint64 id) {
    if (client != NULL && client->ops != NULL &&
        client->ops->close_window != NULL) {
        client->ops->close_window(client, id);
    }
}

void compositor_client_toggle_floating(CompositorClient *client, gint64 id) {
    if (client != NULL && client->ops != NULL &&
        client->ops->toggle_floating != NULL) {
        client->ops->toggle_floating(client, id);
    }
}

void compositor_client_fullscreen(CompositorClient *client, gint64 id) {
    if (client != NULL && client->ops != NULL &&
        client->ops->fullscreen != NULL) {
        client->ops->fullscreen(client, id);
    }
}
