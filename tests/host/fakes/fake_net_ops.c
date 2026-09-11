#include <stdio.h>
#include <string.h>
#include "fake_net_ops.h"

fake_net_ops_state_t g_fake_net;

void fake_net_ops_reset(void) { memset(&g_fake_net, 0, sizeof g_fake_net); }

static void f_get_mcfg(hg_mcfg_t *out) {
    strcpy(g_fake_net.last_call, "get_mcfg");
    g_fake_net.get_mcfg_calls++;
    *out = g_fake_net.mcfg;
}

static void f_wifi_status(wifi_status_t *out) {
    strcpy(g_fake_net.last_call, "wifi_status");
    g_fake_net.wifi_status_calls++;
    *out = g_fake_net.status;
}

static int f_set_sta(const char *ssid, const char *pass) {
    strcpy(g_fake_net.last_call, "set_sta");
    g_fake_net.set_sta_calls++;
    snprintf(g_fake_net.set_sta_ssid, sizeof g_fake_net.set_sta_ssid, "%s", ssid);
    snprintf(g_fake_net.set_sta_pass, sizeof g_fake_net.set_sta_pass, "%s", pass);
    return g_fake_net.set_sta_rc;
}

static int f_set_ap(const char *ssid, const char *pass) {
    strcpy(g_fake_net.last_call, "set_ap");
    g_fake_net.set_ap_calls++;
    snprintf(g_fake_net.set_ap_ssid, sizeof g_fake_net.set_ap_ssid, "%s", ssid);
    snprintf(g_fake_net.set_ap_pass, sizeof g_fake_net.set_ap_pass, "%s", pass);
    return g_fake_net.set_ap_rc;
}

static int f_set_web_password(const char *pw) {
    strcpy(g_fake_net.last_call, "set_web_password");
    g_fake_net.set_pw_calls++;
    snprintf(g_fake_net.set_pw_pw, sizeof g_fake_net.set_pw_pw, "%s", pw);
    return g_fake_net.set_pw_rc;
}

static int f_set_tz(const char *tz) {
    strcpy(g_fake_net.last_call, "set_tz");
    g_fake_net.set_tz_calls++;
    snprintf(g_fake_net.set_tz_tz, sizeof g_fake_net.set_tz_tz, "%s", tz);
    return g_fake_net.set_tz_rc;
}

static int f_seed_mac(uint8_t zone, const uint8_t mac[6]) {
    strcpy(g_fake_net.last_call, "seed_mac");
    g_fake_net.seed_mac_calls++;
    g_fake_net.seed_mac_zone = zone;
    memcpy(g_fake_net.seed_mac_mac, mac, 6);
    return g_fake_net.seed_mac_rc;
}

const net_ops_t FAKE_NET_OPS = {
    .get_mcfg         = f_get_mcfg,
    .set_sta          = f_set_sta,
    .set_ap           = f_set_ap,
    .set_web_password = f_set_web_password,
    .set_tz           = f_set_tz,
    .wifi_status      = f_wifi_status,
    .seed_mac         = f_seed_mac,
};
