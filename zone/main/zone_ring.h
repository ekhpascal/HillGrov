#pragma once
#include "cmd_core.h"

/* Ring glue task: heartbeats, remote command execution, config apply/stream,
 * and the FW_UPDATE handshake. Spawns "zring" (core 0, prio 5, stack 4096,
 * TWDT); call after ring_link_start() so the driver is already installed. */
void zone_ring_start(const cmd_core_t *core);
