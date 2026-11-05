#include "win98.h"

#include "config.h"

#include <math.h>
#include <pango/pangocairo.h>

#define RGB(r, g, b) { (r) / 255.0, (g) / 255.0, (b) / 255.0 }

const Win98Color win98_face = NINTYSH_RGB(NINTYSH_COLOR_FACE);
const Win98Color win98_highlight = NINTYSH_RGB(NINTYSH_COLOR_HIGHLIGHT);
const Win98Color win98_light = NINTYSH_RGB(NINTYSH_COLOR_LIGHT);
const Win98Color win98_shadow = NINTYSH_RGB(NINTYSH_COLOR_SHADOW);
const Win98Color win98_dark = NINTYSH_RGB(NINTYSH_COLOR_DARK);
const Win98Color win98_text = NINTYSH_RGB(NINTYSH_COLOR_TEXT);
const Win98Color win98_text_disabled =
    NINTYSH_RGB(NINTYSH_COLOR_TEXT_DISABLED);
const Win98Color win98_navy = NINTYSH_RGB(NINTYSH_COLOR_NAVY);
const Win98Color win98_white = NINTYSH_RGB(NINTYSH_COLOR_WHITE);
const Win98Color win98_black = NINTYSH_RGB(NINTYSH_COLOR_BLACK);
const Win98Color win98_teal = NINTYSH_RGB(NINTYSH_COLOR_TEAL);

static const Win98Color win98_yellow = RGB(255, 255, 0);
static const Win98Color win98_green = RGB(0, 128, 0);
static const Win98Color win98_red = RGB(255, 0, 0);
static const Win98Color win98_blue = RGB(0, 0, 255);

void win98_fill(cairo_t *cr, double x, double y, double w, double h,
                Win98Color color) {
    if (w <= 0 || h <= 0) {
        return;
    }
    cairo_set_source_rgb(cr, color.r, color.g, color.b);
    cairo_rectangle(cr, x, y, w, h);
    cairo_fill(cr);
}

void win98_bevel(cairo_t *cr, double x, double y, double w, double h,
                 Win98Bevel bevel) {
    if (w < 2 || h < 2) {
        return;
    }

    Win98Color tl1 = bevel == WIN98_BEVEL_RAISED ? win98_highlight : win98_shadow;
    Win98Color tl2 = bevel == WIN98_BEVEL_RAISED ? win98_light : win98_dark;
    Win98Color br1 = bevel == WIN98_BEVEL_RAISED ? win98_dark : win98_highlight;
    Win98Color br2 = bevel == WIN98_BEVEL_RAISED ? win98_shadow : win98_light;

    win98_fill(cr, x, y, w, 1, tl1);
    win98_fill(cr, x, y, 1, h, tl1);
    win98_fill(cr, x + 1, y + 1, w - 2, 1, tl2);
    win98_fill(cr, x + 1, y + 1, 1, h - 2, tl2);

    win98_fill(cr, x, y + h - 1, w, 1, br1);
    win98_fill(cr, x + w - 1, y, 1, h, br1);
    win98_fill(cr, x + 1, y + h - 2, w - 2, 1, br2);
    win98_fill(cr, x + w - 2, y + 1, 1, h - 2, br2);
}

static PangoLayout *create_layout(cairo_t *cr, const char *text,
                                  gboolean bold) {
    PangoLayout *layout = pango_cairo_create_layout(cr);
    PangoFontDescription *desc = pango_font_description_new();

    pango_font_description_set_family(desc, NINTYSH_FONT_FAMILY);
    pango_font_description_set_absolute_size(desc,
                                              NINTYSH_FONT_PX * PANGO_SCALE);
    pango_font_description_set_weight(
        desc, bold ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL);
    pango_layout_set_font_description(layout, desc);
    pango_layout_set_text(layout, text != NULL ? text : "", -1);
    pango_layout_set_single_paragraph_mode(layout, TRUE);

    pango_font_description_free(desc);
    return layout;
}

static void set_pixel_font_options(cairo_t *cr) {
    cairo_font_options_t *options = cairo_font_options_create();
    cairo_font_options_set_antialias(options, CAIRO_ANTIALIAS_NONE);
    cairo_font_options_set_hint_style(options, CAIRO_HINT_STYLE_FULL);
    cairo_font_options_set_hint_metrics(options, CAIRO_HINT_METRICS_ON);
    cairo_set_font_options(cr, options);
    cairo_font_options_destroy(options);
}

void win98_draw_text(cairo_t *cr, double x, double y, double w, double h,
                const char *text, Win98Align align, gboolean bold,
                Win98Color color, gboolean ellipsize) {
    if (text == NULL || text[0] == '\0') {
        return;
    }

    cairo_save(cr);
    set_pixel_font_options(cr);
    cairo_set_source_rgb(cr, color.r, color.g, color.b);

    PangoLayout *layout = create_layout(cr, text, bold);
    int text_w = 0;
    int text_h = 0;

    if (ellipsize) {
        pango_layout_set_width(layout, (int)(w * PANGO_SCALE));
        pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
        if (align == WIN98_ALIGN_CENTER) {
            pango_layout_set_alignment(layout, PANGO_ALIGN_CENTER);
        } else if (align == WIN98_ALIGN_RIGHT) {
            pango_layout_set_alignment(layout, PANGO_ALIGN_RIGHT);
        }
    }

    pango_layout_get_pixel_size(layout, &text_w, &text_h);

    double tx = x;
    if (!ellipsize) {
        if (align == WIN98_ALIGN_CENTER) {
            tx = x + (w - text_w) / 2.0;
        } else if (align == WIN98_ALIGN_RIGHT) {
            tx = x + w - text_w;
        }
    }

    double ty = y + (h - text_h) / 2.0;
    cairo_move_to(cr, round(tx), round(ty));
    pango_cairo_show_layout(cr, layout);

    g_object_unref(layout);
    cairo_restore(cr);
}

int win98_text_width(const char *text, gboolean bold) {
    if (text == NULL || text[0] == '\0') {
        return 0;
    }

    cairo_surface_t *surface =
        cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
    cairo_t *cr = cairo_create(surface);
    PangoLayout *layout = create_layout(cr, text, bold);
    int width = 0;
    int height = 0;

    pango_layout_get_pixel_size(layout, &width, &height);

    g_object_unref(layout);
    cairo_destroy(cr);
    cairo_surface_destroy(surface);
    return width;
}

void win98_focus_rect(cairo_t *cr, double x, double y, double w, double h) {
    cairo_save(cr);
    cairo_set_source_rgb(cr, win98_black.r, win98_black.g, win98_black.b);

    for (int i = 2; i < (int)w - 2; i += 2) {
        cairo_rectangle(cr, x + i, y, 1, 1);
        cairo_rectangle(cr, x + i, y + h - 1, 1, 1);
    }
    for (int i = 2; i < (int)h - 2; i += 2) {
        cairo_rectangle(cr, x, y + i, 1, 1);
        cairo_rectangle(cr, x + w - 1, y + i, 1, 1);
    }

    cairo_fill(cr);
    cairo_restore(cr);
}

static void polygon(cairo_t *cr, const double *points, int count) {
    cairo_move_to(cr, points[0], points[1]);
    for (int i = 2; i < count; i += 2) {
        cairo_line_to(cr, points[i], points[i + 1]);
    }
    cairo_close_path(cr);
}

static void flag_pane(cairo_t *cr, Win98Color color, const double *points) {
    polygon(cr, points, 8);
    cairo_set_source_rgb(cr, color.r, color.g, color.b);
    cairo_fill_preserve(cr);
    cairo_set_source_rgb(cr, win98_black.r, win98_black.g, win98_black.b);
    cairo_set_line_width(cr, 1.0);
    cairo_stroke(cr);
}

void win98_draw_start_flag(cairo_t *cr, double x, double y) {
    cairo_save(cr);
    cairo_translate(cr, x, y);

    const double red[] = { 1, 2, 7, 0, 7, 5, 1, 6 };
    const double green[] = { 8, 0, 15, 1, 15, 5, 8, 5 };
    const double blue[] = { 1, 7, 7, 6, 7, 11, 1, 13 };
    const double yellow[] = { 8, 6, 15, 5, 15, 10, 8, 12 };

    flag_pane(cr, win98_red, red);
    flag_pane(cr, win98_green, green);
    flag_pane(cr, win98_blue, blue);
    flag_pane(cr, win98_yellow, yellow);

    cairo_restore(cr);
}

void win98_draw_app_icon(cairo_t *cr, double x, double y) {
    cairo_save(cr);
    cairo_translate(cr, x, y);

    win98_fill(cr, 1, 1, 14, 14, win98_face);
    win98_bevel(cr, 1, 1, 14, 14, WIN98_BEVEL_RAISED);
    win98_fill(cr, 2, 3, 12, 3, win98_navy);
    win98_fill(cr, 3, 4, 5, 1, win98_white);
    win98_fill(cr, 2, 6, 12, 8, win98_white);
    win98_fill(cr, 2, 6, 12, 1, win98_shadow);
    win98_fill(cr, 2, 13, 12, 1, win98_shadow);

    cairo_restore(cr);
}

void win98_draw_volume_icon(cairo_t *cr, double x, double y, gboolean muted) {
    cairo_save(cr);
    cairo_translate(cr, x, y);

    const double speaker[] = { 2, 6, 5, 6, 8, 3, 8, 12, 5, 9, 2, 9 };
    polygon(cr, speaker, 12);
    cairo_set_source_rgb(cr, win98_yellow.r, win98_yellow.g, win98_yellow.b);
    cairo_fill_preserve(cr);
    cairo_set_source_rgb(cr, win98_black.r, win98_black.g, win98_black.b);
    cairo_set_line_width(cr, 1.0);
    cairo_stroke(cr);

    win98_fill(cr, 3, 7, 2, 1, win98_white);

    if (muted) {
        cairo_set_source_rgb(cr, win98_red.r, win98_red.g, win98_red.b);
        cairo_set_line_width(cr, 2.0);
        cairo_move_to(cr, 10, 4);
        cairo_line_to(cr, 15, 10);
        cairo_move_to(cr, 15, 4);
        cairo_line_to(cr, 10, 10);
        cairo_stroke(cr);
    } else {
        cairo_set_source_rgb(cr, win98_black.r, win98_black.g, win98_black.b);
        cairo_set_line_width(cr, 1.0);
        cairo_arc(cr, 8, 7.5, 4.0, -0.8, 0.8);
        cairo_stroke(cr);
        cairo_arc(cr, 8, 7.5, 6.0, -0.7, 0.7);
        cairo_stroke(cr);
    }

    cairo_restore(cr);
}

static void draw_monitor(cairo_t *cr, double x, double y, double w,
                         double h) {
    win98_fill(cr, x, y, w, h, win98_face);
    win98_bevel(cr, x, y, w, h, WIN98_BEVEL_RAISED);
    win98_fill(cr, x + 2, y + 2, w - 4, h - 5, win98_blue);
    win98_fill(cr, x + 3, y + 3, w - 6, 1, win98_light);
    win98_fill(cr, x + w / 2 - 2, y + h - 3, 4, 2, win98_shadow);
}

void win98_draw_network_icon(cairo_t *cr, double x, double y,
                             gboolean connected) {
    cairo_save(cr);
    cairo_translate(cr, x, y);

    draw_monitor(cr, 6, 1, 9, 7);
    draw_monitor(cr, 1, 6, 11, 9);

    if (!connected) {
        cairo_set_source_rgb(cr, win98_red.r, win98_red.g, win98_red.b);
        cairo_set_line_width(cr, 2.0);
        cairo_move_to(cr, 2, 2);
        cairo_line_to(cr, 14, 14);
        cairo_move_to(cr, 14, 2);
        cairo_line_to(cr, 2, 14);
        cairo_stroke(cr);
    }

    cairo_restore(cr);
}

void win98_draw_battery_icon(cairo_t *cr, double x, double y, int percent,
                             gboolean charging) {
    cairo_save(cr);
    cairo_translate(cr, x, y);

    win98_fill(cr, 1, 4, 12, 9, win98_face);
    win98_bevel(cr, 1, 4, 12, 9, WIN98_BEVEL_SUNKEN);
    win98_fill(cr, 13, 6, 2, 5, win98_face);
    win98_bevel(cr, 13, 6, 2, 5, WIN98_BEVEL_RAISED);

    int fill_width = (int)((percent * 8) / 100);
    if (fill_width < 1) {
        fill_width = 1;
    }
    Win98Color fill = percent <= 20 ? win98_red : win98_green;
    win98_fill(cr, 3, 6, fill_width, 5, fill);

    if (charging) {
        const double bolt[] = { 8, 2, 5, 8, 8, 8, 6, 13, 12, 7, 9, 7 };
        polygon(cr, bolt, 12);
        cairo_set_source_rgb(cr, win98_yellow.r, win98_yellow.g,
                             win98_yellow.b);
        cairo_fill_preserve(cr);
        cairo_set_source_rgb(cr, win98_black.r, win98_black.g, win98_black.b);
        cairo_set_line_width(cr, 1.0);
        cairo_stroke(cr);
    }

    cairo_restore(cr);
}


void win98_draw_clipboard_icon(cairo_t *cr, double x, double y) {
    cairo_save(cr);
    cairo_translate(cr, x, y);

    win98_fill(cr, 1, 3, 14, 12, win98_face);
    win98_bevel(cr, 1, 3, 14, 12, WIN98_BEVEL_RAISED);
    win98_fill(cr, 3, 5, 10, 8, win98_white);
    win98_fill(cr, 3, 5, 10, 1, win98_shadow);

    win98_fill(cr, 5, 1, 6, 4, win98_light);
    win98_bevel(cr, 5, 1, 6, 4, WIN98_BEVEL_RAISED);
    win98_fill(cr, 7, 2, 2, 2, win98_shadow);

    win98_fill(cr, 5, 7, 6, 1, win98_shadow);
    win98_fill(cr, 5, 9, 6, 1, win98_shadow);
    win98_fill(cr, 5, 11, 4, 1, win98_shadow);

    cairo_restore(cr);
}
