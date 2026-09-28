#include <string.h>
#include "hg_blob.h"
#include "pnl_prefs.h"

#define PREFS_PAYLOAD 16u
#define PCT_MIN 5u     /* D4: the minimum duty; bench-tune for flicker, never 0 */

static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }

void pnl_prefs_defaults(pnl_prefs_t *p) {
    memset(p, 0, sizeof *p);
    p->dim.mode = 1;                 /* PNL_DIM_FOLLOW_LIGHTS */
    p->dim.day_pct = 80;
    p->dim.night_pct = 10;
    p->dim.fixed_start_min = 22 * 60;
    p->dim.fixed_end_min = 6 * 60;
    p->dim.idle_s = 60;
    p->wipe_idle_s = 300;
    p->face = 0;
    p->orient = 0;                   /* PNL_ORIENT_NORMAL */
}

static uint8_t clamp_pct(uint8_t v) { return v < PCT_MIN ? PCT_MIN : v > 100 ? 100 : v; }

void pnl_prefs_clamp(pnl_prefs_t *p) {
    if (p->dim.mode > 2) p->dim.mode = 1;
    p->dim.day_pct = clamp_pct(p->dim.day_pct);
    p->dim.night_pct = clamp_pct(p->dim.night_pct);
    if (p->dim.fixed_start_min > 1439) p->dim.fixed_start_min = 22 * 60;
    if (p->dim.fixed_end_min > 1439) p->dim.fixed_end_min = 6 * 60;
    if (p->dim.idle_s < 10) p->dim.idle_s = 10;
    if (p->wipe_idle_s < 60) p->wipe_idle_s = 60;
    if (p->face > 1) p->face = 0;
    if (p->orient > 1) p->orient = 0;
}

size_t pnl_prefs_pack(const pnl_prefs_t *p, uint8_t *out, size_t cap) {
    uint8_t b[PREFS_PAYLOAD];
    memset(b, 0, sizeof b);
    b[0] = p->dim.mode;
    b[1] = p->dim.day_pct;
    b[2] = p->dim.night_pct;
    b[3] = p->face;
    b[4] = p->orient;
    wr16(b + 6, p->dim.fixed_start_min);
    wr16(b + 8, p->dim.fixed_end_min);
    wr16(b + 10, p->dim.idle_s);
    wr16(b + 12, p->wipe_idle_s);
    return hg_blob_wrap(PNL_MAGIC_PREFS, PNL_PREFS_VER, 0, b, PREFS_PAYLOAD, out, cap);
}

int pnl_prefs_unpack(const uint8_t *in, size_t n, pnl_prefs_t *p) {
    uint8_t b[PREFS_PAYLOAD];
    uint32_t gen = 0;
    hg_blob_rc_t rc = in ? hg_blob_unwrap(PNL_MAGIC_PREFS, PNL_PREFS_VER, PNL_PREFS_VER, in, n, b, PREFS_PAYLOAD, &gen)
                         : HG_BLOB_E_SHORT;
    pnl_prefs_defaults(p);
    if (rc != HG_BLOB_OK && rc != HG_BLOB_MIGRATED) return -1;
    p->dim.mode = b[0];
    p->dim.day_pct = b[1];
    p->dim.night_pct = b[2];
    p->face = b[3];
    p->orient = b[4];
    p->dim.fixed_start_min = rd16(b + 6);
    p->dim.fixed_end_min = rd16(b + 8);
    p->dim.idle_s = rd16(b + 10);
    p->wipe_idle_s = rd16(b + 12);
    pnl_prefs_clamp(p);
    return 0;
}
