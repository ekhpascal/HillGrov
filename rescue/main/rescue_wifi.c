#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_mac.h"
#include "esp_task_wdt.h"
#include "esp_err.h"
#include "esp_log.h"
#include "rescue.h"

static const char *TAG = "rescue_wifi";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

/* rescue_wifi_sta() waits for a connect result in slices no longer than
 * this, resetting the TWDT between slices, rather than one long blocking
 * wait -- see the review-round TWDT fix. */
#define WIFI_WAIT_SLICE_MS 5000

static EventGroupHandle_t s_wifi_eg;
static esp_netif_t       *s_ap_netif;      /* created once in wifi_ready() */
static bool               s_wifi_ready;    /* esp_wifi_init() done */
static bool               s_wifi_started;  /* esp_wifi_start() currently active */

/* Copies a NUL-terminated src into a fixed uint8_t field, truncating to fit
 * and always forcing a trailing NUL -- for fields IDF documents as
 * NUL-terminated strings (e.g. the AP/STA password, char[64]).
 *
 * Deliberately strlen()+clamp rather than strnlen(src, dst_len-1): with a
 * compile-time string literal at one call site (the AP password), gcc can
 * inline this and statically see a small literal against a much larger
 * bound, and -Werror=stringop-overread flags that combination even though
 * strnlen's early-stop-at-NUL semantics make it safe. strlen() has no bound
 * argument for that heuristic to trip on. */
static void copy_field(uint8_t *dst, size_t dst_len, const char *src) {
    size_t n = strlen(src);
    if (n > dst_len - 1) n = dst_len - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

/* wifi_sta_config_t.ssid is uint8_t[32] with no ssid_len field: a
 * NUL-terminated string under 32 bytes works like copy_field, but a full
 * 32-character SSID (which fills the array with no room left for a NUL) is
 * also valid and must not be truncated to 31 -- so this copies up to the
 * full 32 bytes instead of always reserving one for a forced NUL. The
 * caller's wifi_config_t is memset to 0 first, so a shorter SSID still ends
 * up implicitly NUL-terminated by the untouched zero bytes. */
static void copy_ssid32(uint8_t dst[32], const char *src) {
    size_t n = strlen(src);
    if (n > 32) n = 32;
    memcpy(dst, src, n);
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
 * either path without re-initializing the driver.
 *
 * The AP netif is created here too, once, before any esp_wifi_start() call
 * from either path: esp_netif_create_default_wifi_ap() after the driver is
 * already started aborts() internally on failure (review-round fix -- the
 * old code created it lazily inside rescue_wifi_ap(), which could be
 * reached with Wi-Fi already started by a prior STA attempt). */
static void wifi_ready(void) {
    if (s_wifi_ready) return;
    s_wifi_ready = true;
    s_wifi_eg = xEventGroupCreate();
    esp_netif_init();
    esp_event_loop_create_default();
    s_ap_netif = esp_netif_create_default_wifi_ap();
    if (!s_ap_netif) ESP_LOGE(TAG, "esp_netif_create_default_wifi_ap failed");
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
    copy_ssid32(wc.sta.ssid, ssid);
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

    /* Wait in <=5 s slices, resetting the TWDT between them, instead of one
     * blocking wait up to timeout_ms (which can be 20 s): against the 30 s
     * TWDT with PANIC=y, a marginal AP could otherwise panic this task
     * mid-connect, losing an already-consumed handover record and update
     * silently (review-round fix; total timeout is unchanged). */
    EventBits_t bits = 0;
    uint32_t waited = 0;
    while (waited < timeout_ms) {
        esp_task_wdt_reset();
        uint32_t slice = (timeout_ms - waited) < WIFI_WAIT_SLICE_MS
                              ? (timeout_ms - waited) : WIFI_WAIT_SLICE_MS;
        bits = xEventGroupWaitBits(s_wifi_eg, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                    pdTRUE, pdFALSE, pdMS_TO_TICKS(slice));
        if (bits & (WIFI_CONNECTED_BIT | WIFI_FAIL_BIT)) break;
        waited += slice;
    }
    if (bits & WIFI_CONNECTED_BIT) return 0;
    ESP_LOGW(TAG, "STA connect to \"%s\" failed/timed out", ssid);
    esp_wifi_disconnect();
    return -1;
}

int rescue_wifi_ap(void) {
    wifi_ready();
    if (!s_ap_netif) {
        ESP_LOGE(TAG, "no AP netif (create failed earlier)");
        return -1;
    }

    /* Canonical bring-up regardless of how we got here: if Wi-Fi is already
     * running (a prior STA attempt started it), stop it first so the mode
     * switch below always goes through the same set_mode -> set_config ->
     * start sequence -- an unconditional esp_wifi_set_mode(AP) on an
     * already-running driver has unspecified WIFI_EVENT_AP_START/DHCP
     * behavior (review-round fix). */
    if (s_wifi_started) {
        esp_err_t err = esp_wifi_stop();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_wifi_stop failed: %s", esp_err_to_name(err));
            return -1;
        }
        s_wifi_started = false;
    }

    uint8_t mac[6] = { 0 };
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    char ssid[24];
    int ssid_len = snprintf(ssid, sizeof ssid, "HillGrow-Rescue-%02X%02X%02X", mac[3], mac[4], mac[5]);

    wifi_config_t wc = { 0 };
    memcpy(wc.ap.ssid, ssid, (size_t)ssid_len);
    wc.ap.ssid_len = (uint8_t)ssid_len;
    copy_field(wc.ap.password, sizeof wc.ap.password, "hillgrow1");
    wc.ap.authmode = WIFI_AUTH_WPA2_PSK;
    wc.ap.max_connection = 2;
    wc.ap.channel = 1;

    esp_err_t err;
    if ((err = esp_wifi_set_mode(WIFI_MODE_AP)) != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_mode(AP) failed: %s", esp_err_to_name(err));
        return -1;
    }
    if ((err = esp_wifi_set_config(WIFI_IF_AP, &wc)) != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_config(AP) failed: %s", esp_err_to_name(err));
        return -1;
    }
    if ((err = esp_wifi_start()) != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start(AP) failed: %s", esp_err_to_name(err));
        return -1;
    }
    s_wifi_started = true;

    /* Fixed IP after the AP is actually started: esp_netif_dhcps_stop/start
     * act on the netif's DHCP server, which is only meaningfully up once
     * the AP interface itself is (review-round fix; previously this ran
     * before esp_wifi_start()). */
    if ((err = esp_netif_dhcps_stop(s_ap_netif)) != ESP_OK &&
        err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        ESP_LOGE(TAG, "esp_netif_dhcps_stop failed: %s", esp_err_to_name(err));
        return -1;
    }
    esp_netif_ip_info_t ip = { 0 };
    ip.ip.addr = ESP_IP4TOADDR(192, 168, 7, 7);
    ip.gw.addr = ESP_IP4TOADDR(192, 168, 7, 7);
    ip.netmask.addr = ESP_IP4TOADDR(255, 255, 255, 0);
    if ((err = esp_netif_set_ip_info(s_ap_netif, &ip)) != ESP_OK) {
        ESP_LOGE(TAG, "esp_netif_set_ip_info failed: %s", esp_err_to_name(err));
        return -1;
    }
    if ((err = esp_netif_dhcps_start(s_ap_netif)) != ESP_OK) {
        ESP_LOGE(TAG, "esp_netif_dhcps_start failed: %s", esp_err_to_name(err));
        return -1;
    }

    ESP_LOGW(TAG, "rescue AP \"%s\" up at 192.168.7.7", ssid);
    return 0;
}
