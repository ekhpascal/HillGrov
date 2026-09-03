#pragma once
#include <stdint.h>
#include "ring_proto.h"

/* UART2 ring link layer, shared by master and zone apps: driver install, a
 * store-and-forward RX task, and the send path. Protocol-only (Task 11) --
 * routing/dup/tracking policy lives in ring_proto; app wiring (node table,
 * ota_trial hooks) is Tasks 12/13. */

typedef struct { ring_hdr_t hdr; uint8_t payload[RING_MAX_PAYLOAD]; } ring_frame_t;
typedef struct { uint32_t rx_crc_err, rx_uart_err, rx_drop, fwd_count, tx_count, drop_self; } ring_counters_t;

/* Installs the UART2 driver at HG_RING_BAUD 8N1 on HG_GPIO_RING_TX/RX and
 * starts the "ring_rx" task (core 0, prio 6). my_id_fn is called fresh for
 * every received frame (a zone's id can change on ASSIGN_ID without a
 * restart); my_mac is captured once at start. */
void ring_link_start(int is_master, uint8_t (*my_id_fn)(void), const uint8_t my_mac[6]);

/* Consumer pop of a frame routed to us (CONSUME / CONSUME_FWD). Returns 0
 * with *f filled on a frame, -1 on timeout. */
int  ring_link_recv(ring_frame_t *f, uint32_t wait_ms);

/* Encodes and writes one frame to the ring. Returns 0 on success, -1 if
 * ring_frame_encode rejects the header/payload. */
int  ring_link_send(const ring_hdr_t *h, const uint8_t *payload);

void ring_link_counters(ring_counters_t *out);

/* Master only: ms timestamp of the last time the master's own TIME_SYNC
 * broadcast came back around the ring (the ring-closed probe, spec
 * §2.5/§2.7); 0 if it has never returned. */
uint32_t ring_link_ts_returned_ms(void);

/* SET RING TRACE: when on, ESP_LOGI's the header (hex) of every decoded frame. */
void ring_link_trace(int on);
