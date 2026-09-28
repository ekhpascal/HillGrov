#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Touch orientation (pure). Rotation and touch mirroring are two separate
 * knobs (what_we_learned 2026-09-18); this owns the touch half. Never enable
 * the esp_lcd_touch mirrors instead: they compute x_max - x on a uint16_t with
 * no clamp. */
#define PNL_ORIENT_NORMAL  0   /* display ESP_LV_ADAPTER_ROTATE_180 + touch passthrough: the bench-proven mapping */
#define PNL_ORIENT_FLIPPED 1   /* display ESP_LV_ADAPTER_ROTATE_0 + a 180-degree touch flip done here, clamped */

/* NORMAL (and any unknown value): clamp to [0,w-1] x [0,h-1].
 * FLIPPED: ox = x >= w ? 0 : w-1-x, oy = y >= h ? 0 : h-1-y. Never wraps.
 * w or h of 0 -> (0,0). */
void pnl_touch_map(uint8_t orient, uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t *ox, uint16_t *oy);

#ifdef __cplusplus
}
#endif
