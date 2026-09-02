#include <string.h>
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_ota_ops.h"
#include "esp_task_wdt.h"
#include "rescue.h"

static const char *TAG = "pull";

const esp_partition_t *rescue_target_slot(void) {
    const esp_partition_t *boot = esp_ota_get_boot_partition();   /* the app that requested rescue */
    const esp_partition_t *ota0 = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
    const esp_partition_t *ota1 = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, NULL);
    if (!ota0 || !ota1) return NULL;
    if (boot && boot->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0) return ota1;
    return ota0;   /* boot==ota_1, factory, or undefined -> ota_0 */
}

int rescue_pull(const char *url) {
    const esp_partition_t *dst = rescue_target_slot();
    if (!dst) return -1;
    esp_http_client_config_t cfg = { .url = url, .timeout_ms = 10000 };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) return -1;
    int rc = -1;
    esp_ota_handle_t ota = 0;
    /* review-round deviation from the verbatim brief (fix round 1, IMPORTANT
     * 3b): esp_http_client_open()+fetch_headers() can together block up to
     * ~20 s on a marginal AP, on top of the ~20 s rescue_wifi_sta() connect
     * this runs after -- against a 30 s TWDT with PANIC=y that risks a panic
     * mid-retry before the read loop's own reset below ever runs, losing an
     * already-consumed handover record silently. Two resets, no other change. */
    esp_task_wdt_reset();
    if (esp_http_client_open(cli, 0) == ESP_OK) {
        esp_task_wdt_reset();
        int64_t len = esp_http_client_fetch_headers(cli);
        int status = esp_http_client_get_status_code(cli);
        ESP_LOGW(TAG, "GET %s -> %d, %lld bytes -> %s", url, status, (long long)len, dst->label);
        if (status == 200 && len > 0 && len <= (int64_t)dst->size &&
            esp_ota_begin(dst, (size_t)len, &ota) == ESP_OK) {
            static char buf[4096];
            int n;
            rc = 0;
            while ((n = esp_http_client_read(cli, buf, sizeof buf)) > 0) {
                esp_task_wdt_reset();
                if (esp_ota_write(ota, buf, (size_t)n) != ESP_OK) { rc = -1; break; }
            }
            if (n < 0) rc = -1;
            if (rc == 0 && esp_ota_end(ota) != ESP_OK) rc = -1;
            else if (rc != 0) esp_ota_abort(ota);
            if (rc == 0 && esp_ota_set_boot_partition(dst) != ESP_OK) rc = -1;
        }
    }
    esp_http_client_cleanup(cli);
    return rc;
}
