#include <string.h>
#include <stdint.h>
#include "ring_proto.h"

/* ============================================================================
   HEARTBEAT
   ============================================================================ */

int hg_hb_pack(const hg_hb_t *h, uint8_t *out, size_t cap) {
    if (!h || !out) return -1;
    if (h->n_shelves > 4) return -1;

    size_t payload_len = 62 + 14 * h->n_shelves;
    if (cap < payload_len) return -1;

    /* Offsets per spec §2.4 */
    memcpy(out + 0, h->mac, 6);                    /* [0..5]: mac */
    out[6] = h->fw_maj;                            /* [6]: fw_maj */
    out[7] = h->fw_min;                            /* [7]: fw_min */
    out[8] = h->fw_patch;                          /* [8]: fw_patch */
    out[9] = h->n_shelves;                         /* [9]: n_shelves */

    out[10] = (uint8_t)(h->uptime_s & 0xFF);       /* [10..13]: uptime_s LE */
    out[11] = (uint8_t)((h->uptime_s >> 8) & 0xFF);
    out[12] = (uint8_t)((h->uptime_s >> 16) & 0xFF);
    out[13] = (uint8_t)((h->uptime_s >> 24) & 0xFF);

    out[14] = (uint8_t)(h->unix_time & 0xFF);      /* [14..17]: unix_time LE */
    out[15] = (uint8_t)((h->unix_time >> 8) & 0xFF);
    out[16] = (uint8_t)((h->unix_time >> 16) & 0xFF);
    out[17] = (uint8_t)((h->unix_time >> 24) & 0xFF);

    out[18] = (uint8_t)(h->cfg_gen & 0xFF);        /* [18..21]: cfg_gen LE */
    out[19] = (uint8_t)((h->cfg_gen >> 8) & 0xFF);
    out[20] = (uint8_t)((h->cfg_gen >> 16) & 0xFF);
    out[21] = (uint8_t)((h->cfg_gen >> 24) & 0xFF);

    out[22] = (uint8_t)(h->cfg_crc & 0xFF);        /* [22..25]: cfg_crc LE */
    out[23] = (uint8_t)((h->cfg_crc >> 8) & 0xFF);
    out[24] = (uint8_t)((h->cfg_crc >> 16) & 0xFF);
    out[25] = (uint8_t)((h->cfg_crc >> 24) & 0xFF);

    out[26] = (uint8_t)(h->hw_crc & 0xFF);         /* [26..29]: hw_crc LE */
    out[27] = (uint8_t)((h->hw_crc >> 8) & 0xFF);
    out[28] = (uint8_t)((h->hw_crc >> 16) & 0xFF);
    out[29] = (uint8_t)((h->hw_crc >> 24) & 0xFF);

    out[30] = h->cfg_src;                          /* [30]: cfg_src */
    out[31] = h->mode;                             /* [31]: mode */
    out[32] = h->reset_reason;                     /* [32]: reset_reason */
    out[33] = h->time_quality;                     /* [33]: time_quality */

    out[34] = (uint8_t)(h->active_faults & 0xFF);  /* [34..41]: active_faults LE (uint64_t) */
    out[35] = (uint8_t)((h->active_faults >> 8) & 0xFF);
    out[36] = (uint8_t)((h->active_faults >> 16) & 0xFF);
    out[37] = (uint8_t)((h->active_faults >> 24) & 0xFF);
    out[38] = (uint8_t)((h->active_faults >> 32) & 0xFF);
    out[39] = (uint8_t)((h->active_faults >> 40) & 0xFF);
    out[40] = (uint8_t)((h->active_faults >> 48) & 0xFF);
    out[41] = (uint8_t)((h->active_faults >> 56) & 0xFF);

    out[42] = (uint8_t)(h->shelf_faults[0] & 0xFF);    /* [42..49]: shelf_faults[0..3] LE */
    out[43] = (uint8_t)((h->shelf_faults[0] >> 8) & 0xFF);
    out[44] = (uint8_t)(h->shelf_faults[1] & 0xFF);
    out[45] = (uint8_t)((h->shelf_faults[1] >> 8) & 0xFF);
    out[46] = (uint8_t)(h->shelf_faults[2] & 0xFF);
    out[47] = (uint8_t)((h->shelf_faults[2] >> 8) & 0xFF);
    out[48] = (uint8_t)(h->shelf_faults[3] & 0xFF);
    out[49] = (uint8_t)((h->shelf_faults[3] >> 8) & 0xFF);

    out[50] = h->link_flags;                       /* [50]: link_flags */
    out[51] = h->override_mask;                    /* [51]: override_mask */

    out[52] = (uint8_t)(h->rx_crc_err & 0xFF);     /* [52..53]: rx_crc_err LE */
    out[53] = (uint8_t)((h->rx_crc_err >> 8) & 0xFF);

    out[54] = (uint8_t)(h->rx_uart_err & 0xFF);    /* [54..55]: rx_uart_err LE */
    out[55] = (uint8_t)((h->rx_uart_err >> 8) & 0xFF);

    out[56] = (uint8_t)(h->rx_drop & 0xFF);        /* [56..57]: rx_drop LE */
    out[57] = (uint8_t)((h->rx_drop >> 8) & 0xFF);

    out[58] = (uint8_t)(h->fwd_count & 0xFF);      /* [58..59]: fwd_count LE */
    out[59] = (uint8_t)((h->fwd_count >> 8) & 0xFF);

    out[60] = (uint8_t)(h->min_free_heap_kb & 0xFF);   /* [60..61]: min_free_heap_kb LE */
    out[61] = (uint8_t)((h->min_free_heap_kb >> 8) & 0xFF);

    /* Shelf data at [62 + i*14] for i = 0..n_shelves-1 */
    for (int i = 0; i < h->n_shelves; i++) {
        uint8_t *shelf_base = out + 62 + i * 14;
        const hg_hb_shelf_t *s = &h->shelf[i];

        shelf_base[0] = (uint8_t)(s->raw_a & 0xFF);        /* [0..1]: raw_a LE */
        shelf_base[1] = (uint8_t)((s->raw_a >> 8) & 0xFF);

        shelf_base[2] = (uint8_t)(s->raw_b & 0xFF);        /* [2..3]: raw_b LE */
        shelf_base[3] = (uint8_t)((s->raw_b >> 8) & 0xFF);

        shelf_base[4] = s->pct_a;                          /* [4]: pct_a */
        shelf_base[5] = s->pct_b;                          /* [5]: pct_b */
        shelf_base[6] = s->white;                          /* [6]: white */
        shelf_base[7] = s->red;                            /* [7]: red */
        shelf_base[8] = s->out_flags;                      /* [8]: out_flags */
        shelf_base[9] = s->light_state;                    /* [9]: light_state */
        shelf_base[10] = s->water_state;                   /* [10]: water_state */
        shelf_base[11] = s->rsvd;                          /* [11]: rsvd */

        shelf_base[12] = (uint8_t)(s->pump_today_s & 0xFF);     /* [12..13]: pump_today_s LE */
        shelf_base[13] = (uint8_t)((s->pump_today_s >> 8) & 0xFF);
    }

    return (int)payload_len;
}

int hg_hb_parse(const uint8_t *p, size_t n, hg_hb_t *out) {
    if (!p || !out) return -1;

    /* Check minimum length */
    if (n < 62) return -1;

    /* Read n_shelves from offset 9 */
    uint8_t n_shelves = p[9];
    if (n_shelves > 4) return -1;

    /* Protocol evolution (spec §2.4, §6.3): everything the declared n_shelves
       promises must be present, but TRAILING bytes are ignored rather than
       rejected. Zones are updated before the master, so a newer zone that has
       APPENDED a field to its heartbeat stays readable by an older master --
       fields are only ever appended, never reordered or resized. */
    size_t expected_len = 62 + 14 * n_shelves;
    if (n < expected_len) return -1;

    /* Parse fixed base structure */
    memcpy(out->mac, p + 0, 6);
    out->fw_maj = p[6];
    out->fw_min = p[7];
    out->fw_patch = p[8];
    out->n_shelves = n_shelves;

    out->uptime_s = ((uint32_t)p[10]) |
                    ((uint32_t)p[11] << 8) |
                    ((uint32_t)p[12] << 16) |
                    ((uint32_t)p[13] << 24);

    out->unix_time = ((uint32_t)p[14]) |
                     ((uint32_t)p[15] << 8) |
                     ((uint32_t)p[16] << 16) |
                     ((uint32_t)p[17] << 24);

    out->cfg_gen = ((uint32_t)p[18]) |
                   ((uint32_t)p[19] << 8) |
                   ((uint32_t)p[20] << 16) |
                   ((uint32_t)p[21] << 24);

    out->cfg_crc = ((uint32_t)p[22]) |
                   ((uint32_t)p[23] << 8) |
                   ((uint32_t)p[24] << 16) |
                   ((uint32_t)p[25] << 24);

    out->hw_crc = ((uint32_t)p[26]) |
                  ((uint32_t)p[27] << 8) |
                  ((uint32_t)p[28] << 16) |
                  ((uint32_t)p[29] << 24);

    out->cfg_src = p[30];
    out->mode = p[31];
    out->reset_reason = p[32];
    out->time_quality = p[33];

    out->active_faults = ((uint64_t)p[34]) |
                         ((uint64_t)p[35] << 8) |
                         ((uint64_t)p[36] << 16) |
                         ((uint64_t)p[37] << 24) |
                         ((uint64_t)p[38] << 32) |
                         ((uint64_t)p[39] << 40) |
                         ((uint64_t)p[40] << 48) |
                         ((uint64_t)p[41] << 56);

    out->shelf_faults[0] = ((uint16_t)p[42]) | ((uint16_t)p[43] << 8);
    out->shelf_faults[1] = ((uint16_t)p[44]) | ((uint16_t)p[45] << 8);
    out->shelf_faults[2] = ((uint16_t)p[46]) | ((uint16_t)p[47] << 8);
    out->shelf_faults[3] = ((uint16_t)p[48]) | ((uint16_t)p[49] << 8);

    out->link_flags = p[50];
    out->override_mask = p[51];

    out->rx_crc_err = ((uint16_t)p[52]) | ((uint16_t)p[53] << 8);
    out->rx_uart_err = ((uint16_t)p[54]) | ((uint16_t)p[55] << 8);
    out->rx_drop = ((uint16_t)p[56]) | ((uint16_t)p[57] << 8);
    out->fwd_count = ((uint16_t)p[58]) | ((uint16_t)p[59] << 8);
    out->min_free_heap_kb = ((uint16_t)p[60]) | ((uint16_t)p[61] << 8);

    /* Parse shelf data */
    for (int i = 0; i < n_shelves; i++) {
        const uint8_t *shelf_base = p + 62 + i * 14;
        out->shelf[i].raw_a = ((uint16_t)shelf_base[0]) | ((uint16_t)shelf_base[1] << 8);
        out->shelf[i].raw_b = ((uint16_t)shelf_base[2]) | ((uint16_t)shelf_base[3] << 8);
        out->shelf[i].pct_a = shelf_base[4];
        out->shelf[i].pct_b = shelf_base[5];
        out->shelf[i].white = shelf_base[6];
        out->shelf[i].red = shelf_base[7];
        out->shelf[i].out_flags = shelf_base[8];
        out->shelf[i].light_state = shelf_base[9];
        out->shelf[i].water_state = shelf_base[10];
        out->shelf[i].rsvd = shelf_base[11];
        out->shelf[i].pump_today_s = ((uint16_t)shelf_base[12]) | ((uint16_t)shelf_base[13] << 8);
    }

    return 0;
}
