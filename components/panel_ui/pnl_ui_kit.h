#pragma once
/* Glue: the handful of widgets every System and Panel section is built from, so each section file holds behaviour, not
 * styling. [LVGL] only: call from the LVGL task (event/timer callbacks) or under panel_lock().
 * The confirm dialog is NOT here: the one confirm (ruling C16) is pnl_confirm() / pnl_confirm_close() in pnl_theme.h,
 * included below so a section that includes the kit reaches it too. */
#include "lvgl.h"
#include "pnl_theme.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { PNL_KIT_OK = 0, PNL_KIT_ERR, PNL_KIT_INFO } pnl_kit_tone_t;
lv_obj_t *pnl_kit_card(lv_obj_t *parent, const char *title);            /* column card, title in Montserrat 28 */
lv_obj_t *pnl_kit_row(lv_obj_t *parent);                                /* transparent wrapping row */
lv_obj_t *pnl_kit_button(lv_obj_t *parent, const char *text, lv_event_cb_t cb, void *ud);   /* 56 px tall */
void      pnl_kit_enable(lv_obj_t *obj, int on);                        /* LV_STATE_DISABLED off/on; NULL-safe */
lv_obj_t *pnl_kit_field(lv_obj_t *parent, const char *caption, lv_event_cb_t cb, void *ud);  /* returns the value label */
lv_obj_t *pnl_kit_msg(lv_obj_t *parent);                                /* wrapping status label */
void      pnl_kit_msg_set(lv_obj_t *lbl, const char *text, pnl_kit_tone_t tone);           /* NULL-safe */

#ifdef __cplusplus
}
#endif
