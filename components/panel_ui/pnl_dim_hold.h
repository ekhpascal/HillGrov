#pragma once
/* Glue ([LVGL] only): the brightness-preview hold (controller ruling C21).
 *
 * A brightness slider on the Panel screen writes the backlight directly while it is dragged, so the operator sees the
 * level being chosen -- including the night level, in daytime. Anything that sets the backlight on its own (Task 27's
 * 1 s dimming timer, pnl_idle.c) would overwrite that preview within a second. The hold says "hands off":
 *
 *   - The previewing code calls pnl_dim_hold(PNL_DIM_PREVIEW_HOLD_MS) on every change it previews, and
 *     pnl_dim_hold(0) when the preview ends (the slider is released). The hold is timed, not a flag, so a release that
 *     never arrives (a lost press, a screen torn down mid-drag) cannot pin the backlight: it lapses by itself.
 *   - An automatic backlight owner checks pnl_dim_held(lv_tick_get()) before calling panel_hw_brightness(). While it
 *     returns 1 it must not touch the backlight. The preview changed the duty behind its back, so any "last applied"
 *     level it caches is stale: it must forget it while held (e.g. set its cache to "none") and re-apply its computed
 *     level on the first tick after the hold ends, even when that level did not change.
 *
 * Both calls use the LVGL tick (lv_tick_get()), the clock lv_display_get_inactive_time() also runs on. */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PNL_DIM_PREVIEW_HOLD_MS 3000u   /* a drag pauses longer than a second between changes; 3 s covers it */

void pnl_dim_hold(uint32_t ms);          /* hold until lv_tick_get() + ms, replacing any earlier hold; 0 = release now */
int  pnl_dim_held(uint32_t now_ms);      /* 1 while now_ms (lv_tick_get()) is before the deadline (wrap-safe), else 0 */

#ifdef __cplusplus
}
#endif
