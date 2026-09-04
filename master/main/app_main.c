#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "nvs_flash.h"
#include "board.h"
#include "notify.h"
#include "cmd_task.h"
#include "cmd_common.h"
#include "cli.h"
#include "esp_timer.h"
#include "ota_trial.h"
#include "app_if_common.h"
#include "ring_link.h"
#include "node_mgr.h"

static const char *TAG = "hg_main";
extern const app_if_t APP_IF_MASTER;
extern const cmd_entry_t *master_table(int *n);
static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }
static uint8_t  master_id_fn(void) { return 0; }   /* RING_ID_MASTER -- the master's ring id never changes */

void app_main(void) {
    ESP_LOGI(TAG, "HillGrow master %s boot", esp_app_get_description()->version);

    notify_init(now_ms, 0);
    cmd_common_init(&APP_IF_MASTER);
    int n;
    static cmd_core_t core;
    core.table = master_table(&n); core.table_len = n;
    core.role = CMD_ROLE_MASTER; core.zone_id = 0;
    core.now_ms = now_ms;
    core.debug_key = CONFIG_HILLGROW_DEBUG_KEY;
    /* node_mgr_start() runs later in this function, but node_mgr_forward has
     * its own boot guard (node_mgr_fwd.c: RING_DOWN until node_mgr_start()
     * has run) -- setting it here alongside the other core fields, before
     * cmd_task_start(), is safe and keeps this block in one place. */
    core.forward = node_mgr_forward;
    cmd_task_start(&core);
    cli_init();
    cli_start();

    /* ruling #1: erase-retry-once (same pattern as hg_store_init/rescue),
     * BEFORE node_store/ring start -- ztab and the OTA-trial breadcrumb both
     * need the "hg" NVS namespace to exist. */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "nvs partition needs erase (%s), retrying once", esp_err_to_name(err));
        err = nvs_flash_erase();
        if (err == ESP_OK) err = nvs_flash_init();
    }
    if (err != ESP_OK) ESP_LOGE(TAG, "nvs_flash_init failed: %s", esp_err_to_name(err));

    ota_trial_start(1);

    uint8_t mac[6];
    hg_app_get_mac(mac);
    ring_link_start(1, master_id_fn, mac);
    node_mgr_start();

    notify_emit(NTF_BOOT, 0, "%s POWERON", esp_app_get_description()->version);
    vTaskDelete(NULL);
}
