#include <stdio.h>
#include <string.h>
#include "cmd_core.h"
#include "fake_app_if.h"

fake_app_if_state_t g_fake_app;

void fake_app_if_reset(void) {
    memset(&g_fake_app, 0, sizeof g_fake_app);
    strcpy(g_fake_app.log_eff, "INFO");
}

static uint8_t f_zone_id(void) { strcpy(g_fake_app.last_call, "zone_id"); return 2; }

static void f_get_mac(uint8_t mac[6]) {
    strcpy(g_fake_app.last_call, "get_mac");
    static const uint8_t m[6] = { 0x24, 0x6F, 0x28, 0xAA, 0xBB, 0x02 };
    memcpy(mac, m, 6);
}

static const char *f_node_name(void) { strcpy(g_fake_app.last_call, "node_name"); return "-"; }

static uint32_t f_uptime_s(void) { strcpy(g_fake_app.last_call, "uptime_s"); return 812; }

static int f_status_lines(char *resp, int len) {
    strcpy(g_fake_app.last_call, "status_lines");
    cmd_linef(resp, len, "  Uptime : %us", (unsigned)f_uptime_s());
    return 0;
}

static int f_log_set(const char *level, const char *tag, char *eff, size_t n) {
    strcpy(g_fake_app.last_call, "log_set");
    g_fake_app.log_calls++;
    strcpy(g_fake_app.log_level, level ? level : "");
    strcpy(g_fake_app.log_tag, tag ? tag : "");
    if (level) { strncpy(g_fake_app.log_eff, level, sizeof g_fake_app.log_eff - 1); g_fake_app.log_eff[sizeof g_fake_app.log_eff - 1] = '\0'; }
    snprintf(eff, n, "%s", g_fake_app.log_eff);
    return 0;
}

static int f_time_get(char *buf, size_t n) {
    strcpy(g_fake_app.last_call, "time_get");
    snprintf(buf, n, "2026-08-31 12:00:00 RTC 5");
    return 0;
}

static int f_time_set(int y, int mo, int d, int h, int mi, int s) {
    strcpy(g_fake_app.last_call, "time_set");
    g_fake_app.time_set_calls++;
    g_fake_app.ts_y = y; g_fake_app.ts_mo = mo; g_fake_app.ts_d = d;
    g_fake_app.ts_h = h; g_fake_app.ts_mi = mi; g_fake_app.ts_s = s;
    return 0;
}

static int f_save_flush(uint32_t timeout_ms) {
    (void)timeout_ms;
    strcpy(g_fake_app.last_call, "save_flush");
    return g_fake_app.fail_save ? -1 : 0;
}

static int f_fw_info(char *buf, size_t n) {
    strcpy(g_fake_app.last_call, "fw_info");
    snprintf(buf, n, "0.1.0 ota_0 VALID NONE");
    return 0;
}

static int f_fw_rollback(void) {
    strcpy(g_fake_app.last_call, "fw_rollback");
    g_fake_app.fw_rollback_calls++;
    return g_fake_app.fail_fw_rollback ? -1 : 0;
}

static int f_fw_update(const char *ssid, const char *pass, const char *url) {
    strcpy(g_fake_app.last_call, "fw_update");
    g_fake_app.fw_update_calls++;
    strncpy(g_fake_app.fu_ssid, ssid, sizeof g_fake_app.fu_ssid - 1);
    strncpy(g_fake_app.fu_pass, pass, sizeof g_fake_app.fu_pass - 1);
    strncpy(g_fake_app.fu_url, url, sizeof g_fake_app.fu_url - 1);
    return g_fake_app.fail_fw_update ? -1 : 0;
}

static void f_reboot(void) {
    strcpy(g_fake_app.last_call, "reboot");
    g_fake_app.reboot_calls++;
}

static int f_factory_reset(void) {
    strcpy(g_fake_app.last_call, "factory_reset");
    g_fake_app.factory_reset_calls++;
    return g_fake_app.fail_factory_reset ? -1 : 0;
}

const app_if_t FAKE_APP_IF = {
    .role_name     = "ZONE",
    .zone_id       = f_zone_id,
    .get_mac       = f_get_mac,
    .node_name     = f_node_name,
    .uptime_s      = f_uptime_s,
    .status_lines  = f_status_lines,
    .log_set       = f_log_set,
    .time_get      = f_time_get,
    .time_set      = f_time_set,
    .save_flush    = f_save_flush,
    .fw_info       = f_fw_info,
    .fw_rollback   = f_fw_rollback,
    .fw_update     = f_fw_update,
    .reboot        = f_reboot,
    .factory_reset = f_factory_reset,
};
