#include "lvgl.h"
#include "pnl_palette.h"
#include "pnl_theme.h"

void pnl_theme_init(lv_display_t *disp) {
    lv_theme_t *th = lv_theme_default_init(disp, lv_color_hex(PNL_C_ACCENT), lv_color_hex(PNL_C_UPDATING),
                                           true, &lv_font_montserrat_20);
    if (th) lv_display_set_theme(disp, th);
    lv_obj_t *scr = lv_display_get_screen_active(disp);
    if (scr) {
        lv_obj_set_style_bg_color(scr, lv_color_hex(PNL_C_BG), 0);
        lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
        lv_obj_set_style_text_color(scr, lv_color_hex(PNL_C_TEXT), 0);
    }
}

lv_color_t pnl_health_color(node_health_t h) {
    switch (h) {
    case NODE_H_ONLINE:   return lv_color_hex(PNL_C_OK);
    case NODE_H_DEGRADED: return lv_color_hex(PNL_C_DEGRADED);
    case NODE_H_OFFLINE:  return lv_color_hex(PNL_C_OFFLINE);
    case NODE_H_UPDATING: return lv_color_hex(PNL_C_UPDATING);
    default:              return lv_color_hex(PNL_C_EMPTY);
    }
}

void pnl_theme_card(lv_obj_t *o) {
    lv_obj_set_style_bg_color(o, lv_color_hex(PNL_C_CARD), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(o, lv_color_hex(PNL_C_BORDER), 0);
    lv_obj_set_style_border_width(o, 1, 0);
    lv_obj_set_style_radius(o, 8, 0);
    lv_obj_set_style_pad_all(o, 12, 0);
}

lv_obj_t *pnl_label(lv_obj_t *parent, const char *text, const lv_font_t *font, uint32_t hex) {
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text ? text : "");
    if (font) lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(hex), 0);
    return l;
}
