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
 * out), before any TIME_SYNC has ever been received, or if called before
 * zone_ring_start() has run at all (safe at any boot ordering relative to
 * whatever SP2 task ends up calling this). */
uint8_t zone_ring_inhibit_mask(void);

/* Uptime second at which a ring TIME_SYNC carrying time_valid last set this
 * node's clock; 0 = never this boot. GET TIME uses it to report the source
 * token RING (app_if.h's documented set) instead of NONE once the master's
 * time has arrived -- the zone's own SET/NONE state knows nothing about the
 * ring. Uptime, not wall clock, so a sync that steps the clock backwards
 * still reads as the most recent source (see hg_app_time_get_ext). */
uint32_t zone_ring_time_synced_at(void);
