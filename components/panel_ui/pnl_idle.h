#pragma once
/* Glue: night dimming, the wake-only first touch and the idle operator-state wipe (D4, D18). */
void pnl_idle_start(void);   /* [LVGL] 1 s lv_timer: pnl_dim_eval -> panel_hw_brightness on change only, hands off while
                                pnl_dim_held() (ruling C21); while dimmed, a transparent full-screen catcher on
                                lv_layer_top() swallows the first press (wake only); when inactive time >= wipe_idle_s
                                and !pnl_worker_pending(): call every section *_wipe(), close the confirm and the
                                keyboard, pnl_nav_go(PNL_DEST_HOME, 0). Call once, under panel_lock(), after
                                pnl_shell_start(). */
