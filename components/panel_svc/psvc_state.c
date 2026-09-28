#include <stdio.h>
#include <string.h>
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "app_if_common.h"
#include "mcfg_store.h"
#include "time_svc.h"
#include "wifi_mgr.h"
#include "node_mgr.h"
#include "alarm_mgr.h"
#include "psvc_fw.h"
#include "psvc_state.h"

static uint8_t (*volatile s_quar_fn)(void);

void psvc_state_set_web_quarantine_fn(uint8_t (*fn)(void)) { s_quar_fn = fn; }

/* Exactly what h_state gathered (http_api.c:61-145 before panel plan Task 9),
 * in the same order, plus the two internal-heap figures the panel shows. */
void psvc_state_fill(psvc_state_t *out, uint32_t flags) {
    wifi_status_t keep = out->wifi;
    memset(out, 0, sizeof *out);   /* load-bearing: node_mgr_get() leaves an unused slot untouched */

    snprintf(out->version, sizeof out->version, "%s", esp_app_get_description()->version);
    out->uptime_s         = hg_app_uptime_s();
    out->heap_min_kb      = esp_get_minimum_free_heap_size() / 1024;
    out->heap_int_free_kb = (uint32_t)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024);
    out->heap_int_min_kb  = (uint32_t)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024);

    char tbuf[48];
    hg_app_time_get_noted(tbuf, sizeof tbuf);
    psvc_parse_time_noted(tbuf, out->time, out->time_src);
    out->time_is_set  = hg_app_time_is_set() ? 1 : 0;
    out->utc_offset_s = time_svc_utc_offset();

    if (flags & PSVC_FILL_SKIP_WIFI) out->wifi = keep;
    else wifi_mgr_status(&out->wifi);

    char fwbuf[96];
    hg_app_fw_info(fwbuf, sizeof fwbuf);
    psvc_parse_fw_info(fwbuf, out->fw_slot, out->fw_state, out->fw_other);

    const char *kind = "";
    uint8_t pct = 0;
    (void)psvc_fw_progress(&kind, &pct);
    snprintf(out->upload_kind, sizeof out->upload_kind, "%s", kind);
    out->upload_pct = pct;

    node_mgr_fw_status(out->fleet_line, sizeof out->fleet_line);

    out->alarms_active = alarm_mgr_active_count();
    out->alarms_total  = alarm_mgr_total();

    uint8_t f = mcfg_get()->flags;
    out->web_default = (f & MCFG_F_WEB_DEFAULT) ? 1 : 0;
    out->ap_default  = (f & MCFG_F_AP_DEFAULT)  ? 1 : 0;

    uint8_t (*q)(void) = s_quar_fn;
    out->web_cmd_quarantined = q ? q() : 0;

    for (int i = 0; i < HG_MAX_ZONES; i++) (void)node_mgr_get(i, &out->node[i]);
    for (int i = 0; i < HG_MAX_ZONES; i++)
        out->cfg_sync_failed[i] = (uint8_t)node_mgr_cfg_sync_failed((uint8_t)(i + 1));
    node_mgr_ring_status(&out->ring);

    out->now_ms = (uint32_t)(esp_timer_get_time() / 1000);   /* same clock as node_mgr's last_hb_ms */
}
