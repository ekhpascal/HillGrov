#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Master's Wi-Fi AP bring-up (Task 15 controller ruling #1): fixed SSID
 * "HillGrow" / pass "hillgrow1", WPA2-PSK, max 4 stations, channel 6 (2.4
 * GHz -- the SP1 bench lesson: any AP a node joins must be 2.4 GHz), fixed
 * IP 192.168.7.7/24. Canonical bring-up order, matching rescue_wifi.c's
 * rescue_wifi_ap() (the SP1 rescue-upload fallback AP, same lesson baked
 * in there): netif+event init -> esp_netif_create_default_wifi_ap() ->
 * esp_wifi_set_mode(AP) -> esp_wifi_set_config(AP) -> esp_wifi_start() ->
 * THEN esp_netif_dhcps_stop/set_ip_info/dhcps_start (the DHCP server is
 * only meaningfully up once the AP interface itself is). Every esp_ call
 * is checked and logged; returns -1 on the first failure (caller must NOT
 * call ota_trial_drivers_ok() in that case). Idempotent: a second call
 * once already up is a cheap no-op returning 0. */
int wifi_ap_start(void);

#ifdef __cplusplus
}
#endif
