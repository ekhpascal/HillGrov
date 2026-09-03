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

/* Full-plane replace for a master-pushed CFG_COMMIT (ring_link): applied
 * VERBATIM -- generation and source are whatever the payload struct itself
 * carries (the master stamps source=MASTER and generation=push-gen into the
 * struct before wrapping it, Task 13's side; reconciliation gen arithmetic
 * is the master's job, spec 4.4). This zone never force-mutates either
 * field -- if a pushed payload carries source=LOCAL that is the master's
 * bug, and hg_model_cfg_src() will faithfully report what was applied. */
void hg_model_apply_cfg(const hg_zone_cfg_t *cfg);
/* Full-plane replace for a master-pushed HW CFG_COMMIT. Validates BEFORE
 * any write (atomicity): hg_hw_validate(hw) first, then hg_cfg_validate()
 * of the *current* (already-applied) cfg against the *new* hw -- a pushed
 * HW change can silently invalidate the live cfg (e.g. lowering
 * pump_max_run_s below a configured dose_s), and that combination must
 * never land in the model. 0 ok; -2 invalid (err = field path, same
 * convention as hg_model_edit) and the model is left untouched. On success,
 * marks HW dirty + restart_pending, same as a local HG_CH_HW edit. */
int hg_model_apply_hw(const hg_zone_hw_t *hw, char *err, size_t errlen);

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
