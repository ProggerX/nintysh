#ifndef NINTYSH_WIN98_H
#define NINTYSH_WIN98_H

#include <cairo/cairo.h>
#include <glib.h>

typedef struct {
    double r;
    double g;
    double b;
} Win98Color;

typedef enum {
    WIN98_ALIGN_LEFT,
    WIN98_ALIGN_CENTER,
    WIN98_ALIGN_RIGHT,
} Win98Align;

typedef enum {
    WIN98_BEVEL_RAISED,
    WIN98_BEVEL_SUNKEN,
} Win98Bevel;

extern const Win98Color win98_face;
extern const Win98Color win98_highlight;
extern const Win98Color win98_light;
extern const Win98Color win98_shadow;
extern const Win98Color win98_dark;
extern const Win98Color win98_text;
extern const Win98Color win98_text_disabled;
extern const Win98Color win98_navy;
extern const Win98Color win98_white;
extern const Win98Color win98_black;
extern const Win98Color win98_teal;

void win98_fill(cairo_t *cr, double x, double y, double w, double h,
                Win98Color color);
void win98_bevel(cairo_t *cr, double x, double y, double w, double h,
                 Win98Bevel bevel);
void win98_draw_text(cairo_t *cr, double x, double y, double w, double h,
                const char *text, Win98Align align, gboolean bold,
                Win98Color color, gboolean ellipsize);
int win98_text_width(const char *text, gboolean bold);
void win98_focus_rect(cairo_t *cr, double x, double y, double w, double h);
void win98_draw_start_flag(cairo_t *cr, double x, double y);
void win98_draw_app_icon(cairo_t *cr, double x, double y);
void win98_draw_volume_icon(cairo_t *cr, double x, double y, gboolean muted);
void win98_draw_network_icon(cairo_t *cr, double x, double y,
                             gboolean connected);
void win98_draw_battery_icon(cairo_t *cr, double x, double y, int percent,
                             gboolean charging);
void win98_draw_clipboard_icon(cairo_t *cr, double x, double y);
void win98_draw_notification_icon(cairo_t *cr, double x, double y,
                                  gboolean active);
void win98_draw_media_icon(cairo_t *cr, double x, double y,
                           gboolean playing);
void win98_draw_session_icon(cairo_t *cr, double x, double y);
void win98_draw_display_icon(cairo_t *cr, double x, double y);

#endif
