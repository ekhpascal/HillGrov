#include <stdlib.h>
#include <string.h>
#include "esp_wifi.h"
#include "esp_log.h"
#include "wifi_mgr_priv.h"

static const char *TAG = "wifi_mgr_scan";

#define SCAN_MAX_AP  24     /* records pulled from the driver per scan */

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
    if (err == ESP_ERR_WIFI_STATE) {
        /* IDF refuses a scan while a connect attempt is in flight, so during a
         * failing reconnect ladder -- exactly the moment someone opens the
         * provisioning page to pick an SSID -- this is the common case, not a
         * rare one. Park the STA (no NOTIFY, no ladder step: the disconnect is
         * marked as ours) and try once more, then put the join back. */
        ESP_LOGI(TAG, "scan refused while connecting; parking the STA and retrying once");
        wifi_mgr_sta_pause();
        err = esp_wifi_scan_start(&sc, true);
        /* Resumed here rather than after the records are read: the blocking
         * scan has already finished and only another scan_start or
         * esp_wifi_clear_ap_list() invalidates the driver's list, so this is
         * safe and keeps the STA parked for as little time as possible. */
        wifi_mgr_sta_resume();
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_scan_start failed: %s", esp_err_to_name(err));
        return -1;
    }
    uint16_t found = 0;
    if (esp_wifi_scan_get_ap_num(&found) != ESP_OK) {
        esp_wifi_clear_ap_list();   /* the records are ours to release even when we can't count them */
        return -1;
    }
    if (found == 0) return 0;

    uint16_t want = found < SCAN_MAX_AP ? found : SCAN_MAX_AP;
    wifi_ap_record_t *recs = calloc(want, sizeof *recs);
    if (!recs) {
        esp_wifi_clear_ap_list();
        ESP_LOGE(TAG, "scan: out of memory for %u records", (unsigned)want);
        return -1;
    }
    if (esp_wifi_scan_get_ap_records(&want, recs) != ESP_OK) {
        esp_wifi_clear_ap_list();
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
