#pragma once
#include <stdint.h>
#include "lvgl.h"
#include "ring_proto.h"   /* node_health_t */

#ifdef __cplusplus
extern "C" {
#endif

/* LVGL-side styling from pnl_palette.h. [LVGL] -- call with the display lock
 * held or from the LVGL task. */
void       pnl_theme_init(lv_display_t *disp);   /* lv_theme_default_init(dark, accent PNL_C_ACCENT, Montserrat 20) */
lv_color_t pnl_health_color(node_health_t h);    /* ONLINE ok / DEGRADED / OFFLINE / UPDATING / EMPTY */
void       pnl_theme_card(lv_obj_t *o);          /* the web's card: PNL_C_CARD bg, 1 px PNL_C_BORDER, radius 8, pad 12 */
lv_obj_t  *pnl_label(lv_obj_t *parent, const char *text, const lv_font_t *font, uint32_t hex);  /* one-call label */

/* The ONE confirm dialog (controller ruling C16): every panel confirm uses it, so the idle wipe's
 * pnl_confirm_close() reaches them all. [LVGL] A modal msgbox on the top layer, [Cancel] + [ok_label]; a new
 * confirm dismisses an open one first. Exactly one of on_ok / on_cancel runs (either may be NULL), after the box is
 * detached, so a callback may open another confirm: on_ok on the OK tap; on_cancel on the Cancel tap and whenever
 * the box goes away otherwise (pnl_confirm_close, a new confirm, the box deleted from outside).
 * on_cancel may run synchronously inside pnl_confirm(), on allocation failure or when an open box is replaced, so
 * callers must set their state flags BEFORE calling it. */
typedef void (*pnl_confirm_fn)(void *ctx);
void pnl_confirm(const char *title, const char *text, const char *ok_label, pnl_confirm_fn on_ok,
                 pnl_confirm_fn on_cancel, void *ctx);
void pnl_confirm_close(void);     /* [LVGL] teardown / idle wipe: dismiss an open box (its on_cancel runs) */
int  pnl_confirm_is_open(void);   /* [LVGL] */

#ifdef __cplusplus
}
#endif
