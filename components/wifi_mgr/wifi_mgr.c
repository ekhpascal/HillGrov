#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_log.h"
#include "notify.h"
#include "mcfg_store.h"
#include "wifi_mgr_priv.h"

static const char *TAG = "wifi_mgr";

wifi_status_t g_wm;
esp_netif_t  *g_wm_sta_netif;
esp_netif_t  *g_wm_ap_netif;
wifi_sta_cb   g_wm_sta_cb;

uint8_t g_wm_started;

#define AP_CHANNEL   6      /* 2.4 GHz: any AP a zone node joins for a fleet OTA must be */
#define AP_MAX_CONN  4
#define SCAN_MAX_AP  24     /* records pulled from the driver per scan */

/* Copies a NUL-terminated src into a fixed uint8_t field, truncating to fit
 * and always forcing a trailing NUL -- strlen()+clamp rather than strnlen,
 * for the same -Werror=stringop-overread reason wifi_ap.c/rescue_wifi.c
 * document at their own copy_field. */
static void copy_field(uint8_t *dst, size_t dst_len, const char *src) {
    size_t n = strlen(src);
    if (n > dst_len - 1) n = dst_len - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

void wifi_mgr_notify_edge(const char *fmt, ...) {
    va_list ap;
    char body[96];
    va_start(ap, fmt);
    vsnprintf(body, sizeof body, fmt, ap);
    va_end(ap);
    notify_reset(NTF_WIFI, 0);
    notify_emit(NTF_WIFI, 0, "%s", body);
}

void wifi_mgr_notify_throttled(const char *fmt, ...) {
    va_list ap;
    char body[96];
    va_start(ap, fmt);
    vsnprintf(body, sizeof body, fmt, ap);
    va_end(ap);
    notify_emit(NTF_WIFI, 0, "%s", body);
}

/* ---- AP ---- */

static void ap_evt(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg; (void)base; (void)data;
    wifi_sta_list_t list;
    uint8_t n = (esp_wifi_ap_get_sta_list(&list) == ESP_OK && list.num > 0) ? (uint8_t)list.num : 0;
    if (id == WIFI_EVENT_AP_STACONNECTED || id == WIFI_EVENT_AP_STADISCONNECTED) {
        if (n == g_wm.ap_clients) return;
        g_wm.ap_clients = n;
        wifi_mgr_notify_throttled("AP CLIENTS %u", (unsigned)n);
    }
}

/* The AP credentials currently loaded into the driver. esp_wifi_set_config(
 * WIFI_IF_AP) restarts the softAP -- it tears down every associated station
 * and its DHCP lease -- so it must only be called when the credentials really
 * changed. Found on the bench: without this, a plain "SET WIFI STA ..." (which
 * touches nothing on the AP side) bounced the AP and would have knocked the
 * web client issuing the change off the network mid-request. */
static char s_ap_ssid_live[33];
static char s_ap_pass_live[65];

static void ap_fill_config(wifi_config_t *wc, const hg_mcfg_t *m) {
    /* wifi_config_t is a union whose .sta member is larger than .ap, so a
     * plain "= { 0 }" only zeroes the smaller .ap prefix -- memset covers
     * the whole union (the trap wifi_ap.c documents at the same spot). */
    memset(wc, 0, sizeof *wc);
    size_t n = strlen(m->ap_ssid);
    if (n > sizeof wc->ap.ssid) n = sizeof wc->ap.ssid;
    memcpy(wc->ap.ssid, m->ap_ssid, n);
    wc->ap.ssid_len = (uint8_t)n;
    copy_field(wc->ap.password, sizeof wc->ap.password, m->ap_pass);
    wc->ap.authmode = WIFI_AUTH_WPA2_PSK;
    wc->ap.max_connection = AP_MAX_CONN;
    wc->ap.channel = AP_CHANNEL;
}

/* force=1 at boot (the driver holds nothing yet); afterwards the write is
 * skipped when ssid and password both already match what the driver has.
 * 0 / -1. */
static int ap_push_config(const hg_mcfg_t *m, int force) {
    if (!force && strcmp(s_ap_ssid_live, m->ap_ssid) == 0 && strcmp(s_ap_pass_live, m->ap_pass) == 0)
        return 0;
    wifi_config_t wc;
    ap_fill_config(&wc, m);
    esp_err_t err = esp_wifi_set_config(WIFI_IF_AP, &wc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_config(AP) failed: %s", esp_err_to_name(err));
        return -1;
    }
    snprintf(s_ap_ssid_live, sizeof s_ap_ssid_live, "%s", m->ap_ssid);
    snprintf(s_ap_pass_live, sizeof s_ap_pass_live, "%s", m->ap_pass);
    snprintf(g_wm.ap_ssid, sizeof g_wm.ap_ssid, "%s", m->ap_ssid);
    return 0;
}

/* Fixed 192.168.7.7/24, applied only after the AP interface is actually up:
 * the netif's DHCP server is not meaningfully running before that
 * (rescue_wifi.c's own review-round lesson, inherited by wifi_ap.c). */
static int ap_static_ip(void) {
    esp_err_t err = esp_netif_dhcps_stop(g_wm_ap_netif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        ESP_LOGE(TAG, "esp_netif_dhcps_stop failed: %s", esp_err_to_name(err));
        return -1;
    }
    esp_netif_ip_info_t ip = { 0 };
    ip.ip.addr      = ESP_IP4TOADDR(192, 168, 7, 7);
    ip.gw.addr      = ESP_IP4TOADDR(192, 168, 7, 7);
    ip.netmask.addr = ESP_IP4TOADDR(255, 255, 255, 0);
    if ((err = esp_netif_set_ip_info(g_wm_ap_netif, &ip)) != ESP_OK) {
        ESP_LOGE(TAG, "esp_netif_set_ip_info failed: %s", esp_err_to_name(err));
        return -1;
    }
    if ((err = esp_netif_dhcps_start(g_wm_ap_netif)) != ESP_OK) {
        ESP_LOGE(TAG, "esp_netif_dhcps_start failed: %s", esp_err_to_name(err));
        return -1;
    }
    snprintf(g_wm.ap_ip, sizeof g_wm.ap_ip, "192.168.7.7");
    return 0;
}

/* ---- bring-up ---- */

int wifi_mgr_start(void) {
    if (g_wm_started) return 0;
    const hg_mcfg_t *m = mcfg_get();
    esp_err_t err;

    if ((err = esp_netif_init()) != ESP_OK) {
        ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(err));
        return -1;
    }
    /* ESP_ERR_INVALID_STATE just means somebody already created the default
     * event loop -- harmless, and not this function's business to fail on
     * (the wifi_ap.c ruling this replaces). */
    if ((err = esp_event_loop_create_default()) != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_event_loop_create_default failed: %s", esp_err_to_name(err));
        return -1;
    }
    g_wm_ap_netif  = esp_netif_create_default_wifi_ap();
    g_wm_sta_netif = esp_netif_create_default_wifi_sta();
    if (!g_wm_ap_netif || !g_wm_sta_netif) {
        ESP_LOGE(TAG, "netif create failed (ap=%p sta=%p)", (void *)g_wm_ap_netif, (void *)g_wm_sta_netif);
        return -1;
    }

    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    if ((err = esp_wifi_init(&wcfg)) != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(err));
        return -1;
    }
    if ((err = esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_AP_STACONNECTED,
                                                    ap_evt, NULL, NULL)) != ESP_OK ||
        (err = esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_AP_STADISCONNECTED,
                                                    ap_evt, NULL, NULL)) != ESP_OK) {
        ESP_LOGE(TAG, "AP event register failed: %s", esp_err_to_name(err));
        return -1;
    }
    if (wifi_mgr_sta_init() != 0) return -1;

    if ((err = esp_wifi_set_mode(WIFI_MODE_APSTA)) != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_mode(APSTA) failed: %s", esp_err_to_name(err));
        return -1;
    }
    if (ap_push_config(m, 1) != 0) return -1;

    /* Hostname must be set before the interface starts, so the DHCP client's
     * very first DISCOVER already carries it and the house router lists the
     * master by name. */
    if ((err = esp_netif_set_hostname(g_wm_sta_netif, m->hostname)) != ESP_OK)
        ESP_LOGW(TAG, "esp_netif_set_hostname(%s) failed: %s", m->hostname, esp_err_to_name(err));

    if (wifi_mgr_sta_apply(m) != 0) return -1;

    if ((err = esp_wifi_start()) != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(err));
        return -1;
    }
    if (ap_static_ip() != 0) return -1;

    g_wm_started = 1;
    ESP_LOGW(TAG, "AP \"%s\" up at 192.168.7.7 (ch %d, max %d); STA %s, host \"%s\"",
             m->ap_ssid, AP_CHANNEL, AP_MAX_CONN,
             m->sta_ssid[0] ? m->sta_ssid : "(unconfigured)", m->hostname);
    return 0;
}

int wifi_mgr_apply(void) {
    if (!g_wm_started) return -1;
    /* Called only after the commit that produced this config, so the RAM
     * buffer this points at is the fresh one and nothing commits underneath
     * us here (mcfg_store.h's "don't hold a pointer across a commit"). */
    const hg_mcfg_t *m = mcfg_get();

    /* Pushed only if the credentials changed: an actual SET WIFI AP does
     * bounce the AP and its stations, which is what the operator asked for,
     * but a SET WIFI STA / SET TZ must not. */
    if (ap_push_config(m, 0) != 0) return -1;
    if (esp_netif_set_hostname(g_wm_sta_netif, m->hostname) != ESP_OK)
        ESP_LOGW(TAG, "hostname \"%s\" takes effect on the next STA join", m->hostname);
    return wifi_mgr_sta_apply(m);
}

/* ---- status ---- */

void wifi_mgr_status(wifi_status_t *out) {
    if (!out) return;
    if (g_wm_started) {
        wifi_sta_list_t list;
        if (esp_wifi_ap_get_sta_list(&list) == ESP_OK)
            g_wm.ap_clients = list.num > 0 ? (uint8_t)list.num : 0;
        if (g_wm.sta_up) {
            /* RSSI moves constantly, so it is read live rather than cached
             * from the join; the same call re-confirms the joined SSID. */
            wifi_ap_record_t ap;
            if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
                g_wm.rssi = ap.rssi;
                snprintf(g_wm.sta_ssid, sizeof g_wm.sta_ssid, "%s", (const char *)ap.ssid);
            }
        }
    }
    *out = g_wm;
}

void wifi_mgr_on_sta(wifi_sta_cb cb) {
    g_wm_sta_cb = cb;
    /* app_main registers this only after time_svc_start(), i.e. after
     * wifi_mgr_start() -- a STA that came up in between would otherwise
     * never start SNTP, so a late registration replays the edge. */
    if (cb && g_wm.sta_up) cb(1);
}

/* ---- scan ---- */

static int rssi_desc(const void *a, const void *b) {
    int8_t ra = ((const wifi_ap_record_t *)a)->rssi;
    int8_t rb = ((const wifi_ap_record_t *)b)->rssi;
    return (rb > ra) - (rb < ra);
}

int wifi_mgr_scan(wifi_scan_t *out, int cap) {
    if (!g_wm_started || !out || cap <= 0) return -1;

    /* All-channel active scan. 14 x 120 ms worst case is ~1.7 s, comfortably
     * inside the 4 s budget the caller (Task 12's /api/wifi/scan) is held to;
     * the minimum keeps quiet channels from costing the full dwell. */
    wifi_scan_config_t sc;
    memset(&sc, 0, sizeof sc);
    sc.scan_type            = WIFI_SCAN_TYPE_ACTIVE;
    sc.scan_time.active.min = 40;
    sc.scan_time.active.max = 120;

    esp_err_t err = esp_wifi_scan_start(&sc, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_scan_start failed: %s", esp_err_to_name(err));
        return -1;
    }
    uint16_t found = 0;
    if (esp_wifi_scan_get_ap_num(&found) != ESP_OK) return -1;
    if (found == 0) return 0;

    uint16_t want = found < SCAN_MAX_AP ? found : SCAN_MAX_AP;
    wifi_ap_record_t *recs = calloc(want, sizeof *recs);
    if (!recs) {
        esp_wifi_clear_ap_list();
        ESP_LOGE(TAG, "scan: out of memory for %u records", (unsigned)want);
        return -1;
    }
    if (esp_wifi_scan_get_ap_records(&want, recs) != ESP_OK) {
        free(recs);
        return -1;
    }
    qsort(recs, want, sizeof *recs, rssi_desc);

    int n = 0;
    for (uint16_t i = 0; i < want && n < cap; i++) {
        const char *ssid = (const char *)recs[i].ssid;
        if (!ssid[0]) continue;                          /* hidden SSID: nothing to show or click */
        int dup = 0;
        for (int j = 0; j < n; j++)
            if (strcmp(out[j].ssid, ssid) == 0) { dup = 1; break; }
        if (dup) continue;                               /* strongest copy wins: the list is RSSI-sorted */
        snprintf(out[n].ssid, sizeof out[n].ssid, "%s", ssid);
        out[n].rssi = recs[i].rssi;
        out[n].auth = (uint8_t)recs[i].authmode;
        n++;
    }
    free(recs);
    return n;
}
