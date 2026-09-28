#include <string.h>
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

/* ---------- the one confirm dialog ---------- */
typedef struct { lv_obj_t *mb; pnl_confirm_fn on_ok, on_cancel; void *ctx; } confirm_t;
static confirm_t s_cf;

static confirm_t cf_detach(void) {          /* the box is no longer "open" from here on: callbacks may reopen */
    confirm_t c = s_cf;
    memset(&s_cf, 0, sizeof s_cf);
    return c;
}
static void cf_ok(lv_event_t *e) {
    (void)e;
    confirm_t c = cf_detach();
    if (!c.mb) return;
    lv_msgbox_close_async(c.mb);            /* the event target lives inside the box: never delete synchronously */
    if (c.on_ok) c.on_ok(c.ctx);
}
static void cf_cancel(lv_event_t *e) {
    (void)e;
    confirm_t c = cf_detach();
    if (!c.mb) return;
    lv_msgbox_close_async(c.mb);
    if (c.on_cancel) c.on_cancel(c.ctx);
}
static void cf_deleted(lv_event_t *e) {     /* deleted by someone else: still exactly one callback */
    if (!s_cf.mb || lv_event_get_target(e) != s_cf.mb) return;
    confirm_t c = cf_detach();
    if (c.on_cancel) c.on_cancel(c.ctx);
}

void pnl_confirm(const char *title, const char *text, const char *ok_label, pnl_confirm_fn on_ok,
                 pnl_confirm_fn on_cancel, void *ctx) {
    pnl_confirm_close();
    lv_obj_t *mb = lv_msgbox_create(NULL);  /* NULL parent: modal on the top layer */
    if (!mb) {                              /* no LVGL memory: never leave the caller waiting for a callback */
        if (on_cancel) on_cancel(ctx);
        return;
    }
    lv_msgbox_add_title(mb, title ? title : "");
    lv_msgbox_add_text(mb, text ? text : "");
    lv_obj_t *b = lv_msgbox_add_footer_button(mb, "Cancel");
    if (b) lv_obj_add_event_cb(b, cf_cancel, LV_EVENT_CLICKED, NULL);
    b = lv_msgbox_add_footer_button(mb, ok_label ? ok_label : "OK");
    if (b) lv_obj_add_event_cb(b, cf_ok, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(mb, cf_deleted, LV_EVENT_DELETE, NULL);
    s_cf = (confirm_t){ .mb = mb, .on_ok = on_ok, .on_cancel = on_cancel, .ctx = ctx };
}

void pnl_confirm_close(void) {
    confirm_t c = cf_detach();
    if (!c.mb) return;
    lv_msgbox_close(c.mb);                  /* cf_deleted sees s_cf cleared and stays quiet */
    if (c.on_cancel) c.on_cancel(c.ctx);
}

int pnl_confirm_is_open(void) { return s_cf.mb != NULL; }
