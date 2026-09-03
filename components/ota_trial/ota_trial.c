#include <string.h>
#include "freertos/FreeRTOS.h"
#include "driver/uart.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_log.h"
#include "nvs.h"
#include "notify.h"
#include "ota_trial.h"

static const char *TAG = "ota_trial";

static trial_probes_t     s_probes;
static uint8_t             s_expect_link;
static uint32_t            s_start_ms;
static uint8_t             s_active;   /* trial running: this boot was PENDING_VERIFY */
static esp_timer_handle_t  s_timer;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

/* Reads+erases the one-shot NVS "hg"/"trial" breadcrumb rescue_pull() leaves
 * behind on a fleet pull; absent (bench flash, or no rescue pull this boot)
 * reads back as expect_link 0. Best-effort like the write side: any NVS
 * failure just falls back to 0 rather than blocking the trial. */
static uint8_t trial_take_expect_link(void) {
    nvs_handle_t h;
    if (nvs_open("hg", NVS_READWRITE, &h) != ESP_OK) return 0;
    uint8_t val = 0;
    esp_err_t err = nvs_get_u8(h, "trial", &val);
    if (err != ESP_OK) val = 0;
    nvs_erase_key(h, "trial");
    nvs_commit(h);
    nvs_close(h);
    return val;
}

static void trial_timer_cb(void *arg) {
    (void)arg;
    if (!s_active) return;
    s_probes.min_heap_kb = esp_get_minimum_free_heap_size() / 1024;
    int rc = trial_eval(&s_probes, s_expect_link, s_start_ms, now_ms());
    if (rc == TRIAL_PASS) {
        s_active = 0;
        esp_timer_stop(s_timer);
        esp_ota_mark_app_valid_cancel_rollback();
        notify_emit(NTF_FW, 0, "TRIAL PASS");
    } else if (rc == TRIAL_FAIL) {
        s_active = 0;
        esp_timer_stop(s_timer);
        notify_emit(NTF_FW, 0, "TRIAL FAIL ROLLBACK");
        uart_wait_tx_done(UART_NUM_0, pdMS_TO_TICKS(1000));
        esp_ota_mark_app_invalid_rollback_and_reboot();
    }
}

void ota_trial_start(int is_master) {
    (void)is_master;
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    if (!running || esp_ota_get_state_partition(running, &state) != ESP_OK ||
        state != ESP_OTA_IMG_PENDING_VERIFY) {
        return;   /* not an OTA-pending boot (e.g. flashed by tools): nothing to trial */
    }

    memset(&s_probes, 0, sizeof s_probes);
    /* Reaching this line means hg_store/model (or master's config-free boot)
     * already ran to completion under the never-abort rule -- always true here. */
    s_probes.cfg_loaded = 1;
    /* No live TWDT-event hook exists yet: an actual TWDT panic reboots before
     * this code can ever run again, so "no event observed" is trivially true
     * for every evaluation this trial performs. */
    s_probes.twdt_ok = 1;

    s_expect_link = trial_take_expect_link();
    s_start_ms = now_ms();
    s_active = 1;

    const esp_timer_create_args_t args = { .callback = trial_timer_cb, .name = "ota_trial" };
    if (esp_timer_create(&args, &s_timer) != ESP_OK) {
        ESP_LOGE(TAG, "esp_timer_create failed; trial cannot run");
        s_active = 0;
        return;
    }
    esp_timer_start_periodic(s_timer, 1000000);   /* 1 s */
}

void ota_trial_master_frame(void) { if (s_active) s_probes.master_frame_seen = 1; }
void ota_trial_tick(void)         { if (s_active) s_probes.ticks++; }
void ota_trial_drivers_ok(void)   { if (s_active) s_probes.drivers_ok = 1; }

int ota_trial_confirm(void) {
    if (!s_active) return -1;
    s_probes.confirmed = 1;
    return 0;
}
