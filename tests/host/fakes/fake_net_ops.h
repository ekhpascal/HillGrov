#pragma once
#include <stdint.h>
#include "master_cmds.h"

/* Fake net_ops_t for the NET/TIME rows (GET WIFI, SET WIFI STA/AP, SET WEB
 * PASSWORD, GET/SET TZ, SET NODE <z> MAC). Same style as fake_node_ops:
 * tests poke the canned mcfg/status straight into the exposed struct and read
 * the recorded call arguments back out of it, no setters. */
typedef struct {
    hg_mcfg_t     mcfg;        /* what get_mcfg() hands out (GET TZ reads .tz) */
    wifi_status_t status;      /* what wifi_status() hands out (GET WIFI reads it) */

    char last_call[24];
    int  get_mcfg_calls;
    int  wifi_status_calls;
    int  set_sta_calls;  char set_sta_ssid[64]; char set_sta_pass[96];
    int  set_ap_calls;   char set_ap_ssid[64];  char set_ap_pass[96];
    int  set_pw_calls;   char set_pw_pw[96];
    int  set_tz_calls;   char set_tz_tz[64];
    int  seed_mac_calls; uint8_t seed_mac_zone; uint8_t seed_mac_mac[6];

    /* canned return values: 0 ok, -1 invalid, -2 valid but not stored, -3 internal fault */
    int set_sta_rc, set_ap_rc, set_pw_rc, set_tz_rc, seed_mac_rc;
} fake_net_ops_state_t;

extern fake_net_ops_state_t g_fake_net;
void fake_net_ops_reset(void);
extern const net_ops_t FAKE_NET_OPS;
