#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_mac.h"
#include "esp_log.h"
#include "rescue.h"

static const char *TAG = "rescue_wifi";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static EventGroupHandle_t s_wifi_eg;
static bool               s_wifi_ready;     /* esp_wifi_init() done */
static bool               s_wifi_started;   /* esp_wifi_start() done */

/* Copies a NUL-terminated src into a fixed uint8_t field, truncating to fit
 * (dst_len includes room for the trailing NUL) rather than relying on a libc
 * strlcpy that may not be present in every newlib build. */
static void copy_field(uint8_t *dst, size_t dst_len, const char *src) {
    size_t n = strnlen(src, dst_len - 1);
    memcpy(dst, src, n);
    dst[n] = 0;
}

static void wifi_evt(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;
    (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupSetBits(s_wifi_eg, WIFI_FAIL_BIT);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_wifi_eg, WIFI_CONNECTED_BIT);
    }
}

/* One-time bring-up shared by STA (pull mode) and AP (manual fallback) --
 * app_main tries STA up to 3 times before ever calling rescue_wifi_ap(), so
 * esp_wifi_init()/esp_netif_init() etc. must tolerate being reached from
 * either path without re-initializing the driver. */
static void wifi_ready(void) {
    if (s_wifi_ready) return;
    s_wifi_ready = true;
    s_wifi_eg = xEventGroupCreate();
    esp_netif_init();
    esp_event_loop_create_default();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_evt, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_evt, NULL, NULL);
}

int rescue_wifi_sta(const char *ssid, const char *pass, uint32_t timeout_ms) {
    wifi_ready();
    static bool sta_netif_made;
    if (!sta_netif_made) {
        esp_netif_create_default_wifi_sta();
        sta_netif_made = true;
    }

    /* wifi_config_t is a union whose .sta member is larger than its first
     * (.ap) member, so a plain "= { 0 }" only zeroes the smaller .ap prefix
     * -- explicit memset is needed to clear the whole .sta struct (e.g. its
     * trailing sae_h2e_identifier[32] and HE feature bitfields). */
    wifi_config_t wc;
    memset(&wc, 0, sizeof wc);
    copy_field(wc.sta.ssid, sizeof wc.sta.ssid, ssid);
    copy_field(wc.sta.password, sizeof wc.sta.password, pass);
    wc.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    xEventGroupClearBits(s_wifi_eg, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    if (!s_wifi_started) {
        if (esp_wifi_start() != ESP_OK) return -1;
        s_wifi_started = true;
    }
    esp_wifi_connect();

    EventBits_t bits = xEventGroupWaitBits(s_wifi_eg, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                            pdTRUE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    if (bits & WIFI_CONNECTED_BIT) return 0;
    ESP_LOGW(TAG, "STA connect to \"%s\" failed/timed out", ssid);
    esp_wifi_disconnect();
    return -1;
}

void rescue_wifi_ap(void) {
    wifi_ready();
    esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();

    uint8_t mac[6] = { 0 };
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    char ssid[24];
    int ssid_len = snprintf(ssid, sizeof ssid, "HillGrow-Rescue-%02X%02X%02X", mac[3], mac[4], mac[5]);

    /* Fixed IP before the AP goes live, so the DHCP server is already
     * handing out 192.168.7.x leases by the time a phone/laptop joins. */
    esp_netif_dhcps_stop(ap_netif);
    esp_netif_ip_info_t ip = { 0 };
    ip.ip.addr = ESP_IP4TOADDR(192, 168, 7, 7);
    ip.gw.addr = ESP_IP4TOADDR(192, 168, 7, 7);
    ip.netmask.addr = ESP_IP4TOADDR(255, 255, 255, 0);
    esp_netif_set_ip_info(ap_netif, &ip);
    esp_netif_dhcps_start(ap_netif);

    wifi_config_t wc = { 0 };
    memcpy(wc.ap.ssid, ssid, (size_t)ssid_len);
    wc.ap.ssid_len = (uint8_t)ssid_len;
    copy_field(wc.ap.password, sizeof wc.ap.password, "hillgrow1");
    wc.ap.authmode = WIFI_AUTH_WPA2_PSK;
    wc.ap.max_connection = 2;
    wc.ap.channel = 1;

    esp_wifi_set_mode(WIFI_MODE_AP);
    esp_wifi_set_config(WIFI_IF_AP, &wc);
    if (!s_wifi_started) {
        esp_wifi_start();
        s_wifi_started = true;
    }

    ESP_LOGW(TAG, "rescue AP \"%s\" up at 192.168.7.7", ssid);
}
