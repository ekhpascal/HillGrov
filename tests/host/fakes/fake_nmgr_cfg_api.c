#include <string.h>
#include "hg_cfg.h"
#include "node_mgr.h"   /* the real prototypes, so a signature drift fails the host build */
#include "fake_nmgr_cfg_api.h"

fake_nmgr_cfg_t g_fnc;

void fake_nmgr_cfg_reset(void) {
    memset(&g_fnc, 0, sizeof g_fnc);
    g_fnc.hw_present = 1;
    g_fnc.gen = 7;
    hg_defaults_cfg(&g_fnc.cfg);
    hg_defaults_hw(&g_fnc.hw);
}

int node_mgr_cfg_busy(uint8_t zone) {
    g_fnc.busy_calls++;
    g_fnc.last_zone = zone;
    return g_fnc.busy_rc;
}

int node_mgr_cfg_get(uint8_t zone, hg_zone_cfg_t *cfg, hg_zone_hw_t *hw, uint32_t *cfg_gen, int *hw_present) {
    g_fnc.get_calls++;
    g_fnc.last_zone = zone;
    if (g_fnc.get_rc != 0) return g_fnc.get_rc;
    *cfg = g_fnc.cfg;
    if (hw) {
        if (g_fnc.hw_present) *hw = g_fnc.hw;
        else memset(hw, 0, sizeof *hw);
    }
    if (cfg_gen) *cfg_gen = g_fnc.gen;
    if (hw_present) *hw_present = g_fnc.hw_present;
    return 0;
}

int node_mgr_cfg_set(uint8_t zone, const hg_zone_cfg_t *cfg) {
    g_fnc.set_calls++;
    g_fnc.last_zone = zone;
    g_fnc.last_set = *cfg;
    return g_fnc.set_rc;
}
