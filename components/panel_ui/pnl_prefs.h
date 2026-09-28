#pragma once
/* Pure (host-tested, tests/host/test_pnl_prefs.c): panel-local preferences and their persisted form -- an hg_blob
 * envelope around a 16-byte payload with explicit offsets:
 *   +0 mode  +1 day_pct  +2 night_pct  +3 face  +4 orient  +5 0
 *   +6 fixed_start_min u16 LE  +8 fixed_end_min u16 LE  +10 idle_s u16 LE  +12 wipe_idle_s u16 LE  +14..15 0 */
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PNL_MAGIC_PREFS 0x4E504748u   /* 'HGPN' LE */
#define PNL_PREFS_VER   1
typedef struct { uint8_t mode;          /* pnl_dim_mode_t (Task 27): 0 OFF, 1 FOLLOW_LIGHTS, 2 FIXED */
                 uint8_t day_pct, night_pct; uint16_t fixed_start_min, fixed_end_min; uint16_t idle_s; } pnl_dim_cfg_t;
typedef struct { pnl_dim_cfg_t dim; uint16_t wipe_idle_s; uint8_t face /*0 digital 1 analogue*/;
                 uint8_t orient /*PNL_ORIENT_**/; } pnl_prefs_t;

void   pnl_prefs_defaults(pnl_prefs_t *p);   /* FOLLOW_LIGHTS, day 80, night 10, fixed 22:00-06:00, idle 60 s, wipe 300 s,
                                                 digital, NORMAL */
void   pnl_prefs_clamp(pnl_prefs_t *p);      /* mode > 2 -> 1; day/night_pct -> 5..100; fixed_*_min > 1439 -> the default;
                                                 idle_s < 10 -> 10; wipe_idle_s < 60 -> 60; face > 1 -> 0; orient > 1 -> 0 */
size_t pnl_prefs_pack(const pnl_prefs_t *p, uint8_t *out, size_t cap);   /* hg_blob envelope; explicit offsets; 0 = cap too small */
int    pnl_prefs_unpack(const uint8_t *in, size_t n, pnl_prefs_t *p);    /* 0 / -1 (p gets defaults); clamps night_pct >= 5 */

#ifdef __cplusplus
}
#endif
