#pragma once
#include <stdint.h>
#include "ring_link.h"

/* Private to zone_ring.c / zone_ring_cfg.c -- not part of the component's
 * public surface (zone_ring.h). Split purely to keep zone_ring.c under the
 * ~300-line cap (spec CFG assembler/streamer block is the natural cut). */

/* zone_ring.c: outbound tx sequence counter + ACK sender, shared by the CFG
 * streamer (CFG_CHUNK frames it emits need the same counter). */
uint16_t zring_next_seq(void);
void     zring_send_ack(uint8_t dst, uint16_t acked_seq, uint8_t status,
                        const char *detail, uint8_t dlen);

/* zone_ring_cfg.c: CFG_CHUNK/CFG_COMMIT/CFG_GET (assembler + streamer). */
void zone_ring_cfg_init(void);
void zone_ring_cfg_chunk(const ring_frame_t *f, uint32_t now);
void zone_ring_cfg_commit(const ring_frame_t *f, uint32_t now);
void zone_ring_cfg_get(const ring_frame_t *f, uint32_t now);
