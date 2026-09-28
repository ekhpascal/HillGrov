#include <stdint.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_task_wdt.h"
#include "esp_system.h"       /* esp_get_free_heap_size */
#include "esp_log.h"
#include "node_mgr.h"         /* node_mgr_fw_status */
#include "fw_srv.h"           /* fw_srv_writer_claim / release */
#include "psvc_fleet.h"       /* psvc_fleet_idle */
#include "psvc_fw.h"

static const char *TAG = "psvc_fw";

/* ---- the ONE install claim (http_upload.c's s_busy, moved): test-and-set under a spinlock ---- */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static uint8_t      s_busy;

static int claim(void) {
    int got = 0;
    portENTER_CRITICAL(&s_mux);
    if (!s_busy) { s_busy = 1; got = 1; }
    portEXIT_CRITICAL(&s_mux);
    return got;
}

static void release(void) {
    portENTER_CRITICAL(&s_mux);
    s_busy = 0;
    portEXIT_CRITICAL(&s_mux);
}

int psvc_fw_busy(void) {
    portENTER_CRITICAL(&s_mux);
    int b = s_busy;
    portEXIT_CRITICAL(&s_mux);
    return b;
}

static int fleet_idle(void) {
    char f[40] = "";
    node_mgr_fw_status(f, sizeof f);
    return psvc_fleet_idle(f);
}

/* The same figure the web guard used (http_upload.c's LOW_HEAP_B check), kept so the web's 503 LOW_HEAP answers exactly
 * as before. It includes PSRAM on the P4, so it never trips there (D22 follow-up for the owner: an internal-RAM guard,
 * heap_caps_get_free_size(MALLOC_CAP_INTERNAL), would be the honest one). */
static uint32_t heap_free(void) { return esp_get_free_heap_size(); }

/* Subscribe for the flash loop only, and only when not already subscribed: deleting a subscription someone else holds
 * would silently unwatch that task (map-svc 1, the httpd TWDT note). Token 1 = we added it and must delete it. */
static int wdt_begin(void) {
    if (esp_task_wdt_status(NULL) == ESP_OK) return 0;
    if (esp_task_wdt_add(NULL) == ESP_OK) return 1;
    ESP_LOGW(TAG, "esp_task_wdt_add failed -- this install runs unwatched");
    return 0;
}

void psvc_fw_wdt_kick(void) {
    if (esp_task_wdt_status(NULL) == ESP_OK) esp_task_wdt_reset();
}

static void wdt_end(int token) { if (token) esp_task_wdt_delete(NULL); }

static void yield(void) { vTaskDelay(1); }   /* KraftWerk lesson: let Wi-Fi and IDLE run during a flash burst */

static int zone_fw_claim(void) { return fw_srv_writer_claim(); }
static void zone_fw_release(void) { fw_srv_writer_release(); }

static const psvc_fw_env_t ENV = {
    .claim = claim, .release = release, .fleet_idle = fleet_idle, .heap_free = heap_free,
    .wdt_begin = wdt_begin, .wdt_kick = psvc_fw_wdt_kick, .wdt_end = wdt_end, .yield = yield,
    .zone_fw_claim = zone_fw_claim, .zone_fw_release = zone_fw_release,
    .self_chip = CONFIG_IDF_FIRMWARE_CHIP_ID,   /* 0x0012 on the P4 (build_p4/sdkconfig), 0x0000 on the ESP32 */
};

const psvc_fw_env_t *psvc_fw_env_default(void) { return &ENV; }

psvc_rc_t psvc_fw_install(psvc_fw_kind_t kind, size_t len, psvc_fw_read_fn rd, void *src,
                          psvc_fw_result_t *res, psvc_fw_stats_t *st) {
    return psvc_fw_install_with(&ENV, kind == PSVC_FW_MASTER ? psvc_fw_sink_master() : psvc_fw_sink_zone(),
                                kind, len, rd, src, res, st);
}
