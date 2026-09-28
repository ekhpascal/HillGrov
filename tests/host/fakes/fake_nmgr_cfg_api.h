#pragma once
#include <stdint.h>
#include "hg_cfg_types.h"

/* Scripted stand-ins for node_mgr's three §4.4 config primitives
 * (node_mgr.h:85-88), for psvc_zcfg.c's host test. node_mgr_cfg_get() copies
 * g_fnc.cfg out (and g_fnc.hw when hw_present, else zeroes), exactly the
 * real primitive's documented contract; node_mgr_cfg_set() records what it
 * was handed. Reset before every test with fake_nmgr_cfg_reset(). */
typedef struct {
    int           busy_rc;      /* node_mgr_cfg_busy() return (default 0) */
    int           get_rc;       /* node_mgr_cfg_get() return (default 0 = CFG cached) */
    int           set_rc;       /* node_mgr_cfg_set() return (default 0 = queued) */
    int           hw_present;   /* default 1 */
    uint32_t      gen;          /* reported cfg_gen (default 7) */
    hg_zone_cfg_t cfg;          /* the cached CFG plane (default hg_defaults_cfg) */
    hg_zone_hw_t  hw;           /* the cached HW plane (default hg_defaults_hw) */
    int           busy_calls, get_calls, set_calls;
    uint8_t       last_zone;
    hg_zone_cfg_t last_set;     /* the cfg the last node_mgr_cfg_set() was handed */
} fake_nmgr_cfg_t;

extern fake_nmgr_cfg_t g_fnc;
void fake_nmgr_cfg_reset(void);
