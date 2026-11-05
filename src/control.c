#include "control.h"

#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>
#include <glib/gstdio.h>
#include <string.h>

struct NintyControl {
    GSocketService *service;
    char *path;
    NintyControlCallback callback;
    gpointer data;
};

typedef struct {
    NintyControl *control;
    GSocketConnection *connection;
    GDataInputStream *input;
} ControlRequest;

static char *control_socket_path(void) {
    const char *override = g_getenv("NINTYSH_SOCKET");
    if (override != NULL && override[0] != '\0') {
        return g_strdup(override);
    }
    const char *display = g_getenv("WAYLAND_DISPLAY");
    if (display == NULL || display[0] == '\0') {
        display = "wayland";
    }
    return g_strdup_printf("%s/nintysh-%s.sock", g_get_user_runtime_dir(),
                           display);
}

static void control_read_done(GObject *source, GAsyncResult *result,
                              gpointer user_data) {
    ControlRequest *request = user_data;
    GError *error = NULL;
    char *line = g_data_input_stream_read_line_finish(
        G_DATA_INPUT_STREAM(source), result, NULL, &error);

    if (line != NULL) {
        g_strstrip(line);
        if (line[0] != '\0' && request->control->callback != NULL) {
            request->control->callback(line, request->control->data);
        }
        g_free(line);
    }
    g_clear_error(&error);
    g_object_unref(request->input);
    g_object_unref(request->connection);
    g_free(request);
}

static gboolean control_incoming(GSocketService *service,
                                 GSocketConnection *connection,
                                 GObject *source, gpointer user_data) {
    (void)service;
    (void)source;
    NintyControl *control = user_data;
    ControlRequest *request = g_new0(ControlRequest, 1);

    request->control = control;
    request->connection = g_object_ref(connection);
    request->input = g_data_input_stream_new(
        g_io_stream_get_input_stream(G_IO_STREAM(connection)));
    g_data_input_stream_read_line_async(request->input, G_PRIORITY_DEFAULT,
                                        NULL, control_read_done, request);
    return TRUE;
}

NintyControl *ninty_control_new(NintyControlCallback callback,
                                gpointer data) {
    NintyControl *control = g_new0(NintyControl, 1);
    control->path = control_socket_path();
    control->callback = callback;
    control->data = data;

    g_unlink(control->path);
    control->service = g_socket_service_new();
    GSocketAddress *address = g_unix_socket_address_new(control->path);
    GError *error = NULL;
    if (!g_socket_listener_add_address(G_SOCKET_LISTENER(control->service),
                                       address, G_SOCKET_TYPE_STREAM,
                                       G_SOCKET_PROTOCOL_DEFAULT, NULL, NULL,
                                       &error)) {
        g_warning("nintysh: cannot listen on control socket %s: %s",
                  control->path, error->message);
        g_clear_error(&error);
        g_object_unref(address);
        g_object_unref(control->service);
        control->service = NULL;
        return control;
    }
    g_object_unref(address);
    g_chmod(control->path, 0600);
    g_signal_connect(control->service, "incoming",
                     G_CALLBACK(control_incoming), control);
    g_socket_service_start(control->service);
    return control;
}

void ninty_control_free(NintyControl *control) {
    if (control == NULL) {
        return;
    }
    if (control->service != NULL) {
        g_socket_service_stop(control->service);
        g_object_unref(control->service);
    }
    if (control->path != NULL) {
        g_unlink(control->path);
    }
    g_free(control->path);
    g_free(control);
}

int ninty_control_send(int argc, char **argv) {
    if (argc <= 0) {
        g_printerr("usage: nintysh msg <command> [args...]\n");
        return 2;
    }

    GString *command = g_string_new(NULL);
    for (int i = 0; i < argc; i++) {
        if (i > 0) {
            g_string_append_c(command, ' ');
        }
        g_string_append(command, argv[i]);
    }
    g_string_append_c(command, '\n');

    char *path = control_socket_path();
    GError *error = NULL;
    GSocket *socket =
        g_socket_new(G_SOCKET_FAMILY_UNIX, G_SOCKET_TYPE_STREAM,
                     G_SOCKET_PROTOCOL_DEFAULT, &error);
    GSocketAddress *address = g_unix_socket_address_new(path);
    int status = 0;

    if (socket == NULL ||
        !g_socket_connect(socket, address, NULL, &error)) {
        g_printerr("nintysh: shell is not running (%s)\n",
                   error != NULL ? error->message : "connection failed");
        g_clear_error(&error);
        status = 1;
    } else {
        gsize sent = 0;
        while (sent < command->len) {
            gssize written = g_socket_send(socket, command->str + sent,
                                           command->len - sent, NULL, &error);
            if (written <= 0) {
                g_printerr("nintysh: cannot send command\n");
                g_clear_error(&error);
                status = 1;
                break;
            }
            sent += (gsize)written;
        }
        g_socket_close(socket, NULL);
    }

    if (socket != NULL) {
        g_object_unref(socket);
    }
    g_object_unref(address);
    g_string_free(command, TRUE);
    g_free(path);
    return status;
}
