#pragma once
/* cfg_frame.h -- internal to the Config destination: what scr_config.c (picker, routing) and cfg_frame.c (the
 * generated-editor frame, rows and cfg_save_*) share. The editors use scr_config.h only. (glue; LVGL task) */
#include "lvgl.h"
#ifdef __cplusplus
extern "C" {
#endif

#define CFG_SLOW_US  200000     /* the <= 200 ms callback budget */
/* tabs, index and picker switch on RELEASE and never repeat while held (a held tab would re-render the list ~10x/s);
 * the same CLICK_TRIG | NO_REPEAT the Task 19 segmented control uses */
#define CFG_BTNM_CTRL (LV_BUTTONMATRIX_CTRL_CHECKABLE | LV_BUTTONMATRIX_CTRL_CLICK_TRIG | LV_BUTTONMATRIX_CTRL_NO_REPEAT)

void cfg_frame_forget(void);   /* the editor is closing and its widgets are about to go: drop every frame pointer */

#ifdef __cplusplus
}
#endif
