#include "gamma.h"

#include "gamma-protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

typedef struct {
    struct wl_output *output;
    struct zwlr_gamma_control_v1 *control;
    uint32_t global_id;
    uint32_t gamma_size;
    char *name;
    char *description;
    int brightness;
    int temperature;
    gboolean hardware_brightness;
} GammaOutput;

struct GammaClient {
    struct wl_display *display;
    struct wl_registry *registry;
    struct zwlr_gamma_control_manager_v1 *manager;
    GPtrArray *outputs;
    char *state_dir;
    char *state_path;
    char *backlight_dir;
    guint save_source;
};

static void output_geometry(void *data, struct wl_output *output, int32_t x,
                            int32_t y, int32_t physical_width,
                            int32_t physical_height, int32_t subpixel,
                            const char *make, const char *model,
                            int32_t transform) {
    (void)data;
    (void)output;
    (void)x;
    (void)y;
    (void)physical_width;
    (void)physical_height;
    (void)subpixel;
    (void)make;
    (void)model;
    (void)transform;
}

static void output_mode(void *data, struct wl_output *output,
                        uint32_t flags, int32_t width, int32_t height,
                        int32_t refresh) {
    (void)data;
    (void)output;
    (void)flags;
    (void)width;
    (void)height;
    (void)refresh;
}

static void output_done(void *data, struct wl_output *output) {
    (void)data;
    (void)output;
}

static void output_scale(void *data, struct wl_output *output,
                         int32_t factor) {
    (void)data;
    (void)output;
    (void)factor;
}

static void output_name(void *data, struct wl_output *output,
                        const char *name) {
    (void)output;
    GammaOutput *entry = data;
    g_free(entry->name);
    entry->name = g_strdup(name);
    entry->hardware_brightness =
        g_str_has_prefix(name, "eDP") ||
        g_str_has_prefix(name, "LVDS") ||
        g_str_has_prefix(name, "DSI") ||
        g_str_has_prefix(name, "MIPI");
}

static void output_description(void *data, struct wl_output *output,
                               const char *description) {
    (void)output;
    GammaOutput *entry = data;
    g_free(entry->description);
    entry->description = g_strdup(description);
}

static const struct wl_output_listener output_listener = {
    .geometry = output_geometry,
    .mode = output_mode,
    .done = output_done,
    .scale = output_scale,
    .name = output_name,
    .description = output_description,
};

static void gamma_output_free(GammaOutput *output) {
    if (output == NULL) {
        return;
    }
    if (output->control != NULL) {
        zwlr_gamma_control_v1_destroy(output->control);
    }
    if (output->output != NULL) {
        wl_output_destroy(output->output);
    }
    g_free(output->name);
    g_free(output->description);
    g_free(output);
}

static void registry_global(void *data, struct wl_registry *registry,
                            uint32_t name, const char *interface,
                            uint32_t version) {
    GammaClient *client = data;
    if (strcmp(interface, wl_output_interface.name) == 0) {
        GammaOutput *output = g_new0(GammaOutput, 1);
        output->global_id = name;
        output->brightness = 100;
        output->temperature = 6500;
        uint32_t bind_version = version < 4 ? version : 4;
        output->output = wl_registry_bind(registry, name,
                                          &wl_output_interface,
                                          bind_version);
        if (output->output != NULL) {
            wl_output_add_listener(output->output, &output_listener,
                                   output);
            g_ptr_array_add(client->outputs, output);
        } else {
            g_free(output);
        }
    } else if (strcmp(interface,
                      zwlr_gamma_control_manager_v1_interface.name) == 0) {
        client->manager = wl_registry_bind(
            registry, name, &zwlr_gamma_control_manager_v1_interface, 1);
    }
}

static void registry_global_remove(void *data, struct wl_registry *registry,
                                   uint32_t name) {
    (void)registry;
    GammaClient *client = data;
    for (guint i = 0; i < client->outputs->len; i++) {
        GammaOutput *output = g_ptr_array_index(client->outputs, i);
        if (output->global_id == name) {
            g_ptr_array_remove_index(client->outputs, i);
            break;
        }
    }
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

static void gamma_size(void *data, struct zwlr_gamma_control_v1 *control,
                       uint32_t size) {
    (void)control;
    GammaOutput *output = data;
    output->gamma_size = size;
}

static void gamma_failed(void *data, struct zwlr_gamma_control_v1 *control) {
    GammaOutput *output = data;
    if (output->control == control) {
        output->control = NULL;
        output->gamma_size = 0;
        zwlr_gamma_control_v1_destroy(control);
    }
}

static const struct zwlr_gamma_control_v1_listener gamma_control_listener = {
    .gamma_size = gamma_size,
    .failed = gamma_failed,
};

static void temperature_rgb(int kelvin, double *red, double *green,
                            double *blue) {
    double temperature = CLAMP(kelvin, 1000, 40000) / 100.0;
    double r = 255.0;
    double g = 255.0;
    double b = 255.0;

    if (temperature <= 66.0) {
        g = 99.4708025861 * log(temperature) - 161.1195681661;
        if (temperature <= 19.0) {
            b = 0.0;
        } else {
            b = 138.5177312231 * log(temperature - 10.0) -
                305.0447927307;
        }
    } else {
        r = 329.698727446 * pow(temperature - 60.0, -0.1332047592);
        g = 288.1221695283 * pow(temperature - 60.0, -0.0755148492);
    }

    *red = CLAMP(r, 0.0, 255.0) / 255.0;
    *green = CLAMP(g, 0.0, 255.0) / 255.0;
    *blue = CLAMP(b, 0.0, 255.0) / 255.0;
}

static gboolean write_all(int fd, const void *data, size_t size) {
    const char *bytes = data;
    size_t written = 0;
    while (written < size) {
        ssize_t result = write(fd, bytes + written, size - written);
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return FALSE;
        }
        written += (size_t)result;
    }
    return TRUE;
}

static char *find_backlight_dir(void) {
    GDir *dir = g_dir_open("/sys/class/backlight", 0, NULL);
    if (dir == NULL) {
        return NULL;
    }
    char *result = NULL;
    const char *name = NULL;
    while ((name = g_dir_read_name(dir)) != NULL) {
        char *max_path = g_build_filename("/sys/class/backlight", name,
                                          "max_brightness", NULL);
        char *contents = NULL;
        gboolean valid = g_file_get_contents(max_path, &contents, NULL,
                                            NULL) &&
                         atoi(contents) > 0;
        g_free(contents);
        g_free(max_path);
        if (valid) {
            result = g_build_filename("/sys/class/backlight", name, NULL);
            break;
        }
    }
    g_dir_close(dir);
    return result;
}

static int read_int_file(const char *path) {
    char *contents = NULL;
    if (!g_file_get_contents(path, &contents, NULL, NULL)) {
        return -1;
    }
    int value = atoi(contents);
    g_free(contents);
    return value;
}

static int backlight_percent(GammaClient *client) {
    if (client->backlight_dir == NULL) {
        return -1;
    }
    char *brightness_path =
        g_build_filename(client->backlight_dir, "brightness", NULL);
    char *max_path =
        g_build_filename(client->backlight_dir, "max_brightness", NULL);
    int brightness = read_int_file(brightness_path);
    int maximum = read_int_file(max_path);
    g_free(brightness_path);
    g_free(max_path);
    if (brightness < 0 || maximum <= 0) {
        return -1;
    }
    return CLAMP((int)round(brightness * 100.0 / maximum), 0, 100);
}

static gboolean set_backlight(GammaClient *client, int percent) {
    if (client->backlight_dir == NULL ||
        g_find_program_in_path("brightnessctl") == NULL) {
        return FALSE;
    }
    char *device = g_path_get_basename(client->backlight_dir);
    char *quoted = g_shell_quote(device);
    char command[256];
    g_snprintf(command, sizeof(command),
               "brightnessctl --device %s set %d%%", quoted,
               CLAMP(percent, 0, 100));
    char *output = NULL;
    gint status = 0;
    GError *error = NULL;
    gboolean ok = g_spawn_command_line_sync(command, &output, NULL,
                                            &status, &error);
    g_clear_error(&error);
    g_free(output);
    g_free(quoted);
    g_free(device);
    return ok && g_spawn_check_wait_status(status, NULL);
}

static gboolean gamma_ensure_control(GammaClient *client,
                                     GammaOutput *output) {
    if (output->control != NULL && output->gamma_size > 0) {
        return TRUE;
    }
    if (client->manager == NULL || output->output == NULL) {
        return FALSE;
    }

    struct zwlr_gamma_control_v1 *control =
        zwlr_gamma_control_manager_v1_get_gamma_control(
            client->manager, output->output);
    if (control == NULL) {
        return FALSE;
    }
    output->control = control;
    output->gamma_size = 0;
    zwlr_gamma_control_v1_add_listener(control, &gamma_control_listener,
                                       output);
    wl_display_roundtrip(client->display);
    return output->control != NULL && output->gamma_size > 0;
}

static void gamma_apply(GammaClient *client, GammaOutput *output) {
    if (!gamma_ensure_control(client, output)) {
        return;
    }

    uint32_t size = output->gamma_size;
    size_t count = (size_t)size * 3u;
    uint16_t *table = g_malloc_n(count, sizeof(uint16_t));
    double red = 1.0;
    double green = 1.0;
    double blue = 1.0;
    temperature_rgb(output->temperature, &red, &green, &blue);
    double level = output->hardware_brightness
                       ? 1.0
                       : CLAMP(output->brightness, 0, 100) / 100.0;

    for (uint32_t i = 0; i < size; i++) {
        double value = size > 1 ? (double)i / (double)(size - 1) : 0.0;
        table[i] =
            (uint16_t)round(CLAMP(value * level * red, 0.0, 1.0) *
                            65535.0);
        table[size + i] =
            (uint16_t)round(CLAMP(value * level * green, 0.0, 1.0) *
                            65535.0);
        table[size * 2u + i] =
            (uint16_t)round(CLAMP(value * level * blue, 0.0, 1.0) *
                            65535.0);
    }

    int fd = memfd_create("nintysh-gamma", MFD_CLOEXEC);
    if (fd >= 0) {
        size_t bytes = count * sizeof(uint16_t);
        if (write_all(fd, table, bytes)) {
            lseek(fd, 0, SEEK_SET);
            zwlr_gamma_control_v1_set_gamma(output->control, fd);
        }
        close(fd);
    }
    g_free(table);
    wl_display_roundtrip(client->display);
}

static void gamma_save_state(GammaClient *client) {
    if (client->state_path == NULL) {
        return;
    }
    GString *text = g_string_new(NULL);
    for (guint i = 0; i < client->outputs->len; i++) {
        GammaOutput *output = g_ptr_array_index(client->outputs, i);
        if (output->name == NULL || output->name[0] == '\0') {
            continue;
        }
        g_string_append_printf(text, "%s\t%d\t%d\n", output->name,
                               output->brightness, output->temperature);
    }
    g_mkdir_with_parents(client->state_dir, 0700);
    g_file_set_contents(client->state_path, text->str, text->len, NULL);
    g_string_free(text, TRUE);
}

static gboolean gamma_save_cb(gpointer user_data) {
    GammaClient *client = user_data;
    client->save_source = 0;
    gamma_save_state(client);
    return G_SOURCE_REMOVE;
}

static void gamma_schedule_save(GammaClient *client) {
    if (client->save_source == 0) {
        client->save_source =
            g_timeout_add(300, gamma_save_cb, client);
    }
}

static GammaOutput *gamma_find_output(GammaClient *client,
                                      const char *name) {
    for (guint i = 0; i < client->outputs->len; i++) {
        GammaOutput *output = g_ptr_array_index(client->outputs, i);
        if (g_strcmp0(output->name, name) == 0) {
            return output;
        }
    }
    return NULL;
}

static void gamma_load_state(GammaClient *client) {
    if (client->state_path == NULL) {
        return;
    }
    char *contents = NULL;
    if (!g_file_get_contents(client->state_path, &contents, NULL, NULL)) {
        return;
    }

    gchar **lines = g_strsplit(contents, "\n", -1);
    for (int i = 0; lines[i] != NULL; i++) {
        gchar **fields = g_strsplit(lines[i], "\t", 3);
        if (fields[0] != NULL && fields[1] != NULL && fields[2] != NULL) {
            GammaOutput *output = gamma_find_output(client, fields[0]);
            if (output != NULL) {
                if (output->hardware_brightness) {
                    int current = backlight_percent(client);
                    output->brightness = current >= 0 ? current : 100;
                } else {
                    output->brightness = CLAMP(atoi(fields[1]), 0, 100);
                }
                output->temperature =
                    CLAMP(atoi(fields[2]), 1000, 10000);
                gamma_apply(client, output);
            }
        }
        g_strfreev(fields);
    }
    g_strfreev(lines);
    g_free(contents);
}

GammaClient *gamma_client_new(void) {
    GammaClient *client = g_new0(GammaClient, 1);
    client->outputs =
        g_ptr_array_new_with_free_func((GDestroyNotify)gamma_output_free);
    client->state_dir =
        g_build_filename(g_get_user_state_dir(), "nintysh", NULL);
    client->state_path =
        g_build_filename(client->state_dir, "display.conf", NULL);
    client->backlight_dir = find_backlight_dir();
    client->display = wl_display_connect(NULL);
    if (client->display == NULL) {
        return client;
    }
    client->registry = wl_display_get_registry(client->display);
    if (client->registry == NULL) {
        return client;
    }
    wl_registry_add_listener(client->registry, &registry_listener, client);
    wl_display_roundtrip(client->display);
    wl_display_roundtrip(client->display);
    gamma_load_state(client);
    return client;
}

void gamma_client_free(GammaClient *client) {
    if (client == NULL) {
        return;
    }
    if (client->save_source != 0) {
        g_source_remove(client->save_source);
        client->save_source = 0;
        gamma_save_state(client);
    }
    if (client->manager != NULL) {
        zwlr_gamma_control_manager_v1_destroy(client->manager);
    }
    if (client->registry != NULL) {
        wl_registry_destroy(client->registry);
    }
    g_ptr_array_unref(client->outputs);
    if (client->display != NULL) {
        wl_display_disconnect(client->display);
    }
    g_free(client->state_dir);
    g_free(client->state_path);
    g_free(client->backlight_dir);
    g_free(client);
}

guint gamma_client_output_count(GammaClient *client) {
    return client != NULL ? client->outputs->len : 0;
}

const char *gamma_client_output_name(GammaClient *client, guint index) {
    if (client == NULL || index >= client->outputs->len) {
        return "";
    }
    GammaOutput *output = g_ptr_array_index(client->outputs, index);
    if (output->name != NULL && output->name[0] != '\0') {
        return output->name;
    }
    if (output->description != NULL && output->description[0] != '\0') {
        return output->description;
    }
    return "Unknown output";
}

int gamma_client_brightness(GammaClient *client, guint index) {
    if (client == NULL || index >= client->outputs->len) {
        return 100;
    }
    GammaOutput *output = g_ptr_array_index(client->outputs, index);
    if (output->hardware_brightness) {
        int current = backlight_percent(client);
        if (current >= 0) {
            output->brightness = current;
        }
    }
    return output->brightness;
}

int gamma_client_temperature(GammaClient *client, guint index) {
    if (client == NULL || index >= client->outputs->len) {
        return 6500;
    }
    return ((GammaOutput *)g_ptr_array_index(client->outputs, index))
        ->temperature;
}

void gamma_client_set_brightness(GammaClient *client, guint index,
                                 int percent) {
    if (client == NULL || index >= client->outputs->len) {
        return;
    }
    GammaOutput *output = g_ptr_array_index(client->outputs, index);
    output->brightness = CLAMP(percent, 0, 100);
    if (output->hardware_brightness &&
        set_backlight(client, output->brightness)) {
        gamma_schedule_save(client);
        return;
    }
    gamma_apply(client, output);
    gamma_schedule_save(client);
}

void gamma_client_set_temperature(GammaClient *client, guint index,
                                  int kelvin) {
    if (client == NULL || index >= client->outputs->len) {
        return;
    }
    GammaOutput *output = g_ptr_array_index(client->outputs, index);
    output->temperature = CLAMP(kelvin, 1000, 10000);
    gamma_apply(client, output);
    gamma_schedule_save(client);
}
