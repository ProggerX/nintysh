#include "compositor-backend.h"

#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>
#include <json-glib/json-glib.h>
#include <string.h>

#define SWAY_IPC_MAGIC "i3-ipc"
#define SWAY_IPC_HEADER_SIZE 14
#define SWAY_IPC_MAX_PAYLOAD (64 * 1024 * 1024)

#define SWAY_IPC_RUN_COMMAND 0
#define SWAY_IPC_GET_WORKSPACES 1
#define SWAY_IPC_SUBSCRIBE 2
#define SWAY_IPC_GET_TREE 4
#define SWAY_IPC_GET_INPUTS 100

typedef struct {
    CompositorClient *base;
    GSocket *socket;
    GSource *io_source;
    GByteArray *rx;
    GPtrArray *workspaces;
    GPtrArray *windows;
    char keyboard_layout[8];
    gboolean connected;
    guint reconnect_source;
    guint refresh_source;
} SwayClient;

static guint32 read_le32(const guchar *data) {
    return (guint32)data[0] | ((guint32)data[1] << 8) |
           ((guint32)data[2] << 16) | ((guint32)data[3] << 24);
}

static void write_le32(guchar *data, guint32 value) {
    data[0] = (guchar)(value & 0xff);
    data[1] = (guchar)((value >> 8) & 0xff);
    data[2] = (guchar)((value >> 16) & 0xff);
    data[3] = (guchar)((value >> 24) & 0xff);
}

static gboolean socket_send_all(GSocket *socket, const guchar *data,
                                gsize length) {
    gsize sent = 0;

    while (sent < length) {
        GError *error = NULL;
        gssize written =
            g_socket_send(socket, (const char *)data + sent, length - sent,
                          NULL, &error);
        if (written <= 0) {
            g_debug("nintysh: sway IPC write failed: %s",
                    error != NULL ? error->message : "socket closed");
            g_clear_error(&error);
            return FALSE;
        }
        sent += (gsize)written;
    }
    return TRUE;
}

static gboolean socket_read_all(GSocket *socket, guchar *data, gsize length) {
    gsize received = 0;

    while (received < length) {
        GError *error = NULL;
        gssize read = g_socket_receive(socket, (char *)data + received,
                                       length - received, NULL, &error);
        if (read <= 0) {
            g_debug("nintysh: sway IPC read failed: %s",
                    error != NULL ? error->message : "socket closed");
            g_clear_error(&error);
            return FALSE;
        }
        received += (gsize)read;
    }
    return TRUE;
}

static gboolean sway_send_message(GSocket *socket, guint32 type,
                                  const char *payload) {
    guint32 length = payload != NULL ? (guint32)strlen(payload) : 0;
    guchar header[SWAY_IPC_HEADER_SIZE];

    memcpy(header, SWAY_IPC_MAGIC, 6);
    write_le32(header + 6, length);
    write_le32(header + 10, type);

    return socket_send_all(socket, header, sizeof(header)) &&
           (length == 0 ||
            socket_send_all(socket, (const guchar *)payload, length));
}

static gchar *sway_ipc_request(guint32 type, const char *payload,
                               guint32 *reply_type) {
    const char *path = g_getenv("SWAYSOCK");
    if (path == NULL || path[0] == '\0') {
        return NULL;
    }

    GError *error = NULL;
    GSocket *socket =
        g_socket_new(G_SOCKET_FAMILY_UNIX, G_SOCKET_TYPE_STREAM,
                     G_SOCKET_PROTOCOL_DEFAULT, &error);
    if (socket == NULL) {
        g_debug("nintysh: cannot create sway IPC socket: %s", error->message);
        g_clear_error(&error);
        return NULL;
    }

    g_socket_set_timeout(socket, 2);

    GSocketAddress *address = g_unix_socket_address_new(path);
    if (!g_socket_connect(socket, address, NULL, &error)) {
        g_debug("nintysh: sway IPC unavailable: %s", error->message);
        g_clear_error(&error);
        g_object_unref(address);
        g_object_unref(socket);
        return NULL;
    }
    g_object_unref(address);

    gchar *reply = NULL;
    guchar header[SWAY_IPC_HEADER_SIZE];

    if (!sway_send_message(socket, type, payload) ||
        !socket_read_all(socket, header, sizeof(header)) ||
        memcmp(header, SWAY_IPC_MAGIC, 6) != 0) {
        goto out;
    }

    guint32 length = read_le32(header + 6);
    if (length > SWAY_IPC_MAX_PAYLOAD) {
        g_debug("nintysh: sway IPC reply too large");
        goto out;
    }

    reply = g_malloc((gsize)length + 1);
    if (length > 0 && !socket_read_all(socket, (guchar *)reply, length)) {
        g_free(reply);
        reply = NULL;
        goto out;
    }
    reply[length] = '\0';
    if (reply_type != NULL) {
        *reply_type = read_le32(header + 10);
    }

out:
    g_socket_close(socket, NULL);
    g_object_unref(socket);
    return reply;
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

static void sway_parse_workspaces(SwayClient *client, const char *json) {
    JsonParser *parser = json_parser_new();
    GError *error = NULL;

    if (!json_parser_load_from_data(parser, json, -1, &error)) {
        g_debug("nintysh: invalid sway workspace reply: %s", error->message);
        g_clear_error(&error);
        g_object_unref(parser);
        return;
    }

    JsonNode *root = json_parser_get_root(parser);
    if (root == NULL || !JSON_NODE_HOLDS_ARRAY(root)) {
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
        workspace->id = (gint64)(i + 1);
        json_get_int64(object, "id", &workspace->id);
        workspace->idx = workspace->id;
        json_get_int64(object, "num", &workspace->idx);
        if (workspace->idx < 0) {
            workspace->idx = workspace->id;
        }
        workspace->name = json_dup_string(object, "name");
        workspace->output = json_dup_string(object, "output");
        workspace->is_active = json_get_boolean(object, "visible");
        workspace->is_focused = json_get_boolean(object, "focused");
        workspace->is_urgent = json_get_boolean(object, "urgent");
        g_ptr_array_add(client->workspaces, workspace);
    }

    g_ptr_array_sort(client->workspaces, workspace_compare);
    g_object_unref(parser);
}

static gboolean sway_node_is_window(JsonObject *object) {
    char *app_id = json_dup_string(object, "app_id");
    gboolean is_window = app_id != NULL && app_id[0] != '\0';
    g_free(app_id);
    if (is_window) {
        return TRUE;
    }

    if (!json_object_has_member(object, "window")) {
        return FALSE;
    }
    JsonNode *node = json_object_get_member(object, "window");
    if (node == NULL || !JSON_NODE_HOLDS_VALUE(node)) {
        return FALSE;
    }
    if (json_node_get_value_type(node) == G_TYPE_INT64 ||
        json_node_get_value_type(node) == G_TYPE_INT) {
        return json_node_get_int(node) != 0;
    }
    return FALSE;
}

static char *sway_node_app_id(JsonObject *object) {
    char *app_id = json_dup_string(object, "app_id");
    if (app_id != NULL && app_id[0] != '\0') {
        return app_id;
    }
    g_free(app_id);

    if (json_object_has_member(object, "window_properties")) {
        JsonNode *node = json_object_get_member(object, "window_properties");
        if (node != NULL && JSON_NODE_HOLDS_OBJECT(node)) {
            JsonObject *properties = json_node_get_object(node);
            char *value = json_dup_string(properties, "class");
            if (value == NULL) {
                value = json_dup_string(properties, "instance");
            }
            if (value != NULL) {
                return value;
            }
        }
    }
    return NULL;
}

static void sway_walk_node(SwayClient *client, JsonObject *object,
                           gint64 workspace_id, gboolean scratchpad) {
    gint64 id = 0;
    json_get_int64(object, "id", &id);

    char *type = json_dup_string(object, "type");
    char *name = json_dup_string(object, "name");

    if (g_strcmp0(type, "workspace") == 0) {
        workspace_id = id;
        scratchpad = name != NULL && g_str_has_prefix(name, "__i3");
    }

    if (!scratchpad &&
        (g_strcmp0(type, "con") == 0 ||
         g_strcmp0(type, "floating_con") == 0) &&
        sway_node_is_window(object)) {
        CompositorWindow *window = compositor_window_new();
        window->id = id;
        window->workspace_id = workspace_id;
        window->title = name != NULL ? g_strdup(name) : NULL;
        window->app_id = sway_node_app_id(object);
        window->is_focused = json_get_boolean(object, "focused");
        window->is_urgent = json_get_boolean(object, "urgent");
        g_ptr_array_add(client->windows, window);
    }

    g_free(type);
    g_free(name);

    const char *children[] = { "nodes", "floating_nodes" };
    for (guint c = 0; c < G_N_ELEMENTS(children); c++) {
        if (!json_object_has_member(object, children[c])) {
            continue;
        }
        JsonNode *node = json_object_get_member(object, children[c]);
        if (node == NULL || !JSON_NODE_HOLDS_ARRAY(node)) {
            continue;
        }
        JsonArray *array = json_node_get_array(node);
        guint length = json_array_get_length(array);
        for (guint i = 0; i < length; i++) {
            JsonNode *element = json_array_get_element(array, i);
            if (element != NULL && JSON_NODE_HOLDS_OBJECT(element)) {
                sway_walk_node(client, json_node_get_object(element),
                               workspace_id, scratchpad);
            }
        }
    }
}

static void sway_parse_tree(SwayClient *client, const char *json) {
    JsonParser *parser = json_parser_new();
    GError *error = NULL;

    if (!json_parser_load_from_data(parser, json, -1, &error)) {
        g_debug("nintysh: invalid sway tree reply: %s", error->message);
        g_clear_error(&error);
        g_object_unref(parser);
        return;
    }

    JsonNode *root = json_parser_get_root(parser);
    if (root == NULL || !JSON_NODE_HOLDS_OBJECT(root)) {
        g_object_unref(parser);
        return;
    }

    compositor_clear_windows(client->windows);
    sway_walk_node(client, json_node_get_object(root), -1, FALSE);
    g_ptr_array_sort(client->windows, window_compare);
    g_object_unref(parser);
}

static void sway_parse_inputs(SwayClient *client, const char *json) {
    JsonParser *parser = json_parser_new();
    GError *error = NULL;

    if (!json_parser_load_from_data(parser, json, -1, &error)) {
        g_debug("nintysh: invalid sway inputs reply: %s", error->message);
        g_clear_error(&error);
        g_object_unref(parser);
        return;
    }

    JsonNode *root = json_parser_get_root(parser);
    if (root == NULL) {
        g_object_unref(parser);
        return;
    }

    JsonArray *array = NULL;
    if (JSON_NODE_HOLDS_ARRAY(root)) {
        array = json_node_get_array(root);
    } else if (JSON_NODE_HOLDS_OBJECT(root)) {
        JsonObject *object = json_node_get_object(root);
        if (json_object_has_member(object, "input")) {
            JsonNode *node = json_object_get_member(object, "input");
            if (node != NULL && JSON_NODE_HOLDS_ARRAY(node)) {
                array = json_node_get_array(node);
            }
        }
    }
    if (array == NULL) {
        g_object_unref(parser);
        return;
    }

    guint length = json_array_get_length(array);
    for (guint i = 0; i < length; i++) {
        JsonNode *element = json_array_get_element(array, i);
        if (element == NULL || !JSON_NODE_HOLDS_OBJECT(element)) {
            continue;
        }
        JsonObject *object = json_node_get_object(element);
        char *type = json_dup_string(object, "type");
        gboolean keyboard = g_strcmp0(type, "keyboard") == 0;
        g_free(type);
        if (!keyboard) {
            continue;
        }

        char *name = json_dup_string(object, "xkb_active_layout_name");
        if (name == NULL || name[0] == '\0') {
            g_free(name);
            name = NULL;
            if (json_object_has_member(object, "xkb_layout_names")) {
                JsonNode *node =
                    json_object_get_member(object, "xkb_layout_names");
                if (node != NULL && JSON_NODE_HOLDS_ARRAY(node)) {
                    JsonArray *names = json_node_get_array(node);
                    if (json_array_get_length(names) > 0) {
                        JsonNode *first = json_array_get_element(names, 0);
                        if (first != NULL && JSON_NODE_HOLDS_VALUE(first) &&
                            json_node_get_value_type(first) == G_TYPE_STRING) {
                            name = g_strdup(json_node_get_string(first));
                        }
                    }
                }
            }
        }
        if (name != NULL) {
            compositor_set_keyboard_layout(client->keyboard_layout,
                                           sizeof(client->keyboard_layout),
                                           name);
            g_free(name);
            break;
        }
    }

    g_object_unref(parser);
}

static void sway_refresh(SwayClient *client) {
    gchar *reply = NULL;
    guint32 type = 0;

    reply = sway_ipc_request(SWAY_IPC_GET_WORKSPACES, NULL, &type);
    if (reply != NULL) {
        sway_parse_workspaces(client, reply);
        g_free(reply);
    }

    reply = sway_ipc_request(SWAY_IPC_GET_TREE, NULL, &type);
    if (reply != NULL) {
        sway_parse_tree(client, reply);
        g_free(reply);
    }

    reply = sway_ipc_request(SWAY_IPC_GET_INPUTS, NULL, &type);
    if (reply != NULL) {
        sway_parse_inputs(client, reply);
        g_free(reply);
    }

    compositor_client_notify(client->base);
}

static gboolean sway_refresh_cb(gpointer data) {
    SwayClient *client = data;
    client->refresh_source = 0;
    sway_refresh(client);
    return G_SOURCE_REMOVE;
}

static void sway_schedule_refresh(SwayClient *client) {
    if (client->refresh_source == 0) {
        client->refresh_source =
            g_timeout_add(50, sway_refresh_cb, client);
    }
}

static void sway_teardown_connection(SwayClient *client) {
    if (client->socket != NULL) {
        g_socket_close(client->socket, NULL);
        g_object_unref(client->socket);
        client->socket = NULL;
    }
    client->connected = FALSE;
    g_byte_array_set_size(client->rx, 0);
    compositor_clear_workspaces(client->workspaces);
    compositor_clear_windows(client->windows);
    client->keyboard_layout[0] = '\0';
}

static void sway_schedule_reconnect(SwayClient *client);

static gboolean sway_event_ready(GSocket *socket, GIOCondition condition,
                                 gpointer data) {
    SwayClient *client = data;

    if (condition & (G_IO_HUP | G_IO_ERR)) {
        GSource *source = client->io_source;
        client->io_source = NULL;
        sway_teardown_connection(client);
        compositor_client_notify(client->base);
        sway_schedule_reconnect(client);
        g_source_unref(source);
        return G_SOURCE_REMOVE;
    }

    guchar buffer[8192];
    for (;;) {
        GError *error = NULL;
        gssize read = g_socket_receive(socket, (char *)buffer, sizeof(buffer),
                                       NULL, &error);
        if (read < 0) {
            if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK)) {
                g_clear_error(&error);
                break;
            }
            g_debug("nintysh: sway event read failed: %s", error->message);
            g_clear_error(&error);

            GSource *source = client->io_source;
            client->io_source = NULL;
            sway_teardown_connection(client);
            compositor_client_notify(client->base);
            sway_schedule_reconnect(client);
            g_source_unref(source);
            return G_SOURCE_REMOVE;
        }
        if (read == 0) {
            GSource *source = client->io_source;
            client->io_source = NULL;
            sway_teardown_connection(client);
            compositor_client_notify(client->base);
            sway_schedule_reconnect(client);
            g_source_unref(source);
            return G_SOURCE_REMOVE;
        }

        g_byte_array_append(client->rx, buffer, (guint)read);
    }

    while (client->rx->len >= SWAY_IPC_HEADER_SIZE) {
        const guchar *data = client->rx->data;
        if (memcmp(data, SWAY_IPC_MAGIC, 6) != 0) {
            g_byte_array_remove_index(client->rx, 0);
            continue;
        }

        guint32 length = read_le32(data + 6);
        if (length > SWAY_IPC_MAX_PAYLOAD) {
            g_byte_array_set_size(client->rx, 0);
            break;
        }
        if (client->rx->len < SWAY_IPC_HEADER_SIZE + length) {
            break;
        }

        g_byte_array_remove_range(client->rx, 0,
                                  SWAY_IPC_HEADER_SIZE + length);
        sway_schedule_refresh(client);
    }

    return G_SOURCE_CONTINUE;
}

static gboolean sway_connect(SwayClient *client) {
    const char *path = g_getenv("SWAYSOCK");
    if (path == NULL || path[0] == '\0') {
        return FALSE;
    }

    GError *error = NULL;
    GSocket *socket =
        g_socket_new(G_SOCKET_FAMILY_UNIX, G_SOCKET_TYPE_STREAM,
                     G_SOCKET_PROTOCOL_DEFAULT, &error);
    if (socket == NULL) {
        g_debug("nintysh: cannot create sway event socket: %s",
                error->message);
        g_clear_error(&error);
        return FALSE;
    }

    GSocketAddress *address = g_unix_socket_address_new(path);
    if (!g_socket_connect(socket, address, NULL, &error)) {
        g_debug("nintysh: sway IPC unavailable: %s", error->message);
        g_clear_error(&error);
        g_object_unref(address);
        g_object_unref(socket);
        return FALSE;
    }
    g_object_unref(address);
    g_socket_set_timeout(socket, 2);

    const char *subscribe = "[\"workspace\",\"window\"]";
    if (!sway_send_message(socket, SWAY_IPC_SUBSCRIBE, subscribe)) {
        g_object_unref(socket);
        return FALSE;
    }

    g_socket_set_blocking(socket, FALSE);
    client->socket = socket;
    client->connected = TRUE;
    client->io_source =
        g_socket_create_source(socket, G_IO_IN | G_IO_HUP | G_IO_ERR, NULL);
    g_source_set_callback(client->io_source,
                          G_SOURCE_FUNC(sway_event_ready), client, NULL);
    g_source_attach(client->io_source, NULL);

    compositor_client_notify(client->base);
    sway_schedule_refresh(client);
    return TRUE;
}

static gboolean sway_reconnect_cb(gpointer data) {
    SwayClient *client = data;
    client->reconnect_source = 0;

    if (!client->connected) {
        sway_connect(client);
    }
    if (!client->connected) {
        sway_schedule_reconnect(client);
    }
    return G_SOURCE_REMOVE;
}

static void sway_schedule_reconnect(SwayClient *client) {
    if (client->reconnect_source != 0 || client->connected) {
        return;
    }
    client->reconnect_source =
        g_timeout_add_seconds(2, sway_reconnect_cb, client);
}

static gboolean sway_ops_init(CompositorClient *base) {
    SwayClient *client = g_new0(SwayClient, 1);

    client->base = base;
    client->rx = g_byte_array_new();
    client->workspaces = g_ptr_array_new_with_free_func(
        (GDestroyNotify)compositor_workspace_free);
    client->windows = g_ptr_array_new_with_free_func(
        (GDestroyNotify)compositor_window_free);
    base->impl = client;

    if (!sway_connect(client)) {
        sway_schedule_reconnect(client);
    }
    return TRUE;
}

static void sway_ops_destroy(CompositorClient *base) {
    SwayClient *client = base->impl;
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

    sway_teardown_connection(client);
    g_ptr_array_unref(client->workspaces);
    g_ptr_array_unref(client->windows);
    g_byte_array_unref(client->rx);
    g_free(client);
    base->impl = NULL;
}

static gboolean sway_ops_is_connected(CompositorClient *base) {
    SwayClient *client = base->impl;
    return client != NULL && client->connected;
}

static guint sway_ops_workspace_count(CompositorClient *base) {
    SwayClient *client = base->impl;
    return client != NULL ? client->workspaces->len : 0;
}

static CompositorWorkspace *sway_ops_workspace(CompositorClient *base,
                                               guint index) {
    SwayClient *client = base->impl;
    if (client == NULL || index >= client->workspaces->len) {
        return NULL;
    }
    return g_ptr_array_index(client->workspaces, index);
}

static guint sway_ops_window_count(CompositorClient *base) {
    SwayClient *client = base->impl;
    return client != NULL ? client->windows->len : 0;
}

static CompositorWindow *sway_ops_window(CompositorClient *base,
                                         guint index) {
    SwayClient *client = base->impl;
    if (client == NULL || index >= client->windows->len) {
        return NULL;
    }
    return g_ptr_array_index(client->windows, index);
}

static const char *sway_ops_keyboard_layout(CompositorClient *base) {
    SwayClient *client = base->impl;
    return client != NULL ? client->keyboard_layout : "";
}

static void sway_run_command(const char *command) {
    gchar *reply = sway_ipc_request(SWAY_IPC_RUN_COMMAND, command, NULL);
    g_free(reply);
}

static char *sway_quote(const char *value) {
    GString *quoted = g_string_new("\"");
    for (const char *p = value; p != NULL && *p != '\0'; p++) {
        if (*p == '\\' || *p == '"') {
            g_string_append_c(quoted, '\\');
        }
        g_string_append_c(quoted, *p);
    }
    g_string_append_c(quoted, '"');
    return g_string_free(quoted, FALSE);
}

static void sway_ops_focus_workspace(CompositorClient *base, gint64 idx) {
    SwayClient *client = base->impl;
    const char *name = NULL;

    if (client != NULL) {
        for (guint i = 0; i < client->workspaces->len; i++) {
            CompositorWorkspace *workspace =
                g_ptr_array_index(client->workspaces, i);
            if (workspace->idx == idx && workspace->name != NULL) {
                name = workspace->name;
                break;
            }
        }
    }

    char *command = NULL;
    if (name != NULL) {
        char *quoted = sway_quote(name);
        command = g_strdup_printf(
            "workspace --no-auto-back-and-forth %s", quoted);
        g_free(quoted);
    } else {
        command = g_strdup_printf("workspace number %" G_GINT64_FORMAT, idx);
    }
    sway_run_command(command);
    g_free(command);
}

static void sway_ops_focus_window(CompositorClient *base, gint64 id) {
    (void)base;
    char *command =
        g_strdup_printf("[con_id=%" G_GINT64_FORMAT "] focus", id);
    sway_run_command(command);
    g_free(command);
}

static void sway_ops_next_layout(CompositorClient *base) {
    (void)base;
    sway_run_command("layout toggle split");
}

static void sway_ops_close_window(CompositorClient *base, gint64 id) {
    (void)base;
    char *command =
        g_strdup_printf("[con_id=%" G_GINT64_FORMAT "] kill", id);
    sway_run_command(command);
    g_free(command);
}

static void sway_ops_toggle_floating(CompositorClient *base, gint64 id) {
    (void)base;
    char *command = g_strdup_printf(
        "[con_id=%" G_GINT64_FORMAT "] floating toggle", id);
    sway_run_command(command);
    g_free(command);
}

static void sway_ops_fullscreen(CompositorClient *base, gint64 id) {
    (void)base;
    char *command = g_strdup_printf(
        "[con_id=%" G_GINT64_FORMAT "] fullscreen toggle", id);
    sway_run_command(command);
    g_free(command);
}

const CompositorOps sway_compositor_ops = {
    .name = "sway",
    .init = sway_ops_init,
    .destroy = sway_ops_destroy,
    .is_connected = sway_ops_is_connected,
    .workspace_count = sway_ops_workspace_count,
    .workspace = sway_ops_workspace,
    .window_count = sway_ops_window_count,
    .window = sway_ops_window,
    .keyboard_layout = sway_ops_keyboard_layout,
    .focus_workspace = sway_ops_focus_workspace,
    .focus_window = sway_ops_focus_window,
    .next_layout = sway_ops_next_layout,
    .close_window = sway_ops_close_window,
    .toggle_floating = sway_ops_toggle_floating,
    .fullscreen = sway_ops_fullscreen,
};
