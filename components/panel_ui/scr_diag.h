#pragma once
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The panel diagnostics screen: firmware version, internal heap free/min, five
 * touch targets (the four corners and the centre) and live touch counters.
 * The stage gates' touch test: the target under the finger must light, never
 * the opposite one. [LVGL] */
void scr_diag_build(lv_obj_t *parent);

#ifdef __cplusplus
}
#endif
