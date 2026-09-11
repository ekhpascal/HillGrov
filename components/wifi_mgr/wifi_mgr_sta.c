#include <string.h>
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "wifi_mgr_priv.h"

static const char *TAG = "wifi_mgr_sta";

/* Reconnect backoff ladder in seconds; the last entry repeats forever. A
 * greenhouse master that outlives a router reboot must keep trying without
 * hammering the air: 5 s catches a blip, 60 s is the steady-state retry. */
static const uint32_t BACKOFF_S[] = { 5, 10, 20, 40, 60 };
#define BACKOFF_N ((int)(sizeof BACKOFF_S / sizeof BACKOFF_S[0]))

/* How long a self-inflicted-disconnect marker stays valid. It needs a deadline
 * at all because the driver posts NO event when esp_wifi_disconnect() hits an
 * already-idle STA, so the marker must expire rather than lie in wait.
 *
 * Erring short is the safe direction, and the sides are not symmetric:
 *   too SHORT -> one spurious "STA DOWN" line and one extra rung. Cosmetic.
 *   too LONG  -> a genuine fast failure is misread as ours, so no NOTIFY and
 *                no retry armed: silently stuck until a reboot.
 * Worst case for the long side is the first SET WIFI STA after boot (idle STA,
 * no self-event, marker live across the fresh connect) where an AUTH_FAIL on a
 * present AP can return in well under a second. Bench self-event latency is
 * < 200 ms, so 300 ms keeps ample margin over the real mechanism while staying
 * far below any plausible genuine association failure. */
#define SELF_DISC_VALID_US 300000ull

static esp_timer_handle_t s_retry;
static int                s_step;          /* index into BACKOFF_S, clamped at BACKOFF_N-1 */
static uint8_t            s_configured;    /* mcfg has a non-empty sta_ssid */
static uint8_t            s_want_down_ntf; /* report the next disconnect even though we were never up */
static uint64_t           s_self_disc_us;  /* 0 = none pending; else esp_timer_get_time() at our disconnect */

static void schedule_retry_ex(int advance);

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

/* EVERY esp_wifi_disconnect() this component issues goes through here, so the
 * disconnect event it provokes is recognisable as ours: a reconfigure, an
 * unconfigure, or the scan pause. Without it a plain "configured -> different
 * SSID" change reported a bogus NOTIFY WIFI 0 STA DOWN OTHER and armed a
 * ladder step that could fire esp_wifi_connect() mid-association. */
static void self_disconnect(void) {
    if (!g_wm_started) return;
    s_self_disc_us = (uint64_t)esp_timer_get_time();
    esp_wifi_disconnect();   /* harmless when not associated -- then no event comes and the marker expires */
}

/* Gated on s_configured ONLY, deliberately not on g_wm_started: this runs
 * from the WIFI_EVENT_STA_START handler, which the event-loop task dispatches
 * concurrently with the tail of wifi_mgr_start(), and g_wm_started is not set
 * until that tail finishes. Bench-found: with a g_wm_started check in here the
 * boot-time join silently never happened -- STA_START lost the race, nothing
 * called esp_wifi_connect(), and the STA sat idle with no disconnect event and
 * an empty StaReason until the next SET WIFI STA. Callers that can run BEFORE
 * the driver is started (wifi_mgr_sta_apply) do the g_wm_started check
 * themselves instead.
 *
 * A refused connect (ESP_ERR_WIFI_CONN, ESP_ERR_WIFI_STATE, ...) produces no
 * disconnect event, so nothing would ever re-arm the ladder: arm it here, or
 * one transient refusal wedges the STA until the next reboot. */
static void sta_connect_now(void) {
    if (!s_configured) return;
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_connect failed: %s -- retrying at the same rung",
                 esp_err_to_name(err));
        schedule_retry_ex(0);
    }
}

static void retry_cb(void *arg) {
    (void)arg;
    ESP_LOGI(TAG, "reconnect attempt to \"%s\"", g_wm.sta_ssid);
    sta_connect_now();
}

/* advance = 1 for a genuine association failure: the ladder climbs, because
 * the network really is not answering. advance = 0 when we could not even hand
 * the connect to the driver (busy with a scan, or another apply) -- that is
 * contention on our own side, says nothing about the AP, and must not stretch
 * the interval towards 60 s. */
static void schedule_retry_ex(int advance) {
    if (!s_configured || !s_retry) return;
    int i = s_step < BACKOFF_N ? s_step : BACKOFF_N - 1;
    if (advance && s_step < BACKOFF_N) s_step++;
    esp_timer_stop(s_retry);   /* ESP_ERR_INVALID_STATE when not armed: expected, ignored */
    esp_err_t err = esp_timer_start_once(s_retry, (uint64_t)BACKOFF_S[i] * 1000000ull);
    if (err != ESP_OK) ESP_LOGE(TAG, "esp_timer_start_once failed: %s", esp_err_to_name(err));
    else               ESP_LOGI(TAG, "retry in %us", (unsigned)BACKOFF_S[i]);
}

static void schedule_retry(void) { schedule_retry_ex(1); }

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
        unsigned raw = (unsigned)(d ? d->reason : 0);
        const char *why = reason_text(d ? d->reason : 0);

        int self = s_self_disc_us != 0 &&
                   (uint64_t)esp_timer_get_time() - s_self_disc_us < SELF_DISC_VALID_US;
        if (s_self_disc_us) s_self_disc_us = 0;   /* one marker, one event */

        char reason[sizeof g_wm.sta_reason];
        snprintf(reason, sizeof reason, "%s", self ? "" : why);

        /* The event handler owns every sta_up transition, so was_up here is
         * the real previous state -- wifi_mgr_sta_apply() must NOT pre-clear
         * it, or the observer below never fires and time_svc keeps SNTP
         * running against a link that is gone. prev is kept for the
         * reason-changed edge test, which has to compare against what the
         * status held BEFORE this event overwrote it. */
        char prev[sizeof g_wm.sta_reason];
        wifi_mgr_lock();
        int was_up = g_wm.sta_up;
        memcpy(prev, g_wm.sta_reason, sizeof prev);
        g_wm.sta_up = 0;
        g_wm.sta_ip[0] = '\0';
        g_wm.rssi = 0;
        memcpy(g_wm.sta_reason, reason, sizeof g_wm.sta_reason);
        wifi_mgr_unlock();

        /* Our own disconnect: nothing to report, nothing to retry (the
         * caller either re-issues the join itself or deliberately stopped). */
        if (self) {
            ESP_LOGI(TAG, "STA disconnected by us (reason %u)", raw);
            if (was_up && g_wm_sta_cb) { ESP_LOGI(TAG, "STA observer: down"); g_wm_sta_cb(0); }
            return;
        }

        ESP_LOGW(TAG, "STA disconnected: %s (reason %u)", why, raw);

        /* An edge is: we were associated and lost it; or this is the first
         * failure after an explicit connect request; or the reason itself
         * changed. Every further identical retry failure stays silent -- the
         * ladder can run for days. */
        if (was_up || s_want_down_ntf || strcmp(prev, why) != 0)
            wifi_mgr_notify_edge("STA DOWN %s", why);
        s_want_down_ntf = 0;

        if (was_up && g_wm_sta_cb) { ESP_LOGI(TAG, "STA observer: down"); g_wm_sta_cb(0); }
        schedule_retry();
        return;
    }
    if (id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *e = (const ip_event_got_ip_t *)data;
        char ip[sizeof g_wm.sta_ip]   = "";
        char ssid[sizeof g_wm.sta_ssid];
        int  have_ap;

        if (e) snprintf(ip, sizeof ip, IPSTR, IP2STR(&e->ip_info.ip));
        wifi_ap_record_t ap;
        have_ap = esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
        if (have_ap) snprintf(ssid, sizeof ssid, "%s", (const char *)ap.ssid);

        /* sta_ip is written BEFORE sta_up, so a reader that sees UP can never
         * see an empty address. */
        wifi_mgr_lock();
        memcpy(g_wm.sta_ip, ip, sizeof g_wm.sta_ip);
        if (have_ap) {
            memcpy(g_wm.sta_ssid, ssid, sizeof g_wm.sta_ssid);
            g_wm.rssi = ap.rssi;
        }
        g_wm.sta_reason[0] = '\0';
        g_wm.sta_up = 1;
        wifi_mgr_unlock();

        s_step = 0;                       /* ladder resets on success */
        s_want_down_ntf = 0;
        s_self_disc_us = 0;
        if (s_retry) esp_timer_stop(s_retry);
        ESP_LOGW(TAG, "STA up: %s on \"%s\" (%d dBm)", g_wm.sta_ip, g_wm.sta_ssid, (int)g_wm.rssi);
        wifi_mgr_notify_edge("STA UP %s", g_wm.sta_ip);
        if (g_wm_sta_cb) { ESP_LOGI(TAG, "STA observer: up"); g_wm_sta_cb(1); }
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

void wifi_mgr_sta_pause(void) {
    if (s_retry) esp_timer_stop(s_retry);
    self_disconnect();
}

void wifi_mgr_sta_resume(void) { sta_connect_now(); }

int wifi_mgr_sta_apply(const hg_mcfg_t *m) {
    esp_err_t err;
    s_step = 0;
    if (s_retry) esp_timer_stop(s_retry);

    if (!m->sta_ssid[0]) {
        /* No house Wi-Fi configured: stop trying, but keep the AP (and the
         * whole greenhouse) running. s_configured goes first so a retry_cb
         * that is already queued on the esp_timer task turns into a no-op,
         * and the stored config is zeroed so even a connect that slips
         * through has no network left to rejoin. */
        s_configured = 0;
        s_want_down_ntf = 0;
        wifi_mgr_lock();
        g_wm.sta_ssid[0] = '\0';
        g_wm.sta_reason[0] = '\0';
        wifi_mgr_unlock();
        self_disconnect();
        if (g_wm_started) {
            wifi_config_t zero;
            memset(&zero, 0, sizeof zero);
            if ((err = esp_wifi_set_config(WIFI_IF_STA, &zero)) != ESP_OK)
                ESP_LOGW(TAG, "clearing the stored STA config failed: %s", esp_err_to_name(err));
        }
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

    self_disconnect();
    if ((err = esp_wifi_set_config(WIFI_IF_STA, &wc)) != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_config(STA) failed: %s", esp_err_to_name(err));
        return -1;
    }
    s_configured = 1;
    s_want_down_ntf = 1;         /* the first failure after this request is worth a NOTIFY */
    /* Formatted outside the lock, copied in with a fixed-size memcpy: the lock
     * is a portMUX critical section and must hold nothing slower than that. */
    char ssid[sizeof g_wm.sta_ssid];
    snprintf(ssid, sizeof ssid, "%s", m->sta_ssid);
    wifi_mgr_lock();
    memcpy(g_wm.sta_ssid, ssid, sizeof g_wm.sta_ssid);
    g_wm.sta_reason[0] = '\0';
    wifi_mgr_unlock();

    /* Two callers, two paths. At boot (g_wm_started still 0) the driver is
     * not started yet, so the join is left to WIFI_EVENT_STA_START, which
     * esp_wifi_start() posts a moment later. From SET WIFI STA (started) the
     * join begins here -- no further STA_START is coming. */
    if (g_wm_started) sta_connect_now();
    return 0;
}
