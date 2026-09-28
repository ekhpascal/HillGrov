#pragma once
/* Glue: the two sections Task 22 adds below the zone view's shelf table (scr_zone_extra_area(), Task 14). [LVGL] */
#include <stdint.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

void zone_console_build(lv_obj_t *parent, uint8_t zone);   /* per-zone console allocated lazily (PSRAM); the send job's arg
                                                               carries a pointer to the entry, whose sent line and reply
                                                               buffer belong to the worker until done() */
void zone_console_teardown(void);
void zone_console_wipe_all(void);                           /* Task 27: every zone's transcript, history and draft */
void zone_replace_build(lv_obj_t *parent, uint8_t zone);   /* MAC via HEX keyboard; bad -> "Enter a MAC like aa:bb:cc:dd:ee:ff";
                                                               job pnl_cmd_run("SET NODE <z> MAC <mac>") -> reply verbatim */
void zone_replace_teardown(void);
void zone_replace_wipe(void);                               /* Task 27: every zone's MAC draft and the shown reply */

#ifdef __cplusplus
}
#endif
