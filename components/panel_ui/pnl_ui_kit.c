#include "lvgl.h"
#include "pnl_palette.h"
#include "pnl_theme.h"
#include "pnl_ui_kit.h"

lv_obj_t *pnl_kit_card(lv_obj_t *parent, const char *title) {
    lv_obj_t *c = lv_obj_create(parent);
    pnl_theme_card(c);                          /* the web's card, as the dashboard draws it */
    lv_obj_set_width(c, LV_PCT(100));
    lv_obj_set_height(c, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(c, 8, 0);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    if (title) {
        lv_obj_t *t = lv_label_create(c);
        lv_label_set_text(t, title);
        lv_obj_set_style_text_font(t, &lv_font_montserrat_28, 0);
    }
    return c;
}

lv_obj_t *pnl_kit_row(lv_obj_t *parent) {
    lv_obj_t *r = lv_obj_create(parent);
    lv_obj_set_width(r, LV_PCT(100));
    lv_obj_set_height(r, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_all(r, 0, 0);
    lv_obj_set_style_pad_column(r, 12, 0);
    lv_obj_set_style_pad_row(r, 8, 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(r, 0, 0);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    return r;
}

lv_obj_t *pnl_kit_button(lv_obj_t *parent, const char *text, lv_event_cb_t cb, void *ud) {
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_height(b, 56);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text ? text : "");
    lv_obj_center(l);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    return b;
}

void pnl_kit_enable(lv_obj_t *obj, int on) {
    if (!obj) return;
    if (on) lv_obj_remove_state(obj, LV_STATE_DISABLED);
    else    lv_obj_add_state(obj, LV_STATE_DISABLED);
}

lv_obj_t *pnl_kit_field(lv_obj_t *parent, const char *caption, lv_event_cb_t cb, void *ud) {
    lv_obj_t *r = pnl_kit_row(parent);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *cap = lv_label_create(r);
    lv_label_set_text(cap, caption ? caption : "");
    lv_obj_set_width(cap, 220);
    lv_obj_set_style_text_color(cap, lv_color_hex(PNL_C_MUTED), 0);
    lv_obj_t *b = lv_button_create(r);
    lv_obj_set_height(b, 56);
    lv_obj_set_flex_grow(b, 1);
    lv_obj_set_style_bg_color(b, lv_color_hex(PNL_C_BG), 0);
    lv_obj_set_style_border_color(b, lv_color_hex(PNL_C_BORDER), 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_t *v = lv_label_create(b);
    lv_label_set_text(v, "");
    lv_obj_align(v, LV_ALIGN_LEFT_MID, 0, 0);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    return v;
}

lv_obj_t *pnl_kit_msg(lv_obj_t *parent) {
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_width(l, LV_PCT(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    lv_label_set_text(l, "");
    return l;
}

void pnl_kit_msg_set(lv_obj_t *lbl, const char *text, pnl_kit_tone_t tone) {
    if (!lbl) return;
    lv_label_set_text(lbl, text ? text : "");
    uint32_t c = tone == PNL_KIT_ERR ? PNL_C_OFFLINE_TEXT : tone == PNL_KIT_OK ? PNL_C_OK_TEXT : PNL_C_MUTED;
    lv_obj_set_style_text_color(lbl, lv_color_hex(c), 0);
}
