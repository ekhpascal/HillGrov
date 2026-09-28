#pragma once
#include "lvgl.h"

/* The home clock's face: a Montserrat-Medium subset (space, '-', '0'-'9', ':')
 * pre-rendered at 180 px, 4 bpp, by lv_font_conv 1.5.2 into font_clock_180.c
 * (D2 route a; the generated file records the exact command). Built-in
 * Montserrat stops at 48 px and bitmap fonts do not scale cleanly. */
LV_FONT_DECLARE(font_clock_180);

/* The one accessor screens use, so the font route can change in one place. */
static inline const lv_font_t *pnl_font_clock(void) { return &font_clock_180; }
