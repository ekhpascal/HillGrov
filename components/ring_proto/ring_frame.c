#include <string.h>
#include "ring_proto.h"

/* Pre-COBS content layout (RING_HDR_LEN=9 bytes, magic first):
   magic, src, dst, type, flags, ttl, len, seq_lo, seq_hi, payload[len], crc_lo, crc_hi
   CRC covers everything up to (but not including) the trailing crc bytes. */

int ring_frame_encode(const ring_hdr_t *h, const uint8_t *payload, uint8_t *wire, size_t cap) {
    if (h->len > RING_MAX_PAYLOAD) return -1;

    uint8_t raw[RING_FRAME_MAX];
    size_t rn = 0;
    raw[rn++] = RING_MAGIC;
    raw[rn++] = h->src;
    raw[rn++] = h->dst;
    raw[rn++] = h->type;
    raw[rn++] = h->flags;
    raw[rn++] = h->ttl;
    raw[rn++] = h->len;
    raw[rn++] = (uint8_t)(h->seq & 0xFF);
    raw[rn++] = (uint8_t)(h->seq >> 8);
    if (h->len) memcpy(raw + rn, payload, h->len);
    rn += h->len;

    uint16_t crc = ring_crc16(raw, rn);
    raw[rn++] = (uint8_t)(crc & 0xFF);
    raw[rn++] = (uint8_t)(crc >> 8);

    uint8_t cobs[RING_WIRE_MAX - 2];
    size_t clen = ring_cobs_encode(raw, rn, cobs);

    size_t wlen = clen + 2;   /* leading + trailing 0x00 delimiters */
    if (wlen > cap) return -1;

    wire[0] = 0x00;
    memcpy(wire + 1, cobs, clen);
    wire[1 + clen] = 0x00;
    return (int)wlen;
}

void ring_dec_init(ring_dec_t *d) { d->n = 0; }
void ring_dec_reset(ring_dec_t *d) { d->n = 0; }

/* d->n doubles as a discard-mode sentinel: on buffer overrun it is set past the valid
   [0, RING_WIRE_MAX-2] range so garbage bytes before the next 0x00 are swallowed silently
   (one -1 per dropped frame, not one per byte), without needing an extra struct field. */
#define RING_DEC_DISCARD 0xFFFFu

int ring_dec_feed(ring_dec_t *d, uint8_t byte, ring_hdr_t *h, uint8_t payload[RING_MAX_PAYLOAD]) {
    if (byte == 0x00) {
        if (d->n == 0 || d->n == RING_DEC_DISCARD) {
            d->n = 0;
            return 0;                                  /* idle delimiter or end of a discard run */
        }

        uint8_t raw[RING_WIRE_MAX - 2];
        int dn = ring_cobs_decode(d->buf, d->n, raw);
        d->n = 0;                                       /* ready for the next frame either way */

        if (dn < RING_HDR_LEN) return -1;                /* bad COBS or too short for a header */
        if (raw[0] != RING_MAGIC) return -1;
        uint8_t len = raw[6];
        if ((size_t)dn != (size_t)RING_HDR_LEN + len + 2) return -1;
        if (len > RING_MAX_PAYLOAD) return -1;

        uint16_t crc_calc = ring_crc16(raw, (size_t)RING_HDR_LEN + len);
        uint16_t crc_recv = (uint16_t)raw[RING_HDR_LEN + len] |
                             (uint16_t)((uint16_t)raw[RING_HDR_LEN + len + 1] << 8);
        if (crc_calc != crc_recv) return -1;

        h->src = raw[1]; h->dst = raw[2]; h->type = raw[3];
        h->flags = raw[4]; h->ttl = raw[5]; h->len = len;
        h->seq = (uint16_t)raw[7] | (uint16_t)((uint16_t)raw[8] << 8);
        memcpy(payload, raw + RING_HDR_LEN, len);
        return 1;
    }

    if (d->n == RING_DEC_DISCARD) return 0;               /* still discarding until next 0x00 */
    if (d->n >= RING_WIRE_MAX - 2) {                       /* overrun: >141 content bytes, no delimiter */
        d->n = RING_DEC_DISCARD;
        return -1;
    }
    d->buf[d->n++] = byte;
    return 0;
}
