#pragma once
#include <stdint.h>
#include "cmd_core.h"
#include "ring_link.h"
#include "zone_ring.h"   /* the public half: every TU here also DEFINES part of it
                            (zone_ring_time_synced_at, zone_ring_inhibit_mask), so its
                            prototypes must be visible where the definitions are */

/* Private to zone_ring.c / zone_ring_cfg.c / zone_ring_sync.c -- not part of
 * the component's public surface (zone_ring.h). Split purely to keep each
 * file under the ~300-line cap. */

/* zone_ring.c: outbound tx sequence counter + ACK sender, shared by the CFG
 * streamer (CFG_CHUNK frames it emits need the same counter) and by
 * zone_ring_sync.c; immediate-HB request, used by zone_ring_sync.c right
 * after ASSIGN_ID (spec: "confirmed by the next heartbeat"). */
uint16_t zring_next_seq(void);
void     zring_send_ack(uint8_t dst, uint16_t acked_seq, uint8_t status,
                        const char *detail, uint8_t dlen);
void     zring_request_immediate_hb(void);

/* zone_ring.c: the single at-most-once (spec 2.6) cache, shared by CMD and
 * CFG_COMMIT -- the only two tracked, side-effecting types the master sends,
 * and it keeps one tracked frame outstanding ring-wide. begin() returns 1 when
 * the frame was a retransmit and has already been dealt with (absorbed, or the
 * cached reply re-sent); finish() records the reply a retransmit will replay. */
int      zring_dup_begin(const ring_frame_t *f, uint32_t now);
void     zring_dup_finish(uint16_t seq, uint8_t status, const char *detail);

/* zone_ring_cfg.c: CFG_CHUNK/CFG_COMMIT/CFG_GET (assembler + streamer). */
void zone_ring_cfg_init(void);
void zone_ring_cfg_chunk(const ring_frame_t *f, uint32_t now);
void zone_ring_cfg_commit(const ring_frame_t *f, uint32_t now);
void zone_ring_cfg_get(const ring_frame_t *f, uint32_t now);

/* zone_ring_sync.c: TIME_SYNC/ASSIGN_ID, zone link-state (W_LINK_LOST), and
 * the two values zone_ring.c's build_hb() needs derived from them. */
void    zone_ring_sync_init(cmd_core_t *core);
void    zsync_note_rx(uint32_t now);              /* any received frame: upstream leg alive */
void    zsync_note_master(uint32_t now);           /* master-sourced frame received */
void    zsync_time_sync(const ring_frame_t *f, uint32_t now);
void    zsync_assign_id(const ring_frame_t *f);
void    zsync_link_tick(uint32_t now);             /* W_LINK_LOST edge check, call every loop */
uint8_t zsync_time_quality(void);                  /* HB time_quality field */
uint8_t zsync_link_flags(uint32_t now, uint8_t zid); /* HB link_flags field */
