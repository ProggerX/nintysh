#include "wallpaper.h"

#include <gsk/gsk.h>
#include <gtk4-layer-shell.h>
#include <math.h>

typedef struct {
    WallpaperManager *manager;
    GtkWidget *window;
    GtkWidget *area;
    GdkMonitor *monitor;
} WallpaperSurface;

struct WallpaperManager {
    GtkApplication *app;
    GPtrArray *surfaces;
    GdkTexture *texture;
    char *current_path;
    char *state_dir;
    char *state_path;
};

static void wallpaper_draw(GtkDrawingArea *area, cairo_t *cr, int width,
                           int height, gpointer user_data) {
    (void)area;
    WallpaperSurface *surface = user_data;
    WallpaperManager *manager = surface->manager;

    cairo_set_source_rgb(cr, 0, 0, 0);
    cairo_paint(cr);
    if (manager->texture == NULL) {
        return;
    }

    int image_width = gdk_texture_get_width(manager->texture);
    int image_height = gdk_texture_get_height(manager->texture);
    if (image_width <= 0 || image_height <= 0) {
        return;
    }

    double scale = MAX((double)width / image_width,
                       (double)height / image_height);
    double draw_width = image_width * scale;
    double draw_height = image_height * scale;

    GtkSnapshot *snapshot = gtk_snapshot_new();
    gdk_paintable_snapshot(GDK_PAINTABLE(manager->texture), snapshot,
                           (int)ceil(draw_width), (int)ceil(draw_height));
    GskRenderNode *node = gtk_snapshot_free_to_node(snapshot);
    if (node == NULL) {
        return;
    }

    cairo_save(cr);
    cairo_translate(cr, (width - draw_width) / 2.0,
                    (height - draw_height) / 2.0);
    gsk_render_node_draw(node, cr);
    cairo_restore(cr);
    gsk_render_node_unref(node);
}

static void wallpaper_surface_free(WallpaperSurface *surface) {
    if (surface == NULL) {
        return;
    }
    if (surface->window != NULL) {
        gtk_window_destroy(GTK_WINDOW(surface->window));
    }
    g_free(surface);
}

static void wallpaper_create_surfaces(WallpaperManager *manager) {
    if (manager->surfaces->len > 0) {
        return;
    }

    GListModel *monitors =
        gdk_display_get_monitors(gdk_display_get_default());
    guint count = monitors != NULL ? g_list_model_get_n_items(monitors) : 0;
    if (count == 0) {
        count = 1;
    }

    for (guint i = 0; i < count; i++) {
        WallpaperSurface *surface = g_new0(WallpaperSurface, 1);
        surface->manager = manager;
        if (monitors != NULL) {
            surface->monitor = g_list_model_get_item(monitors, i);
        }

        GdkRectangle geometry = { 0 };
        if (surface->monitor != NULL) {
            gdk_monitor_get_geometry(surface->monitor, &geometry);
        }

        GtkWidget *window = gtk_application_window_new(manager->app);
        gtk_window_set_title(GTK_WINDOW(window), "nintysh-wallpaper");
        gtk_window_set_decorated(GTK_WINDOW(window), FALSE);
        gtk_window_set_resizable(GTK_WINDOW(window), FALSE);
        gtk_window_set_default_size(GTK_WINDOW(window),
                                    geometry.width > 0 ? geometry.width : 1,
                                    geometry.height > 0 ? geometry.height : 1);

        gtk_layer_init_for_window(GTK_WINDOW(window));
        gtk_layer_set_namespace(GTK_WINDOW(window), "nintysh-wallpaper");
        gtk_layer_set_layer(GTK_WINDOW(window),
                            GTK_LAYER_SHELL_LAYER_BACKGROUND);
        if (surface->monitor != NULL) {
            gtk_layer_set_monitor(GTK_WINDOW(window), surface->monitor);
        }
        gtk_layer_set_anchor(GTK_WINDOW(window), GTK_LAYER_SHELL_EDGE_TOP,
                             TRUE);
        gtk_layer_set_anchor(GTK_WINDOW(window),
                             GTK_LAYER_SHELL_EDGE_BOTTOM, TRUE);
        gtk_layer_set_anchor(GTK_WINDOW(window),
                             GTK_LAYER_SHELL_EDGE_LEFT, TRUE);
        gtk_layer_set_anchor(GTK_WINDOW(window),
                             GTK_LAYER_SHELL_EDGE_RIGHT, TRUE);
        gtk_layer_set_exclusive_zone(GTK_WINDOW(window), -1);
        gtk_layer_set_keyboard_mode(GTK_WINDOW(window),
                                    GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);

        surface->window = window;
        surface->area = gtk_drawing_area_new();
        gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(surface->area),
                                       wallpaper_draw, surface, NULL);
        gtk_window_set_child(GTK_WINDOW(window), surface->area);
        gtk_window_present(GTK_WINDOW(window));
        g_ptr_array_add(manager->surfaces, surface);
    }
}

static void wallpaper_queue_draw(WallpaperManager *manager) {
    for (guint i = 0; i < manager->surfaces->len; i++) {
        WallpaperSurface *surface =
            g_ptr_array_index(manager->surfaces, i);
        gtk_widget_queue_draw(surface->area);
    }
}

WallpaperManager *wallpaper_manager_new(GtkApplication *app) {
    WallpaperManager *manager = g_new0(WallpaperManager, 1);
    manager->app = app;
    manager->surfaces = g_ptr_array_new_with_free_func(
        (GDestroyNotify)wallpaper_surface_free);
    manager->state_dir =
        g_build_filename(g_get_user_state_dir(), "nintysh", NULL);
    manager->state_path =
        g_build_filename(manager->state_dir, "wallpaper", NULL);

    char *path = NULL;
    if (g_file_get_contents(manager->state_path, &path, NULL, NULL)) {
        g_strstrip(path);
        if (path[0] != '\0' && g_file_test(path, G_FILE_TEST_IS_REGULAR)) {
            wallpaper_manager_set(manager, path);
        }
        g_free(path);
    }
    return manager;
}

void wallpaper_manager_free(WallpaperManager *manager) {
    if (manager == NULL) {
        return;
    }
    if (manager->texture != NULL) {
        g_object_unref(manager->texture);
    }
    g_ptr_array_unref(manager->surfaces);
    g_free(manager->current_path);
    g_free(manager->state_dir);
    g_free(manager->state_path);
    g_free(manager);
}

gboolean wallpaper_manager_set(WallpaperManager *manager,
                               const char *path) {
    if (manager == NULL || path == NULL || path[0] == '\0') {
        return FALSE;
    }

    GError *error = NULL;
    GFile *file = g_file_new_for_path(path);
    GdkTexture *texture = gdk_texture_new_from_file(file, &error);
    g_object_unref(file);
    if (texture == NULL) {
        g_clear_error(&error);
        return FALSE;
    }

    if (manager->texture != NULL) {
        g_object_unref(manager->texture);
    }
    manager->texture = texture;
    g_free(manager->current_path);
    manager->current_path = g_strdup(path);
    wallpaper_create_surfaces(manager);
    wallpaper_queue_draw(manager);

    g_mkdir_with_parents(manager->state_dir, 0700);
    g_file_set_contents(manager->state_path, path, -1, NULL);
    return TRUE;
}

const char *wallpaper_manager_current(WallpaperManager *manager) {
    return manager != NULL ? manager->current_path : NULL;
}
