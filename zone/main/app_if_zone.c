#include <string.h>
#include "esp_system.h"
#include "cmd_core.h"
#include "cmd_common.h"
#include "app_if_common.h"
#include "hg_model.h"
#include "hg_store.h"
#include "rescue_handover.h"
#include "bootloader_common.h"
#include "board.h"
#include "cli.h"
#include "zone_ring.h"

/* Reboots into the rescue image; the bootloader side of this handshake lands
 * in Task 15, which is expected to declare and consume this prototype from a
 * shared header once it exists. */
void hg_reboot_to_rescue(void);

/* ---- id / uptime / status ---- */

static const char *zone_node_name(void) {
    static hg_zone_cfg_t cfg;
    hg_model_snapshot_cfg(&cfg, NULL);
    return cfg.name[0] ? cfg.name : "-";
}

static int zone_status_lines(char *resp, int len) {
    hg_zone_cfg_t cfg;
    hg_model_snapshot_cfg(&cfg, NULL);
    cmd_linef(resp, len, "  Uptime : %u s", (unsigned)hg_app_uptime_s());
    cmd_linef(resp, len, "  Heap min : %u", (unsigned)esp_get_minimum_free_heap_size());
    cmd_linef(resp, len, "  Log drops : %u", (unsigned)cli_log_drops());
    cmd_linef(resp, len, "  Cfg gen : %u", (unsigned)cfg.generation);
    cmd_linef(resp, len, "  Restart pending : %d", hg_model_restart_pending());
    return 0;
}

/* ---- time ---- */

/* A zone has a second clock source the shared body knows nothing about: the
 * master's TIME_SYNC. Report it as RING (app_if.h's token set) whenever no
 * local SET TIME is newer. */
static int zone_time_get(char *buf, size_t n) {
    return hg_app_time_get_ext(buf, n, "RING", zone_ring_time_synced_at());
}

/* ---- firmware ---- */

/* spec 4.3: flush before every restart. factory_reset (hg_store_factory_reset)
 * is unaffected -- it erases the "hg" NVS namespace outright, so there is
 * nothing worth flushing first. */
static void zone_reboot(void) {
    hg_store_flush(2000);
    esp_restart();
}

static int zone_fw_update(const char *ssid, const char *pass, const char *url) {
    /* spec 4.3: flush before every restart -- this call reboots into rescue
     * on success, so any dirty config must hit NVS before the handover write. */
    hg_store_flush(2000);
    hg_handover_t h;
    memset(&h, 0, sizeof h);
    /* Belt-and-braces: cmd_common's A_FWUP arg maxes already reject an
     * oversized field at dispatch, but never silently truncate here too --
     * a truncated URL would reboot the node into rescue with an unfetchable
     * address. */
    if (strlen(ssid) >= sizeof h.ssid || strlen(pass) >= sizeof h.pass || strlen(url) >= sizeof h.url)
        return -1;
    h.expect_link = 0;
    memcpy(h.ssid, ssid, strlen(ssid) + 1);
    memcpy(h.pass, pass, strlen(pass) + 1);
    memcpy(h.url,  url,  strlen(url)  + 1);
    if (hg_handover_write(&h) != 0) return -1;
    hg_reboot_to_rescue();
    return -1; /* unreachable: hg_reboot_to_rescue() reboots */
}

/* ---- table ---- */

const app_if_t APP_IF_ZONE = {
    .role_name     = HG_ROLE_NAME,
    .zone_id       = hg_store_zid,
    .get_mac       = hg_app_get_mac,
    .node_name     = zone_node_name,
    .uptime_s      = hg_app_uptime_s,
    .status_lines  = zone_status_lines,
    .log_set       = hg_app_log_set,
    .time_get      = zone_time_get,
    .time_set      = hg_app_time_set,
    .save_flush    = hg_store_flush,
    .fw_info       = hg_app_fw_info,
    .fw_rollback   = hg_app_fw_rollback,
    .fw_update     = zone_fw_update,
    .reboot        = zone_reboot,
    .factory_reset = hg_store_factory_reset,
};

/* SP1 placeholder rescue-reboot handshake: stash a magic word in the RTC
 * retain-memory scratch area and restart; Task 15's bootloader reads it back
 * to decide whether to boot the rescue image. custom[] is a byte array, so
 * the magic is written as a full 32-bit little-endian word at custom[0..3]
 * rather than truncated into a single byte. */
void hg_reboot_to_rescue(void) {
    rtc_retain_mem_t *rtc = bootloader_common_get_rtc_retain_mem();
    uint32_t magic = 0xB0FAAF0Bu;
    memcpy(rtc->custom, &magic, sizeof magic);   /* avoid strict-aliasing on the uint8_t[] buffer */
    bootloader_common_update_rtc_retain_mem(NULL, false);
    esp_restart();
}
