#include "bar.h"

#include "config.h"
#include "control.h"
#include "niri.h"
#include "win98.h"

#include <gsk/gsk.h>
#include <gtk4-layer-shell.h>
#include <math.h>
#include <string.h>

#define BAR_HEIGHT NINTYSH_BAR_HEIGHT
#define BUTTON_Y NINTYSH_BUTTON_Y
#define BUTTON_HEIGHT NINTYSH_BUTTON_HEIGHT
#define MENU_ROW_HEIGHT NINTYSH_MENU_ROW_HEIGHT
#define MENU_WIDTH NINTYSH_MENU_WIDTH
#define MENU_SIDEBAR NINTYSH_MENU_SIDEBAR

typedef enum {
    HIT_NONE,
    HIT_START,
    HIT_WORKSPACE,
    HIT_WINDOW,
    HIT_VOLUME,
    HIT_CLOCK,
    HIT_LAYOUT,
    HIT_NETWORK,
    HIT_BATTERY,
    HIT_CLIPBOARD,
} HitKind;

typedef struct {
    HitKind kind;
    double x;
    double y;
    double w;
    double h;
    gint64 value;
} BarHit;

typedef struct {
    double x;
    double y;
    double w;
    double h;
    double sidebar;
    double content_x;
    double content_y;
    double content_w;
    double items_y;
    int rows;
    gboolean scrollbar;
} MenuLayout;

typedef enum {
    PANEL_NONE,
    PANEL_VOLUME,
    PANEL_CALENDAR,
    PANEL_NETWORK,
    PANEL_POWER,
    PANEL_TASKMENU,
    PANEL_CLIPBOARD,
} PanelKind;

typedef struct {
    double x;
    double y;
    double w;
    double h;
    double title_h;
} PanelFrame;

typedef struct {
    NintyBar *bar;
    GdkMonitor *monitor;
    GtkWidget *window;
    GtkWidget *area;
    GPtrArray *hits;
    char *output;
} BarSurface;

struct NintyBar {
    GtkApplication *app;
    GtkWidget *window;
    GtkWidget *area;
    GtkWidget *menu_window;
    GtkWidget *menu_area;
    GtkWidget *panel_window;
    GtkWidget *panel_area;
    GtkWidget *osd_window;
    GtkWidget *osd_area;

    NiriClient *niri;
    GPtrArray *apps;
    GPtrArray *menu_results;
    GPtrArray *surfaces;
    GPtrArray *active_hits;
    const char *active_output;
    GHashTable *icon_cache;
    GHashTable *app_index;
    GtkIconTheme *icon_theme;
    GPtrArray *clipboard_history;
    char *last_clipboard;

    guint tick_source;
    guint slow_source;
    guint clipboard_source;
    gulong network_handler;

    gboolean volume_available;
    gboolean volume_muted;
    int volume_percent;
    gboolean network_available;
    gboolean battery_available;
    gboolean battery_charging;
    int battery_percent;

    gboolean menu_open;
    char *menu_query;
    int menu_scroll;
    int menu_hover;
    int menu_selected;

    PanelKind panel_kind;
    int calendar_year;
    int calendar_month;
    gboolean volume_dragging;
    double volume_drag_x0;
    int panel_hover;
    int clipboard_scroll;
    gint64 context_window_id;
    double context_x;

    NintyControl *control;
    guint osd_source;
    char osd_text[160];
    int osd_value;
    gboolean osd_mute;
};

static void close_menu(NintyBar *bar);
static void close_panel(NintyBar *bar);
static void open_panel(NintyBar *bar, PanelKind kind);
static void osd_show_volume(NintyBar *bar);
static void osd_show_text(NintyBar *bar, const char *text);

static void bar_clear_hits(NintyBar *bar) {
    if (bar->active_hits == NULL) {
        return;
    }
    while (bar->active_hits->len > 0) {
        g_ptr_array_remove_index(bar->active_hits,
                                 bar->active_hits->len - 1);
    }
}

static void bar_add_hit(NintyBar *bar, HitKind kind, double x, double y,
                        double w, double h, gint64 value) {
    BarHit *hit = g_new0(BarHit, 1);
    hit->kind = kind;
    hit->x = x;
    hit->y = y;
    hit->w = w;
    hit->h = h;
    hit->value = value;
    if (bar->active_hits != NULL) {
        g_ptr_array_add(bar->active_hits, hit);
    } else {
        g_free(hit);
    }
}

static BarHit *bar_hit_at(GPtrArray *hits, double x, double y) {
    for (guint i = hits->len; i > 0; i--) {
        BarHit *hit = g_ptr_array_index(hits, i - 1);
        if (x >= hit->x && x < hit->x + hit->w && y >= hit->y &&
            y < hit->y + hit->h) {
            return hit;
        }
    }
    return NULL;
}

static void bar_draw(GtkDrawingArea *area, cairo_t *cr, int width, int height,
                     gpointer user_data);
static void bar_queue_redraw(NintyBar *bar);

static void bar_surface_free(BarSurface *surface) {
    g_free(surface->output);
    g_ptr_array_unref(surface->hits);
    g_free(surface);
}

static void bar_surface_draw(GtkDrawingArea *area, cairo_t *cr, int width,
                             int height, gpointer user_data) {
    BarSurface *surface = user_data;
    NintyBar *bar = surface->bar;
    bar->active_hits = surface->hits;
    bar->active_output = surface->output;
    bar_draw(area, cr, width, height, bar);
}

static void bar_queue_redraw(NintyBar *bar) {
    if (bar->surfaces == NULL) {
        return;
    }
    for (guint i = 0; i < bar->surfaces->len; i++) {
        BarSurface *surface = g_ptr_array_index(bar->surfaces, i);
        gtk_widget_queue_draw(surface->area);
    }
}

static const char *current_output(NiriClient *niri) {
    guint count = niri_client_workspace_count(niri);

    for (guint i = 0; i < count; i++) {
        NiriWorkspace *workspace = niri_client_workspace(niri, i);
        if (workspace->is_focused && workspace->output != NULL) {
            return workspace->output;
        }
    }
    for (guint i = 0; i < count; i++) {
        NiriWorkspace *workspace = niri_client_workspace(niri, i);
        if (workspace->is_active && workspace->output != NULL) {
            return workspace->output;
        }
    }
    return NULL;
}

static gboolean workspace_on_output(NiriWorkspace *workspace,
                                    const char *output) {
    if (output == NULL || workspace->output == NULL) {
        return TRUE;
    }
    return g_strcmp0(workspace->output, output) == 0;
}

static gboolean window_on_output(NiriClient *niri, NiriWindow *window,
                                 const char *output) {
    if (output == NULL || window->workspace_id < 0) {
        return TRUE;
    }
    guint count = niri_client_workspace_count(niri);
    for (guint i = 0; i < count; i++) {
        NiriWorkspace *workspace = niri_client_workspace(niri, i);
        if (workspace->id == window->workspace_id) {
            return workspace->output == NULL ||
                   g_strcmp0(workspace->output, output) == 0;
        }
    }
    return TRUE;
}

static void format_clock(char *buffer, gsize size) {
    GDateTime *now = g_date_time_new_now_local();
    gchar *text = g_date_time_format(now, "%I:%M %p");

    if (text[0] == '0') {
        memmove(text, text + 1, strlen(text));
    }
    g_strlcpy(buffer, text, size);

    g_free(text);
    g_date_time_unref(now);
}

static gboolean paintable_is_monochrome(GtkIconPaintable *paintable) {
    GFile *file = gtk_icon_paintable_get_file(paintable);
    if (file == NULL) {
        return FALSE;
    }

    char *path = g_file_get_path(file);
    gboolean monochrome = FALSE;
    if (path != NULL) {
        monochrome = strstr(path, "/symbolic/") != NULL ||
                     g_str_has_suffix(path, "-symbolic.svg");
        if (!monochrome && g_str_has_suffix(path, ".svg")) {
            char *contents = NULL;
            gsize length = 0;
            GError *error = NULL;
            if (g_file_load_contents(file, NULL, &contents, &length, NULL,
                                     &error)) {
                if (g_strstr_len(contents, (gssize)length, "currentColor") !=
                        NULL ||
                    g_strstr_len(contents, (gssize)length, "ColorScheme-") !=
                        NULL) {
                    monochrome = TRUE;
                }
                g_free(contents);
            }
            g_clear_error(&error);
        }
    }
    g_free(path);
    return monochrome;
}

static GtkIconPaintable *lookup_icon(NintyBar *bar, GIcon *icon,
                                    const char *cache_key) {
    if (icon == NULL) {
        return NULL;
    }

    GtkIconPaintable *paintable =
        g_hash_table_lookup(bar->icon_cache, cache_key);
    if (paintable != NULL) {
        return paintable;
    }

    GtkIconTheme *global =
        gtk_icon_theme_get_for_display(gdk_display_get_default());
    GtkIconPaintable *global_icon = gtk_icon_theme_lookup_by_gicon(
        global, icon, NINTYSH_ICON_LOOKUP_SIZE, NINTYSH_ICON_LOOKUP_SCALE, GTK_TEXT_DIR_NONE,
        GTK_ICON_LOOKUP_FORCE_REGULAR);

    paintable = NULL;
    if (global_icon != NULL && !paintable_is_monochrome(global_icon)) {
        paintable = global_icon;
        global_icon = NULL;
    } else {
        paintable = gtk_icon_theme_lookup_by_gicon(
            bar->icon_theme, icon, NINTYSH_ICON_LOOKUP_SIZE, NINTYSH_ICON_LOOKUP_SCALE, GTK_TEXT_DIR_NONE,
            GTK_ICON_LOOKUP_FORCE_REGULAR);
        if (paintable == NULL) {
            paintable = global_icon;
            global_icon = NULL;
        }
    }
    if (global_icon != NULL) {
        g_object_unref(global_icon);
    }

    if (paintable != NULL) {
        g_hash_table_insert(bar->icon_cache, g_strdup(cache_key), paintable);
    }
    return paintable;
}

static char *app_index_key(const char *value) {
    if (value == NULL) {
        return NULL;
    }
    char *key = g_utf8_casefold(value, -1);
    if (g_str_has_suffix(key, ".desktop")) {
        key[strlen(key) - strlen(".desktop")] = '\0';
    }
    return key;
}

static void index_apps(NintyBar *bar) {
    for (guint i = 0; i < bar->apps->len; i++) {
        GAppInfo *app = g_ptr_array_index(bar->apps, i);
        const char *id = g_app_info_get_id(app);
        char *key = app_index_key(id);
        if (key != NULL) {
            g_hash_table_insert(bar->app_index, key, app);
        }
        const char *executable = g_app_info_get_executable(app);
        const char *base = executable != NULL ? strrchr(executable, '/') : NULL;
        key = app_index_key(base != NULL ? base + 1 : executable);
        if (key != NULL) {
            g_hash_table_insert(bar->app_index, key, app);
        }
    }
}

static GtkIconPaintable *app_icon(NintyBar *bar, GAppInfo *app) {
    GIcon *icon = g_app_info_get_icon(app);
    char *key = NULL;

    if (icon != NULL) {
        key = g_icon_to_string(icon);
    }
    if (key == NULL) {
        const char *id = g_app_info_get_id(app);
        key = g_strdup_printf("app:%s", id != NULL ? id : "");
    }
    GtkIconPaintable *paintable = lookup_icon(bar, icon, key);
    g_free(key);
    return paintable;
}

static GtkIconPaintable *window_icon(NintyBar *bar, NiriWindow *window) {
    if (window->app_id == NULL || window->app_id[0] == '\0') {
        return NULL;
    }

    char *lookup_key = app_index_key(window->app_id);
    GAppInfo *app =
        lookup_key != NULL
            ? g_hash_table_lookup(bar->app_index, lookup_key)
            : NULL;
    g_free(lookup_key);
    if (app != NULL) {
        GtkIconPaintable *paintable = app_icon(bar, app);
        if (paintable != NULL) {
            return paintable;
        }
    }

    char *key = g_strdup_printf("appid:%s", window->app_id);
    GIcon *icon = g_themed_icon_new_with_default_fallbacks(window->app_id);
    GtkIconPaintable *paintable = lookup_icon(bar, icon, key);
    g_object_unref(icon);
    g_free(key);
    return paintable;
}

static void draw_paintable(cairo_t *cr, GtkIconPaintable *paintable,
                           double x, double y, double size) {
    GtkSnapshot *snapshot = gtk_snapshot_new();
    gdk_paintable_snapshot(GDK_PAINTABLE(paintable), snapshot, (int)size,
                           (int)size);
    GskRenderNode *node = gtk_snapshot_free_to_node(snapshot);
    if (node == NULL) {
        return;
    }

    cairo_save(cr);
    cairo_translate(cr, x, y);
    gsk_render_node_draw(node, cr);
    cairo_restore(cr);
    gsk_render_node_unref(node);
}

static char *window_label(NiriWindow *window) {
    if (window->title != NULL && window->title[0] != '\0') {
        return g_strdup(window->title);
    }
    if (window->app_id != NULL && window->app_id[0] != '\0') {
        return g_strdup(window->app_id);
    }
    return g_strdup("Untitled");
}

static gboolean run_command_sync(const char *command, char **output) {
    gint status = 0;
    GError *error = NULL;
    gboolean spawned = g_spawn_command_line_sync(command, output, NULL,
                                                 &status, &error);
    if (!spawned) {
        g_clear_error(&error);
        return FALSE;
    }
    if (!g_spawn_check_wait_status(status, &error)) {
        g_clear_error(&error);
        return FALSE;
    }
    return TRUE;
}

static gboolean update_volume(NintyBar *bar) {
    gboolean old_available = bar->volume_available;
    gboolean old_muted = bar->volume_muted;
    int old_percent = bar->volume_percent;
    char *output = NULL;

    bar->volume_available = FALSE;
    if (run_command_sync("wpctl get-volume @DEFAULT_AUDIO_SINK@", &output)) {
        const char *volume = strstr(output, "Volume:");
        if (volume != NULL) {
            volume += strlen("Volume:");
            while (*volume == ' ') {
                volume++;
            }
            double value = g_ascii_strtod(volume, NULL);
            bar->volume_available = TRUE;
            bar->volume_percent = (int)round(value * 100.0);
            bar->volume_muted = strstr(output, "MUTED") != NULL;
        }
    }
    g_free(output);

    return old_available != bar->volume_available ||
           old_muted != bar->volume_muted ||
           old_percent != bar->volume_percent;
}

static gboolean toggle_volume(NintyBar *bar) {
    char *output = NULL;
    gboolean ok = run_command_sync(
        "wpctl set-mute @DEFAULT_AUDIO_SINK@ toggle", &output);
    g_free(output);
    update_volume(bar);
    bar_queue_redraw(bar);
    if (bar->panel_area != NULL) {
        gtk_widget_queue_draw(bar->panel_area);
    }
    osd_show_volume(bar);
    return ok;
}

static void apply_volume(NintyBar *bar, int percent, gboolean commit) {
    percent = CLAMP(percent, 0, 100);
    bar->volume_available = TRUE;
    bar->volume_percent = percent;

    if (commit) {
        char command[128];
        char *output = NULL;
        g_snprintf(command, sizeof(command),
                   "wpctl set-volume @DEFAULT_AUDIO_SINK@ %.2f",
                   percent / 100.0);
        run_command_sync(command, &output);
        g_free(output);
        update_volume(bar);
        bar_queue_redraw(bar);
    }
    if (bar->panel_area != NULL) {
        gtk_widget_queue_draw(bar->panel_area);
    }
    osd_show_volume(bar);
}

static gboolean read_text_file(const char *path, char **contents) {
    GError *error = NULL;
    if (!g_file_get_contents(path, contents, NULL, &error)) {
        g_clear_error(&error);
        return FALSE;
    }
    g_strstrip(*contents);
    return TRUE;
}

static gboolean update_battery(NintyBar *bar) {
    gboolean old_available = bar->battery_available;
    gboolean old_charging = bar->battery_charging;
    int old_percent = bar->battery_percent;

    bar->battery_available = FALSE;
    bar->battery_charging = FALSE;
    bar->battery_percent = 0;

    GDir *dir = g_dir_open("/sys/class/power_supply", 0, NULL);
    if (dir == NULL) {
        return old_available;
    }

    const char *name = NULL;
    while ((name = g_dir_read_name(dir)) != NULL) {
        char *path = g_build_filename("/sys/class/power_supply", name, "type",
                                      NULL);
        char *type = NULL;
        gboolean is_battery =
            read_text_file(path, &type) && g_strcmp0(type, "Battery") == 0;
        g_free(type);
        g_free(path);

        if (!is_battery) {
            continue;
        }

        path = g_build_filename("/sys/class/power_supply", name, "scope",
                                NULL);
        char *scope = NULL;
        if (read_text_file(path, &scope) &&
            g_strcmp0(scope, "System") != 0) {
            g_free(scope);
            g_free(path);
            continue;
        }
        g_free(scope);
        g_free(path);

        path = g_build_filename("/sys/class/power_supply", name, "capacity",
                                NULL);
        char *capacity = NULL;
        gboolean have_capacity = read_text_file(path, &capacity);
        g_free(path);

        if (!have_capacity) {
            g_free(capacity);
            continue;
        }

        bar->battery_available = TRUE;
        bar->battery_percent = atoi(capacity);
        g_free(capacity);

        path = g_build_filename("/sys/class/power_supply", name, "status",
                                NULL);
        char *status = NULL;
        if (read_text_file(path, &status)) {
            bar->battery_charging =
                g_strcmp0(status, "Charging") == 0 ||
                g_strcmp0(status, "Full") == 0;
            g_free(status);
        }
        g_free(path);
        break;
    }
    g_dir_close(dir);

    return old_available != bar->battery_available ||
           old_charging != bar->battery_charging ||
           old_percent != bar->battery_percent;
}

static gboolean update_network(NintyBar *bar) {
    gboolean old_available = bar->network_available;
    GNetworkMonitor *monitor = g_network_monitor_get_default();
    bar->network_available =
        g_network_monitor_get_network_available(monitor);
    return old_available != bar->network_available;
}

static void clipboard_trim(NintyBar *bar) {
    while (bar->clipboard_history->len > NINTYSH_CLIPBOARD_MAX_ENTRIES) {
        g_ptr_array_remove_index(
            bar->clipboard_history, bar->clipboard_history->len - 1);
    }
}

static void clipboard_add(NintyBar *bar, const char *text) {
    if (text == NULL || text[0] == '\0') {
        return;
    }

    for (guint i = 0; i < bar->clipboard_history->len; i++) {
        const char *entry = g_ptr_array_index(bar->clipboard_history, i);
        if (g_strcmp0(entry, text) == 0) {
            g_ptr_array_remove_index(bar->clipboard_history, i);
            break;
        }
    }

    g_ptr_array_insert(bar->clipboard_history, 0,
                       g_strndup(text, NINTYSH_CLIPBOARD_MAX_BYTES));
    clipboard_trim(bar);
}

static void clipboard_copy(NintyBar *bar, const char *text) {
    GError *error = NULL;
    GSubprocess *process = g_subprocess_new(
        G_SUBPROCESS_FLAGS_STDIN_PIPE, &error, "wl-copy", NULL);
    if (process == NULL) {
        g_clear_error(&error);
        return;
    }

    GBytes *bytes = g_bytes_new(text, strlen(text));
    g_subprocess_communicate(process, bytes, NULL, NULL, NULL, &error);
    g_bytes_unref(bytes);
    g_clear_error(&error);
    g_object_unref(process);

    g_free(bar->last_clipboard);
    bar->last_clipboard = g_strdup(text);
    osd_show_text(bar, "Copied to clipboard");
    gtk_widget_queue_draw(bar->panel_area);
}

static gboolean clipboard_tick(gpointer user_data) {
    NintyBar *bar = user_data;
    char *output = NULL;
    if (!run_command_sync("wl-paste --no-newline --type text", &output)) {
        return G_SOURCE_CONTINUE;
    }

    if (output != NULL && output[0] != '\0' &&
        g_strcmp0(output, bar->last_clipboard) != 0) {
        g_free(bar->last_clipboard);
        bar->last_clipboard = g_strdup(output);
        clipboard_add(bar, output);
        gtk_widget_queue_draw(bar->panel_area);
    }
    g_free(output);
    return G_SOURCE_CONTINUE;
}

static gint app_compare(gconstpointer a, gconstpointer b) {
    GAppInfo *app_a = *(GAppInfo *const *)a;
    GAppInfo *app_b = *(GAppInfo *const *)b;
    const char *name_a = g_app_info_get_display_name(app_a);
    const char *name_b = g_app_info_get_display_name(app_b);
    return g_utf8_collate(name_a != NULL ? name_a : "",
                          name_b != NULL ? name_b : "");
}

static void load_apps(NintyBar *bar) {
    GList *all = g_app_info_get_all();

    for (GList *item = all; item != NULL; item = item->next) {
        GAppInfo *app = item->data;
        if (g_app_info_should_show(app)) {
            g_ptr_array_add(bar->apps, g_object_ref(app));
        }
    }
    g_list_free_full(all, g_object_unref);
    g_ptr_array_sort(bar->apps, app_compare);
}

static void launch_app(NintyBar *bar, GAppInfo *app) {
    (void)bar;
    if (app == NULL) {
        return;
    }

    GdkAppLaunchContext *context =
        gdk_display_get_app_launch_context(gdk_display_get_default());
    GError *error = NULL;

    gdk_app_launch_context_set_timestamp(context, GDK_CURRENT_TIME);
    if (!g_app_info_launch(app, NULL, G_APP_LAUNCH_CONTEXT(context), &error)) {
        g_warning("nintysh: cannot launch %s: %s",
                  g_app_info_get_display_name(app), error->message);
        g_error_free(error);
    }
    g_object_unref(context);
}

static gboolean menu_command_mode(NintyBar *bar) {
    return bar->menu_query != NULL && bar->menu_query[0] == '/';
}

static const char *menu_command_text(NintyBar *bar) {
    if (!menu_command_mode(bar)) {
        return "";
    }
    return bar->menu_query + 1;
}

static void run_shell_command(const char *command) {
    if (command == NULL || command[0] == '\0') {
        return;
    }
    const char *argv[] = { "/bin/sh", "-c", command, NULL };
    GError *error = NULL;
    if (!g_spawn_async(NULL, (char **)argv, NULL, G_SPAWN_SEARCH_PATH,
                       NULL, NULL, NULL, &error)) {
        g_warning("nintysh: cannot run command: %s", error->message);
        g_error_free(error);
    }
}

static void menu_update_results(NintyBar *bar) {
    g_ptr_array_set_size(bar->menu_results, 0);
    if (menu_command_mode(bar)) {
        bar->menu_scroll = 0;
        bar->menu_selected = 0;
        bar->menu_hover = -1;
        return;
    }
    gboolean has_query = bar->menu_query != NULL && bar->menu_query[0] != '\0';
    char *query = has_query ? g_utf8_casefold(bar->menu_query, -1) : NULL;

    for (guint i = 0; i < bar->apps->len; i++) {
        GAppInfo *app = g_ptr_array_index(bar->apps, i);
        if (!has_query) {
            g_ptr_array_add(bar->menu_results, app);
            continue;
        }

        const char *name = g_app_info_get_display_name(app);
        const char *id = g_app_info_get_id(app);
        gboolean match = FALSE;

        if (name != NULL) {
            char *folded = g_utf8_casefold(name, -1);
            match = strstr(folded, query) != NULL;
            g_free(folded);
        }
        if (!match && id != NULL) {
            char *folded = g_utf8_casefold(id, -1);
            match = strstr(folded, query) != NULL;
            g_free(folded);
        }
        if (match) {
            g_ptr_array_add(bar->menu_results, app);
        }
    }
    g_free(query);

    bar->menu_scroll = 0;
    bar->menu_selected = bar->menu_results->len > 0 ? 0 : -1;
    bar->menu_hover = -1;
}

static void menu_clear_query(NintyBar *bar) {
    g_free(bar->menu_query);
    bar->menu_query = g_strdup("");
    menu_update_results(bar);
}

static void menu_append_query(NintyBar *bar, gunichar character) {
    char utf8[8] = { 0 };
    int length = g_unichar_to_utf8(character, utf8);
    utf8[length] = '\0';
    char *next = g_strconcat(bar->menu_query, utf8, NULL);
    g_free(bar->menu_query);
    bar->menu_query = next;
    menu_update_results(bar);
}

static void menu_backspace(NintyBar *bar) {
    gsize length = strlen(bar->menu_query);
    if (length == 0) {
        return;
    }
    char *previous =
        g_utf8_find_prev_char(bar->menu_query, bar->menu_query + length);
    if (previous != NULL) {
        *previous = '\0';
        menu_update_results(bar);
    }
}

static GAppInfo *menu_result_at(NintyBar *bar, int index) {
    if (index < 0 || index >= (int)bar->menu_results->len) {
        return NULL;
    }
    return g_ptr_array_index(bar->menu_results, (guint)index);
}

static void close_menu(NintyBar *bar);

static void niri_changed_cb(NiriClient *client, gpointer user_data) {
    (void)client;
    NintyBar *bar = user_data;
    bar_queue_redraw(bar);
}

static MenuLayout menu_layout(NintyBar *bar, double screen_w,
                              double screen_h) {
    MenuLayout layout = { 0 };
    int count = menu_command_mode(bar) ? 1 : (int)bar->menu_results->len;
    double max_height = screen_h - BAR_HEIGHT - 8.0;
    if (max_height < 48.0) {
        max_height = 48.0;
    }

    const double search_h = NINTYSH_MENU_SEARCH_HEIGHT;
    double wanted = 8.0 + search_h + 4.0 +
                    (count > 0 ? count : 1) * MENU_ROW_HEIGHT;
    layout.w = MENU_WIDTH;
    if (layout.w > screen_w - 4.0) {
        layout.w = screen_w - 4.0;
    }
    if (layout.w < 120.0) {
        layout.w = 120.0;
    }
    layout.h = MIN(wanted, MIN(NINTYSH_MENU_MAX_HEIGHT, max_height));
    layout.x = 2.0;
    layout.y = screen_h - BAR_HEIGHT - layout.h - 1.0;
    if (layout.y < 2.0) {
        layout.y = 2.0;
    }
    layout.sidebar = MENU_SIDEBAR;
    layout.content_x = layout.x + 4.0 + layout.sidebar;
    layout.content_y = layout.y + 4.0;
    layout.content_w = layout.x + layout.w - 6.0 - layout.content_x;
    layout.items_y = layout.content_y + search_h + 5.0;

    int rows = (int)((layout.y + layout.h - 4.0 - layout.items_y) /
                     MENU_ROW_HEIGHT);
    if (count > 0 && rows < 1) {
        rows = 1;
    }
    if (count == 0) {
        rows = 0;
    }
    layout.rows = MIN(rows, count);
    layout.scrollbar = count > layout.rows;
    if (layout.scrollbar) {
        layout.content_w -= 14.0;
    }
    if (layout.content_w < 80.0) {
        layout.content_w = 80.0;
    }
    return layout;
}

static int menu_item_at(NintyBar *bar, const MenuLayout *layout, double x,
                        double y) {
    if (x < layout->content_x ||
        x >= layout->content_x + layout->content_w ||
        y < layout->items_y ||
        y >= layout->items_y + layout->rows * MENU_ROW_HEIGHT) {
        return -1;
    }
    if (menu_command_mode(bar)) {
        return 0;
    }

    int row = (int)((y - layout->items_y) / MENU_ROW_HEIGHT);
    int index = bar->menu_scroll + row;
    if (index < 0 || index >= (int)bar->menu_results->len) {
        return -1;
    }
    return index;
}

static void menu_draw(GtkDrawingArea *area, cairo_t *cr, int width, int height,
                      gpointer user_data) {
    (void)area;
    NintyBar *bar = user_data;

    cairo_save(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);
    cairo_restore(cr);

    if (!bar->menu_open || width < 64 || height < 64) {
        return;
    }

    cairo_set_antialias(cr, CAIRO_ANTIALIAS_NONE);
    MenuLayout layout = menu_layout(bar, width, height);

    win98_fill(cr, layout.x, layout.y, layout.w, layout.h, win98_face);
    win98_bevel(cr, layout.x, layout.y, layout.w, layout.h,
                WIN98_BEVEL_RAISED);

    cairo_pattern_t *gradient = cairo_pattern_create_linear(
        layout.x + 3, layout.y + 3, layout.x + 3,
        layout.y + layout.h - 3);
    cairo_pattern_add_color_stop_rgb(
        gradient, 0.0, NINTYSH_RGB_R(NINTYSH_COLOR_TITLE_TOP),
        NINTYSH_RGB_G(NINTYSH_COLOR_TITLE_TOP),
        NINTYSH_RGB_B(NINTYSH_COLOR_TITLE_TOP));
    cairo_pattern_add_color_stop_rgb(
        gradient, 1.0, NINTYSH_RGB_R(NINTYSH_COLOR_TITLE_BOTTOM),
        NINTYSH_RGB_G(NINTYSH_COLOR_TITLE_BOTTOM),
        NINTYSH_RGB_B(NINTYSH_COLOR_TITLE_BOTTOM));
    cairo_set_source(cr, gradient);
    cairo_rectangle(cr, layout.x + 2, layout.y + 2, layout.sidebar,
                    layout.h - 4);
    cairo_fill(cr);
    cairo_pattern_destroy(gradient);

    if (layout.h > 76) {
        cairo_save(cr);
        cairo_translate(cr, layout.x + 7, layout.y + layout.h - 6);
        cairo_rotate(cr, -G_PI_2);
        win98_draw_text(cr, 0, 0, layout.h - 16, 14, "ninty98",
                   WIN98_ALIGN_LEFT, TRUE, win98_white, FALSE);
        cairo_restore(cr);
    }

    double search_x = layout.content_x;
    double search_y = layout.content_y;
    double search_w = layout.content_w;
    win98_fill(cr, search_x, search_y, search_w, 22.0, win98_white);
    win98_bevel(cr, search_x, search_y, search_w, 22.0,
                WIN98_BEVEL_SUNKEN);

    const char *query = bar->menu_query != NULL ? bar->menu_query : "";
    if (query[0] == '\0') {
        win98_draw_text(cr, search_x + 4.0, search_y + 2.0, search_w - 8.0,
                        18.0, "Type to search...", WIN98_ALIGN_LEFT, FALSE,
                        win98_text_disabled, TRUE);
    } else {
        win98_draw_text(cr, search_x + 4.0, search_y + 2.0, search_w - 8.0,
                        18.0, query, WIN98_ALIGN_LEFT, FALSE, win98_text,
                        TRUE);
    }
    int query_width = win98_text_width(query, FALSE);
    double caret_x =
        MIN(search_x + 4.0 + query_width, search_x + search_w - 5.0);
    win98_fill(cr, caret_x, search_y + 4.0, 1.0, 14.0, win98_text);

    if (menu_command_mode(bar)) {
        const char *command = menu_command_text(bar);
        win98_fill(cr, layout.content_x, layout.items_y, layout.content_w,
                   MENU_ROW_HEIGHT, win98_navy);
        win98_draw_app_icon(cr, layout.content_x + 2, layout.items_y + 1);
        char text[512];
        g_snprintf(text, sizeof(text), "Run: %s",
                   command[0] != '\0' ? command : "type a command...");
        win98_draw_text(cr, layout.content_x + 26, layout.items_y,
                        layout.content_w - 28, MENU_ROW_HEIGHT, text,
                        WIN98_ALIGN_LEFT, FALSE, win98_white, TRUE);
    } else if (bar->menu_results->len == 0) {
        win98_draw_text(cr, layout.content_x, layout.items_y,
                        layout.content_w, MENU_ROW_HEIGHT,
                        "No matching applications", WIN98_ALIGN_CENTER,
                        FALSE, win98_text_disabled, TRUE);
    }

    for (int row = 0; !menu_command_mode(bar) && row < layout.rows; row++) {
        guint index = (guint)(bar->menu_scroll + row);
        if (index >= bar->menu_results->len) {
            break;
        }

        double y = layout.items_y + row * MENU_ROW_HEIGHT;
        gboolean selected = (int)index == bar->menu_selected ||
                            (int)index == bar->menu_hover;
        if (selected) {
            win98_fill(cr, layout.content_x, y, layout.content_w,
                       MENU_ROW_HEIGHT, win98_navy);
        }

        GAppInfo *app = g_ptr_array_index(bar->menu_results, index);
        const char *name = g_app_info_get_display_name(app);
        GtkIconPaintable *icon = app_icon(bar, app);
        if (icon != NULL) {
            draw_paintable(cr, icon, layout.content_x + 2, y + 1, NINTYSH_ICON_SIZE);
        } else {
            win98_draw_app_icon(cr, layout.content_x + 2, y + 1);
        }
        win98_draw_text(cr, layout.content_x + 26, y, layout.content_w - 28,
                        MENU_ROW_HEIGHT, name, WIN98_ALIGN_LEFT, FALSE,
                        selected ? win98_white : win98_text, TRUE);
    }

    if (layout.scrollbar && layout.h > 40) {
        double sx = layout.x + layout.w - 14;
        double sy = layout.items_y;
        double sh = layout.y + layout.h - 4.0 - layout.items_y;

        win98_fill(cr, sx, sy, 12, sh, win98_light);
        win98_bevel(cr, sx, sy, 12, sh, WIN98_BEVEL_SUNKEN);

        double track_y = sy + 12;
        double track_h = sh - 24;
        if (track_h > 4) {
            int max_scroll = (int)bar->menu_results->len - layout.rows;
            double thumb_h =
                MAX(16.0, track_h * layout.rows / bar->menu_results->len);
            thumb_h = MIN(thumb_h, track_h);
            double fraction = max_scroll > 0
                                  ? (double)bar->menu_scroll / max_scroll
                                  : 0.0;
            double thumb_y =
                track_y + (track_h - thumb_h) * fraction;

            win98_fill(cr, sx + 1, thumb_y, 10, thumb_h, win98_face);
            win98_bevel(cr, sx + 1, thumb_y, 10, thumb_h,
                        WIN98_BEVEL_RAISED);

            cairo_set_source_rgb(cr, 0, 0, 0);
            cairo_move_to(cr, sx + 4, sy + 9);
            cairo_line_to(cr, sx + 8, sy + 9);
            cairo_line_to(cr, sx + 6, sy + 5);
            cairo_close_path(cr);
            cairo_fill(cr);

            cairo_move_to(cr, sx + 4, sy + sh - 9);
            cairo_line_to(cr, sx + 8, sy + sh - 9);
            cairo_line_to(cr, sx + 6, sy + sh - 5);
            cairo_close_path(cr);
            cairo_fill(cr);
        }
    }
}

static void open_menu(NintyBar *bar) {
    if (bar->menu_open) {
        return;
    }
    close_panel(bar);
    bar->menu_open = TRUE;
    g_free(bar->menu_query);
    bar->menu_query = g_strdup("");
    menu_update_results(bar);
    gtk_widget_set_visible(bar->menu_window, TRUE);
    gtk_window_present(GTK_WINDOW(bar->menu_window));
    gtk_widget_grab_focus(bar->menu_area);
    gtk_widget_queue_draw(bar->menu_area);
    bar_queue_redraw(bar);
}

static void close_menu(NintyBar *bar) {
    if (!bar->menu_open) {
        return;
    }
    bar->menu_open = FALSE;
    bar->menu_hover = -1;
    gtk_widget_set_visible(bar->menu_window, FALSE);
    bar_queue_redraw(bar);
}

static void menu_clicked(GtkGestureClick *gesture, int n_press, double x,
                         double y, gpointer user_data) {
    (void)gesture;
    (void)n_press;
    NintyBar *bar = user_data;
    double width = gtk_widget_get_width(bar->menu_area);
    double height = gtk_widget_get_height(bar->menu_area);
    MenuLayout layout = menu_layout(bar, width, height);

    if (x >= layout.x && x < layout.x + layout.w && y >= layout.y &&
        y < layout.y + layout.h) {
        int index = menu_item_at(bar, &layout, x, y);
        if (index >= 0) {
            bar->menu_selected = index;
            if (menu_command_mode(bar)) {
                run_shell_command(menu_command_text(bar));
            } else {
                launch_app(bar, menu_result_at(bar, index));
            }
            close_menu(bar);
        }
        return;
    }

    close_menu(bar);
}

static gboolean menu_scrolled(GtkEventControllerScroll *controller,
                              double dx, double dy, gpointer user_data) {
    (void)controller;
    (void)dx;
    NintyBar *bar = user_data;
    double width = gtk_widget_get_width(bar->menu_area);
    double height = gtk_widget_get_height(bar->menu_area);
    MenuLayout layout = menu_layout(bar, width, height);
    int max_scroll = (int)bar->menu_results->len - layout.rows;

    if (max_scroll <= 0) {
        return TRUE;
    }

    int step = dy > 0 ? 3 : -3;
    int next = CLAMP(bar->menu_scroll + step, 0, max_scroll);
    if (next != bar->menu_scroll) {
        bar->menu_scroll = next;
        bar->menu_hover = -1;
        gtk_widget_queue_draw(bar->menu_area);
    }
    (void)height;
    return TRUE;
}

static void menu_motion(GtkEventControllerMotion *controller, double x,
                        double y, gpointer user_data) {
    (void)controller;
    NintyBar *bar = user_data;
    double width = gtk_widget_get_width(bar->menu_area);
    double height = gtk_widget_get_height(bar->menu_area);
    MenuLayout layout = menu_layout(bar, width, height);
    int index = menu_item_at(bar, &layout, x, y);

    if (index != bar->menu_hover) {
        bar->menu_hover = index;
        if (index >= 0) {
            bar->menu_selected = index;
        }
        gtk_widget_queue_draw(bar->menu_area);
    }
}

static gboolean menu_key_pressed(GtkEventControllerKey *controller,
                                 guint keyval, guint keycode,
                                 GdkModifierType state, gpointer user_data) {
    (void)controller;
    (void)keycode;
    NintyBar *bar = user_data;
    int count = (int)bar->menu_results->len;

    if (keyval == GDK_KEY_Escape) {
        if (bar->menu_query != NULL && bar->menu_query[0] != '\0') {
            menu_clear_query(bar);
            gtk_widget_queue_draw(bar->menu_area);
        } else {
            close_menu(bar);
        }
        return TRUE;
    }

    if (keyval == GDK_KEY_BackSpace) {
        if (bar->menu_query != NULL && bar->menu_query[0] != '\0') {
            menu_backspace(bar);
            gtk_widget_queue_draw(bar->menu_area);
        }
        return TRUE;
    }

    if (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) {
        if (menu_command_mode(bar)) {
            if (menu_command_text(bar)[0] != '\0') {
                run_shell_command(menu_command_text(bar));
                close_menu(bar);
            }
            return TRUE;
        }
        GAppInfo *app =
            menu_result_at(bar, bar->menu_selected >= 0 ? bar->menu_selected
                                                        : 0);
        if (app != NULL) {
            launch_app(bar, app);
            close_menu(bar);
        }
        return TRUE;
    }

    if ((keyval == GDK_KEY_Up || keyval == GDK_KEY_Down) && count > 0) {
        int selected = bar->menu_selected;
        if (selected < 0) {
            selected = 0;
        } else if (keyval == GDK_KEY_Up) {
            selected--;
        } else {
            selected++;
        }
        selected = CLAMP(selected, 0, count - 1);

        double width = gtk_widget_get_width(bar->menu_area);
        double height = gtk_widget_get_height(bar->menu_area);
        MenuLayout layout = menu_layout(bar, width, height);
        if (selected < bar->menu_scroll) {
            bar->menu_scroll = selected;
        } else if (selected >= bar->menu_scroll + layout.rows) {
            bar->menu_scroll = selected - layout.rows + 1;
        }
        bar->menu_selected = selected;
        bar->menu_hover = -1;
        gtk_widget_queue_draw(bar->menu_area);
        return TRUE;
    }

    if (state & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK)) {
        return FALSE;
    }

    gunichar character = gdk_keyval_to_unicode(keyval);
    if (character != 0 && !g_unichar_iscntrl(character)) {
        menu_append_query(bar, character);
        gtk_widget_queue_draw(bar->menu_area);
        return TRUE;
    }

    return FALSE;
}

static PanelFrame panel_frame_for(NintyBar *bar, PanelKind kind,
                                  double screen_w, double screen_h) {
    (void)bar;
    PanelFrame frame = { 0 };

    switch (kind) {
    case PANEL_VOLUME:
        frame.w = 252.0;
        frame.h = 138.0;
        break;
    case PANEL_CALENDAR:
        frame.w = 260.0;
        frame.h = 224.0;
        break;
    case PANEL_NETWORK:
        frame.w = 252.0;
        frame.h = 122.0;
        break;
    case PANEL_POWER:
        frame.w = 252.0;
        frame.h = 122.0;
        break;
    case PANEL_TASKMENU:
        frame.w = 176.0;
        frame.h = 84.0;
        break;
    case PANEL_CLIPBOARD:
        frame.w = 320.0;
        frame.h = 260.0;
        break;
    default:
        frame.w = 1.0;
        frame.h = 1.0;
        break;
    }

    if (kind == PANEL_TASKMENU) {
        frame.x = bar->context_x;
        frame.y = screen_h - BAR_HEIGHT - frame.h - 4.0;
    } else {
        frame.x = screen_w - frame.w - 6.0;
        frame.y = screen_h - BAR_HEIGHT - frame.h - 6.0;
    }
    if (frame.x + frame.w > screen_w - 2.0) {
        frame.x = screen_w - frame.w - 2.0;
    }
    if (frame.x < 2.0) {
        frame.x = 2.0;
    }
    if (frame.y < 2.0) {
        frame.y = 2.0;
    }
    frame.title_h = 18.0;
    return frame;
}

static void panel_close_rect(const PanelFrame *frame, double *x, double *y,
                             double *w, double *h) {
    *w = 14.0;
    *h = 14.0;
    *x = frame->x + frame->w - *w - 6.0;
    *y = frame->y + 4.0;
}

static void volume_track_rect(const PanelFrame *frame, double *x, double *y,
                              double *w, double *h) {
    *x = frame->x + 16.0;
    *y = frame->y + 76.0;
    *w = frame->w - 32.0;
    *h = 6.0;
}

static void draw_panel_frame(cairo_t *cr, const PanelFrame *frame,
                             const char *title) {
    win98_fill(cr, frame->x, frame->y, frame->w, frame->h, win98_face);
    win98_bevel(cr, frame->x, frame->y, frame->w, frame->h,
                WIN98_BEVEL_RAISED);

    double tx = frame->x + 3.0;
    double ty = frame->y + 3.0;
    double tw = frame->w - 6.0;
    double th = frame->title_h;

    cairo_pattern_t *gradient =
        cairo_pattern_create_linear(tx, ty, tx, ty + th);
    cairo_pattern_add_color_stop_rgb(
        gradient, 0.0, NINTYSH_RGB_R(NINTYSH_COLOR_TITLE_TOP),
        NINTYSH_RGB_G(NINTYSH_COLOR_TITLE_TOP),
        NINTYSH_RGB_B(NINTYSH_COLOR_TITLE_TOP));
    cairo_pattern_add_color_stop_rgb(
        gradient, 1.0, NINTYSH_RGB_R(NINTYSH_COLOR_TITLE_BOTTOM),
        NINTYSH_RGB_G(NINTYSH_COLOR_TITLE_BOTTOM),
        NINTYSH_RGB_B(NINTYSH_COLOR_TITLE_BOTTOM));
    cairo_set_source(cr, gradient);
    cairo_rectangle(cr, tx, ty, tw, th);
    cairo_fill(cr);
    cairo_pattern_destroy(gradient);

    win98_draw_text(cr, tx + 5.0, ty, tw - 28.0, th, title,
                    WIN98_ALIGN_LEFT, TRUE, win98_white, FALSE);

    double cx = 0;
    double cy = 0;
    double cw = 0;
    double ch = 0;
    panel_close_rect(frame, &cx, &cy, &cw, &ch);
    win98_fill(cr, cx, cy, cw, ch, win98_face);
    win98_bevel(cr, cx, cy, cw, ch, WIN98_BEVEL_RAISED);
    cairo_set_source_rgb(cr, 0, 0, 0);
    cairo_set_line_width(cr, 1.0);
    cairo_move_to(cr, cx + 4.5, cy + 4.5);
    cairo_line_to(cr, cx + 9.5, cy + 9.5);
    cairo_move_to(cr, cx + 9.5, cy + 4.5);
    cairo_line_to(cr, cx + 4.5, cy + 9.5);
    cairo_stroke(cr);
}

static void panel_draw_volume(cairo_t *cr, NintyBar *bar,
                              const PanelFrame *frame) {
    char percent_text[64];
    if (bar->volume_muted) {
        g_snprintf(percent_text, sizeof(percent_text), "Muted  %d%%",
                   bar->volume_percent);
    } else {
        g_snprintf(percent_text, sizeof(percent_text), "%d%%",
                   bar->volume_percent);
    }

    cairo_save(cr);
    cairo_translate(cr, frame->x + 12.0, frame->y + 28.0);
    cairo_scale(cr, 2.0, 2.0);
    win98_draw_volume_icon(cr, 0, 0, bar->volume_muted);
    cairo_restore(cr);

    win98_draw_text(cr, frame->x + 54.0, frame->y + 28.0,
                    frame->w - 68.0, 18.0, "Volume", WIN98_ALIGN_LEFT, TRUE,
                    win98_text, FALSE);
    win98_draw_text(cr, frame->x + 54.0, frame->y + 46.0,
                    frame->w - 68.0, 18.0, percent_text, WIN98_ALIGN_RIGHT,
                    FALSE, win98_text, FALSE);

    double tx = 0;
    double ty = 0;
    double tw = 0;
    double th = 0;
    volume_track_rect(frame, &tx, &ty, &tw, &th);

    win98_fill(cr, tx, ty, tw, th, win98_light);
    win98_bevel(cr, tx, ty, tw, th, WIN98_BEVEL_SUNKEN);
    win98_fill(cr, tx + 2.0, ty + 2.0, tw - 4.0, 2.0, win98_shadow);
    win98_fill(cr, tx + 2.0, ty + th - 3.0, tw - 4.0, 1.0, win98_white);

    double usable = tw - 16.0;
    double fraction = CLAMP(bar->volume_percent / 100.0, 0.0, 1.0);
    double thumb_x = tx + 2.0 + usable * fraction;
    win98_fill(cr, thumb_x, ty - 6.0, 12.0, 18.0, win98_face);
    win98_bevel(cr, thumb_x, ty - 6.0, 12.0, 18.0, WIN98_BEVEL_RAISED);
    win98_fill(cr, thumb_x + 5.0, ty - 2.0, 2.0, 10.0, win98_shadow);

    for (int i = 0; i <= 10; i++) {
        double tick_x = tx + 2.0 + (tw - 4.0) * i / 10.0;
        double tick_h = (i % 5 == 0) ? 5.0 : 3.0;
        win98_fill(cr, tick_x, ty + th + 3.0, 1.0, tick_h, win98_shadow);
    }

    double cbx = frame->x + 16.0;
    double cby = frame->y + frame->h - 26.0;
    win98_fill(cr, cbx, cby, 13.0, 13.0, win98_white);
    win98_bevel(cr, cbx, cby, 13.0, 13.0, WIN98_BEVEL_SUNKEN);
    if (bar->volume_muted) {
        cairo_set_source_rgb(cr, 0, 0, 0);
        cairo_set_line_width(cr, 2.0);
        cairo_move_to(cr, cbx + 3.0, cby + 4.0);
        cairo_line_to(cr, cbx + 5.5, cby + 7.0);
        cairo_line_to(cr, cbx + 10.0, cby + 2.0);
        cairo_stroke(cr);
    }
    win98_draw_text(cr, cbx + 19.0, cby - 2.0, frame->w - 44.0, 17.0,
                    "Mute", WIN98_ALIGN_LEFT, FALSE, win98_text, FALSE);
}

static void calendar_nav_rects(const PanelFrame *frame, double *prev_x,
                               double *next_x, double *y, double *w,
                               double *h) {
    *w = 20.0;
    *h = 18.0;
    *y = frame->y + frame->title_h + 8.0;
    *prev_x = frame->x + 12.0;
    *next_x = frame->x + frame->w - 12.0 - *w;
}

static void panel_draw_calendar(cairo_t *cr, NintyBar *bar,
                                const PanelFrame *frame) {
    draw_panel_frame(cr, frame, "Date/Time");

    double prev_x = 0;
    double next_x = 0;
    double nav_y = 0;
    double nav_w = 0;
    double nav_h = 0;
    calendar_nav_rects(frame, &prev_x, &next_x, &nav_y, &nav_w, &nav_h);

    win98_fill(cr, prev_x, nav_y, nav_w, nav_h, win98_face);
    win98_bevel(cr, prev_x, nav_y, nav_w, nav_h, WIN98_BEVEL_RAISED);
    win98_draw_text(cr, prev_x, nav_y, nav_w, nav_h, "<",
                    WIN98_ALIGN_CENTER, TRUE, win98_text, FALSE);

    win98_fill(cr, next_x, nav_y, nav_w, nav_h, win98_face);
    win98_bevel(cr, next_x, nav_y, nav_w, nav_h, WIN98_BEVEL_RAISED);
    win98_draw_text(cr, next_x, nav_y, nav_w, nav_h, ">",
                    WIN98_ALIGN_CENTER, TRUE, win98_text, FALSE);

    GDateTime *first =
        g_date_time_new_local(bar->calendar_year, bar->calendar_month, 1, 0,
                              0, 0);
    gchar *month = g_date_time_format(first, "%B %Y");
    win98_draw_text(cr, prev_x + nav_w + 4.0, nav_y,
                    next_x - prev_x - nav_w - 8.0, nav_h, month,
                    WIN98_ALIGN_CENTER, TRUE, win98_text, FALSE);
    g_free(month);

    const char *weekdays[] = { "S", "M", "T", "W", "T", "F", "S" };
    double grid_x = frame->x + 10.0;
    double grid_w = frame->w - 20.0;
    double cell_w = grid_w / 7.0;
    double week_y = nav_y + nav_h + 4.0;

    for (int col = 0; col < 7; col++) {
        win98_draw_text(cr, grid_x + col * cell_w, week_y, cell_w, 16.0,
                        weekdays[col], WIN98_ALIGN_CENTER, TRUE, win98_text,
                        FALSE);
    }

    double grid_y = week_y + 18.0;
    double row_h = 22.0;
    int first_col = g_date_time_get_day_of_week(first) % 7;
    GDate month_date;
    g_date_clear(&month_date, 1);
    g_date_set_dmy(&month_date, 1, (GDateMonth)bar->calendar_month,
                   (GDateYear)bar->calendar_year);
    int days = g_date_get_days_in_month(g_date_get_month(&month_date),
                                        g_date_get_year(&month_date));

    GDateTime *now = g_date_time_new_now_local();
    int today_day = g_date_time_get_day_of_month(now);
    int today_month = g_date_time_get_month(now);
    int today_year = g_date_time_get_year(now);

    for (int day = 1; day <= days; day++) {
        int position = first_col + day - 1;
        int row = position / 7;
        int col = position % 7;
        double x = grid_x + col * cell_w;
        double y = grid_y + row * row_h;
        gboolean today = day == today_day &&
                         bar->calendar_month == today_month &&
                         bar->calendar_year == today_year;
        if (today) {
            win98_fill(cr, x + 2.0, y, cell_w - 4.0, row_h, win98_navy);
        }
        char day_text[8];
        g_snprintf(day_text, sizeof(day_text), "%d", day);
        win98_draw_text(cr, x, y, cell_w, row_h, day_text,
                        WIN98_ALIGN_CENTER, today,
                        today ? win98_white : win98_text, FALSE);
    }

    g_date_time_unref(now);
    g_date_time_unref(first);
}

static void panel_draw_network(cairo_t *cr, NintyBar *bar,
                               const PanelFrame *frame) {
    draw_panel_frame(cr, frame, "Network");

    cairo_save(cr);
    cairo_translate(cr, frame->x + 14.0, frame->y + 32.0);
    cairo_scale(cr, 2.0, 2.0);
    win98_draw_network_icon(cr, 0, 0, bar->network_available);
    cairo_restore(cr);

    win98_draw_text(cr, frame->x + 54.0, frame->y + 30.0,
                    frame->w - 68.0, 18.0,
                    bar->network_available ? "Connected" : "Not connected",
                    WIN98_ALIGN_LEFT, TRUE, win98_text, FALSE);

    char host_text[256];
    g_snprintf(host_text, sizeof(host_text), "Host: %s",
               g_get_host_name());
    win98_draw_text(cr, frame->x + 54.0, frame->y + 50.0,
                    frame->w - 68.0, 17.0, host_text, WIN98_ALIGN_LEFT,
                    FALSE, win98_text, TRUE);

    win98_draw_text(cr, frame->x + 54.0, frame->y + 68.0,
                    frame->w - 68.0, 17.0,
                    niri_client_is_connected(bar->niri)
                        ? "Niri IPC: online"
                        : "Niri IPC: offline",
                    WIN98_ALIGN_LEFT, FALSE, win98_text, TRUE);
}

static void panel_draw_power(cairo_t *cr, NintyBar *bar,
                             const PanelFrame *frame) {
    draw_panel_frame(cr, frame, "Power");

    cairo_save(cr);
    cairo_translate(cr, frame->x + 14.0, frame->y + 32.0);
    cairo_scale(cr, 2.0, 2.0);
    win98_draw_battery_icon(cr, 0, 0, bar->battery_percent,
                            bar->battery_charging);
    cairo_restore(cr);

    char percent_text[32];
    g_snprintf(percent_text, sizeof(percent_text), "%d%%",
               bar->battery_percent);
    win98_draw_text(cr, frame->x + 54.0, frame->y + 30.0,
                    frame->w - 68.0, 18.0, percent_text,
                    WIN98_ALIGN_LEFT, TRUE, win98_text, FALSE);
    win98_draw_text(cr, frame->x + 54.0, frame->y + 50.0,
                    frame->w - 68.0, 17.0,
                    bar->battery_charging ? "Charging" : "On battery",
                    WIN98_ALIGN_LEFT, FALSE, win98_text, FALSE);
}

static int clipboard_rows(const PanelFrame *frame) {
    return (int)((frame->h - frame->title_h - 16.0) / 20.0);
}

static int clipboard_row_at(const PanelFrame *frame, double y) {
    double list_y = frame->y + frame->title_h + 8.0;
    if (y < list_y) {
        return -1;
    }
    int row = (int)((y - list_y) / 20.0);
    return row < clipboard_rows(frame) ? row : -1;
}

static void panel_draw_clipboard(cairo_t *cr, NintyBar *bar,
                                 const PanelFrame *frame) {
    draw_panel_frame(cr, frame, "Clipboard");

    double list_x = frame->x + 8.0;
    double list_y = frame->y + frame->title_h + 8.0;
    double list_w = frame->w - 24.0;
    int rows = clipboard_rows(frame);
    int count = (int)bar->clipboard_history->len;
    int max_scroll = MAX(0, count - rows);
    bar->clipboard_scroll = CLAMP(bar->clipboard_scroll, 0, max_scroll);

    if (count == 0) {
        win98_draw_text(cr, list_x, list_y, list_w, 20.0,
                        "Clipboard history is empty", WIN98_ALIGN_CENTER,
                        FALSE, win98_text_disabled, TRUE);
    }

    for (int row = 0; row < rows; row++) {
        int index = bar->clipboard_scroll + row;
        if (index >= count) {
            break;
        }
        double y = list_y + row * 20.0;
        gboolean selected = index == bar->panel_hover;
        if (selected) {
            win98_fill(cr, list_x, y, list_w, 20.0, win98_navy);
        }
        const char *text = g_ptr_array_index(bar->clipboard_history, index);
        win98_draw_text(cr, list_x + 4.0, y, list_w - 8.0, 20.0, text,
                        WIN98_ALIGN_LEFT, FALSE,
                        selected ? win98_white : win98_text, TRUE);
    }

    if (max_scroll > 0) {
        double sx = frame->x + frame->w - 14.0;
        double sy = list_y;
        double sh = frame->h - frame->title_h - 16.0;
        win98_fill(cr, sx, sy, 12.0, sh, win98_light);
        win98_bevel(cr, sx, sy, 12.0, sh, WIN98_BEVEL_SUNKEN);
        double track_y = sy + 12.0;
        double track_h = sh - 24.0;
        double thumb_h = MAX(16.0, track_h * rows / count);
        thumb_h = MIN(thumb_h, track_h);
        double fraction = (double)bar->clipboard_scroll / max_scroll;
        double thumb_y = track_y + (track_h - thumb_h) * fraction;
        win98_fill(cr, sx + 1.0, thumb_y, 10.0, thumb_h, win98_face);
        win98_bevel(cr, sx + 1.0, thumb_y, 10.0, thumb_h,
                    WIN98_BEVEL_RAISED);
    }
}

static const char *task_menu_items[] = {
    "Focus", "Toggle Floating", "Fullscreen", NULL, "Close"
};

static int task_menu_row_at(const PanelFrame *frame, double y) {
    double row_y = frame->y + 3.0;
    for (int row = 0; row < 5; row++) {
        if (task_menu_items[row] == NULL) {
            row_y += 6.0;
            continue;
        }
        if (y >= row_y && y < row_y + 18.0) {
            return row;
        }
        row_y += 18.0;
    }
    return -1;
}

static void panel_draw_task_menu(cairo_t *cr, NintyBar *bar,
                                 const PanelFrame *frame) {
    win98_fill(cr, frame->x, frame->y, frame->w, frame->h, win98_face);
    win98_bevel(cr, frame->x, frame->y, frame->w, frame->h,
                WIN98_BEVEL_RAISED);

    double x = frame->x + 3.0;
    double w = frame->w - 6.0;
    double y = frame->y + 3.0;

    for (int row = 0; row < 5; row++) {
        if (task_menu_items[row] == NULL) {
            win98_fill(cr, x, y + 2.0, w, 1.0, win98_shadow);
            win98_fill(cr, x, y + 3.0, w, 1.0, win98_highlight);
            y += 6.0;
            continue;
        }

        if (bar->panel_hover == row) {
            win98_fill(cr, x, y, w, 18.0, win98_navy);
        }
        win98_draw_text(cr, x + 8.0, y, w - 10.0, 18.0,
                        task_menu_items[row], WIN98_ALIGN_LEFT, FALSE,
                        bar->panel_hover == row ? win98_white : win98_text,
                        FALSE);
        y += 18.0;
    }
}

static void panel_draw(GtkDrawingArea *area, cairo_t *cr, int width,
                       int height, gpointer user_data) {
    (void)area;
    NintyBar *bar = user_data;

    cairo_save(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);
    cairo_restore(cr);

    if (bar->panel_kind == PANEL_NONE || width < 64 || height < 64) {
        return;
    }

    cairo_set_antialias(cr, CAIRO_ANTIALIAS_NONE);
    PanelFrame frame = panel_frame_for(bar, bar->panel_kind, width, height);

    switch (bar->panel_kind) {
    case PANEL_VOLUME:
        draw_panel_frame(cr, &frame, "Volume");
        panel_draw_volume(cr, bar, &frame);
        break;
    case PANEL_CALENDAR:
        panel_draw_calendar(cr, bar, &frame);
        break;
    case PANEL_NETWORK:
        panel_draw_network(cr, bar, &frame);
        break;
    case PANEL_POWER:
        panel_draw_power(cr, bar, &frame);
        break;
    case PANEL_TASKMENU:
        panel_draw_task_menu(cr, bar, &frame);
        break;
    case PANEL_CLIPBOARD:
        panel_draw_clipboard(cr, bar, &frame);
        break;
    default:
        break;
    }
}

static int volume_percent_from_x(const PanelFrame *frame, double x) {
    double tx = 0;
    double ty = 0;
    double tw = 0;
    double th = 0;
    (void)ty;
    (void)th;
    volume_track_rect(frame, &tx, &ty, &tw, &th);

    double usable = tw - 16.0;
    double fraction = (x - (tx + 2.0)) / usable;
    return CLAMP((int)round(fraction * 100.0), 0, 100);
}

static void open_panel(NintyBar *bar, PanelKind kind) {
    if (kind == PANEL_NONE) {
        return;
    }

    close_menu(bar);
    if (kind == PANEL_CLIPBOARD) {
        bar->clipboard_scroll = 0;
        bar->panel_hover = -1;
    }
    if (kind == PANEL_CALENDAR) {
        GDateTime *now = g_date_time_new_now_local();
        bar->calendar_year = g_date_time_get_year(now);
        bar->calendar_month = g_date_time_get_month(now);
        g_date_time_unref(now);
    }

    bar->panel_kind = kind;
    bar->panel_hover = -1;
    bar->volume_dragging = FALSE;
    gtk_widget_set_visible(bar->panel_window, TRUE);
    gtk_window_present(GTK_WINDOW(bar->panel_window));
    gtk_widget_grab_focus(bar->panel_area);
    gtk_widget_queue_draw(bar->panel_area);
    bar_queue_redraw(bar);
}

static void open_task_menu(NintyBar *bar, gint64 window_id, double x) {
    close_menu(bar);
    if (bar->panel_kind == PANEL_TASKMENU) {
        close_panel(bar);
    }
    bar->panel_kind = PANEL_TASKMENU;
    bar->panel_hover = -1;
    bar->context_window_id = window_id;
    bar->context_x = x - 10.0;
    gtk_widget_set_visible(bar->panel_window, TRUE);
    gtk_window_present(GTK_WINDOW(bar->panel_window));
    gtk_widget_grab_focus(bar->panel_area);
    gtk_widget_queue_draw(bar->panel_area);
    bar_queue_redraw(bar);
}

static void close_panel(NintyBar *bar) {
    if (bar->panel_kind == PANEL_NONE) {
        return;
    }
    bar->panel_kind = PANEL_NONE;
    bar->volume_dragging = FALSE;
    bar->panel_hover = -1;
    gtk_widget_set_visible(bar->panel_window, FALSE);
    bar_queue_redraw(bar);
}

static void panel_clicked(GtkGestureClick *gesture, int n_press, double x,
                          double y, gpointer user_data) {
    (void)gesture;
    (void)n_press;
    NintyBar *bar = user_data;
    double width = gtk_widget_get_width(bar->panel_area);
    double height = gtk_widget_get_height(bar->panel_area);
    PanelFrame frame = panel_frame_for(bar, bar->panel_kind, width, height);

    if (x < frame.x || x >= frame.x + frame.w || y < frame.y ||
        y >= frame.y + frame.h) {
        close_panel(bar);
        return;
    }

    if (bar->panel_kind == PANEL_TASKMENU) {
        int row = task_menu_row_at(&frame, y);
        if (row == 0) {
            niri_client_focus_window(bar->niri, bar->context_window_id);
        } else if (row == 1) {
            niri_client_toggle_floating(bar->niri, bar->context_window_id);
        } else if (row == 2) {
            niri_client_fullscreen(bar->niri, bar->context_window_id);
        } else if (row == 4) {
            niri_client_close_window(bar->niri, bar->context_window_id);
        }
        if (row >= 0) {
            close_panel(bar);
        }
        return;
    }

    if (bar->panel_kind == PANEL_CLIPBOARD) {
        int row = clipboard_row_at(&frame, y);
        double list_x = frame.x + 8.0;
        double list_w = frame.w - 24.0;
        if (row >= 0 && x >= list_x && x < list_x + list_w) {
            int index = bar->clipboard_scroll + row;
            if (index < (int)bar->clipboard_history->len) {
                clipboard_copy(bar,
                               g_ptr_array_index(bar->clipboard_history,
                                                 index));
            }
        }
        return;
    }

    double cx = 0;
    double cy = 0;
    double cw = 0;
    double ch = 0;
    panel_close_rect(&frame, &cx, &cy, &cw, &ch);
    if (x >= cx && x < cx + cw && y >= cy && y < cy + ch) {
        close_panel(bar);
        return;
    }

    if (bar->panel_kind == PANEL_VOLUME) {
        double cbx = frame.x + 16.0;
        double cby = frame.y + frame.h - 26.0;
        if (x >= cbx - 2.0 && x < cbx + 16.0 && y >= cby - 2.0 &&
            y < cby + 16.0) {
            toggle_volume(bar);
            return;
        }

        double tx = 0;
        double ty = 0;
        double tw = 0;
        double th = 0;
        volume_track_rect(&frame, &tx, &ty, &tw, &th);
        if (x >= tx - 6.0 && x < tx + tw + 6.0 && y >= ty - 12.0 &&
            y < ty + th + 14.0) {
            apply_volume(bar, volume_percent_from_x(&frame, x), TRUE);
        }
    } else if (bar->panel_kind == PANEL_CALENDAR) {
        double prev_x = 0;
        double next_x = 0;
        double nav_y = 0;
        double nav_w = 0;
        double nav_h = 0;
        calendar_nav_rects(&frame, &prev_x, &next_x, &nav_y, &nav_w,
                           &nav_h);
        (void)nav_y;
        (void)nav_h;

        if (x >= prev_x && x < prev_x + nav_w && y >= nav_y &&
            y < nav_y + nav_h) {
            bar->calendar_month--;
            if (bar->calendar_month < 1) {
                bar->calendar_month = 12;
                bar->calendar_year--;
            }
            gtk_widget_queue_draw(bar->panel_area);
        } else if (x >= next_x && x < next_x + nav_w && y >= nav_y &&
                   y < nav_y + nav_h) {
            bar->calendar_month++;
            if (bar->calendar_month > 12) {
                bar->calendar_month = 1;
                bar->calendar_year++;
            }
            gtk_widget_queue_draw(bar->panel_area);
        }
    }
}

static void panel_drag_begin(GtkGestureDrag *gesture, double start_x,
                             double start_y, gpointer user_data) {
    (void)gesture;
    (void)start_y;
    NintyBar *bar = user_data;
    if (bar->panel_kind != PANEL_VOLUME) {
        return;
    }

    double width = gtk_widget_get_width(bar->panel_area);
    double height = gtk_widget_get_height(bar->panel_area);
    PanelFrame frame = panel_frame_for(bar, PANEL_VOLUME, width, height);
    double tx = 0;
    double ty = 0;
    double tw = 0;
    double th = 0;
    volume_track_rect(&frame, &tx, &ty, &tw, &th);

    if (start_x >= tx - 6.0 && start_x < tx + tw + 6.0 &&
        start_y >= ty - 12.0 && start_y < ty + th + 14.0) {
        bar->volume_dragging = TRUE;
        bar->volume_drag_x0 = start_x;
    }
}

static void panel_drag_update(GtkGestureDrag *gesture, double offset_x,
                              double offset_y, gpointer user_data) {
    (void)gesture;
    (void)offset_y;
    NintyBar *bar = user_data;
    if (!bar->volume_dragging) {
        return;
    }

    double width = gtk_widget_get_width(bar->panel_area);
    double height = gtk_widget_get_height(bar->panel_area);
    PanelFrame frame = panel_frame_for(bar, PANEL_VOLUME, width, height);
    int percent =
        volume_percent_from_x(&frame, bar->volume_drag_x0 + offset_x);
    apply_volume(bar, percent, FALSE);
}

static void panel_drag_end(GtkGestureDrag *gesture, double offset_x,
                           double offset_y, gpointer user_data) {
    (void)gesture;
    (void)offset_x;
    (void)offset_y;
    NintyBar *bar = user_data;
    if (!bar->volume_dragging) {
        return;
    }
    bar->volume_dragging = FALSE;
    apply_volume(bar, bar->volume_percent, TRUE);
}

static void panel_motion(GtkEventControllerMotion *controller, double x,
                         double y, gpointer user_data) {
    (void)controller;
    (void)x;
    NintyBar *bar = user_data;
    if (bar->panel_kind != PANEL_TASKMENU &&
        bar->panel_kind != PANEL_CLIPBOARD) {
        return;
    }

    double width = gtk_widget_get_width(bar->panel_area);
    double height = gtk_widget_get_height(bar->panel_area);
    PanelFrame frame =
        panel_frame_for(bar, bar->panel_kind, width, height);
    int hover = bar->panel_kind == PANEL_TASKMENU
                    ? task_menu_row_at(&frame, y)
                    : clipboard_row_at(&frame, y);
    if (hover != bar->panel_hover) {
        bar->panel_hover = hover;
        gtk_widget_queue_draw(bar->panel_area);
    }
}

static gboolean panel_scrolled(GtkEventControllerScroll *controller,
                               double dx, double dy, gpointer user_data) {
    (void)controller;
    (void)dx;
    NintyBar *bar = user_data;
    if (bar->panel_kind == PANEL_VOLUME) {
        int delta = dy > 0 ? -2 : 2;
        apply_volume(bar, bar->volume_percent + delta, TRUE);
    } else if (bar->panel_kind == PANEL_CLIPBOARD) {
        double width = gtk_widget_get_width(bar->panel_area);
        double height = gtk_widget_get_height(bar->panel_area);
        PanelFrame frame = panel_frame_for(bar, PANEL_CLIPBOARD, width, height);
        int max_scroll = MAX(0, (int)bar->clipboard_history->len -
                                   clipboard_rows(&frame));
        int step = dy > 0 ? 3 : -3;
        bar->clipboard_scroll =
            CLAMP(bar->clipboard_scroll + step, 0, max_scroll);
        gtk_widget_queue_draw(bar->panel_area);
    }
    return TRUE;
}

static gboolean panel_key_pressed(GtkEventControllerKey *controller,
                                  guint keyval, guint keycode,
                                  GdkModifierType state, gpointer user_data) {
    (void)controller;
    (void)keycode;
    (void)state;
    NintyBar *bar = user_data;

    if (keyval == GDK_KEY_Escape) {
        close_panel(bar);
        return TRUE;
    }
    return FALSE;
}

static gboolean osd_hide_cb(gpointer user_data) {
    NintyBar *bar = user_data;
    bar->osd_source = 0;
    gtk_widget_set_visible(bar->osd_window, FALSE);
    return G_SOURCE_REMOVE;
}

static void osd_present(NintyBar *bar) {
    if (bar->osd_source != 0) {
        g_source_remove(bar->osd_source);
        bar->osd_source = 0;
    }
    gtk_widget_set_visible(bar->osd_window, TRUE);
    gtk_window_present(GTK_WINDOW(bar->osd_window));
    gtk_widget_queue_draw(bar->osd_area);
    bar->osd_source = g_timeout_add(NINTYSH_OSD_TIMEOUT_MS, osd_hide_cb, bar);
}

static void osd_show_volume(NintyBar *bar) {
    if (bar->volume_muted) {
        g_snprintf(bar->osd_text, sizeof(bar->osd_text), "Muted");
    } else {
        g_snprintf(bar->osd_text, sizeof(bar->osd_text), "Volume %d%%",
                   bar->volume_percent);
    }
    bar->osd_value = bar->volume_percent;
    bar->osd_mute = bar->volume_muted;
    osd_present(bar);
}

static void osd_show_text(NintyBar *bar, const char *text) {
    g_strlcpy(bar->osd_text, text != NULL ? text : "",
              sizeof(bar->osd_text));
    bar->osd_value = -1;
    bar->osd_mute = FALSE;
    osd_present(bar);
}

static void osd_draw(GtkDrawingArea *area, cairo_t *cr, int width,
                     int height, gpointer user_data) {
    (void)area;
    NintyBar *bar = user_data;

    cairo_save(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);
    cairo_restore(cr);

    if (width < 80 || height < 30) {
        return;
    }

    cairo_set_antialias(cr, CAIRO_ANTIALIAS_NONE);
    win98_fill(cr, 0, 0, width, height, win98_face);
    win98_bevel(cr, 0, 0, width, height, WIN98_BEVEL_RAISED);

    if (bar->osd_value >= 0) {
        win98_draw_volume_icon(cr, 10, 10, bar->osd_mute);
        win98_draw_text(cr, 34, 8, width - 44, 18, bar->osd_text,
                        WIN98_ALIGN_LEFT, TRUE, win98_text, TRUE);
        win98_fill(cr, 34, 32, width - 46, 12, win98_light);
        win98_bevel(cr, 34, 32, width - 46, 12, WIN98_BEVEL_SUNKEN);
        int fill = (int)((width - 50) * bar->osd_value / 100.0);
        if (fill > 0) {
            win98_fill(cr, 36, 34, fill, 8, win98_navy);
        }
    } else {
        win98_draw_text(cr, 8, 4, width - 16, height - 8, bar->osd_text,
                        WIN98_ALIGN_CENTER, TRUE, win98_text, TRUE);
    }
}

static void draw_separator(cairo_t *cr, double x, double y, double h) {
    win98_fill(cr, x, y, 1, h, win98_shadow);
    win98_fill(cr, x + 1, y, 1, h, win98_highlight);
}

static void bar_draw(GtkDrawingArea *area, cairo_t *cr, int width, int height,
                     gpointer user_data) {
    (void)area;
    NintyBar *bar = user_data;
    double w = width;
    double h = height;

    cairo_set_antialias(cr, CAIRO_ANTIALIAS_NONE);
    win98_fill(cr, 0, 0, w, h, win98_face);
    win98_fill(cr, 0, 0, w, 1, win98_highlight);
    bar_clear_hits(bar);

    const char *output = bar->active_output != NULL
                             ? bar->active_output
                             : current_output(bar->niri);
    double x = 2.0;
    double y = BUTTON_Y;
    double button_h = BUTTON_HEIGHT;

    int start_text_w = win98_text_width("Start", TRUE);
    double start_w = 4.0 + 16.0 + 5.0 + start_text_w + 8.0;
    if (start_w < 54.0) {
        start_w = 54.0;
    }
    gboolean start_pressed = bar->menu_open;
    win98_fill(cr, x, y, start_w, button_h, win98_face);
    win98_bevel(cr, x, y, start_w, button_h,
                start_pressed ? WIN98_BEVEL_SUNKEN : WIN98_BEVEL_RAISED);
    double offset = start_pressed ? 1.0 : 0.0;
    win98_draw_start_flag(cr, x + 4.0 + offset, y + 3.0 + offset);
    win98_draw_text(cr, x + 25.0 + offset, y + 2.0 + offset, start_w - 27.0,
               button_h - 4.0, "Start", WIN98_ALIGN_LEFT, TRUE, win98_text,
               FALSE);
    bar_add_hit(bar, HIT_START, x, y, start_w, button_h, 0);
    x += start_w + 4.0;

    draw_separator(cr, x, y + 1.0, button_h - 2.0);
    x += 4.0;

    guint workspace_count = niri_client_workspace_count(bar->niri);
    guint drawn_workspaces = 0;
    for (guint i = 0; i < workspace_count; i++) {
        NiriWorkspace *workspace = niri_client_workspace(bar->niri, i);
        if (!workspace_on_output(workspace, output)) {
            continue;
        }

        char index_text[32];
        const char *label = workspace->name;
        if (label == NULL || label[0] == '\0') {
            g_snprintf(index_text, sizeof(index_text), "%" G_GINT64_FORMAT,
                       workspace->idx);
            label = index_text;
        }

        int text_w = win98_text_width(label, FALSE);
        double workspace_w = CLAMP((double)text_w + 12.0, 24.0, 64.0);
        gboolean pressed = workspace->is_focused;
        offset = pressed ? 1.0 : 0.0;

        win98_fill(cr, x, y, workspace_w, button_h, win98_face);
        win98_bevel(cr, x, y, workspace_w, button_h,
                    pressed ? WIN98_BEVEL_SUNKEN : WIN98_BEVEL_RAISED);
        win98_draw_text(cr, x + 3.0 + offset, y + 2.0 + offset,
                   workspace_w - 6.0, button_h - 4.0, label,
                   WIN98_ALIGN_CENTER, FALSE, win98_text, TRUE);
        if (pressed) {
            win98_focus_rect(cr, x + 3.0, y + 3.0, workspace_w - 6.0,
                             button_h - 6.0);
        }
        bar_add_hit(bar, HIT_WORKSPACE, x, y, workspace_w, button_h,
                    workspace->idx);
        x += workspace_w + 2.0;
        drawn_workspaces++;
    }

    if (drawn_workspaces > 0) {
        x += 2.0;
        draw_separator(cr, x, y + 1.0, button_h - 2.0);
        x += 4.0;
    }

    char clock_text[32];
    format_clock(clock_text, sizeof(clock_text));
    int clock_w = win98_text_width("00:00 PM", FALSE) + 10;
    const char *layout_name = niri_client_keyboard_layout(bar->niri);
    int layout_w = (layout_name != NULL && layout_name[0] != '\0')
                       ? MAX(20, win98_text_width(layout_name, TRUE) + 8)
                       : 0;
    double tray_w = 4.0 + 20.0 + 20.0 + 20.0 +
                    (bar->battery_available ? 20.0 : 0.0) + layout_w + 4.0 +
                    clock_w + 4.0;
    double tray_x = w - 2.0 - tray_w;
    if (tray_x < x + 40.0) {
        tray_x = x + 40.0;
    }

    guint window_count = niri_client_window_count(bar->niri);
    guint visible_windows = 0;
    for (guint i = 0; i < window_count; i++) {
        NiriWindow *window = niri_client_window(bar->niri, i);
        if (window_on_output(bar->niri, window, output)) {
            visible_windows++;
        }
    }

    double task_x = x;
    double task_end = tray_x - 4.0;
    if (visible_windows == 0) {
        if (!niri_client_is_connected(bar->niri)) {
            win98_draw_text(cr, task_x + 4.0, y, MAX(0.0, task_end - task_x - 4.0),
                       button_h, "Niri IPC offline", WIN98_ALIGN_LEFT, FALSE,
                       win98_text_disabled, TRUE);
        }
    } else {
        NiriWindow **windows = g_new0(NiriWindow *, visible_windows);
        double *widths = g_new0(double, visible_windows);
        guint index = 0;
        double desired_total = 0.0;

        for (guint i = 0; i < window_count; i++) {
            NiriWindow *window = niri_client_window(bar->niri, i);
            if (!window_on_output(bar->niri, window, output)) {
                continue;
            }
            windows[index] = window;
            char *label = window_label(window);
            int text_w = win98_text_width(label, FALSE);
            widths[index] = CLAMP((double)text_w + 32.0, 74.0, 170.0);
            desired_total += widths[index] + (index > 0 ? 2.0 : 0.0);
            g_free(label);
            index++;
        }

        double available = task_end - task_x;
        if (desired_total > available && visible_windows > 0) {
            double each =
                (available - (visible_windows - 1) * 2.0) / visible_windows;
            each = CLAMP(each, 30.0, 160.0);
            for (guint i = 0; i < visible_windows; i++) {
                widths[i] = each;
            }
        }

        double current_x = task_x;
        for (guint i = 0; i < visible_windows; i++) {
            if (current_x + widths[i] > task_end) {
                break;
            }

            NiriWindow *window = windows[i];
            gboolean pressed = window->is_focused;
            offset = pressed ? 1.0 : 0.0;
            win98_fill(cr, current_x, y, widths[i], button_h, win98_face);
            win98_bevel(cr, current_x, y, widths[i], button_h,
                        pressed ? WIN98_BEVEL_SUNKEN : WIN98_BEVEL_RAISED);
            GtkIconPaintable *icon = window_icon(bar, window);
            if (icon != NULL) {
                draw_paintable(cr, icon, current_x + 3.0 + offset,
                               y + 1.0 + offset, NINTYSH_ICON_SIZE);
            } else {
                win98_draw_app_icon(cr, current_x + 3.0 + offset,
                                    y + 3.0 + offset);
            }
            char *label = window_label(window);
            win98_draw_text(cr, current_x + 26.0 + offset, y + 2.0 + offset,
                            widths[i] - 29.0, button_h - 4.0, label,
                            WIN98_ALIGN_LEFT, FALSE, win98_text, TRUE);
            g_free(label);
            if (pressed) {
                win98_focus_rect(cr, current_x + 2.0, y + 2.0,
                                 widths[i] - 4.0, button_h - 4.0);
            }
            bar_add_hit(bar, HIT_WINDOW, current_x, y, widths[i], button_h,
                        window->id);
            current_x += widths[i] + 2.0;
        }

        g_free(widths);
        g_free(windows);
    }

    win98_fill(cr, tray_x, y, tray_w, button_h, win98_face);
    win98_bevel(cr, tray_x, y, tray_w, button_h, WIN98_BEVEL_SUNKEN);

    double icon_x = tray_x + 4.0;
    double icon_y = y + 3.0;
    if (bar->battery_available) {
        win98_draw_battery_icon(cr, icon_x, icon_y, bar->battery_percent,
                                bar->battery_charging);
        bar_add_hit(bar, HIT_BATTERY, icon_x - 1.0, icon_y, 18.0, 16.0, 0);
        icon_x += 20.0;
    }

    win98_draw_network_icon(cr, icon_x, icon_y, bar->network_available);
    bar_add_hit(bar, HIT_NETWORK, icon_x - 1.0, icon_y, 18.0, 16.0, 0);
    icon_x += 20.0;

    win98_draw_volume_icon(cr, icon_x, icon_y,
                           bar->volume_available && bar->volume_muted);
    bar_add_hit(bar, HIT_VOLUME, icon_x - 1.0, icon_y, 18.0, 16.0, 0);
    icon_x += 20.0;

    win98_draw_clipboard_icon(cr, icon_x, icon_y);
    bar_add_hit(bar, HIT_CLIPBOARD, icon_x - 1.0, icon_y, 18.0, 16.0, 0);
    icon_x += 20.0;

    if (layout_w > 0) {
        win98_draw_text(cr, icon_x, y + 2.0, layout_w, button_h - 4.0,
                   layout_name, WIN98_ALIGN_CENTER, TRUE, win98_text, FALSE);
        bar_add_hit(bar, HIT_LAYOUT, icon_x, y, layout_w, button_h, 0);
        icon_x += layout_w;
    }

    double clock_x = tray_x + tray_w - clock_w - 2.0;
    if (clock_x > icon_x) {
        draw_separator(cr, clock_x - 3.0, y + 4.0, button_h - 8.0);
    }
    win98_draw_text(cr, clock_x, y + 2.0, clock_w, button_h - 4.0, clock_text,
               WIN98_ALIGN_RIGHT, FALSE, win98_text, FALSE);
    bar_add_hit(bar, HIT_CLOCK, clock_x, y, clock_w, button_h, 0);
}

static void bar_clicked(GtkGestureClick *gesture, int n_press, double x,
                        double y, gpointer user_data) {
    (void)gesture;
    (void)n_press;
    BarSurface *surface = user_data;
    NintyBar *bar = surface->bar;
    BarHit *hit = bar_hit_at(surface->hits, x, y);
    if (hit == NULL) {
        return;
    }

    switch (hit->kind) {
    case HIT_START:
        if (bar->menu_open) {
            close_menu(bar);
        } else {
            open_menu(bar);
        }
        break;
    case HIT_WORKSPACE:
        niri_client_focus_workspace(bar->niri, hit->value);
        break;
    case HIT_WINDOW:
        niri_client_focus_window(bar->niri, hit->value);
        break;
    case HIT_VOLUME:
        open_panel(bar, PANEL_VOLUME);
        break;
    case HIT_CLOCK:
        open_panel(bar, PANEL_CALENDAR);
        break;
    case HIT_LAYOUT:
        niri_client_next_layout(bar->niri);
        break;
    case HIT_NETWORK:
        open_panel(bar, PANEL_NETWORK);
        break;
    case HIT_BATTERY:
        open_panel(bar, PANEL_POWER);
        break;
    case HIT_CLIPBOARD:
        open_panel(bar, PANEL_CLIPBOARD);
        break;
    default:
        break;
    }
}

static void bar_right_clicked(GtkGestureClick *gesture, int n_press,
                              double x, double y, gpointer user_data) {
    (void)gesture;
    (void)n_press;
    BarSurface *surface = user_data;
    NintyBar *bar = surface->bar;
    BarHit *hit = bar_hit_at(surface->hits, x, y);
    if (hit == NULL) {
        return;
    }

    if (hit->kind == HIT_WINDOW) {
        open_task_menu(bar, hit->value, x);
    } else if (hit->kind == HIT_VOLUME) {
        toggle_volume(bar);
    }
}

static gboolean bar_tick(gpointer user_data) {
    NintyBar *bar = user_data;
    bar_queue_redraw(bar);
    return G_SOURCE_CONTINUE;
}

static gboolean bar_slow_tick(gpointer user_data) {
    NintyBar *bar = user_data;
    gboolean changed = update_volume(bar);
    changed |= update_battery(bar);
    changed |= update_network(bar);
    if (changed) {
        bar_queue_redraw(bar);
        if (bar->panel_kind != PANEL_NONE) {
            gtk_widget_queue_draw(bar->panel_area);
        }
    }
    return G_SOURCE_CONTINUE;
}

static void network_changed_cb(GNetworkMonitor *monitor,
                               gboolean available, gpointer user_data) {
    (void)monitor;
    NintyBar *bar = user_data;
    bar->network_available = available;
    bar_queue_redraw(bar);
    if (bar->panel_kind != PANEL_NONE) {
        gtk_widget_queue_draw(bar->panel_area);
    }
}

static void load_css(void) {
    GtkCssProvider *provider = gtk_css_provider_new();
    gtk_css_provider_load_from_string(
        provider,
        "window.nintysh { background-color: transparent; }\n"
        "window.nintysh > * { background-color: transparent; }\n");
    gtk_style_context_add_provider_for_display(
        gdk_display_get_default(), GTK_STYLE_PROVIDER(provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
}

static void primary_monitor_geometry(GdkRectangle *geometry) {
    geometry->x = 0;
    geometry->y = 0;
    geometry->width = 1;
    geometry->height = 1;

    GListModel *monitors =
        gdk_display_get_monitors(gdk_display_get_default());
    if (monitors == NULL) {
        return;
    }

    GdkMonitor *monitor = g_list_model_get_item(monitors, 0);
    if (monitor == NULL) {
        return;
    }
    gdk_monitor_get_geometry(monitor, geometry);
    g_object_unref(monitor);
}

static void create_bar_surface(NintyBar *bar, GdkMonitor *monitor) {
    BarSurface *surface = g_new0(BarSurface, 1);
    surface->bar = bar;
    surface->monitor = monitor;
    surface->hits = g_ptr_array_new_with_free_func(g_free);

    GdkRectangle geometry = { 0 };
    if (monitor != NULL) {
        gdk_monitor_get_geometry(monitor, &geometry);
        const char *connector = gdk_monitor_get_connector(monitor);
        surface->output = g_strdup(connector);
    } else {
        primary_monitor_geometry(&geometry);
    }

    GtkWidget *window = gtk_application_window_new(bar->app);
    gtk_widget_add_css_class(window, "nintysh");
    gtk_window_set_title(GTK_WINDOW(window), "nintysh");
    gtk_window_set_decorated(GTK_WINDOW(window), FALSE);
    gtk_window_set_resizable(GTK_WINDOW(window), FALSE);
    gtk_window_set_default_size(GTK_WINDOW(window), geometry.width,
                                BAR_HEIGHT);

    gtk_layer_init_for_window(GTK_WINDOW(window));
    gtk_layer_set_namespace(GTK_WINDOW(window), "nintysh-bar");
    gtk_layer_set_layer(GTK_WINDOW(window), GTK_LAYER_SHELL_LAYER_TOP);
    if (monitor != NULL) {
        gtk_layer_set_monitor(GTK_WINDOW(window), monitor);
    }
    gtk_layer_set_anchor(GTK_WINDOW(window), GTK_LAYER_SHELL_EDGE_BOTTOM,
                         TRUE);
    gtk_layer_set_anchor(GTK_WINDOW(window), GTK_LAYER_SHELL_EDGE_LEFT, TRUE);
    gtk_layer_set_anchor(GTK_WINDOW(window), GTK_LAYER_SHELL_EDGE_RIGHT,
                         TRUE);
    gtk_layer_set_exclusive_zone(GTK_WINDOW(window), BAR_HEIGHT);
    gtk_layer_set_keyboard_mode(GTK_WINDOW(window),
                                GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);

    surface->window = window;
    surface->area = gtk_drawing_area_new();
    gtk_widget_set_size_request(surface->area, -1, BAR_HEIGHT);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(surface->area),
                                   bar_surface_draw, surface, NULL);
    gtk_window_set_child(GTK_WINDOW(window), surface->area);

    GtkGesture *click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), 1);
    g_signal_connect(click, "pressed", G_CALLBACK(bar_clicked), surface);
    gtk_widget_add_controller(surface->area, GTK_EVENT_CONTROLLER(click));

    GtkGesture *right_click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(right_click), 3);
    g_signal_connect(right_click, "pressed", G_CALLBACK(bar_right_clicked),
                     surface);
    gtk_widget_add_controller(surface->area,
                              GTK_EVENT_CONTROLLER(right_click));

    g_ptr_array_add(bar->surfaces, surface);
    if (bar->area == NULL) {
        bar->window = window;
        bar->area = surface->area;
    }
    gtk_window_present(GTK_WINDOW(window));
}

static GtkWidget *create_menu_window(NintyBar *bar) {
    GtkWidget *window = gtk_application_window_new(bar->app);
    gtk_widget_add_css_class(window, "nintysh");
    gtk_window_set_title(GTK_WINDOW(window), "nintysh-start");
    gtk_window_set_decorated(GTK_WINDOW(window), FALSE);
    gtk_window_set_resizable(GTK_WINDOW(window), FALSE);

    GdkRectangle geometry = { 0 };
    primary_monitor_geometry(&geometry);
    gtk_window_set_default_size(GTK_WINDOW(window), geometry.width,
                                geometry.height);

    gtk_layer_init_for_window(GTK_WINDOW(window));
    gtk_layer_set_namespace(GTK_WINDOW(window), "nintysh-start");
    gtk_layer_set_layer(GTK_WINDOW(window), GTK_LAYER_SHELL_LAYER_OVERLAY);
    gtk_layer_set_anchor(GTK_WINDOW(window), GTK_LAYER_SHELL_EDGE_TOP, TRUE);
    gtk_layer_set_anchor(GTK_WINDOW(window), GTK_LAYER_SHELL_EDGE_BOTTOM,
                         TRUE);
    gtk_layer_set_anchor(GTK_WINDOW(window), GTK_LAYER_SHELL_EDGE_LEFT,
                         TRUE);
    gtk_layer_set_anchor(GTK_WINDOW(window), GTK_LAYER_SHELL_EDGE_RIGHT,
                         TRUE);
    gtk_layer_set_exclusive_zone(GTK_WINDOW(window), -1);
    gtk_layer_set_keyboard_mode(GTK_WINDOW(window),
                                GTK_LAYER_SHELL_KEYBOARD_MODE_EXCLUSIVE);

    bar->menu_area = gtk_drawing_area_new();
    gtk_widget_set_hexpand(bar->menu_area, TRUE);
    gtk_widget_set_vexpand(bar->menu_area, TRUE);
    gtk_widget_set_focusable(bar->menu_area, TRUE);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(bar->menu_area), menu_draw,
                                   bar, NULL);
    gtk_window_set_child(GTK_WINDOW(window), bar->menu_area);

    GtkGesture *click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), 1);
    g_signal_connect(click, "pressed", G_CALLBACK(menu_clicked), bar);
    gtk_widget_add_controller(bar->menu_area, GTK_EVENT_CONTROLLER(click));

    GtkEventController *motion = gtk_event_controller_motion_new();
    g_signal_connect(motion, "motion", G_CALLBACK(menu_motion), bar);
    gtk_widget_add_controller(bar->menu_area, motion);

    GtkEventController *scroll = gtk_event_controller_scroll_new(
        GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
    g_signal_connect(scroll, "scroll", G_CALLBACK(menu_scrolled), bar);
    gtk_widget_add_controller(bar->menu_area, scroll);

    GtkEventController *keys = gtk_event_controller_key_new();
    g_signal_connect(keys, "key-pressed", G_CALLBACK(menu_key_pressed), bar);
    gtk_widget_add_controller(window, keys);

    gtk_widget_set_visible(window, FALSE);
    return window;
}

static GtkWidget *create_panel_window(NintyBar *bar) {
    GtkWidget *window = gtk_application_window_new(bar->app);
    gtk_widget_add_css_class(window, "nintysh");
    gtk_window_set_title(GTK_WINDOW(window), "nintysh-panel");
    gtk_window_set_decorated(GTK_WINDOW(window), FALSE);
    gtk_window_set_resizable(GTK_WINDOW(window), FALSE);

    GdkRectangle geometry = { 0 };
    primary_monitor_geometry(&geometry);
    gtk_window_set_default_size(GTK_WINDOW(window), geometry.width,
                                geometry.height);

    gtk_layer_init_for_window(GTK_WINDOW(window));
    gtk_layer_set_namespace(GTK_WINDOW(window), "nintysh-panel");
    gtk_layer_set_layer(GTK_WINDOW(window), GTK_LAYER_SHELL_LAYER_OVERLAY);
    gtk_layer_set_anchor(GTK_WINDOW(window), GTK_LAYER_SHELL_EDGE_TOP, TRUE);
    gtk_layer_set_anchor(GTK_WINDOW(window), GTK_LAYER_SHELL_EDGE_BOTTOM,
                         TRUE);
    gtk_layer_set_anchor(GTK_WINDOW(window), GTK_LAYER_SHELL_EDGE_LEFT,
                         TRUE);
    gtk_layer_set_anchor(GTK_WINDOW(window), GTK_LAYER_SHELL_EDGE_RIGHT,
                         TRUE);
    gtk_layer_set_exclusive_zone(GTK_WINDOW(window), -1);
    gtk_layer_set_keyboard_mode(GTK_WINDOW(window),
                                GTK_LAYER_SHELL_KEYBOARD_MODE_EXCLUSIVE);

    bar->panel_area = gtk_drawing_area_new();
    gtk_widget_set_hexpand(bar->panel_area, TRUE);
    gtk_widget_set_vexpand(bar->panel_area, TRUE);
    gtk_widget_set_focusable(bar->panel_area, TRUE);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(bar->panel_area),
                                   panel_draw, bar, NULL);
    gtk_window_set_child(GTK_WINDOW(window), bar->panel_area);

    GtkGesture *click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), 1);
    g_signal_connect(click, "pressed", G_CALLBACK(panel_clicked), bar);
    gtk_widget_add_controller(bar->panel_area, GTK_EVENT_CONTROLLER(click));

    GtkEventController *motion = gtk_event_controller_motion_new();
    g_signal_connect(motion, "motion", G_CALLBACK(panel_motion), bar);
    gtk_widget_add_controller(bar->panel_area, motion);

    GtkGesture *drag = gtk_gesture_drag_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(drag), 1);
    g_signal_connect(drag, "drag-begin", G_CALLBACK(panel_drag_begin), bar);
    g_signal_connect(drag, "drag-update", G_CALLBACK(panel_drag_update), bar);
    g_signal_connect(drag, "drag-end", G_CALLBACK(panel_drag_end), bar);
    gtk_widget_add_controller(bar->panel_area, GTK_EVENT_CONTROLLER(drag));

    GtkEventController *scroll = gtk_event_controller_scroll_new(
        GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
    g_signal_connect(scroll, "scroll", G_CALLBACK(panel_scrolled), bar);
    gtk_widget_add_controller(bar->panel_area, scroll);

    GtkEventController *keys = gtk_event_controller_key_new();
    g_signal_connect(keys, "key-pressed", G_CALLBACK(panel_key_pressed), bar);
    gtk_widget_add_controller(window, keys);

    gtk_widget_set_visible(window, FALSE);
    return window;
}

static GtkWidget *create_osd_window(NintyBar *bar) {
    GtkWidget *window = gtk_application_window_new(bar->app);
    gtk_widget_add_css_class(window, "nintysh");
    gtk_window_set_title(GTK_WINDOW(window), "nintysh-osd");
    gtk_window_set_decorated(GTK_WINDOW(window), FALSE);
    gtk_window_set_resizable(GTK_WINDOW(window), FALSE);

    GdkRectangle geometry = { 0 };
    primary_monitor_geometry(&geometry);
    gtk_window_set_default_size(GTK_WINDOW(window), NINTYSH_OSD_WIDTH,
                                NINTYSH_OSD_HEIGHT);

    gtk_layer_init_for_window(GTK_WINDOW(window));
    gtk_layer_set_namespace(GTK_WINDOW(window), "nintysh-osd");
    gtk_layer_set_layer(GTK_WINDOW(window), GTK_LAYER_SHELL_LAYER_OVERLAY);
    gtk_layer_set_anchor(GTK_WINDOW(window), GTK_LAYER_SHELL_EDGE_BOTTOM,
                         TRUE);
    gtk_layer_set_anchor(GTK_WINDOW(window), GTK_LAYER_SHELL_EDGE_LEFT,
                         TRUE);
    gtk_layer_set_margin(GTK_WINDOW(window), GTK_LAYER_SHELL_EDGE_BOTTOM,
                         NINTYSH_OSD_MARGIN_BOTTOM);
    gtk_layer_set_margin(GTK_WINDOW(window), GTK_LAYER_SHELL_EDGE_LEFT,
                         MAX(2, (geometry.width - NINTYSH_OSD_WIDTH) / 2));
    gtk_layer_set_exclusive_zone(GTK_WINDOW(window), -1);
    gtk_layer_set_keyboard_mode(GTK_WINDOW(window),
                                GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);

    bar->osd_area = gtk_drawing_area_new();
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(bar->osd_area), osd_draw,
                                   bar, NULL);
    gtk_window_set_child(GTK_WINDOW(window), bar->osd_area);
    gtk_widget_set_visible(window, FALSE);
    return window;
}

void ninty_bar_command(NintyBar *bar, const char *command) {
    gchar **parts = g_strsplit(command, " ", 0);
    int count = g_strv_length(parts);

    if (count == 0) {
        g_strfreev(parts);
        return;
    }

    if (g_strcmp0(parts[0], "launcher") == 0 ||
        g_strcmp0(parts[0], "start") == 0) {
        open_menu(bar);
    } else if (g_strcmp0(parts[0], "close") == 0) {
        close_menu(bar);
        close_panel(bar);
    } else if (g_strcmp0(parts[0], "calendar") == 0 ||
               g_strcmp0(parts[0], "clock") == 0) {
        open_panel(bar, PANEL_CALENDAR);
    } else if (g_strcmp0(parts[0], "network") == 0) {
        open_panel(bar, PANEL_NETWORK);
    } else if (g_strcmp0(parts[0], "clipboard") == 0) {
        open_panel(bar, PANEL_CLIPBOARD);
    } else if (g_strcmp0(parts[0], "power") == 0) {
        open_panel(bar, PANEL_POWER);
    } else if (g_strcmp0(parts[0], "layout") == 0) {
        niri_client_next_layout(bar->niri);
    } else if (g_strcmp0(parts[0], "workspace") == 0 && count > 1) {
        niri_client_focus_workspace(bar->niri,
                                    g_ascii_strtoll(parts[1], NULL, 10));
    } else if (g_strcmp0(parts[0], "volume") == 0) {
        if (count == 1) {
            open_panel(bar, PANEL_VOLUME);
        } else if (g_strcmp0(parts[1], "up") == 0) {
            apply_volume(bar, bar->volume_percent + 5, TRUE);
        } else if (g_strcmp0(parts[1], "down") == 0) {
            apply_volume(bar, bar->volume_percent - 5, TRUE);
        } else if (g_strcmp0(parts[1], "mute") == 0) {
            toggle_volume(bar);
        } else {
            apply_volume(bar, atoi(parts[1]), TRUE);
        }
    } else if (g_strcmp0(parts[0], "osd") == 0) {
        const char *text = strchr(command, ' ');
        osd_show_text(bar, text != NULL ? text + 1 : "");
    } else if (g_strcmp0(parts[0], "run") == 0) {
        const char *text = strchr(command, ' ');
        run_shell_command(text != NULL ? text + 1 : "");
    }

    g_strfreev(parts);
}

static void control_command_cb(const char *command, gpointer data) {
    ninty_bar_command(data, command);
}

NintyBar *ninty_bar_new(GtkApplication *app) {
    NintyBar *bar = g_new0(NintyBar, 1);
    bar->app = app;
    bar->apps = g_ptr_array_new_with_free_func(g_object_unref);
    bar->menu_results = g_ptr_array_new();
    bar->menu_query = g_strdup("");
    bar->clipboard_history =
        g_ptr_array_new_with_free_func((GDestroyNotify)g_free);
    bar->surfaces = g_ptr_array_new_with_free_func(
        (GDestroyNotify)bar_surface_free);
    bar->icon_cache = g_hash_table_new_full(
        g_str_hash, g_str_equal, g_free, g_object_unref);
    bar->app_index = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                           NULL);
    bar->icon_theme = gtk_icon_theme_new();
    gtk_icon_theme_set_theme_name(bar->icon_theme, "hicolor");
    bar->menu_hover = -1;
    bar->panel_hover = -1;

    load_css();
    load_apps(bar);
    index_apps(bar);
    menu_update_results(bar);

    GListModel *monitors =
        gdk_display_get_monitors(gdk_display_get_default());
    guint monitor_count =
        monitors != NULL ? g_list_model_get_n_items(monitors) : 0;
    if (monitor_count == 0) {
        create_bar_surface(bar, NULL);
    } else {
        for (guint i = 0; i < monitor_count; i++) {
            GdkMonitor *monitor = g_list_model_get_item(monitors, i);
            create_bar_surface(bar, monitor);
            g_object_unref(monitor);
        }
    }

    bar->menu_window = create_menu_window(bar);
    bar->panel_window = create_panel_window(bar);
    bar->osd_window = create_osd_window(bar);
    bar->osd_value = -1;

    GDateTime *now = g_date_time_new_now_local();
    bar->calendar_year = g_date_time_get_year(now);
    bar->calendar_month = g_date_time_get_month(now);
    g_date_time_unref(now);

    bar->niri = niri_client_new(niri_changed_cb, bar);

    update_volume(bar);
    update_battery(bar);
    update_network(bar);

    GNetworkMonitor *monitor = g_network_monitor_get_default();
    bar->network_handler = g_signal_connect(
        monitor, "network-changed", G_CALLBACK(network_changed_cb), bar);

    bar->tick_source = g_timeout_add_seconds(NINTYSH_CLOCK_TICK_SECONDS, bar_tick, bar);
    bar->slow_source = g_timeout_add_seconds(NINTYSH_SYSTEM_POLL_SECONDS, bar_slow_tick, bar);
    bar->clipboard_source = g_timeout_add_seconds(NINTYSH_CLIPBOARD_POLL_SECONDS, clipboard_tick, bar);
    clipboard_tick(bar);
    bar->control = ninty_control_new(control_command_cb, bar);

    return bar;
}

void ninty_bar_free(NintyBar *bar) {
    if (bar == NULL) {
        return;
    }

    if (bar->tick_source != 0) {
        g_source_remove(bar->tick_source);
    }
    if (bar->slow_source != 0) {
        g_source_remove(bar->slow_source);
    }
    if (bar->clipboard_source != 0) {
        g_source_remove(bar->clipboard_source);
    }
    if (bar->osd_source != 0) {
        g_source_remove(bar->osd_source);
    }
    if (bar->network_handler != 0) {
        g_signal_handler_disconnect(g_network_monitor_get_default(),
                                    bar->network_handler);
    }
    if (bar->control != NULL) {
        ninty_control_free(bar->control);
    }
    if (bar->niri != NULL) {
        niri_client_free(bar->niri);
    }

    g_hash_table_destroy(bar->app_index);
    g_ptr_array_unref(bar->apps);
    g_ptr_array_unref(bar->menu_results);
    g_ptr_array_unref(bar->clipboard_history);
    g_free(bar->last_clipboard);
    g_free(bar->menu_query);
    g_ptr_array_unref(bar->surfaces);
    g_hash_table_destroy(bar->icon_cache);
    g_object_unref(bar->icon_theme);
    g_free(bar);
}
