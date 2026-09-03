#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "board.h"
#include "hg_store.h"
#include "hg_model.h"
#include "notify.h"
#include "cmd_task.h"
#include "cmd_common.h"
#include "cli.h"
#include "esp_timer.h"
#include "ota_trial.h"
#include "app_if_common.h"
#include "ring_link.h"
#include "zone_ring.h"

static const char *TAG = "hg_main";
extern const app_if_t APP_IF_ZONE;
extern const cmd_entry_t *zone_table(int *n);
static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

void app_main(void) {
    /* step 1 (spec 3.3): outputs stay hardware-safe before anything else */
    gpio_config_t oe = { .pin_bit_mask = 1ULL << HG_GPIO_PCA_OE, .mode = GPIO_MODE_OUTPUT,
                         .pull_up_en = GPIO_PULLUP_ENABLE };
    gpio_config(&oe);
    gpio_set_level(HG_GPIO_PCA_OE, 1);

    ESP_LOGI(TAG, "HillGrow zone %s boot", esp_app_get_description()->version);
    if (hg_store_init() != 0) ESP_LOGE(TAG, "store init failed (defaults active)");
    hg_store_start();

    notify_init(now_ms, hg_store_zid());
    cmd_common_init(&APP_IF_ZONE);
    int n;
    static cmd_core_t core;
    core.table = zone_table(&n); core.table_len = n;
    core.role = CMD_ROLE_ZONE; core.zone_id = hg_store_zid();
    core.now_ms = now_ms;
    core.debug_key = CONFIG_HILLGROW_DEBUG_KEY;
    cmd_task_start(&core);
    cli_init();
    cli_start();

    ota_trial_start(0);   /* Task 10 ordering: before ring start -- zone_ring_start() below fires
                            * ota_trial_drivers_ok() right after, so the probe is still set once */
    uint8_t mac[6];
    hg_app_get_mac(mac);
    ring_link_start(0, hg_store_zid, mac);
    zone_ring_start(&core);
    notify_emit(NTF_BOOT, 0, "%s POWERON", esp_app_get_description()->version);
    vTaskDelete(NULL);
}
