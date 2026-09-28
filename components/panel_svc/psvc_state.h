#pragma once
#include <stdint.h>
#include "hg_cfg_types.h"   /* HG_MAX_ZONES */
#include "ring_proto.h"     /* hg_node_t, ring_status_t */
#include "wifi_mgr.h"       /* wifi_status_t */
#include "state_snap.h"     /* snap_master_t */

#ifdef __cplusplus
extern "C" {
#endif

/* The ONE state gather, for /api/state and the panel's 1 Hz poller alike --
 * extracted from h_state (http_api.c), which it now serves. Unlike
 * snap_master_t this struct OWNS its strings, so a copy can be published to
 * another task. */
typedef struct {
    char          version[32];
    uint32_t      uptime_s, heap_min_kb;              /* heap_min_kb = /api/state's figure (includes PSRAM on the P4) */
    uint32_t      heap_int_free_kb, heap_int_min_kb;  /* MALLOC_CAP_INTERNAL -- panel About only, not in /api/state */
    char          time[20];                           /* UTC "YYYY-MM-DD HH:MM:SS" (first 19 chars of hg_app_time_get_noted) */
    char          time_src[8];                        /* "NTP" | "SET" | "NONE" */
    uint8_t       time_is_set;                        /* hg_app_time_is_set() */
    int32_t       utc_offset_s;                       /* time_svc_utc_offset() */
    wifi_status_t wifi;                               /* untouched when PSVC_FILL_SKIP_WIFI */
    char          fw_slot[16], fw_state[16], fw_other[16];   /* hg_app_fw_info tokens; fw_state "PENDING" = OTA trial */
    char          upload_kind[8];                     /* "" | "master" | "zone" (psvc_fw_progress) */
    uint8_t       upload_pct;
    char          fleet_line[40];                     /* "IDLE" | "<z> PRECHECK|UPDATING|WAIT_HB" */
    int           alarms_active, alarms_total;
    uint8_t       web_default, ap_default;            /* MCFG_F_WEB_DEFAULT / MCFG_F_AP_DEFAULT */
    uint8_t       web_cmd_quarantined;                /* the registered web hook; 0 when the web never started */
    hg_node_t     node[HG_MAX_ZONES];                 /* slot = id-1; memset first; used == 0 => not enrolled */
    uint8_t       cfg_sync_failed[HG_MAX_ZONES];
    ring_status_t ring;
    uint32_t      now_ms;                             /* esp_timer ms (node_mgr's clock) */
} psvc_state_t;

#define PSVC_FILL_ALL       0u
#define PSVC_FILL_SKIP_WIFI 1u   /* keep out->wifi as the caller left it: wifi_mgr_status() is two esp_hosted RPCs
                                    of up to 5 s each on the P4, so the panel reads it on its own 5 s task */

/* [WORKER] 17 node_mgr lock round trips (portMAX_DELAY, held across NVS
 * writes), an otadata read, the fleet mutex and -- unless SKIP_WIFI -- two
 * RPCs. Never on the LVGL task. */
void psvc_state_fill(psvc_state_t *out, uint32_t flags);

/* Pure. m's const char* members point INTO s, so s must outlive every use of
 * m (h_state's static s does; so does a stack s used within one call). */
void psvc_state_to_snap(const psvc_state_t *s, snap_master_t *m);

/* [ANY] http_srv_start() registers http_cmd_quarantined here, so the panel can
 * show the web's degraded-slot count without panel_ui requiring http_srv. */
void psvc_state_set_web_quarantine_fn(uint8_t (*fn)(void));

/* Pure: h_state's own parses of hg_app_time_get_noted()'s
 * "YYYY-MM-DD HH:MM:SS <SRC> <age>" and hg_app_fw_info()'s
 * "<ver> <slot> <state> <other>". Outs always written ("" when absent). */
void psvc_parse_time_noted(const char *noted, char time_out[20], char src_out[8]);
void psvc_parse_fw_info(const char *info, char slot[16], char state[16], char other[16]);

#ifdef __cplusplus
}
#endif
