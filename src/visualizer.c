#include "visualizer.h"

#include <gio/gio.h>
#include <math.h>
#include <string.h>

struct Visualizer {
    GSubprocess *process;
    GDataInputStream *stream;
    GCancellable *cancellable;
    GArray *levels;
    gboolean running;
    char *config_path;
    VisualizerChangedFunc changed;
    gpointer user_data;
};

static void visualizer_process_line(Visualizer *visualizer,
                                    const char *line) {
    gchar **parts = g_strsplit(line, ";", -1);
    g_array_set_size(visualizer->levels, 0);
    for (int i = 0; parts[i] != NULL; i++) {
        if (parts[i][0] == '\0') {
            continue;
        }
        double value = g_ascii_strtod(parts[i], NULL) / 100.0;
        value = CLAMP(value, 0.0, 1.0);
        g_array_append_val(visualizer->levels, value);
    }
    g_strfreev(parts);

    if (visualizer->changed != NULL && visualizer->levels->len > 0) {
        visualizer->changed(visualizer, visualizer->user_data);
    }
}

static void visualizer_read_line(GObject *source, GAsyncResult *result,
                                 gpointer user_data) {
    Visualizer *visualizer = user_data;
    GError *error = NULL;
    gsize length = 0;
    char *line = g_data_input_stream_read_line_finish(
        G_DATA_INPUT_STREAM(source), result, &length, &error);
    (void)length;

    if (line != NULL) {
        if (visualizer->running) {
            visualizer_process_line(visualizer, line);
        }
        g_free(line);
    }
    g_clear_error(&error);

    if (visualizer->running && visualizer->stream != NULL) {
        g_data_input_stream_read_line_async(
            visualizer->stream, G_PRIORITY_DEFAULT,
            visualizer->cancellable, visualizer_read_line, visualizer);
    }
}

Visualizer *visualizer_new(VisualizerChangedFunc changed,
                           gpointer user_data) {
    Visualizer *visualizer = g_new0(Visualizer, 1);
    visualizer->changed = changed;
    visualizer->user_data = user_data;
    visualizer->levels = g_array_new(FALSE, FALSE, sizeof(double));
    return visualizer;
}

void visualizer_free(Visualizer *visualizer) {
    if (visualizer == NULL) {
        return;
    }
    visualizer_stop(visualizer);
    g_array_unref(visualizer->levels);
    g_free(visualizer->config_path);
    g_free(visualizer);
}

static void visualizer_write_config(Visualizer *visualizer) {
    char *dir = g_build_filename(g_get_user_state_dir(), "nintysh", NULL);
    g_mkdir_with_parents(dir, 0700);
    visualizer->config_path =
        g_build_filename(dir, "cava.conf", NULL);
    g_free(dir);

    static const char config[] =
        "[general]\n"
        "bars = 32\n"
        "framerate = 60\n"
        "[output]\n"
        "method = raw\n"
        "raw_target = /dev/stdout\n"
        "data_format = ascii\n"
        "ascii_max_range = 100\n"
        "bar_delimiter = 59\n"
        "frame_delimiter = 10\n";
    g_file_set_contents(visualizer->config_path, config, -1, NULL);
}

void visualizer_start(Visualizer *visualizer) {
    if (visualizer == NULL || visualizer->running) {
        return;
    }
    if (visualizer->config_path == NULL) {
        visualizer_write_config(visualizer);
    }

    const char *argv[] = { "cava", "-p", visualizer->config_path, NULL };
    GError *error = NULL;
    visualizer->process = g_subprocess_newv(
        argv, G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                  G_SUBPROCESS_FLAGS_STDERR_SILENCE,
        &error);
    if (visualizer->process == NULL) {
        g_clear_error(&error);
        return;
    }

    GInputStream *output =
        g_subprocess_get_stdout_pipe(visualizer->process);
    if (output == NULL) {
        g_object_unref(visualizer->process);
        visualizer->process = NULL;
        return;
    }

    visualizer->stream = g_data_input_stream_new(output);
    visualizer->cancellable = g_cancellable_new();
    visualizer->running = TRUE;
    g_data_input_stream_read_line_async(
        visualizer->stream, G_PRIORITY_DEFAULT, visualizer->cancellable,
        visualizer_read_line, visualizer);
}

void visualizer_stop(Visualizer *visualizer) {
    if (visualizer == NULL || !visualizer->running) {
        return;
    }
    visualizer->running = FALSE;
    if (visualizer->cancellable != NULL) {
        g_cancellable_cancel(visualizer->cancellable);
        g_object_unref(visualizer->cancellable);
        visualizer->cancellable = NULL;
    }
    if (visualizer->process != NULL) {
        g_subprocess_force_exit(visualizer->process);
        g_object_unref(visualizer->process);
        visualizer->process = NULL;
    }
    if (visualizer->stream != NULL) {
        g_object_unref(visualizer->stream);
        visualizer->stream = NULL;
    }
    g_array_set_size(visualizer->levels, 0);
}

guint visualizer_level_count(Visualizer *visualizer) {
    return visualizer != NULL ? visualizer->levels->len : 0;
}

double visualizer_level(Visualizer *visualizer, guint index) {
    if (visualizer == NULL || index >= visualizer->levels->len) {
        return 0.0;
    }
    return g_array_index(visualizer->levels, double, index);
}
