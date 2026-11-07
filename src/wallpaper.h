#ifndef NINTYSH_WALLPAPER_H
#define NINTYSH_WALLPAPER_H

#include <gtk/gtk.h>

typedef struct WallpaperManager WallpaperManager;

WallpaperManager *wallpaper_manager_new(GtkApplication *app);
void wallpaper_manager_free(WallpaperManager *manager);

gboolean wallpaper_manager_set(WallpaperManager *manager, const char *path);
const char *wallpaper_manager_current(WallpaperManager *manager);

#endif
