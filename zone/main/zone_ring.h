#pragma once
#include <stdint.h>
#include "cmd_core.h"

/* Ring glue task: heartbeats, remote command execution, config apply/stream,
 * and the FW_UPDATE handshake. Spawns "zring" (core 0, prio 5, stack 4096,
 * TWDT); call after ring_link_start() so the driver is already installed.
 * core is non-const: ASSIGN_ID updates core->zone_id live (the same struct
 * cmd_task_start() already holds a pointer to), so cmd_dispatch sees the
 * new id immediately instead of staying at whatever app_main set at boot. */
void zone_ring_start(cmd_core_t *core);

/* SP2 pump-gate primitive: bit set = that channel is inhibited fleet-wide,
 * per the master's last TIME_SYNC inhibit_mask (b0 PUMPS b1 LIGHTS b2 ALL);
 * returns 0 once 600 s have passed without a fresh TIME_SYNC (spec 2.7 age-
 * out) or before any TIME_SYNC has ever been received. */
uint8_t zone_ring_inhibit_mask(void);
