#include "niri.h"

#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>
#include <json-glib/json-glib.h>
#include <string.h>

struct NiriClient {
    GSocket *socket;
    GSource *io_source;
    GString *rx;
    GPtrArray *workspaces;
    GPtrArray *windows;
    GPtrArray *keyboard_names;
    int keyboard_index;
    char keyboard_layout[8];
    gboolean connected;
    guint reconnect_source;
    NiriChangedFunc changed;
    gpointer user_data;
};

static void niri_workspace_free(NiriWorkspace *workspace) {
    if (workspace == NULL) {
        return;
    }
    g_free(workspace->name);
    g_free(workspace->output);
    g_free(workspace);
}

static void niri_window_free(NiriWindow *window) {
    if (window == NULL) {
        return;
    }
    g_free(window->title);
    g_free(window->app_id);
    g_free(window);
}

static void clear_ptr_array(GPtrArray *array) {
    while (array->len > 0) {
        g_ptr_array_remove_index(array, array->len - 1);
    }
}

static void niri_notify(NiriClient *client) {
    if (client->changed != NULL) {
        client->changed(client, client->user_data);
    }
}

static gboolean json_get_int64(JsonObject *object, const char *member,
                               gint64 *value) {
    if (!json_object_has_member(object, member)) {
        return FALSE;
    }
    JsonNode *node = json_object_get_member(object, member);
    if (node == NULL || !JSON_NODE_HOLDS_VALUE(node)) {
        return FALSE;
    }
    *value = json_node_get_int(node);
    return TRUE;
}

static char *json_dup_string(JsonObject *object, const char *member) {
    if (!json_object_has_member(object, member)) {
        return NULL;
    }
    JsonNode *node = json_object_get_member(object, member);
    if (node == NULL || !JSON_NODE_HOLDS_VALUE(node) ||
        json_node_get_value_type(node) != G_TYPE_STRING) {
        return NULL;
    }
    return g_strdup(json_node_get_string(node));
}

static gboolean json_get_boolean(JsonObject *object, const char *member) {
    return json_object_has_member(object, member) &&
           json_object_get_boolean_member(object, member);
}

static NiriWorkspace *parse_workspace(JsonObject *object) {
    NiriWorkspace *workspace = g_new0(NiriWorkspace, 1);
    gint64 value = 0;

    json_get_int64(object, "id", &workspace->id);
    json_get_int64(object, "idx", &workspace->idx);
    if (json_get_int64(object, "active_window_id", &value)) {
        workspace->active_window_id = value;
    } else {
        workspace->active_window_id = -1;
    }
    workspace->name = json_dup_string(object, "name");
    workspace->output = json_dup_string(object, "output");
    workspace->is_active = json_get_boolean(object, "is_active");
    workspace->is_focused = json_get_boolean(object, "is_focused");
    workspace->is_urgent = json_get_boolean(object, "is_urgent");
    return workspace;
}

static NiriWindow *parse_window(JsonObject *object) {
    NiriWindow *window = g_new0(NiriWindow, 1);
    gint64 value = 0;

    json_get_int64(object, "id", &window->id);
    if (json_get_int64(object, "workspace_id", &value)) {
        window->workspace_id = value;
    } else {
        window->workspace_id = -1;
    }
    window->title = json_dup_string(object, "title");
    window->app_id = json_dup_string(object, "app_id");
    window->is_focused = json_get_boolean(object, "is_focused");
    window->is_urgent = json_get_boolean(object, "is_urgent");
    return window;
}

static gint workspace_compare(gconstpointer a, gconstpointer b) {
    const NiriWorkspace *wa = *(NiriWorkspace *const *)a;
    const NiriWorkspace *wb = *(NiriWorkspace *const *)b;
    if (wa->idx < wb->idx) {
        return -1;
    }
    if (wa->idx > wb->idx) {
        return 1;
    }
    return 0;
}

static gint window_compare(gconstpointer a, gconstpointer b) {
    const NiriWindow *wa = *(NiriWindow *const *)a;
    const NiriWindow *wb = *(NiriWindow *const *)b;
    if (wa->id < wb->id) {
        return -1;
    }
    if (wa->id > wb->id) {
        return 1;
    }
    return 0;
}

static NiriWindow *find_window(NiriClient *client, gint64 id) {
    for (guint i = 0; i < client->windows->len; i++) {
        NiriWindow *window = g_ptr_array_index(client->windows, i);
        if (window->id == id) {
            return window;
        }
    }
    return NULL;
}

static void update_keyboard_code(NiriClient *client) {
    if (client->keyboard_names->len == 0) {
        client->keyboard_layout[0] = '\0';
        return;
    }

    guint idx = (client->keyboard_index >= 0 &&
                 (guint)client->keyboard_index < client->keyboard_names->len)
                    ? (guint)client->keyboard_index
                    : 0;
    const char *name = g_ptr_array_index(client->keyboard_names, idx);
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
    g_strlcpy(client->keyboard_layout, code, sizeof(client->keyboard_layout));
}

static void parse_keyboard_layout(NiriClient *client, JsonObject *payload) {
    if (!json_object_has_member(payload, "keyboard_layouts")) {
        return;
    }

    JsonNode *node = json_object_get_member(payload, "keyboard_layouts");
    if (node == NULL || !JSON_NODE_HOLDS_OBJECT(node)) {
        return;
    }
    JsonObject *layouts = json_node_get_object(node);
    gint64 value = 0;

    if (json_get_int64(layouts, "current_idx", &value)) {
        client->keyboard_index = (int)value;
    }

    clear_ptr_array(client->keyboard_names);
    if (json_object_has_member(layouts, "names")) {
        JsonNode *names_node = json_object_get_member(layouts, "names");
        if (names_node != NULL && JSON_NODE_HOLDS_ARRAY(names_node)) {
            JsonArray *names = json_node_get_array(names_node);
            guint length = json_array_get_length(names);
            for (guint i = 0; i < length; i++) {
                JsonNode *name_node = json_array_get_element(names, i);
                const char *name = json_node_get_string(name_node);
                g_ptr_array_add(client->keyboard_names, g_strdup(name));
            }
        }
    }

    update_keyboard_code(client);
}

static void parse_workspaces_changed(NiriClient *client, JsonObject *payload) {
    if (!json_object_has_member(payload, "workspaces")) {
        return;
    }
    JsonNode *node = json_object_get_member(payload, "workspaces");
    if (node == NULL || !JSON_NODE_HOLDS_ARRAY(node)) {
        return;
    }

    clear_ptr_array(client->workspaces);
    JsonArray *array = json_node_get_array(node);
    guint length = json_array_get_length(array);
    for (guint i = 0; i < length; i++) {
        JsonNode *element = json_array_get_element(array, i);
        if (element != NULL && JSON_NODE_HOLDS_OBJECT(element)) {
            g_ptr_array_add(client->workspaces,
                            parse_workspace(json_node_get_object(element)));
        }
    }
    g_ptr_array_sort(client->workspaces, workspace_compare);
}

static void parse_windows_changed(NiriClient *client, JsonObject *payload) {
    if (!json_object_has_member(payload, "windows")) {
        return;
    }
    JsonNode *node = json_object_get_member(payload, "windows");
    if (node == NULL || !JSON_NODE_HOLDS_ARRAY(node)) {
        return;
    }

    clear_ptr_array(client->windows);
    JsonArray *array = json_node_get_array(node);
    guint length = json_array_get_length(array);
    for (guint i = 0; i < length; i++) {
        JsonNode *element = json_array_get_element(array, i);
        if (element != NULL && JSON_NODE_HOLDS_OBJECT(element)) {
            g_ptr_array_add(client->windows,
                            parse_window(json_node_get_object(element)));
        }
    }
    g_ptr_array_sort(client->windows, window_compare);
}

static void parse_event(NiriClient *client, JsonObject *object) {
    GList *members = json_object_get_members(object);
    if (members == NULL) {
        return;
    }

    const char *name = members->data;
    JsonNode *payload_node = json_object_get_member(object, name);
    if (payload_node == NULL || !JSON_NODE_HOLDS_OBJECT(payload_node)) {
        g_list_free(members);
        return;
    }

    JsonObject *payload = json_node_get_object(payload_node);
    gboolean notify = FALSE;
    gint64 value = 0;

    if (g_strcmp0(name, "WorkspacesChanged") == 0) {
        parse_workspaces_changed(client, payload);
        notify = TRUE;
    } else if (g_strcmp0(name, "WindowsChanged") == 0) {
        parse_windows_changed(client, payload);
        notify = TRUE;
    } else if (g_strcmp0(name, "WorkspaceActivated") == 0) {
        if (json_get_int64(payload, "id", &value)) {
            gboolean focused = json_get_boolean(payload, "focused");
            for (guint i = 0; i < client->workspaces->len; i++) {
                NiriWorkspace *workspace =
                    g_ptr_array_index(client->workspaces, i);
                if (focused) {
                    workspace->is_focused = workspace->id == value;
                }
                if (workspace->id == value) {
                    workspace->is_active = TRUE;
                }
            }
            notify = TRUE;
        }
    } else if (g_strcmp0(name, "WorkspaceActiveWindowChanged") == 0) {
        gint64 workspace_id = 0;
        gint64 active_window_id = -1;
        if (json_get_int64(payload, "workspace_id", &workspace_id)) {
            json_get_int64(payload, "active_window_id", &active_window_id);
            for (guint i = 0; i < client->workspaces->len; i++) {
                NiriWorkspace *workspace =
                    g_ptr_array_index(client->workspaces, i);
                if (workspace->id == workspace_id) {
                    workspace->active_window_id = active_window_id;
                    break;
                }
            }
            notify = TRUE;
        }
    } else if (g_strcmp0(name, "WindowOpenedOrChanged") == 0) {
        if (json_object_has_member(payload, "window")) {
            JsonNode *window_node =
                json_object_get_member(payload, "window");
            if (window_node != NULL && JSON_NODE_HOLDS_OBJECT(window_node)) {
                NiriWindow *new_window =
                    parse_window(json_node_get_object(window_node));
                if (new_window->is_focused) {
                    for (guint i = 0; i < client->windows->len; i++) {
                        NiriWindow *window =
                            g_ptr_array_index(client->windows, i);
                        window->is_focused = FALSE;
                    }
                }
                NiriWindow *old_window =
                    find_window(client, new_window->id);
                if (old_window != NULL) {
                    for (guint i = 0; i < client->windows->len; i++) {
                        if (g_ptr_array_index(client->windows, i) ==
                            old_window) {
                            niri_window_free(old_window);
                            g_ptr_array_index(client->windows, i) =
                                new_window;
                            break;
                        }
                    }
                } else {
                    g_ptr_array_add(client->windows, new_window);
                }
                g_ptr_array_sort(client->windows, window_compare);
                notify = TRUE;
            }
        }
    } else if (g_strcmp0(name, "WindowClosed") == 0) {
        if (json_get_int64(payload, "id", &value)) {
            for (guint i = 0; i < client->windows->len; i++) {
                NiriWindow *window =
                    g_ptr_array_index(client->windows, i);
                if (window->id == value) {
                    g_ptr_array_remove_index(client->windows, i);
                    break;
                }
            }
            notify = TRUE;
        }
    } else if (g_strcmp0(name, "WindowFocusChanged") == 0) {
        gint64 focused_id = -1;
        json_get_int64(payload, "id", &focused_id);
        for (guint i = 0; i < client->windows->len; i++) {
            NiriWindow *window = g_ptr_array_index(client->windows, i);
            window->is_focused = window->id == focused_id;
        }
        notify = TRUE;
    } else if (g_strcmp0(name, "KeyboardLayoutsChanged") == 0) {
        parse_keyboard_layout(client, payload);
        notify = TRUE;
    } else if (g_strcmp0(name, "KeyboardLayoutSwitched") == 0) {
        if (json_get_int64(payload, "idx", &value)) {
            client->keyboard_index = (int)value;
            update_keyboard_code(client);
            notify = TRUE;
        }
    }

    g_list_free(members);

    if (notify) {
        niri_notify(client);
    }
}

static void parse_line(NiriClient *client, const char *line) {
    if (line[0] == '\0') {
        return;
    }

    JsonParser *parser = json_parser_new();
    GError *error = NULL;

    if (!json_parser_load_from_data(parser, line, -1, &error)) {
        g_debug("nintysh: invalid niri event: %s", error->message);
        g_error_free(error);
        g_object_unref(parser);
        return;
    }

    JsonNode *root = json_parser_get_root(parser);
    if (root != NULL && JSON_NODE_HOLDS_OBJECT(root)) {
        parse_event(client, json_node_get_object(root));
    }

    g_object_unref(parser);
}

static void niri_schedule_reconnect(NiriClient *client);

static void niri_teardown_socket(NiriClient *client) {
    if (client->socket != NULL) {
        g_socket_close(client->socket, NULL);
        g_object_unref(client->socket);
        client->socket = NULL;
    }
    client->connected = FALSE;
    g_string_truncate(client->rx, 0);
    clear_ptr_array(client->workspaces);
    clear_ptr_array(client->windows);
    clear_ptr_array(client->keyboard_names);
    client->keyboard_layout[0] = '\0';
}

static gboolean niri_socket_ready(GSocket *socket, GIOCondition condition,
                                  gpointer data) {
    NiriClient *client = data;

    if (condition & (G_IO_HUP | G_IO_ERR)) {
        GSource *source = client->io_source;
        client->io_source = NULL;
        niri_teardown_socket(client);
        niri_notify(client);
        niri_schedule_reconnect(client);
        g_source_unref(source);
        return G_SOURCE_REMOVE;
    }

    char buffer[8192];
    for (;;) {
        GError *error = NULL;
        gssize read = g_socket_receive(socket, buffer, sizeof(buffer), NULL,
                                       &error);
        if (read < 0) {
            if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK)) {
                g_clear_error(&error);
                break;
            }
            g_debug("nintysh: niri socket read failed: %s", error->message);
            g_clear_error(&error);

            GSource *source = client->io_source;
            client->io_source = NULL;
            niri_teardown_socket(client);
            niri_notify(client);
            niri_schedule_reconnect(client);
            g_source_unref(source);
            return G_SOURCE_REMOVE;
        }
        if (read == 0) {
            GSource *source = client->io_source;
            client->io_source = NULL;
            niri_teardown_socket(client);
            niri_notify(client);
            niri_schedule_reconnect(client);
            g_source_unref(source);
            return G_SOURCE_REMOVE;
        }

        g_string_append_len(client->rx, buffer, read);
    }

    for (;;) {
        char *newline = strchr(client->rx->str, '\n');
        if (newline == NULL) {
            break;
        }
        gsize line_len = (gsize)(newline - client->rx->str);
        char *line = g_strndup(client->rx->str, line_len);
        g_string_erase(client->rx, 0, line_len + 1);
        parse_line(client, line);
        g_free(line);
    }

    return G_SOURCE_CONTINUE;
}

static gboolean niri_reconnect_cb(gpointer data) {
    NiriClient *client = data;
    client->reconnect_source = 0;

    const char *path = g_getenv("NIRI_SOCKET");
    if (path == NULL || path[0] == '\0') {
        niri_schedule_reconnect(client);
        return G_SOURCE_REMOVE;
    }

    GError *error = NULL;
    GSocket *socket =
        g_socket_new(G_SOCKET_FAMILY_UNIX, G_SOCKET_TYPE_STREAM,
                     G_SOCKET_PROTOCOL_DEFAULT, &error);
    if (socket == NULL) {
        g_warning("nintysh: cannot create niri socket: %s", error->message);
        g_clear_error(&error);
        niri_schedule_reconnect(client);
        return G_SOURCE_REMOVE;
    }

    GSocketAddress *address = g_unix_socket_address_new(path);
    if (!g_socket_connect(socket, address, NULL, &error)) {
        g_debug("nintysh: niri IPC unavailable: %s", error->message);
        g_clear_error(&error);
        g_object_unref(address);
        g_object_unref(socket);
        niri_schedule_reconnect(client);
        return G_SOURCE_REMOVE;
    }
    g_object_unref(address);

    const char request[] = "\"EventStream\"\n";
    gsize sent = 0;
    while (sent < sizeof(request) - 1) {
        gssize written =
            g_socket_send(socket, request + sent,
                          sizeof(request) - 1 - sent, NULL, &error);
        if (written <= 0) {
            g_debug("nintysh: cannot subscribe to niri events: %s",
                    error != NULL ? error->message : "socket closed");
            g_clear_error(&error);
            g_object_unref(socket);
            niri_schedule_reconnect(client);
            return G_SOURCE_REMOVE;
        }
        sent += (gsize)written;
    }

    g_socket_set_blocking(socket, FALSE);
    client->socket = socket;
    client->connected = TRUE;
    client->io_source =
        g_socket_create_source(socket, G_IO_IN | G_IO_HUP | G_IO_ERR, NULL);
    g_source_set_callback(client->io_source,
                          G_SOURCE_FUNC(niri_socket_ready), client, NULL);
    g_source_attach(client->io_source, NULL);

    niri_notify(client);
    return G_SOURCE_REMOVE;
}

static void niri_schedule_reconnect(NiriClient *client) {
    if (client->reconnect_source != 0) {
        return;
    }
    client->reconnect_source =
        g_timeout_add_seconds(2, niri_reconnect_cb, client);
}

NiriClient *niri_client_new(NiriChangedFunc changed, gpointer user_data) {
    NiriClient *client = g_new0(NiriClient, 1);

    client->rx = g_string_new(NULL);
    client->workspaces =
        g_ptr_array_new_with_free_func((GDestroyNotify)niri_workspace_free);
    client->windows =
        g_ptr_array_new_with_free_func((GDestroyNotify)niri_window_free);
    client->keyboard_names =
        g_ptr_array_new_with_free_func((GDestroyNotify)g_free);
    client->changed = changed;
    client->user_data = user_data;

    niri_reconnect_cb(client);
    return client;
}

void niri_client_free(NiriClient *client) {
    if (client == NULL) {
        return;
    }

    if (client->reconnect_source != 0) {
        g_source_remove(client->reconnect_source);
        client->reconnect_source = 0;
    }
    if (client->io_source != NULL) {
        g_source_destroy(client->io_source);
        g_source_unref(client->io_source);
        client->io_source = NULL;
    }
    niri_teardown_socket(client);

    g_ptr_array_unref(client->workspaces);
    g_ptr_array_unref(client->windows);
    g_ptr_array_unref(client->keyboard_names);
    g_string_free(client->rx, TRUE);
    g_free(client);
}

gboolean niri_client_is_connected(NiriClient *client) {
    return client->connected;
}

guint niri_client_workspace_count(NiriClient *client) {
    return client->workspaces->len;
}

NiriWorkspace *niri_client_workspace(NiriClient *client, guint index) {
    if (index >= client->workspaces->len) {
        return NULL;
    }
    return g_ptr_array_index(client->workspaces, index);
}

guint niri_client_window_count(NiriClient *client) {
    return client->windows->len;
}

NiriWindow *niri_client_window(NiriClient *client, guint index) {
    if (index >= client->windows->len) {
        return NULL;
    }
    return g_ptr_array_index(client->windows, index);
}

const char *niri_client_keyboard_layout(NiriClient *client) {
    return client->keyboard_layout;
}

static void niri_spawn_action(const char *const *argv) {
    GError *error = NULL;
    if (!g_spawn_async(NULL, (char **)argv, NULL, G_SPAWN_SEARCH_PATH,
                       NULL, NULL, NULL, &error)) {
        g_warning("nintysh: cannot run niri action: %s", error->message);
        g_error_free(error);
    }
}

void niri_client_focus_workspace(NiriClient *client, gint64 idx) {
    (void)client;
    char index[32];
    g_snprintf(index, sizeof(index), "%" G_GINT64_FORMAT, idx);
    const char *argv[] = { "niri", "msg", "action", "focus-workspace",
                           index, NULL };
    niri_spawn_action(argv);
}

void niri_client_focus_window(NiriClient *client, gint64 id) {
    (void)client;
    char window_id[32];
    g_snprintf(window_id, sizeof(window_id), "%" G_GINT64_FORMAT, id);
    const char *argv[] = { "niri", "msg", "action", "focus-window",
                           "--id", window_id, NULL };
    niri_spawn_action(argv);
}

void niri_client_next_layout(NiriClient *client) {
    (void)client;
    const char *argv[] = { "niri", "msg", "action", "switch-layout",
                           "next", NULL };
    niri_spawn_action(argv);
}

static void niri_window_action(const char *action, gint64 id) {
    char window_id[32];
    g_snprintf(window_id, sizeof(window_id), "%" G_GINT64_FORMAT, id);
    const char *argv[] = { "niri", "msg", "action", action, "--id",
                           window_id, NULL };
    niri_spawn_action(argv);
}

void niri_client_close_window(NiriClient *client, gint64 id) {
    (void)client;
    niri_window_action("close-window", id);
}

void niri_client_toggle_floating(NiriClient *client, gint64 id) {
    (void)client;
    niri_window_action("toggle-window-floating", id);
}

void niri_client_fullscreen(NiriClient *client, gint64 id) {
    (void)client;
    niri_window_action("fullscreen-window", id);
}
