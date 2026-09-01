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

#ifdef __cplusplus
}
#endif
