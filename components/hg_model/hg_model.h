#pragma once
#include <stdint.h>
#include <stddef.h>
#include "hg_cfg.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HG_CH_HW      0x01   /* hardware map changed -> restart pending */
#define HG_CH_CFG     0x02   /* logical config changed */
#define HG_CH_HW_LIVE 0x04   /* calibration / safety-limit fields: persist as hw, no restart flag */

void     hg_model_init(void);                                   /* defaults, generation 0, seq 0 */
void     hg_model_boot_load(const hg_zone_hw_t *hw_or_null, const hg_zone_cfg_t *cfg_or_null);
void     hg_model_snapshot_hw(hg_zone_hw_t *out);
void     hg_model_snapshot_cfg(hg_zone_cfg_t *out, uint32_t *seq_or_null);
uint32_t hg_model_cfg_seq(void);

typedef uint32_t (*hg_edit_fn)(hg_zone_hw_t *hw, hg_zone_cfg_t *cfg, void *arg); /* returns HG_CH_* mask, 0 = no change */
int      hg_model_edit(hg_edit_fn fn, void *arg, char *err, size_t errlen);      /* 0 ok, -1 busy, -2 invalid (err = path) */
uint32_t hg_model_take_dirty(uint8_t kind, void *staging, uint32_t *gen_out);    /* HG_CH_HW|HG_CH_CFG: copies struct, clears bit, returns size or 0 */
uint8_t  hg_model_dirty_mask(void);
int      hg_model_restart_pending(void);

/* ---- SP3 ring glue (Task 12) ---- */

/* Full-plane replace for a master-pushed CFG_COMMIT (ring_link): envelope
 * generation is adopted as-is from *cfg (reconciliation gen arithmetic is
 * the master's job, spec 4.4) rather than bumped like a local edit; source
 * is forced HG_SRC_MASTER regardless of what the wire payload carried. */
void hg_model_apply_cfg(const hg_zone_cfg_t *cfg);
/* Full-plane replace for a master-pushed HW CFG_COMMIT; marks HW dirty +
 * restart_pending, same as a local HG_CH_HW edit (a wiring/pin change needs
 * a reboot regardless of who pushed it). */
void hg_model_apply_hw(const hg_zone_hw_t *hw);

/* cfg_gen (== s_cfg.generation) + CRC-32 over the CFG plane -- payload-only,
 * envelope excluded -- the master computes cache CRCs the same way
 * (spec 4.4/2.9): a content identity, not a transfer identity. */
void     hg_model_cfg_info(uint32_t *gen, uint32_t *crc);
/* CRC-32 over the HW plane -- payload-only, envelope excluded -- same rule
 * as hg_model_cfg_info's crc. */
void     hg_model_hw_crc(uint32_t *crc);
/* 0 DEFAULTS (generation == 0, never written) / 1 LOCAL / 2 MASTER, derived
 * from the existing generation + source fields -- no extra state needed. */
uint8_t  hg_model_cfg_src(void);

#ifdef __cplusplus
}
#endif
