#pragma once
#include <stdint.h>
#include "hg_cfg_types.h"
#include "psvc_state.h"
#include "pnl_home.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The panel's read side (panel plan Global Constraints, "Reads leave the LVGL
 * task too"). pnl_poll gathers once a second on its own task and publishes a
 * COPY; the LVGL task only ever copies that out (pnl_poll_latest) and diffs it
 * against what it shows. ~2.2 KB: keep a pnl_snap_t static or in PSRAM, never
 * on the LVGL task's 8 KB stack. */
typedef struct {
    psvc_state_t st;                       /* st.wifi comes from pnl_wifi (5 s) -- a silent C6 costs 5 s per RPC */
    uint8_t      cfg_busy[HG_MAX_ZONES];   /* psvc_zone_cfg_busy per used zone ("save landed" = 0) */
    pnl_sched_t  sched;                    /* rebuilt every 10 s or when any used node's hb.cfg_gen changed */
    uint8_t      panel_cmd_quarantined;    /* Task 22 fills it; 0 before */
    uint8_t      started;                  /* 0 until the first fill */
    uint32_t     seq;
} pnl_snap_t;

/* Tasks pnl_poll (core 0, prio 2, 6144 B) and pnl_wifi (core 0, prio 1,
 * 4096 B), internal stacks, NEITHER TWDT-subscribed (both can sit in an
 * esp_hosted RPC or behind nmgr_lock). Stage + published copy in PSRAM,
 * guarded by a portMUX. Idempotent. */
void     pnl_poll_start(void);
void     pnl_poll_latest(pnl_snap_t *out); /* [ANY] copy of the last publish (all zero, started 0, before the first) */
uint32_t pnl_poll_seq(void);               /* [ANY] increments once per publish */
void     pnl_poll_kick(void);              /* [ANY] refill now instead of at the next second (after a save) */

/* [ANY] Smallest free stack, in bytes, pnl_poll and pnl_wifi have had
 * (uxTaskGetStackHighWaterMark); 0 for a task that was not created. Either
 * pointer may be NULL. */
void     pnl_poll_stack_free(uint32_t *poll_b, uint32_t *wifi_b);

/* [LVGL] Called by a 1 s lv_timer. The poller logs ERROR and emits
 * NOTIFY ALARM 0 W_PANEL_FROZEN LVGL task silent <N>s once after 10 s without
 * a beat (an ACTIVE alarm, key "ALARM 0"), and NOTIFY ALARM 0 CLEARED
 * PANEL_FROZEN on the next beat after that (D5), which clears it. */
void     pnl_lvgl_heartbeat(void);

#ifdef __cplusplus
}
#endif
