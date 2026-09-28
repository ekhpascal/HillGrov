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

#ifdef __cplusplus
}
#endif
