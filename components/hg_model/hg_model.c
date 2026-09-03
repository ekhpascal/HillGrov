#include <string.h>
#include "hg_model.h"
#include "hg_cfg.h"
#include "hg_blob.h"

#ifndef HOST_TEST
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
static SemaphoreHandle_t s_mux;
#define LOCK()      do { if (xSemaphoreTake(s_mux, pdMS_TO_TICKS(100)) != pdTRUE) return -1; } while (0)
#define LOCK_WAIT() xSemaphoreTake(s_mux, portMAX_DELAY)   /* struct-copy paths: no busy return available, block instead */
#define UNLOCK()    xSemaphoreGive(s_mux)
#else
#define LOCK()      ((void)0)
#define LOCK_WAIT() ((void)0)
#define UNLOCK()    ((void)0)
#endif

static hg_zone_hw_t  s_hw;
static hg_zone_cfg_t s_cfg;
static uint32_t      s_seq;
static uint32_t      s_hw_gen;   /* dedicated hw-save envelope counter; ++ on every hw-dirty commit */
static uint8_t       s_dirty;
static int           s_restart_pending;

static uint32_t gen_next(uint32_t g) {
    return g + 1 ? g + 1 : 1;  /* skip 0 on wrap */
}

void hg_model_init(void) {
    hg_defaults_hw(&s_hw);
    hg_defaults_cfg(&s_cfg);
    s_seq = 0;
    s_hw_gen = 0;
    s_dirty = 0;
    s_restart_pending = 0;
#ifndef HOST_TEST
    s_mux = xSemaphoreCreateMutex();
#endif
}

void hg_model_boot_load(const hg_zone_hw_t *hw_or_null, const hg_zone_cfg_t *cfg_or_null) {
    LOCK_WAIT();
    if (hw_or_null)  s_hw  = *hw_or_null;
    if (cfg_or_null) s_cfg = *cfg_or_null;
    s_dirty = 0;
    s_restart_pending = 0;
    UNLOCK();
}

void hg_model_snapshot_hw(hg_zone_hw_t *out) {
    LOCK_WAIT();
    *out = s_hw;
    UNLOCK();
}

void hg_model_snapshot_cfg(hg_zone_cfg_t *out, uint32_t *seq_or_null) {
    LOCK_WAIT();
    *out = s_cfg;
    if (seq_or_null) *seq_or_null = s_seq;
    UNLOCK();
}

uint32_t hg_model_cfg_seq(void) {
    return s_seq;
}

int hg_model_edit(hg_edit_fn fn, void *arg, char *err, size_t errlen) {
    LOCK();
    hg_zone_hw_t  hw  = s_hw;
    hg_zone_cfg_t cfg = s_cfg;
    uint32_t mask = fn(&hw, &cfg, arg);
    if (mask & (HG_CH_HW | HG_CH_HW_LIVE)) {
        if (hg_hw_validate(&hw, err, errlen) != 0) { UNLOCK(); return -2; }
    }
    if (mask & (HG_CH_CFG | HG_CH_HW | HG_CH_HW_LIVE)) {
        if (hg_cfg_validate(&cfg, &hw, err, errlen) != 0) { UNLOCK(); return -2; }
    }
    s_hw  = hw;
    s_cfg = cfg;
    if (mask & HG_CH_CFG) {
        s_cfg.generation = gen_next(s_cfg.generation);
        s_cfg.source = HG_SRC_LOCAL;
    }
    if (mask & (HG_CH_HW | HG_CH_HW_LIVE)) s_hw_gen++;
    s_seq++;
    s_dirty |= (uint8_t)((mask & HG_CH_HW_LIVE ? HG_CH_HW : 0) | (mask & (HG_CH_HW | HG_CH_CFG)));
    if (mask & HG_CH_HW) s_restart_pending = 1;
    UNLOCK();
    return 0;
}

uint32_t hg_model_take_dirty(uint8_t kind, void *staging, uint32_t *gen_out) {
    uint32_t size = 0;
    LOCK_WAIT();
    if ((kind == HG_CH_HW || kind == HG_CH_CFG) && (s_dirty & kind)) {
        if (kind == HG_CH_HW) {
            memcpy(staging, &s_hw, sizeof s_hw);
            size = (uint32_t)sizeof s_hw;
            if (gen_out) *gen_out = s_hw_gen;         /* dedicated hw-save counter, not seq */
        } else {
            memcpy(staging, &s_cfg, sizeof s_cfg);
            size = (uint32_t)sizeof s_cfg;
            if (gen_out) *gen_out = s_cfg.generation; /* envelope gen == config generation */
        }
        s_dirty &= (uint8_t)~kind;
    }
    UNLOCK();
    return size;
}

uint8_t hg_model_dirty_mask(void) {
    return s_dirty;
}

int hg_model_restart_pending(void) {
    return s_restart_pending;
}

/* ---- SP3 ring glue (Task 12) ---- */

void hg_model_apply_cfg(const hg_zone_cfg_t *cfg) {
    LOCK_WAIT();
    s_cfg = *cfg;                 /* verbatim: generation + source are whatever the payload carries */
    s_dirty |= HG_CH_CFG;
    s_seq++;
    UNLOCK();
}

int hg_model_apply_hw(const hg_zone_hw_t *hw, char *err, size_t errlen) {
    LOCK_WAIT();
    if (hg_hw_validate(hw, err, errlen) != 0) { UNLOCK(); return -2; }
    if (hg_cfg_validate(&s_cfg, hw, err, errlen) != 0) { UNLOCK(); return -2; }   /* cross-plane: current cfg vs new hw */
    s_hw = *hw;
    s_hw_gen++;
    s_dirty |= HG_CH_HW;
    s_restart_pending = 1;   /* a pushed wiring/pin change needs a reboot, same as a local HG_CH_HW edit */
    s_seq++;
    UNLOCK();
    return 0;
}

/* CRC-32 over the raw plane payload only -- the packed struct as it goes on
 * the wire -- with the 16-byte hg_blob envelope (and therefore any
 * generation value, including the RAM-only s_hw_gen counter that resets on
 * every reboot) excluded. This is a content identity, not a transfer
 * identity: the master computes its cache CRCs the same way (spec 4.4/2.9),
 * so the same config content must yield the same crc across reboots, and
 * only a real content edit may change it. */
void hg_model_cfg_info(uint32_t *gen, uint32_t *crc) {
    LOCK_WAIT();
    uint32_t g = s_cfg.generation;
    uint32_t c = hg_crc32(0, &s_cfg, sizeof s_cfg);
    UNLOCK();
    if (gen) *gen = g;
    if (crc) *crc = c;
}

/* Payload-only, envelope excluded -- see hg_model_cfg_info's comment. */
void hg_model_hw_crc(uint32_t *crc) {
    LOCK_WAIT();
    uint32_t c = hg_crc32(0, &s_hw, sizeof s_hw);
    UNLOCK();
    if (crc) *crc = c;
}

uint8_t hg_model_cfg_src(void) {
    LOCK_WAIT();
    uint8_t r = (s_cfg.generation == 0) ? 0 : (uint8_t)(s_cfg.source + 1);
    UNLOCK();
    return r;
}
