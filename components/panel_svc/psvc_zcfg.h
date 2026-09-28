#pragma once
#include <stddef.h>
#include <stdint.h>
#include "hg_cfg.h"
#include "psvc_rc.h"
#include "psvc_edit.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The ONE zone-config read and write for both faces, over node_mgr's §4.4
 * primitives. The web's PUT /api/config?zone=N calls psvc_zone_cfg_edit() with
 * psvc_zone_json_fn; the panel's Config editor with psvc_zone_fields_fn.
 * Threading: [WORKER] -- node_mgr's lock waits forever and is held across NVS
 * writes (node_mgr.c:40-41), so never from the LVGL task. */

typedef int (*psvc_zcfg_fn)(hg_zone_cfg_t *cfg, const hg_zone_hw_t *hw_or_null, void *ctx,
                            char *err, size_t errcap, char *warn, size_t warncap);   /* 0 proceed / PSVC_EDIT_* */

/* zone outside 1..HG_MAX_ZONES -> ZONE_UNKNOWN; node_mgr_cfg_get -1 -> NO_CACHE.
 * hw may be NULL; when *hw_present is 0 the struct is zeroed, never stale.
 * gen and hw_present may be NULL. */
psvc_rc_t psvc_zone_cfg_get(uint8_t zone, hg_zone_cfg_t *cfg, hg_zone_hw_t *hw, uint32_t *gen, int *hw_present);

/* The web's exact order (http_api_cfg.c:118-160): range -> ZONE_UNKNOWN;
 * node_mgr_cfg_busy -> BUSY; node_mgr_cfg_get(FRESH copy, &hw_present) -1 ->
 * NO_CACHE; fn(&cfg, hw_present ? &hw : NULL, ...) 1/2/3 -> BAD_JSON /
 * INVALID_FIELD(err) / VALIDATION(err); hg_cfg_validate(&cfg,
 * hw_present ? &hw : NULL, err) -> VALIDATION; node_mgr_cfg_set: -1
 * ZONE_UNKNOWN, -2 BUSY, -3 ZONE_NOT_ONLINE, 0 PSVC_OK (queued -- the node_mgr
 * tick pushes; watch psvc_zone_cfg_busy() for "landed"). err and warn are
 * always written ("" when nothing to say). */
psvc_rc_t psvc_zone_cfg_edit(uint8_t zone, psvc_zcfg_fn fn, void *ctx,
                             char *err, size_t errcap, char *warn, size_t warncap);

int psvc_zone_cfg_busy(uint8_t zone);   /* node_mgr_cfg_busy: 1 while a write is queued or on the wire */

/* ctx = const char *json. hg_json_merge_cfg(hw_or_null, cfg, json, ...): -1
 * BAD_JSON, -2 INVALID_FIELD, -3 VALIDATION, 0 -> 0 with warn filled ("hw.*
 * readonly" and unknown keys). */
int psvc_zone_json_fn(hg_zone_cfg_t *cfg, const hg_zone_hw_t *hw_or_null, void *ctx,
                      char *err, size_t errcap, char *warn, size_t warncap);

/* ctx = const psvc_fedits_t *. The hardware plane (HG_G_HW, HG_G_HWSHELF,
 * HG_G_CAL) is refused with err "hw.<GROUP>.<KEY>" -- the panel never writes
 * it (system spec §4.4). A write failure reports the web merge's path shape:
 * "cfg.<GROUP>.<KEY>" | "cfg.shelf[<i>].<GROUP>.<KEY>" | "cfg.aux[<i>].AUX.<KEY>". */
int psvc_zone_fields_fn(hg_zone_cfg_t *cfg, const hg_zone_hw_t *hw_or_null, void *ctx,
                        char *err, size_t errcap, char *warn, size_t warncap);

#ifdef __cplusplus
}
#endif
