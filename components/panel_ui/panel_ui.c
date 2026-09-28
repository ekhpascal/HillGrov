#include <stdint.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "lvgl.h"
#include "panel_hw.h"
#include "panel_lock.h"
#include "pnl_touch.h"
#include "pnl_theme.h"
#include "pnl_worker.h"
#include "pnl_poll.h"
#include "pnl_cmd.h"
#include "pnl_prefs_nvs.h"
#include "scr_shell.h"
#include "pnl_idle.h"
#include "panel_ui.h"

static const char *TAG = "panel";
static uint8_t s_lit;

/* Internal RAM is the pool that can run out on the P4 (PSRAM hides it from
 * esp_get_minimum_free_heap_size()); every gate records these two numbers. */
static void log_heap(const char *when) {
    ESP_LOGI(TAG, "internal heap %s: free %u, min %u", when,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
}

/* [LVGL] 1 s: proves the LVGL task is still turning (the poller watches it). */
static void heartbeat_cb(lv_timer_t *t) {
    (void)t;
    pnl_lvgl_heartbeat();
}

int panel_start(void) {
    int64_t t0 = esp_timer_get_time();
    log_heap("before panel");
    pnl_prefs_load();         /* Task 26: NVS is up (panel_start runs after nvs_flash_init in app_main) */
    if (panel_hw_start(pnl_prefs_get()->orient) != 0) {
        ESP_LOGE(TAG, "no display");
        return -1;
    }
    if (!panel_lock(2000)) {
        ESP_LOGE(TAG, "display lock not taken in 2000 ms -- panel left dark");
        return -1;
    }
    pnl_theme_init(panel_hw_display());
    pnl_shell_start();
    pnl_idle_start();          /* Task 27: dimming + idle wipe; the shell must exist before it can navigate */
    panel_unlock();
    (void)panel_hw_brightness(pnl_prefs_get()->dim.day_pct);
    s_lit = 1;

    panel_hw_status_t st;
    panel_hw_status(&st);
    ESP_LOGI(TAG, "up in %u ms (lvgl c1/p3, touch %s)",
             (unsigned)((esp_timer_get_time() - t0) / 1000), st.touch_ok ? "ok" : "UNAVAILABLE");
    log_heap("after panel");
    return 0;
}

int panel_services_start(void) {
    if (!s_lit) return -1;   /* a dark panel runs no worker or poller */
    pnl_cmd_init();          /* Task 22: the panel's own command sessions (a dark panel allocates none) */
    pnl_worker_start();
    pnl_poll_start();
    if (panel_lock(2000)) {
        (void)lv_timer_create(heartbeat_cb, 1000, NULL);
        panel_unlock();
    } else {
        ESP_LOGE(TAG, "display lock not taken -- no LVGL heartbeat, a frozen panel would go unreported");
    }
    return 0;
}
