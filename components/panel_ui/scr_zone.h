#pragma once
#include <stdint.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The zone view's hooks for Task 22 (console + Replace board). [LVGL] */
lv_obj_t *scr_zone_extra_area(void);   /* the container below the shelf table; NULL when no zone is shown */
uint8_t   scr_zone_current(void);      /* the zone id on screen, 0 when none */

#ifdef __cplusplus
}
#endif
