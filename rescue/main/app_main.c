#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "board.h"
#include "rescue_handover.h"
#include "rescue.h"

static const char *TAG = "rescue_main";

extern void ring_fwd_start(void);

/* ---- status LED: esp_timer periodic toggle on GPIO2 -------------------- */

typedef enum { LED_BLINK, LED_ERROR } led_mode_t;

static esp_timer_handle_t s_led_timer;
static led_mode_t         s_led_mode;
static int                s_led_level;
static int                s_led_burst_left;

static void led_cb(void *arg) {
    (void)arg;
    if (s_led_mode == LED_ERROR) {
        if (s_led_burst_left > 0) {
            s_led_level ^= 1;
            s_led_burst_left--;
        } else {
            s_led_level = 0;
            esp_timer_stop(s_led_timer);
        }
    } else {
        s_led_level ^= 1;
    }
    gpio_set_level(HG_GPIO_STATUS_LED, s_led_level);
}

static void led_init(void) {
    gpio_config_t cfg = { .pin_bit_mask = 1ULL << HG_GPIO_STATUS_LED, .mode = GPIO_MODE_OUTPUT };
    gpio_config(&cfg);
    const esp_timer_create_args_t args = { .callback = led_cb, .name = "led" };
    esp_timer_create(&args, &s_led_timer);
}

static void led_blink(uint32_t half_period_us) {
    esp_timer_stop(s_led_timer);   /* harmless (ESP_ERR_INVALID_STATE) if not running */
    s_led_mode = LED_BLINK;
    s_led_level = 0;
    gpio_set_level(HG_GPIO_STATUS_LED, 0);
    esp_timer_start_periodic(s_led_timer, half_period_us);
}

static void led_pull(void)   { led_blink(100000); }   /* 5 Hz */
static void led_manual(void) { led_blink(500000); }   /* 1 Hz */

/* Blocking 3-blink error burst; returns once the burst has finished so the
 * caller's next led_* call (or exit) doesn't race the timer callback. */
static void led_error_burst(void) {
    esp_timer_stop(s_led_timer);
    s_led_mode = LED_ERROR;
    s_led_level = 0;
    s_led_burst_left = 6;   /* 6 toggles == 3 on/off blinks */
    gpio_set_level(HG_GPIO_STATUS_LED, 0);
    esp_timer_start_periodic(s_led_timer, 150000);
    vTaskDelay(pdMS_TO_TICKS(150 * 6 + 50));
}

/* ---- boot sequence (spec 1.4/6.1): pull mode, then manual-AP fallback -- */

void app_main(void) {
    led_init();

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) ESP_LOGE(TAG, "nvs_flash_init failed: %d", err);

    ring_fwd_start();

    /* Subscribe the main task once, up front: both the STA-connect/pull
     * retry loop below and the final idle loop are long-running relative to
     * the 30 s TWDT timeout, and rescue_pull()'s own read loop already
     * assumes a subscribed caller (it calls esp_task_wdt_reset() per chunk). */
    esp_task_wdt_add(NULL);

    hg_handover_t h;
    if (hg_handover_take(&h) == 0) {
        ESP_LOGW(TAG, "handover present: pulling from %s", h.url);
        led_pull();
        for (int a = 0; a < 3; a++) {
            esp_task_wdt_reset();
            if (rescue_wifi_sta(h.ssid, h.pass, 20000) == 0 && rescue_pull(h.url) == 0) {
                esp_restart();
            }
            vTaskDelay(pdMS_TO_TICKS(5000));
        }
        ESP_LOGW(TAG, "pull mode exhausted, falling back to manual AP");
        led_error_burst();
    }

    rescue_wifi_ap();
    led_manual();
    rescue_http_start();

    for (;;) {
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}
