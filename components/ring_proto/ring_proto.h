#pragma once
#include <stdint.h>
#include <stddef.h>

#define RING_MAGIC          0xA1
#define RING_HDR_LEN        9
#define RING_MAX_PAYLOAD    128
#define RING_FRAME_MAX      (RING_HDR_LEN + RING_MAX_PAYLOAD + 2)   /* 139: hdr+payload+crc */
#define RING_WIRE_MAX       143                                     /* 0x00 + COBS(139)<=141 + 0x00 */
#define RING_ID_MASTER      0x00
#define RING_ID_UNASSIGNED  0xFE
#define RING_ID_BCAST       0xFF
#define HG_MAX_ZONES        8
#define RING_TTL_INIT       16
#define RING_F_ACK_REQ      0x01

enum { RING_T_HEARTBEAT = 0x01, RING_T_ACK = 0x02, RING_T_FAULT_EVENT = 0x03, RING_T_NOTIFY = 0x04,
       RING_T_CMD = 0x10, RING_T_TIME_SYNC = 0x11, RING_T_ASSIGN_ID = 0x12, RING_T_CFG_CHUNK = 0x13,
       RING_T_CFG_COMMIT = 0x14, RING_T_FW_UPDATE = 0x15, RING_T_CFG_GET = 0x16 };

uint16_t ring_crc16(const uint8_t *p, size_t n);                       /* CCITT-FALSE, own 512 B table */
size_t   ring_cobs_encode(const uint8_t *in, size_t n, uint8_t *out);  /* returns encoded len; out cap >= n + n/254 + 1 */
int      ring_cobs_decode(const uint8_t *in, size_t n, uint8_t *out);  /* decoded len, or -1 malformed (embedded zero, bad code) */
