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

typedef struct { uint8_t src, dst, type, flags, ttl, len; uint16_t seq; } ring_hdr_t;

int  ring_frame_encode(const ring_hdr_t *h, const uint8_t *payload,
                       uint8_t *wire, size_t cap);
     /* builds 0x00 + COBS(hdr+payload+crc16le) + 0x00; returns wire length, or -1
        (len > 128, cap too small). h->len must equal the payload length. */

typedef struct { uint8_t buf[RING_WIRE_MAX]; uint16_t n; } ring_dec_t;
void ring_dec_init(ring_dec_t *d);
void ring_dec_reset(ring_dec_t *d);          /* on UART BREAK/error events */
int  ring_dec_feed(ring_dec_t *d, uint8_t byte, ring_hdr_t *h, uint8_t payload[RING_MAX_PAYLOAD]);
     /* 1 = complete valid frame in (h, payload); 0 = need more; -1 = frame dropped
        (bad COBS / magic / len mismatch / CRC / oversize) — decoder has resynced, caller counts. */

typedef enum { RING_RT_DROP_SELF, RING_RT_CONSUME, RING_RT_CONSUME_FWD,
               RING_RT_FORWARD, RING_RT_DROP } ring_rt_t;
ring_rt_t ring_route(int is_master, uint8_t my_id, const uint8_t my_mac[6],
                     const ring_hdr_t *h, const uint8_t *payload);
     /* Zone rules (spec §2.5): src==my_id -> DROP_SELF; dst==my_id -> CONSUME;
        dst==0xFF -> CONSUME_FWD; dst==0xFE -> (type==ASSIGN_ID && payload mac[0..5]==my_mac)
        ? CONSUME : FORWARD; else ttl<=1 ? RING_RT_DROP : FORWARD.
        Master rules: src==MASTER -> DROP_SELF (returned broadcast = ring-closed probe — caller
        inspects type before dropping); everything else CONSUME; master NEVER forwards. */

enum { RING_DUP_EXEC = 0, RING_DUP_ABSORB = 1, RING_DUP_REPLAY = 2 };
typedef struct { uint16_t seq; uint8_t state, status; uint32_t t_ms; char detail[126]; } ring_dup_t;
void ring_dup_init(ring_dup_t *c);
int  ring_dup_check(ring_dup_t *c, uint16_t seq, uint32_t now_ms);
     /* EXEC (new seq, or same seq older than 3000 ms) / ABSORB (same seq, IN_PROGRESS)
        / REPLAY (same seq, DONE, within window) */
void ring_dup_start(ring_dup_t *c, uint16_t seq, uint32_t now_ms);   /* BEFORE dispatch (spec §2.6) */
void ring_dup_done(ring_dup_t *c, uint16_t seq, uint8_t status, const char *detail);

#define RING_TRK_DEPTH 8
uint32_t ring_ack_timeout_ms(uint8_t ring_size);   /* clamp(200 + 40*ring_size, 400, 900) */

typedef enum { RING_TRK_EV_NONE = 0, RING_TRK_EV_SEND, RING_TRK_EV_DONE, RING_TRK_EV_FAIL } ring_trk_ev_kind_t;
typedef struct {
    ring_trk_ev_kind_t kind;
    uint16_t seq; uint8_t dst, type;
    const uint8_t *wire; uint16_t wire_len;      /* EV_SEND: frame to transmit (valid until next call) */
    uint8_t  status;  char detail[126];          /* EV_DONE: ACK status + verbatim reply line */
    const char *fail_token;                      /* EV_FAIL: "ZONE_TIMEOUT" | "ZONE_UNKNOWN" */
} ring_trk_ev_t;

typedef struct {
    uint16_t seq;
    uint8_t  dst, type;
    uint8_t  wire[RING_WIRE_MAX];   /* encoded once at submit(); retries replay the same bytes */
    uint16_t wire_len;
    uint8_t  attempts;              /* 0 = queued, not yet sent; 1..3 once in flight (send count) */
    uint32_t last_send_ms;          /* timestamp of the most recent send, for timeout math */
} ring_trk_slot_t;

typedef struct ring_trk {
    ring_trk_slot_t q[RING_TRK_DEPTH];   /* queue order; q[0] is head-of-line */
    uint8_t n;                           /* entries in use, q[0..n-1] */
} ring_trk_t;                            /* q[0] is in flight once its attempts > 0 -- at most
                                             one in-flight entry ring-wide (stop-and-wait) */

void ring_trk_init(ring_trk_t *t);
int  ring_trk_submit(ring_trk_t *t, const ring_hdr_t *h, const uint8_t *payload, uint32_t now_ms);
     /* encodes ONCE (byte-identical retries); CMD/FW_UPDATE insert ahead of CFG_COMMIT/CFG_GET
        already queued but not in flight; returns -1 when full (caller answers BUSY) */
int  ring_trk_tick(ring_trk_t *t, uint32_t now_ms, uint8_t ring_size, ring_trk_ev_t *ev);
     /* returns 1 with ev when there is something to do: first send, timeout resend (attempt 2..3),
        or FAIL after 3rd timeout; else 0. One outstanding in-flight frame ring-wide. */
int  ring_trk_ack(ring_trk_t *t, uint16_t acked_seq, uint8_t status, const char *detail,
                  uint8_t detail_len, ring_trk_ev_t *ev);   /* 1 = EV_DONE for the in-flight frame; late/unknown ACK -> 0 */
int  ring_trk_unclaimed(ring_trk_t *t, const ring_hdr_t *returned, ring_trk_ev_t *ev);
     /* master saw its own ACK_REQ unicast come back: immediate EV_FAIL ZONE_UNKNOWN */

/* Telemetry payload codecs (ring_tlm.c) */

typedef struct {                       /* HEARTBEAT payload, spec §2.4: 62 + 14*n bytes */
    uint8_t  mac[6];                   /* off 0 */
    uint8_t  fw_maj, fw_min, fw_patch; /* 6..8 */
    uint8_t  n_shelves;                /* 9 (0..4; parse rejects >4) */
    uint32_t uptime_s;                 /* 10..13 */
    uint32_t unix_time;                /* 14..17 */
    uint32_t cfg_gen;                  /* 18..21 */
    uint32_t cfg_crc;                  /* 22..25 */
    uint32_t hw_crc;                   /* 26..29 */
    uint8_t  cfg_src;                  /* 30: 0 DEFAULTS, 1 LOCAL, 2 MASTER */
    uint8_t  mode;                     /* 31 */
    uint8_t  reset_reason;             /* 32 */
    uint8_t  time_quality;             /* 33: 0 NONE, 1 COARSE, 2 SYNCED */
    uint64_t active_faults;            /* 34..41 */
    uint16_t shelf_faults[4];          /* 42..49 */
    uint8_t  link_flags;               /* 50: b0 upstream_alive, b1 master_alive, b2 heard_by_master */
    uint8_t  override_mask;            /* 51 */
    uint16_t rx_crc_err, rx_uart_err, rx_drop, fwd_count, min_free_heap_kb;  /* 52..61 */
    struct { uint16_t raw_a, raw_b; uint8_t pct_a, pct_b, white, red,
             out_flags, light_state, water_state, rsvd; uint16_t pump_today_s; } shelf[4]; /* 62 + i*14 */
} hg_hb_t;

int hg_hb_pack(const hg_hb_t *h, uint8_t *out, size_t cap);          /* returns payload len 62+14n / -1 */
int hg_hb_parse(const uint8_t *p, size_t n, hg_hb_t *out);           /* -1 short / n_shelves>4 / len mismatch */

typedef struct {                       /* TIME_SYNC payload, 13 B */
    uint32_t utc;                      /* 0..3 */
    int32_t  utc_offset_s;             /* 4..7 */
    uint8_t  flags;                    /* 8: b0 time_valid, b1 ntp */
    uint8_t  ring_size;                /* 9 */
    uint16_t online_mask;              /* 10..11: bits 1..8 */
    uint8_t  inhibit_mask;             /* 12: b0 PUMPS b1 LIGHTS b2 ALL */
} hg_ts_t;

int hg_ts_pack(const hg_ts_t *t, uint8_t out[13]);
int hg_ts_parse(const uint8_t *p, size_t n, hg_ts_t *out);

typedef struct { uint8_t mac[6]; uint8_t zone_id; } hg_assign_t;      /* 7 B */
int hg_assign_pack(const hg_assign_t *a, uint8_t out[7]);
int hg_assign_parse(const uint8_t *p, size_t n, hg_assign_t *out);

typedef struct { uint16_t reboot_delay_ms; char ssid[33]; char pass[65]; } hg_fwu_t;
int hg_fwu_pack(const hg_fwu_t *f, uint8_t *out, size_t cap);        /* 2 + 1+ssid_len + 1+pass_len <= 99 */
int hg_fwu_parse(const uint8_t *p, size_t n, hg_fwu_t *out);         /* enforces NUL-termination + len bounds */

typedef struct { uint16_t acked_seq; uint8_t status; char detail[126]; uint8_t detail_len; } hg_ack_t;
int hg_ack_pack(const hg_ack_t *a, uint8_t *out, size_t cap);        /* 3 + detail_len */
int hg_ack_parse(const uint8_t *p, size_t n, hg_ack_t *out);         /* detail NUL-terminated on parse */
