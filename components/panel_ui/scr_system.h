#pragma once
/* Glue: the System destination -- the web's System page (web/app.js:913-1020) as sections on one screen. */
#include "lvgl.h"
#include "pnl_poll.h"
#include "scr_shell.h"

typedef struct { const char *title; void (*build)(lv_obj_t *parent); void (*update)(const pnl_snap_t *s);
                 void (*teardown)(void); } pnl_sys_section_t;
extern const pnl_sys_section_t PNL_SYS_WIFI, PNL_SYS_TIME, PNL_SYS_PASSWORD, PNL_SYS_FLEET, PNL_SYS_FIRMWARE,
                               PNL_SYS_PLACEHOLDER;   /* registry in scr_system.c; PASSWORD/FLEET/FIRMWARE -> PLACEHOLDER until
                                                        Tasks 24/25/32 swap their line */
enum { PNL_SYS_SEC_WIFI = 0, PNL_SYS_SEC_TIME, PNL_SYS_SEC_PASSWORD, PNL_SYS_SEC_FLEET, PNL_SYS_SEC_FIRMWARE,
       PNL_SYS_SEC_COUNT };   /* pnl_nav_go(PNL_DEST_SYSTEM, sec); outside the range = last shown */
void sys_wifi_wipe(void);   /* Wi-Fi form text (Task 27) */
void sys_time_wipe(void);   /* the TZ draft and kept outcomes (Task 27); a draft is re-read from the config next build */
