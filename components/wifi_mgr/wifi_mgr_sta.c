#include <string.h>
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "mdns.h"
#include "wifi_mgr_priv.h"

static const char *TAG = "wifi_mgr_sta";

/* Reconnect backoff ladder in seconds; the last entry repeats forever. A
 * greenhouse master that outlives a router reboot must keep trying without
 * hammering the air: 5 s catches a blip, 60 s is the steady-state retry. */
static const uint32_t BACKOFF_S[] = { 5, 10, 20, 40, 60 };
#define BACKOFF_N ((int)(sizeof BACKOFF_S / sizeof BACKOFF_S[0]))

static esp_timer_handle_t s_retry;
static int                s_step;          /* index into BACKOFF_S, clamped at BACKOFF_N-1 */
static uint8_t            s_configured;    /* mcfg has a non-empty sta_ssid */
static uint8_t            s_want_down_ntf; /* report the next disconnect even though we were never up */
static uint8_t            s_mdns_up;
static char               s_hostname[24];

/* Short, operator-readable disconnect reasons. The brief's four buckets are
 * the whole vocabulary; the raw numeric code goes to the log, which is where
 * anyone debugging a genuinely odd reason will look. */
static const char *reason_text(uint8_t r) {
    switch (r) {
    case WIFI_REASON_NO_AP_FOUND:
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
    case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
        return "NO_AP";
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_MIC_FAILURE:
    case WIFI_REASON_802_1X_AUTH_FAILED:
    case WIFI_REASON_AKMP_INVALID:
    case WIFI_REASON_PAIRWISE_CIPHER_INVALID:
    case WIFI_REASON_GROUP_CIPHER_INVALID:
        return "AUTH_FAIL";
    case WIFI_REASON_AUTH_EXPIRE:
    case WIFI_REASON_ASSOC_FAIL:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_CONNECTION_FAIL:
    case WIFI_REASON_BEACON_TIMEOUT:
    case WIFI_REASON_TIMEOUT:
        return "TIMEOUT";
    default:
        return "OTHER";
    }
}

/* Gated on s_configured ONLY, deliberately not on g_wm_started: this runs
 * from the WIFI_EVENT_STA_START handler, which the event-loop task dispatches
 * concurrently with the tail of wifi_mgr_start(), and g_wm_started is not set
 * until that tail finishes. Bench-found: with a g_wm_started check in here the
 * boot-time join silently never happened -- STA_START lost the race, nothing
 * called esp_wifi_connect(), and the STA sat idle with no disconnect event and
 * an empty StaReason until the next SET WIFI STA. Callers that can run BEFORE
 * the driver is started (wifi_mgr_sta_apply) do the g_wm_started check
 * themselves instead. */
static void sta_connect_now(void) {
    if (!s_configured) return;
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) ESP_LOGW(TAG, "esp_wifi_connect failed: %s", esp_err_to_name(err));
}

static void retry_cb(void *arg) {
    (void)arg;
    ESP_LOGI(TAG, "reconnect attempt to \"%s\"", g_wm.sta_ssid);
    sta_connect_now();
}

static void schedule_retry(void) {
    if (!s_configured || !s_retry) return;
    int i = s_step < BACKOFF_N ? s_step : BACKOFF_N - 1;
    if (s_step < BACKOFF_N) s_step++;
    esp_timer_stop(s_retry);   /* ESP_ERR_INVALID_STATE when not armed: expected, ignored */
    esp_err_t err = esp_timer_start_once(s_retry, (uint64_t)BACKOFF_S[i] * 1000000ull);
    if (err != ESP_OK) ESP_LOGE(TAG, "esp_timer_start_once failed: %s", esp_err_to_name(err));
    else               ESP_LOGI(TAG, "retry in %us", (unsigned)BACKOFF_S[i]);
}

/* mDNS is published once, on the first address we ever get: it binds to the
 * netifs, not to a particular lease, so a later re-join needs no repeat. */
static void mdns_start_once(void) {
    if (s_mdns_up) return;
    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mdns_init failed: %s", esp_err_to_name(err));
        return;
    }
    if ((err = mdns_hostname_set(s_hostname)) != ESP_OK)
        ESP_LOGW(TAG, "mdns_hostname_set(%s) failed: %s", s_hostname, esp_err_to_name(err));
    if ((err = mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0)) != ESP_OK)
        ESP_LOGW(TAG, "mdns_service_add failed: %s", esp_err_to_name(err));
    s_mdns_up = 1;
    ESP_LOGW(TAG, "mDNS up: %s.local (_http._tcp:80)", s_hostname);
}

static void sta_evt(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg; (void)base;

    if (id == WIFI_EVENT_STA_START) {
        /* Connecting from the event, not straight after esp_wifi_start(),
         * is the pattern IDF guarantees: the driver is provably ready here. */
        sta_connect_now();
        return;
    }
    if (id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *d = (const wifi_event_sta_disconnected_t *)data;
        const char *why = reason_text(d ? d->reason : 0);
        int was_up = g_wm.sta_up;
        int changed = strcmp(g_wm.sta_reason, why) != 0;

        g_wm.sta_up = 0;
        g_wm.sta_ip[0] = '\0';
        g_wm.rssi = 0;

        /* With no SSID configured, the only thing that can produce this event
         * is our own esp_wifi_disconnect() from wifi_mgr_sta_apply -- there is
         * nothing to report and nothing to retry. Bench-found: without this,
         * "SET WIFI STA - -" left GET WIFI showing "StaReason : OTHER"
         * (WIFI_REASON_STA_LEAVING) for a link nobody asked for. */
        if (!s_configured) {
            g_wm.sta_reason[0] = '\0';
            ESP_LOGI(TAG, "STA disconnected (unconfigured), reason %u", (unsigned)(d ? d->reason : 0));
            if (was_up && g_wm_sta_cb) g_wm_sta_cb(0);
            return;
        }

        snprintf(g_wm.sta_reason, sizeof g_wm.sta_reason, "%s", why);
        ESP_LOGW(TAG, "STA disconnected: %s (reason %u)", why, (unsigned)(d ? d->reason : 0));

        /* An edge is: we were associated and lost it; or this is the first
         * failure after an explicit connect request; or the reason itself
         * changed. Every further identical retry failure stays silent -- the
         * ladder can run for days. */
        if (was_up || s_want_down_ntf || changed)
            wifi_mgr_notify_edge("STA DOWN %s", why);
        s_want_down_ntf = 0;

        if (was_up && g_wm_sta_cb) g_wm_sta_cb(0);
        schedule_retry();
        return;
    }
    if (id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *e = (const ip_event_got_ip_t *)data;
        g_wm.sta_up = 1;
        g_wm.sta_reason[0] = '\0';
        if (e) snprintf(g_wm.sta_ip, sizeof g_wm.sta_ip, IPSTR, IP2STR(&e->ip_info.ip));
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            g_wm.rssi = ap.rssi;
            snprintf(g_wm.sta_ssid, sizeof g_wm.sta_ssid, "%s", (const char *)ap.ssid);
        }
        s_step = 0;                       /* ladder resets on success */
        s_want_down_ntf = 0;
        if (s_retry) esp_timer_stop(s_retry);
        ESP_LOGW(TAG, "STA up: %s on \"%s\" (%d dBm)", g_wm.sta_ip, g_wm.sta_ssid, (int)g_wm.rssi);
        mdns_start_once();
        wifi_mgr_notify_edge("STA UP %s", g_wm.sta_ip);
        if (g_wm_sta_cb) g_wm_sta_cb(1);
    }
}

int wifi_mgr_sta_init(void) {
    esp_err_t err;
    if ((err = esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_STA_START,
                                                    sta_evt, NULL, NULL)) != ESP_OK ||
        (err = esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
                                                    sta_evt, NULL, NULL)) != ESP_OK ||
        (err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                    sta_evt, NULL, NULL)) != ESP_OK) {
        ESP_LOGE(TAG, "STA event register failed: %s", esp_err_to_name(err));
        return -1;
    }
    const esp_timer_create_args_t args = {
        .callback = retry_cb, .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK, .name = "wifi_retry",
    };
    if ((err = esp_timer_create(&args, &s_retry)) != ESP_OK) {
        ESP_LOGE(TAG, "esp_timer_create failed: %s", esp_err_to_name(err));
        return -1;
    }
    return 0;
}

int wifi_mgr_sta_apply(const hg_mcfg_t *m) {
    esp_err_t err;
    snprintf(s_hostname, sizeof s_hostname, "%s", m->hostname);
    s_step = 0;
    if (s_retry) esp_timer_stop(s_retry);

    if (!m->sta_ssid[0]) {
        /* No house Wi-Fi configured: stop trying, but keep the AP (and the
         * whole greenhouse) running. */
        s_configured = 0;
        s_want_down_ntf = 0;
        g_wm.sta_up = 0;
        g_wm.sta_ip[0] = '\0';
        g_wm.sta_ssid[0] = '\0';
        g_wm.rssi = 0;
        if (g_wm_started) esp_wifi_disconnect();
        return 0;
    }

    wifi_config_t wc;
    memset(&wc, 0, sizeof wc);   /* the union's .sta member is the large one */
    /* wifi_sta_config_t.ssid is uint8_t[32] with no length field, so a full
     * 32-character SSID legitimately fills it with no room for a NUL; the
     * memset above supplies the terminator for anything shorter. */
    size_t n = strlen(m->sta_ssid);
    if (n > sizeof wc.sta.ssid) n = sizeof wc.sta.ssid;
    memcpy(wc.sta.ssid, m->sta_ssid, n);
    if (m->sta_pass[0]) {
        size_t pn = strlen(m->sta_pass);
        if (pn > sizeof wc.sta.password - 1) pn = sizeof wc.sta.password - 1;
        memcpy(wc.sta.password, m->sta_pass, pn);
        /* Threshold WPA_PSK, not WPA2_PSK: it still rejects open and WEP
         * APs but accepts WPA, WPA2 and WPA3 house routers, which is the
         * whole point of a field-configurable STA. */
        wc.sta.threshold.authmode = WIFI_AUTH_WPA_PSK;
    } else {
        wc.sta.threshold.authmode = WIFI_AUTH_OPEN;
    }

    if (g_wm_started) esp_wifi_disconnect();   /* harmless no-op when not associated */
    if ((err = esp_wifi_set_config(WIFI_IF_STA, &wc)) != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_config(STA) failed: %s", esp_err_to_name(err));
        return -1;
    }
    s_configured = 1;
    s_want_down_ntf = 1;         /* the first failure after this request is worth a NOTIFY */
    g_wm.sta_up = 0;
    g_wm.sta_ip[0] = '\0';
    g_wm.sta_reason[0] = '\0';
    snprintf(g_wm.sta_ssid, sizeof g_wm.sta_ssid, "%s", m->sta_ssid);

    /* Two callers, two paths. At boot (g_wm_started still 0) the driver is
     * not started yet, so the join is left to WIFI_EVENT_STA_START, which
     * esp_wifi_start() posts a moment later. From SET WIFI STA (started) the
     * join begins here -- no further STA_START is coming. */
    if (g_wm_started) sta_connect_now();
    return 0;
}
