#include <stdint.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "lvgl.h"
#include "panel_hw.h"
#include "panel_lock.h"
#include "pnl_touch.h"
#include "pnl_theme.h"
#include "scr_diag.h"
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

int panel_start(void) {
    int64_t t0 = esp_timer_get_time();
    log_heap("before panel");
    if (panel_hw_start(PNL_ORIENT_NORMAL) != 0) {
        ESP_LOGE(TAG, "no display");
        return -1;
    }
    if (!panel_lock(2000)) {
        ESP_LOGE(TAG, "display lock not taken in 2000 ms -- panel left dark");
        return -1;
    }
    pnl_theme_init(panel_hw_display());
    scr_diag_build(lv_screen_active());
    panel_unlock();
    (void)panel_hw_brightness(80);
    s_lit = 1;

    panel_hw_status_t st;
    panel_hw_status(&st);
    ESP_LOGI(TAG, "up in %u ms (lvgl c1/p3, touch %s)",
             (unsigned)((esp_timer_get_time() - t0) / 1000), st.touch_ok ? "ok" : "UNAVAILABLE");
    log_heap("after panel");
    return 0;
}

int panel_services_start(void) {
    return s_lit ? 0 : -1;   /* the header's contract: -1 when the panel is dark */
}
