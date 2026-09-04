#include <string.h>
#include <stdint.h>
#include "ring_proto.h"

/* CFG_CHUNK payload: 0 kind u8 · 1 gen u32 LE · 5 idx u8 · 6 count u8 · 7 total u16 LE
   · 9 data[<=112]  (spec §2.9). Chunker splits a config/hw blob into <=7 fixed-size chunks;
   reassembler (ring_casm_t) buffers them back into a contiguous blob out of order. */

#define RING_CFG_HDR_LEN     9
#define RING_CFG_MAX_CHUNKS  ((RING_CFG_BUF_MAX + RING_CFG_DATA_MAX - 1) / RING_CFG_DATA_MAX)  /* 7 */

int ring_cfg_chunk_count(size_t blob_len)
{
    return (int)((blob_len + RING_CFG_DATA_MAX - 1) / RING_CFG_DATA_MAX);
}

int ring_cfg_chunk_build(uint8_t kind, uint32_t gen, const uint8_t *blob, size_t blob_len,
                         uint8_t idx, uint8_t *payload_out, size_t cap)
{
    if (!payload_out) return -1;
    if (blob_len > RING_CFG_BUF_MAX) return -1;
    if (blob_len > 0 && !blob) return -1;

    int count = ring_cfg_chunk_count(blob_len);
    if (idx >= count) return -1;

    size_t off = (size_t)idx * RING_CFG_DATA_MAX;
    size_t remaining = blob_len - off;
    size_t data_len = (remaining < RING_CFG_DATA_MAX) ? remaining : RING_CFG_DATA_MAX;

    size_t payload_len = RING_CFG_HDR_LEN + data_len;
    if (cap < payload_len) return -1;

    payload_out[0] = kind;                                  /* [0]: kind */

    payload_out[1] = (uint8_t)(gen & 0xFF);                  /* [1..4]: gen LE */
    payload_out[2] = (uint8_t)((gen >> 8) & 0xFF);
    payload_out[3] = (uint8_t)((gen >> 16) & 0xFF);
    payload_out[4] = (uint8_t)((gen >> 24) & 0xFF);

    payload_out[5] = idx;                                    /* [5]: idx */
    payload_out[6] = (uint8_t)count;                         /* [6]: count */

    uint16_t total = (uint16_t)blob_len;
    payload_out[7] = (uint8_t)(total & 0xFF);                /* [7..8]: total LE */
    payload_out[8] = (uint8_t)((total >> 8) & 0xFF);

    if (data_len) memcpy(payload_out + RING_CFG_HDR_LEN, blob + off, data_len);   /* [9..]: data */

    return (int)payload_len;
}

void ring_casm_init(ring_casm_t *a)
{
    memset(a, 0, sizeof(*a));
}

static void casm_start(ring_casm_t *a, uint8_t kind, uint32_t gen, uint8_t count, uint16_t total)
{
    a->active = 1;
    a->kind = kind;
    a->gen = gen;
    a->count = count;
    a->total = total;
    a->got_mask = 0;
    memset(a->buf, 0, sizeof(a->buf));
}

int ring_casm_feed(ring_casm_t *a, const uint8_t *payload, size_t n, uint32_t now_ms)
{
    if (!a || !payload || n < RING_CFG_HDR_LEN) return -1;

    uint8_t kind = payload[0];
    uint32_t gen = ((uint32_t)payload[1]) | ((uint32_t)payload[2] << 8) |
                   ((uint32_t)payload[3] << 16) | ((uint32_t)payload[4] << 24);
    uint8_t idx = payload[5];
    uint8_t count = payload[6];
    uint16_t total = ((uint16_t)payload[7]) | ((uint16_t)payload[8] << 8);
    size_t data_len = n - RING_CFG_HDR_LEN;

    if (count == 0 || count > RING_CFG_MAX_CHUNKS) return -1;
    if (idx >= count) return -1;
    if (total > RING_CFG_BUF_MAX) return -1;
    /* count and total must agree at ESTABLISH time (Task 6 parked item), not
       only against an already-running transfer: a header claiming e.g. 3 chunks
       for a 112 B blob would otherwise start a transfer that can never complete
       and would sit in the buffer until the 2 s idle abort. */
    if (count != ring_cfg_chunk_count(total)) return -1;

    /* interior chunks are always a full 112 B; the last chunk carries the remainder */
    size_t expected_len;
    if (idx == count - 1) {
        int32_t rem = (int32_t)total - (int32_t)RING_CFG_DATA_MAX * (int32_t)idx;
        if (rem <= 0 || rem > RING_CFG_DATA_MAX) return -1;
        expected_len = (size_t)rem;
    } else {
        expected_len = RING_CFG_DATA_MAX;
    }
    if (data_len != expected_len) return -1;

    if (!a->active || a->kind != kind || a->gen != gen) {
        casm_start(a, kind, gen, count, total);   /* fresh transfer, or gen/kind mismatch restarts it */
    } else if (a->count != count || a->total != total) {
        return -1;                                /* same transfer but count/total disagree: reject */
    }

    memcpy(a->buf + (size_t)idx * RING_CFG_DATA_MAX, payload + RING_CFG_HDR_LEN, data_len);
    a->got_mask = (uint8_t)(a->got_mask | (1u << idx));   /* duplicate idx: overwrite, counted once */
    a->last_ms = now_ms;

    uint8_t full_mask = (uint8_t)((1u << a->count) - 1u);
    return (a->got_mask == full_mask) ? 1 : 0;
}

int ring_casm_idle_expired(const ring_casm_t *a, uint32_t now_ms)
{
    if (!a->active) return 0;
    return (now_ms - a->last_ms) > RING_CFG_IDLE_MS;
}
