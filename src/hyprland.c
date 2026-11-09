#include "compositor-backend.h"

#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>
#include <json-glib/json-glib.h>
#include <string.h>

typedef struct {
    CompositorClient *base;
    GSocket *socket;
    GSource *io_source;
    GString *rx;
    GPtrArray *workspaces;
    GPtrArray *windows;
    char keyboard_layout[8];
    gboolean connected;
    guint reconnect_source;
    guint refresh_source;
} HyprlandClient;

static gboolean socket_send_all(GSocket *socket, const char *data,
                                gsize length) {
    gsize sent = 0;

    while (sent < length) {
        GError *error = NULL;
        gssize written = g_socket_send(socket, data + sent, length - sent,
                                       NULL, &error);
        if (written <= 0) {
            g_debug("nintysh: hyprland IPC write failed: %s",
                    error != NULL ? error->message : "socket closed");
            g_clear_error(&error);
            return FALSE;
        }
        sent += (gsize)written;
    }
    return TRUE;
}

static char *hyprland_socket_path(const char *name) {
    const char *signature = g_getenv("HYPRLAND_INSTANCE_SIGNATURE");
    if (signature == NULL || signature[0] == '\0') {
        return NULL;
    }

    const char *runtime = g_getenv("XDG_RUNTIME_DIR");
    if (runtime != NULL && runtime[0] != '\0') {
        char *path = g_build_filename(runtime, "hypr", signature, name, NULL);
        if (g_file_test(path, G_FILE_TEST_EXISTS)) {
            return path;
        }
        g_free(path);
    }

    return g_build_filename("/tmp/hypr", signature, name, NULL);
}

static char *hyprland_request(const char *request, gsize *out_length) {
    char *path = hyprland_socket_path(".socket.sock");
    if (path == NULL) {
        return NULL;
    }

    GError *error = NULL;
    GSocket *socket =
        g_socket_new(G_SOCKET_FAMILY_UNIX, G_SOCKET_TYPE_STREAM,
                     G_SOCKET_PROTOCOL_DEFAULT, &error);
    if (socket == NULL) {
        g_debug("nintysh: cannot create hyprland IPC socket: %s",
                error->message);
        g_clear_error(&error);
        g_free(path);
        return NULL;
    }

    g_socket_set_timeout(socket, 2);

    GSocketAddress *address = g_unix_socket_address_new(path);
    g_free(path);
    if (!g_socket_connect(socket, address, NULL, &error)) {
        g_debug("nintysh: hyprland IPC unavailable: %s", error->message);
        g_clear_error(&error);
        g_object_unref(address);
        g_object_unref(socket);
        return NULL;
    }
    g_object_unref(address);

    if (!socket_send_all(socket, request, strlen(request))) {
        g_socket_close(socket, NULL);
        g_object_unref(socket);
        return NULL;
    }

    GString *reply = g_string_new(NULL);
    char buffer[4096];
    for (;;) {
        GError *read_error = NULL;
        gssize read = g_socket_receive(socket, buffer, sizeof(buffer), NULL,
                                       &read_error);
        if (read < 0) {
            g_debug("nintysh: hyprland IPC read failed: %s",
                    read_error->message);
            g_clear_error(&read_error);
            g_string_free(reply, TRUE);
            reply = NULL;
            break;
        }
        if (read == 0) {
            break;
        }
        g_string_append_len(reply, buffer, read);
    }

    g_socket_close(socket, NULL);
    g_object_unref(socket);

    if (reply != NULL && out_length != NULL) {
        *out_length = reply->len;
    }
    return reply != NULL ? g_string_free(reply, FALSE) : NULL;
}

static gboolean json_get_int64(JsonObject *object, const char *member,
                               gint64 *value) {
    if (object == NULL || !json_object_has_member(object, member)) {
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
    if (object == NULL || !json_object_has_member(object, member)) {
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
    return object != NULL && json_object_has_member(object, member) &&
           json_object_get_boolean_member(object, member);
}

static gboolean json_has_object(JsonObject *object, const char *member,
                                JsonObject **value) {
    if (object == NULL || !json_object_has_member(object, member)) {
        return FALSE;
    }
    JsonNode *node = json_object_get_member(object, member);
    if (node == NULL || !JSON_NODE_HOLDS_OBJECT(node)) {
        return FALSE;
    }
    *value = json_node_get_object(node);
    return TRUE;
}

static gint64 parse_hex_id(const char *value) {
    if (value == NULL || value[0] == '\0') {
        return -1;
    }
    if (g_str_has_prefix(value, "0x") || g_str_has_prefix(value, "0X")) {
        value += 2;
    }
    return (gint64)g_ascii_strtoull(value, NULL, 16);
}

static gint workspace_compare(gconstpointer a, gconstpointer b) {
    const CompositorWorkspace *wa = *(CompositorWorkspace *const *)a;
    const CompositorWorkspace *wb = *(CompositorWorkspace *const *)b;
    if (wa->idx < wb->idx) {
        return -1;
    }
    if (wa->idx > wb->idx) {
        return 1;
    }
    if (wa->id < wb->id) {
        return -1;
    }
    if (wa->id > wb->id) {
        return 1;
    }
    return 0;
}

static gint window_compare(gconstpointer a, gconstpointer b) {
    const CompositorWindow *wa = *(CompositorWindow *const *)a;
    const CompositorWindow *wb = *(CompositorWindow *const *)b;
    if (wa->id < wb->id) {
        return -1;
    }
    if (wa->id > wb->id) {
        return 1;
    }
    return 0;
}

static JsonNode *parse_json(const char *json, JsonParser **out_parser,
                            const char *kind) {
    JsonParser *parser = json_parser_new();
    GError *error = NULL;

    if (!json_parser_load_from_data(parser, json, -1, &error)) {
        g_debug("nintysh: invalid hyprland %s reply: %s", kind,
                error->message);
        g_clear_error(&error);
        g_object_unref(parser);
        return NULL;
    }

    *out_parser = parser;
    return json_parser_get_root(parser);
}

static gint64 hyprland_focused_workspace_id(JsonNode *root) {
    if (root == NULL || !JSON_NODE_HOLDS_ARRAY(root)) {
        return -1;
    }

    JsonArray *array = json_node_get_array(root);
    guint length = json_array_get_length(array);
    for (guint i = 0; i < length; i++) {
        JsonNode *element = json_array_get_element(array, i);
        if (element == NULL || !JSON_NODE_HOLDS_OBJECT(element)) {
            continue;
        }
        JsonObject *monitor = json_node_get_object(element);
        if (!json_get_boolean(monitor, "focused")) {
            continue;
        }
        JsonObject *active = NULL;
        if (json_has_object(monitor, "activeWorkspace", &active)) {
            gint64 id = -1;
            if (json_get_int64(active, "id", &id)) {
                return id;
            }
        }
    }
    return -1;
}

static void hyprland_parse_workspaces(HyprlandClient *client,
                                      const char *json,
                                      gint64 focused_workspace_id) {
    JsonParser *parser = NULL;
    JsonNode *root = parse_json(json, &parser, "workspace");
    if (root == NULL) {
        return;
    }
    if (!JSON_NODE_HOLDS_ARRAY(root)) {
        g_object_unref(parser);
        return;
    }

    compositor_clear_workspaces(client->workspaces);
    JsonArray *array = json_node_get_array(root);
    guint length = json_array_get_length(array);

    for (guint i = 0; i < length; i++) {
        JsonNode *element = json_array_get_element(array, i);
        if (element == NULL || !JSON_NODE_HOLDS_OBJECT(element)) {
            continue;
        }
        JsonObject *object = json_node_get_object(element);
        CompositorWorkspace *workspace = compositor_workspace_new();
        json_get_int64(object, "id", &workspace->id);
        workspace->idx = workspace->id;
        workspace->name = json_dup_string(object, "name");
        workspace->output = json_dup_string(object, "monitor");
        workspace->is_focused = workspace->id == focused_workspace_id;
        workspace->is_active = workspace->is_focused;

        char *last_window = json_dup_string(object, "lastwindow");
        if (last_window != NULL) {
            workspace->active_window_id = parse_hex_id(last_window);
            g_free(last_window);
        }

        g_ptr_array_add(client->workspaces, workspace);
    }

    g_ptr_array_sort(client->workspaces, workspace_compare);
    g_object_unref(parser);
}

static void hyprland_parse_clients(HyprlandClient *client, const char *json) {
    JsonParser *parser = NULL;
    JsonNode *root = parse_json(json, &parser, "client");
    if (root == NULL) {
        return;
    }
    if (!JSON_NODE_HOLDS_ARRAY(root)) {
        g_object_unref(parser);
        return;
    }

    compositor_clear_windows(client->windows);
    JsonArray *array = json_node_get_array(root);
    guint length = json_array_get_length(array);

    for (guint i = 0; i < length; i++) {
        JsonNode *element = json_array_get_element(array, i);
        if (element == NULL || !JSON_NODE_HOLDS_OBJECT(element)) {
            continue;
        }
        JsonObject *object = json_node_get_object(element);

        JsonObject *workspace = NULL;
        gint64 workspace_id = -1;
        if (json_has_object(object, "workspace", &workspace)) {
            json_get_int64(workspace, "id", &workspace_id);
        }
        if (workspace_id < 0) {
            continue;
        }

        CompositorWindow *window = compositor_window_new();
        char *address = json_dup_string(object, "address");
        window->id = parse_hex_id(address);
        g_free(address);
        window->workspace_id = workspace_id;
        window->title = json_dup_string(object, "title");
        window->app_id = json_dup_string(object, "class");
        if (window->app_id == NULL) {
            window->app_id = json_dup_string(object, "initialClass");
        }

        gint64 focus_history = -1;
        json_get_int64(object, "focusHistoryID", &focus_history);
        window->is_focused = focus_history == 0;

        g_ptr_array_add(client->windows, window);
    }

    g_ptr_array_sort(client->windows, window_compare);
    g_object_unref(parser);
}

static void hyprland_parse_devices(HyprlandClient *client, const char *json) {
    JsonParser *parser = NULL;
    JsonNode *root = parse_json(json, &parser, "device");
    if (root == NULL) {
        return;
    }
    if (!JSON_NODE_HOLDS_OBJECT(root)) {
        g_object_unref(parser);
        return;
    }

    JsonObject *object = json_node_get_object(root);
    if (!json_object_has_member(object, "keyboards")) {
        g_object_unref(parser);
        return;
    }
    JsonNode *node = json_object_get_member(object, "keyboards");
    if (node == NULL || !JSON_NODE_HOLDS_ARRAY(node)) {
        g_object_unref(parser);
        return;
    }

    JsonArray *keyboards = json_node_get_array(node);
    guint length = json_array_get_length(keyboards);
    const char *chosen = NULL;
    char *chosen_name = NULL;

    for (guint i = 0; i < length; i++) {
        JsonNode *element = json_array_get_element(keyboards, i);
        if (element == NULL || !JSON_NODE_HOLDS_OBJECT(element)) {
            continue;
        }
        JsonObject *keyboard = json_node_get_object(element);
        char *name = json_dup_string(keyboard, "active_keymap");
        if (name == NULL || name[0] == '\0') {
            g_free(name);
            name = json_dup_string(keyboard, "layout");
        }
        if (name == NULL || name[0] == '\0') {
            g_free(name);
            continue;
        }
        if (chosen_name == NULL || json_get_boolean(keyboard, "main")) {
            g_free(chosen_name);
            chosen_name = name;
            chosen = chosen_name;
        } else {
            g_free(name);
        }
    }

    if (chosen != NULL) {
        compositor_set_keyboard_layout(client->keyboard_layout,
                                       sizeof(client->keyboard_layout),
                                       chosen);
    }
    g_free(chosen_name);
    g_object_unref(parser);
}

static void hyprland_refresh(HyprlandClient *client) {
    char *workspaces = hyprland_request("j/workspaces", NULL);
    char *clients = hyprland_request("j/clients", NULL);
    char *monitors = hyprland_request("j/monitors", NULL);
    char *devices = hyprland_request("j/devices", NULL);

    gint64 focused_id = -1;
    if (monitors != NULL) {
        JsonParser *parser = NULL;
        JsonNode *root = parse_json(monitors, &parser, "monitor");
        if (root != NULL) {
            focused_id = hyprland_focused_workspace_id(root);
        }
        if (parser != NULL) {
            g_object_unref(parser);
        }
    }

    if (workspaces != NULL) {
        hyprland_parse_workspaces(client, workspaces, focused_id);
    }
    if (clients != NULL) {
        hyprland_parse_clients(client, clients);
    }
    if (devices != NULL) {
        hyprland_parse_devices(client, devices);
    }

    g_free(workspaces);
    g_free(clients);
    g_free(monitors);
    g_free(devices);

    compositor_client_notify(client->base);
}

static gboolean hyprland_refresh_cb(gpointer data) {
    HyprlandClient *client = data;
    client->refresh_source = 0;
    hyprland_refresh(client);
    return G_SOURCE_REMOVE;
}

static void hyprland_schedule_refresh(HyprlandClient *client) {
    if (client->refresh_source == 0) {
        client->refresh_source =
            g_timeout_add(50, hyprland_refresh_cb, client);
    }
}

static void hyprland_teardown_connection(HyprlandClient *client) {
    if (client->socket != NULL) {
        g_socket_close(client->socket, NULL);
        g_object_unref(client->socket);
        client->socket = NULL;
    }
    client->connected = FALSE;
    g_string_truncate(client->rx, 0);
    compositor_clear_workspaces(client->workspaces);
    compositor_clear_windows(client->windows);
    client->keyboard_layout[0] = '\0';
}

static void hyprland_schedule_reconnect(HyprlandClient *client);

static gboolean hyprland_event_ready(GSocket *socket, GIOCondition condition,
                                     gpointer data) {
    HyprlandClient *client = data;

    if (condition & (G_IO_HUP | G_IO_ERR)) {
        GSource *source = client->io_source;
        client->io_source = NULL;
        hyprland_teardown_connection(client);
        compositor_client_notify(client->base);
        hyprland_schedule_reconnect(client);
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
            g_debug("nintysh: hyprland event read failed: %s",
                    error->message);
            g_clear_error(&error);

            GSource *source = client->io_source;
            client->io_source = NULL;
            hyprland_teardown_connection(client);
            compositor_client_notify(client->base);
            hyprland_schedule_reconnect(client);
            g_source_unref(source);
            return G_SOURCE_REMOVE;
        }
        if (read == 0) {
            GSource *source = client->io_source;
            client->io_source = NULL;
            hyprland_teardown_connection(client);
            compositor_client_notify(client->base);
            hyprland_schedule_reconnect(client);
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
        gsize line_length = (gsize)(newline - client->rx->str);
        g_string_erase(client->rx, 0, line_length + 1);
        hyprland_schedule_refresh(client);
    }

    return G_SOURCE_CONTINUE;
}

static gboolean hyprland_connect(HyprlandClient *client) {
    char *path = hyprland_socket_path(".socket2.sock");
    if (path == NULL) {
        return FALSE;
    }

    GError *error = NULL;
    GSocket *socket =
        g_socket_new(G_SOCKET_FAMILY_UNIX, G_SOCKET_TYPE_STREAM,
                     G_SOCKET_PROTOCOL_DEFAULT, &error);
    if (socket == NULL) {
        g_debug("nintysh: cannot create hyprland event socket: %s",
                error->message);
        g_clear_error(&error);
        g_free(path);
        return FALSE;
    }

    GSocketAddress *address = g_unix_socket_address_new(path);
    g_free(path);
    if (!g_socket_connect(socket, address, NULL, &error)) {
        g_debug("nintysh: hyprland IPC unavailable: %s", error->message);
        g_clear_error(&error);
        g_object_unref(address);
        g_object_unref(socket);
        return FALSE;
    }
    g_object_unref(address);

    g_socket_set_blocking(socket, FALSE);
    client->socket = socket;
    client->connected = TRUE;
    client->io_source =
        g_socket_create_source(socket, G_IO_IN | G_IO_HUP | G_IO_ERR, NULL);
    g_source_set_callback(client->io_source,
                          G_SOURCE_FUNC(hyprland_event_ready), client, NULL);
    g_source_attach(client->io_source, NULL);

    compositor_client_notify(client->base);
    hyprland_schedule_refresh(client);
    return TRUE;
}

static gboolean hyprland_reconnect_cb(gpointer data) {
    HyprlandClient *client = data;
    client->reconnect_source = 0;

    if (!client->connected) {
        hyprland_connect(client);
    }
    if (!client->connected) {
        hyprland_schedule_reconnect(client);
    }
    return G_SOURCE_REMOVE;
}

static void hyprland_schedule_reconnect(HyprlandClient *client) {
    if (client->reconnect_source != 0 || client->connected) {
        return;
    }
    client->reconnect_source =
        g_timeout_add_seconds(2, hyprland_reconnect_cb, client);
}

static gboolean hyprland_ops_init(CompositorClient *base) {
    HyprlandClient *client = g_new0(HyprlandClient, 1);

    client->base = base;
    client->rx = g_string_new(NULL);
    client->workspaces = g_ptr_array_new_with_free_func(
        (GDestroyNotify)compositor_workspace_free);
    client->windows = g_ptr_array_new_with_free_func(
        (GDestroyNotify)compositor_window_free);
    base->impl = client;

    if (!hyprland_connect(client)) {
        hyprland_schedule_reconnect(client);
    }
    return TRUE;
}

static void hyprland_ops_destroy(CompositorClient *base) {
    HyprlandClient *client = base->impl;
    if (client == NULL) {
        return;
    }

    if (client->reconnect_source != 0) {
        g_source_remove(client->reconnect_source);
        client->reconnect_source = 0;
    }
    if (client->refresh_source != 0) {
        g_source_remove(client->refresh_source);
        client->refresh_source = 0;
    }
    if (client->io_source != NULL) {
        g_source_destroy(client->io_source);
        g_source_unref(client->io_source);
        client->io_source = NULL;
    }

    hyprland_teardown_connection(client);
    g_ptr_array_unref(client->workspaces);
    g_ptr_array_unref(client->windows);
    g_string_free(client->rx, TRUE);
    g_free(client);
    base->impl = NULL;
}

static gboolean hyprland_ops_is_connected(CompositorClient *base) {
    HyprlandClient *client = base->impl;
    return client != NULL && client->connected;
}

static guint hyprland_ops_workspace_count(CompositorClient *base) {
    HyprlandClient *client = base->impl;
    return client != NULL ? client->workspaces->len : 0;
}

static CompositorWorkspace *hyprland_ops_workspace(CompositorClient *base,
                                                   guint index) {
    HyprlandClient *client = base->impl;
    if (client == NULL || index >= client->workspaces->len) {
        return NULL;
    }
    return g_ptr_array_index(client->workspaces, index);
}

static guint hyprland_ops_window_count(CompositorClient *base) {
    HyprlandClient *client = base->impl;
    return client != NULL ? client->windows->len : 0;
}

static CompositorWindow *hyprland_ops_window(CompositorClient *base,
                                             guint index) {
    HyprlandClient *client = base->impl;
    if (client == NULL || index >= client->windows->len) {
        return NULL;
    }
    return g_ptr_array_index(client->windows, index);
}

static const char *hyprland_ops_keyboard_layout(CompositorClient *base) {
    HyprlandClient *client = base->impl;
    return client != NULL ? client->keyboard_layout : "";
}

static void hyprland_send(const char *request) {
    gchar *reply = hyprland_request(request, NULL);
    g_free(reply);
}

static void hyprland_window_request(const char *verb, gint64 id) {
    char *request = g_strdup_printf(
        "dispatch %s address:0x%" G_GINT64_MODIFIER "x", verb, id);
    hyprland_send(request);
    g_free(request);
}

static void hyprland_ops_focus_workspace(CompositorClient *base, gint64 idx) {
    (void)base;
    char *request =
        g_strdup_printf("dispatch workspace %" G_GINT64_FORMAT, idx);
    hyprland_send(request);
    g_free(request);
}

static void hyprland_ops_focus_window(CompositorClient *base, gint64 id) {
    (void)base;
    hyprland_window_request("focuswindow", id);
}

static void hyprland_ops_next_layout(CompositorClient *base) {
    (void)base;
    hyprland_send("switchxkblayout all next");
}

static void hyprland_ops_close_window(CompositorClient *base, gint64 id) {
    (void)base;
    hyprland_window_request("closewindow", id);
}

static void hyprland_ops_toggle_floating(CompositorClient *base, gint64 id) {
    (void)base;
    hyprland_window_request("togglefloating", id);
}

static void hyprland_ops_fullscreen(CompositorClient *base, gint64 id) {
    (void)base;
    hyprland_window_request("focuswindow", id);
    hyprland_send("dispatch fullscreen 0");
}

const CompositorOps hyprland_compositor_ops = {
    .name = "hyprland",
    .init = hyprland_ops_init,
    .destroy = hyprland_ops_destroy,
    .is_connected = hyprland_ops_is_connected,
    .workspace_count = hyprland_ops_workspace_count,
    .workspace = hyprland_ops_workspace,
    .window_count = hyprland_ops_window_count,
    .window = hyprland_ops_window,
    .keyboard_layout = hyprland_ops_keyboard_layout,
    .focus_workspace = hyprland_ops_focus_workspace,
    .focus_window = hyprland_ops_focus_window,
    .next_layout = hyprland_ops_next_layout,
    .close_window = hyprland_ops_close_window,
    .toggle_floating = hyprland_ops_toggle_floating,
    .fullscreen = hyprland_ops_fullscreen,
};
