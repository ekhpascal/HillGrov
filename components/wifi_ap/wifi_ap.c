#include <string.h>
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_log.h"
#include "wifi_ap.h"

static const char *TAG = "wifi_ap";
static uint8_t s_started;

/* Copies a NUL-terminated src into a fixed uint8_t field, truncating to fit
 * and always forcing a trailing NUL -- mirrors rescue_wifi.c's copy_field
 * (same rationale: strlen()+clamp, not strnlen, keeps -Werror=stringop-
 * overread happy against a compile-time literal at this call site). */
static void copy_field(uint8_t *dst, size_t dst_len, const char *src) {
    size_t n = strlen(src);
    if (n > dst_len - 1) n = dst_len - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

int wifi_ap_start(void) {
    if (s_started) return 0;   /* idempotent: called exactly once in practice (app_main) */

    esp_err_t err;
    if ((err = esp_netif_init()) != ESP_OK) {
        ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(err));
        return -1;
    }
    if ((err = esp_event_loop_create_default()) != ESP_OK) {
        ESP_LOGE(TAG, "esp_event_loop_create_default failed: %s", esp_err_to_name(err));
        return -1;
    }
    esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();
    if (!ap_netif) {
        ESP_LOGE(TAG, "esp_netif_create_default_wifi_ap failed");
        return -1;
    }

    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    if ((err = esp_wifi_init(&wcfg)) != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(err));
        return -1;
    }

    /* wifi_config_t is a union whose .sta member is larger than .ap -- a
     * plain "= { 0 }" only zeroes the smaller .ap prefix (same trap
     * rescue_wifi.c's rescue_wifi_sta comment documents for the .sta side);
     * explicit memset covers the whole union regardless of which member is
     * actually used here. */
    wifi_config_t wc;
    memset(&wc, 0, sizeof wc);
    memcpy(wc.ap.ssid, "HillGrow", 8);
    wc.ap.ssid_len = 8;
    copy_field(wc.ap.password, sizeof wc.ap.password, "hillgrow1");
    wc.ap.authmode = WIFI_AUTH_WPA2_PSK;
    wc.ap.max_connection = 4;
    wc.ap.channel = 6;   /* 2.4 GHz -- ruling #1: any AP a node joins must be 2.4 GHz */

    if ((err = esp_wifi_set_mode(WIFI_MODE_AP)) != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_mode(AP) failed: %s", esp_err_to_name(err));
        return -1;
    }
    if ((err = esp_wifi_set_config(WIFI_IF_AP, &wc)) != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_config(AP) failed: %s", esp_err_to_name(err));
        return -1;
    }
    if ((err = esp_wifi_start()) != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(err));
        return -1;
    }

    /* Fixed IP AFTER the AP is actually started (rescue_wifi.c's own fix-
     * round lesson): the netif's DHCP server is only meaningfully up once
     * the AP interface itself is. */
    if ((err = esp_netif_dhcps_stop(ap_netif)) != ESP_OK &&
        err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        ESP_LOGE(TAG, "esp_netif_dhcps_stop failed: %s", esp_err_to_name(err));
        return -1;
    }
    esp_netif_ip_info_t ip = { 0 };
    ip.ip.addr = ESP_IP4TOADDR(192, 168, 7, 7);
    ip.gw.addr = ESP_IP4TOADDR(192, 168, 7, 7);
    ip.netmask.addr = ESP_IP4TOADDR(255, 255, 255, 0);
    if ((err = esp_netif_set_ip_info(ap_netif, &ip)) != ESP_OK) {
        ESP_LOGE(TAG, "esp_netif_set_ip_info failed: %s", esp_err_to_name(err));
        return -1;
    }
    if ((err = esp_netif_dhcps_start(ap_netif)) != ESP_OK) {
        ESP_LOGE(TAG, "esp_netif_dhcps_start failed: %s", esp_err_to_name(err));
        return -1;
    }

    s_started = 1;
    ESP_LOGW(TAG, "AP \"HillGrow\" up at 192.168.7.7 (channel 6, max 4 stations)");
    return 0;
}
