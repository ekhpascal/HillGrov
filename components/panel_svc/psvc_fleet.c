#include <string.h>
#include "node_mgr.h"      /* node_mgr_fw_zone/all/abort, HG_MAX_ZONES */
#include "psvc_fleet.h"

psvc_rc_t psvc_fleet_zone(uint8_t zone) {
    if (zone < 1 || zone > HG_MAX_ZONES) return PSVC_E_INVALID;
    return psvc_rc_from_fleet(node_mgr_fw_zone(zone), 0);
}

psvc_rc_t psvc_fleet_all(void) { return psvc_rc_from_fleet(node_mgr_fw_all(), 0); }

psvc_rc_t psvc_fleet_abort(void) { return psvc_rc_from_fleet(node_mgr_fw_abort(), 1); }

int psvc_fleet_idle(const char *fleet_line) { return fleet_line && strcmp(fleet_line, "IDLE") == 0; }
