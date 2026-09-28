#include "pnl_touch.h"

void pnl_touch_map(uint8_t orient, uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t *ox, uint16_t *oy) {
    if (w == 0 || h == 0) { *ox = 0; *oy = 0; return; }
    if (orient == PNL_ORIENT_FLIPPED) {
        *ox = (x >= w) ? 0 : (uint16_t)(w - 1u - x);
        *oy = (y >= h) ? 0 : (uint16_t)(h - 1u - y);
    } else {
        *ox = (x >= w) ? (uint16_t)(w - 1u) : x;
        *oy = (y >= h) ? (uint16_t)(h - 1u) : y;
    }
}
