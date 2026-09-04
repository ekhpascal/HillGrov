#include <string.h>
#include <stdint.h>
#include "ring_proto.h"

/* ============================================================================
   TIME_SYNC
   ============================================================================ */

int hg_ts_pack(const hg_ts_t *t, uint8_t out[HG_TS_LEN]) {
    if (!t || !out) return -1;

    out[0] = (uint8_t)(t->utc & 0xFF);             /* [0..3]: utc LE */
    out[1] = (uint8_t)((t->utc >> 8) & 0xFF);
    out[2] = (uint8_t)((t->utc >> 16) & 0xFF);
    out[3] = (uint8_t)((t->utc >> 24) & 0xFF);

    out[4] = (uint8_t)(t->utc_offset_s & 0xFF);    /* [4..7]: utc_offset_s LE (int32_t) */
    out[5] = (uint8_t)((t->utc_offset_s >> 8) & 0xFF);
    out[6] = (uint8_t)((t->utc_offset_s >> 16) & 0xFF);
    out[7] = (uint8_t)((t->utc_offset_s >> 24) & 0xFF);

    out[8] = t->flags;                             /* [8]: flags */
    out[9] = t->ring_size;                         /* [9]: ring_size */

    out[10] = (uint8_t)(t->online_mask & 0xFF);    /* [10..11]: online_mask LE */
    out[11] = (uint8_t)((t->online_mask >> 8) & 0xFF);

    out[12] = t->inhibit_mask;                     /* [12]: inhibit_mask */

    return 0;
}

/* n >= HG_TS_LEN, not ==: trailing bytes are ignored so a newer sender that has
   APPENDED a field stays readable by an older receiver (spec §2.4/§6.3, same
   rule as hg_hb_parse). Short is still a reject -- the promised fields are not
   all there. */
int hg_ts_parse(const uint8_t *p, size_t n, hg_ts_t *out) {
    if (!p || !out || n < HG_TS_LEN) return -1;

    out->utc = ((uint32_t)p[0]) |
               ((uint32_t)p[1] << 8) |
               ((uint32_t)p[2] << 16) |
               ((uint32_t)p[3] << 24);

    /* Assemble as uint32_t to avoid signed left-shift UB, then memcpy */
    uint32_t offset_u = ((uint32_t)p[4]) |
                        ((uint32_t)p[5] << 8) |
                        ((uint32_t)p[6] << 16) |
                        ((uint32_t)p[7] << 24);
    memcpy(&out->utc_offset_s, &offset_u, sizeof(int32_t));

    out->flags = p[8];
    out->ring_size = p[9];

    out->online_mask = ((uint16_t)p[10]) | ((uint16_t)p[11] << 8);
    out->inhibit_mask = p[12];

    return 0;
}

/* ============================================================================
   ASSIGN_ID
   ============================================================================ */

int hg_assign_pack(const hg_assign_t *a, uint8_t out[HG_ASSIGN_LEN]) {
    if (!a || !out) return -1;

    memcpy(out + 0, a->mac, 6);                    /* [0..5]: mac */
    out[6] = a->zone_id;                           /* [6]: zone_id */

    return 0;
}

int hg_assign_parse(const uint8_t *p, size_t n, hg_assign_t *out) {
    if (!p || !out || n != HG_ASSIGN_LEN) return -1;

    memcpy(out->mac, p + 0, 6);
    out->zone_id = p[6];

    return 0;
}

/* ============================================================================
   FW_UPDATE
   ============================================================================ */

int hg_fwu_pack(const hg_fwu_t *f, uint8_t *out, size_t cap) {
    if (!f || !out) return -1;

    /* Use strnlen to safely measure string lengths within field bounds */
    size_t ssid_len = strnlen(f->ssid, sizeof f->ssid);
    size_t pass_len = strnlen(f->pass, sizeof f->pass);

    /* Reject if no room for NUL terminator */
    if (ssid_len >= 33 || pass_len >= 65) return -1;

    size_t payload_len = 2 + 1 + ssid_len + 1 + pass_len;
    if (payload_len > 99 || cap < payload_len) return -1;

    out[0] = (uint8_t)(f->reboot_delay_ms & 0xFF);     /* [0..1]: reboot_delay_ms LE */
    out[1] = (uint8_t)((f->reboot_delay_ms >> 8) & 0xFF);

    out[2] = (uint8_t)ssid_len;                        /* [2]: ssid_len */
    memcpy(out + 3, f->ssid, ssid_len);                /* [3..]: ssid bytes */

    out[3 + ssid_len] = (uint8_t)pass_len;             /* pass_len after ssid */
    memcpy(out + 3 + ssid_len + 1, f->pass, pass_len); /* pass bytes */

    return (int)payload_len;
}

int hg_fwu_parse(const uint8_t *p, size_t n, hg_fwu_t *out) {
    if (!p || !out || n < 3) return -1;

    out->reboot_delay_ms = ((uint16_t)p[0]) | ((uint16_t)p[1] << 8);

    uint8_t ssid_len = p[2];
    if (ssid_len >= 33) return -1;
    if (3 + ssid_len >= n) return -1;

    memcpy(out->ssid, p + 3, ssid_len);
    out->ssid[ssid_len] = '\0';

    uint8_t pass_len = p[3 + ssid_len];
    if (pass_len >= 65) return -1;
    if (3 + ssid_len + 1 + pass_len > n) return -1;

    memcpy(out->pass, p + 3 + ssid_len + 1, pass_len);
    out->pass[pass_len] = '\0';

    return 0;
}

/* ============================================================================
   ACK
   ============================================================================ */

int hg_ack_pack(const hg_ack_t *a, uint8_t *out, size_t cap) {
    if (!a || !out) return -1;

    if (a->detail_len > 125) return -1;

    size_t payload_len = 3 + a->detail_len;
    if (cap < payload_len) return -1;

    out[0] = (uint8_t)(a->acked_seq & 0xFF);           /* [0..1]: acked_seq LE */
    out[1] = (uint8_t)((a->acked_seq >> 8) & 0xFF);

    out[2] = a->status;                                /* [2]: status */

    memcpy(out + 3, a->detail, a->detail_len);         /* [3..]: detail bytes */

    return (int)payload_len;
}

int hg_ack_parse(const uint8_t *p, size_t n, hg_ack_t *out) {
    if (!p || !out || n < 3) return -1;

    /* Reject if payload is too large */
    if (n > 128) return -1;

    out->acked_seq = ((uint16_t)p[0]) | ((uint16_t)p[1] << 8);
    out->status = p[2];

    uint8_t detail_len = (uint8_t)(n - 3);
    if (detail_len > 125) return -1;

    out->detail_len = detail_len;
    memcpy(out->detail, p + 3, detail_len);
    out->detail[detail_len] = '\0';

    return 0;
}
