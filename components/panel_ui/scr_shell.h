#pragma once
#include <stdint.h>
#include "lvgl.h"
#include "pnl_poll.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The panel shell: home + the HillGrow app's persistent left rail (spec "The
 * HillGrow app": five destinations, matching the web) + the panel-local
 * screens. Only the open destination's widget tree exists; it is built into a
 * fresh page on navigation and deleted on the next. Everything here is [LVGL]. */

typedef enum { PNL_DEST_HOME = 0, PNL_DEST_DASHBOARD, PNL_DEST_ZONE, PNL_DEST_CONFIG, PNL_DEST_ALARMS, PNL_DEST_SYSTEM,
               PNL_DEST_PANEL, PNL_DEST_AUDIO, PNL_DEST_DIAG, PNL_DEST_COUNT } pnl_dest_t;

typedef struct {
    const char *title;
    void (*build)(lv_obj_t *content, int arg);   /* LVGL task; widgets under content only */
    void (*update)(const pnl_snap_t *snap);      /* LVGL task; on every new poll seq; NULL allowed */
    void (*teardown)(void);                      /* LVGL task; before content is deleted: drop widget pointers */
    uint8_t in_rail;                             /* Dashboard, Zone, Config, Alarms, System */
} pnl_screen_ops_t;

extern const pnl_screen_ops_t PNL_SCR_HOME, PNL_SCR_DASHBOARD, PNL_SCR_ZONE, PNL_SCR_CONFIG, PNL_SCR_ALARMS,
                              PNL_SCR_SYSTEM, PNL_SCR_PANEL, PNL_SCR_AUDIO, PNL_SCR_DIAG, PNL_SCR_PLACEHOLDER;

/* Root screen, the 120 px rail and the content area; a 250 ms timer calls the
 * open screen's update() whenever pnl_poll_seq() moved; opens HOME. Call once,
 * under panel_lock(). */
void       pnl_shell_start(void);

/* teardown -> pnl_screen_gen_bump -> build, DEFERRED to the next LVGL cycle
 * (lv_async_call), so a tap handler may call it safely. arg: the zone id for
 * ZONE/CONFIG (0 = master config, -1 = the last one used there), else 0. */
void       pnl_nav_go(pnl_dest_t d, int arg);
pnl_dest_t pnl_nav_current(void);
int        pnl_nav_arg(void);
const char *pnl_nav_title(void);              /* the current destination's registry title */

/* The shell's ONE LVGL-side copy of the last poll (started == 0 until the
 * first publish). It lives in PSRAM, never as a static in internal RAM
 * (controller ruling C11): screens read this copy, they never keep their own.
 * NULL only while PSRAM cannot supply it (the shell retries on every
 * navigation and tick); screens treat NULL exactly like started == 0. */
const pnl_snap_t *pnl_shell_snap(void);

void       pnl_label_set_if_changed(lv_obj_t *label, const char *text);   /* the "update only what changed" primitive */
void       pnl_obj_show(lv_obj_t *o, int show);                           /* toggles LV_OBJ_FLAG_HIDDEN only on change */

#ifdef __cplusplus
}
#endif
