#pragma once
/* Glue: the System destination -- the web's System page (web/app.js:913-1020) as sections on one screen. */
#include "lvgl.h"
#include "pnl_poll.h"
#include "scr_shell.h"

typedef struct { const char *title; void (*build)(lv_obj_t *parent); void (*update)(const pnl_snap_t *s);
                 void (*teardown)(void); } pnl_sys_section_t;
extern const pnl_sys_section_t PNL_SYS_WIFI, PNL_SYS_TIME, PNL_SYS_PASSWORD, PNL_SYS_FLEET,
                               PNL_SYS_FIRMWARE;   /* registry in scr_system.c */
enum { PNL_SYS_SEC_WIFI = 0, PNL_SYS_SEC_TIME, PNL_SYS_SEC_PASSWORD, PNL_SYS_SEC_FLEET, PNL_SYS_SEC_FIRMWARE,
       PNL_SYS_SEC_COUNT };   /* pnl_nav_go(PNL_DEST_SYSTEM, sec); outside the range = last shown */
void sys_wifi_wipe(void);   /* Wi-Fi form text (Task 27) */
void sys_password_wipe(void);   /* the unsubmitted new password and the success box (Task 27) */
void sys_fleet_wipe(void);      /* the kept fleet outcome and a failed-reboot overlay (Task 27) */
void sys_reboot_confirm(const char *why);   /* [LVGL] the ONE reboot flow (sys_fleet.c): confirm; a second confirm while
                                               st.fw_state is "PENDING" (D16); "Rebooting..."; job REBOOT CONFIRM */
void sys_time_wipe(void);   /* the TZ draft and kept outcomes (Task 27); a draft is re-read from the config next build */
void sys_fw_wipe(void);     /* the kept install outcome (and its Reboot now) and the card listing (Task 32) */
