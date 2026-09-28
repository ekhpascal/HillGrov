#include <string.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "driver/ledc.h"
#include "lvgl.h"
#include "bsp/esp-bsp.h"
#include "bsp/touch.h"          /* bsp_touch_new(): esp-bsp.h does not include it in BSP 3.0.1 */
#include "esp_lv_adapter.h"
#include "esp_lcd_touch.h"
#include "board.h"
#include "pnl_touch.h"
#include "panel_hw.h"

#if CONFIG_BSP_ERROR_CHECK
#error "CONFIG_BSP_ERROR_CHECK=y turns every BSP failure into an abort of the greenhouse controller (system spec 3.3) -- keep CONFIG_BSP_ERROR_CHECK=n in master/sdkconfig.defaults.esp32p4"
#endif

/* The one translation unit that sees both headers: board.h's P4 I2C names must
 * be the bus the BSP actually drives (the GT911 proves the BSP's). */
_Static_assert(HG_GPIO_I2C_SDA == BSP_I2C_SDA && HG_GPIO_I2C_SCL == BSP_I2C_SCL,
               "board.h HG_GPIO_I2C_* disagree with the BSP's I2C pins");
/* pnl_touch_map() clamps to PANEL_HW_W x PANEL_HW_H; the adapter is given BSP_LCD_*_RES. */
_Static_assert(PANEL_HW_W == BSP_LCD_H_RES && PANEL_HW_H == BSP_LCD_V_RES,
               "PANEL_HW_W/H disagree with the BSP's panel size");

static const char *TAG = "panel_hw";

static lv_display_t          *s_disp;
static lv_indev_t            *s_touch;
static esp_lcd_touch_handle_t s_tp;
static portMUX_TYPE           s_mux = portMUX_INITIALIZER_UNLOCKED;
static panel_hw_status_t      s_st;
static uint32_t               s_log_ms;
static uint8_t                s_logged;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

/* The adapter's custom_touch_read hook (esp_lv_adapter_touch_callbacks_t) --
 * runs on the LVGL task in place of the adapter's own read_data + get_data,
 * whose read_data rc the adapter discards. A failed touch read is otherwise
 * invisible (what_we_learned 2026-09-18), so both return codes are counted and
 * a failure is logged at most once per 5 s (UART0 is the CLI). The points are
 * mapped here; the adapter's own processing after this hook (scale 1.0, last
 * point held on release, a non-OK rc = released) is the bench-proven path. */
static esp_err_t touch_read(esp_lcd_touch_handle_t tp, esp_lcd_touch_point_data_t *pts, uint8_t *count,
                            uint8_t max_count, void *user_ctx) {
    (void)user_ctx;
    uint8_t cnt = 0;
    esp_err_t r_read = esp_lcd_touch_read_data(tp);
    esp_err_t r_get = ESP_OK;
    if (r_read == ESP_OK) r_get = esp_lcd_touch_get_data(tp, pts, &cnt, max_count);
    esp_err_t err = (r_read != ESP_OK) ? r_read : r_get;
    if (err != ESP_OK || cnt > max_count) cnt = 0;
    for (uint8_t i = 0; i < cnt; i++) {
        uint16_t x, y;
        pnl_touch_map(s_st.orient, pts[i].x, pts[i].y, PANEL_HW_W, PANEL_HW_H, &x, &y);
        pts[i].x = x;
        pts[i].y = y;
    }
    *count = cnt;

    portENTER_CRITICAL(&s_mux);
    s_st.reads++;
    if (err != ESP_OK) { s_st.read_errs++; s_st.last_err = (int)err; }
    if (cnt > 0) { s_st.points++; s_st.last_x = pts[0].x; s_st.last_y = pts[0].y; }
    uint32_t errs = s_st.read_errs;
    portEXIT_CRITICAL(&s_mux);

    if (err != ESP_OK) {
        uint32_t t = now_ms();
        if (!s_logged || t - s_log_ms >= 5000) {
            s_logged = 1;
            s_log_ms = t;
            if (r_read != ESP_OK) {
                ESP_LOGW(TAG, "touch read_data failed: %s (%u failed reads so far)",
                         esp_err_to_name(r_read), (unsigned)errs);
            } else {
                ESP_LOGW(TAG, "touch get_data failed: %s (%u failed reads so far)",
                         esp_err_to_name(r_get), (unsigned)errs);
            }
        }
    }
    return err;
}

int panel_hw_start(uint8_t orient) {
    memset(&s_st, 0, sizeof s_st);
    s_st.orient = (orient == PNL_ORIENT_FLIPPED) ? PNL_ORIENT_FLIPPED : PNL_ORIENT_NORMAL;
    uint32_t t0 = now_ms();

    esp_lv_adapter_config_t acfg = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    acfg.task_core_id  = 1;   /* away from ring_rx (c0/p6), cmd_task/httpd (c0/p5) */
    acfg.task_priority = 3;   /* below node_mgr (c1/p4): a long redraw never starves the ring logic */
    esp_err_t rc = esp_lv_adapter_init(&acfg);
    if (rc != ESP_OK) { ESP_LOGE(TAG, "esp_lv_adapter_init: %s", esp_err_to_name(rc)); return -1; }

    /* LVGL 9.5.0 defaults: LV_USE_ASSERT_MALLOC on and LV_ASSERT_HANDLER `while(1);` (lv_conf_internal.h:1491-1496,
     * no Kconfig override). A NULL lv_malloc would spin the LVGL task (c1/p3) for good, starve IDLE1, and the TWDT
     * (IDLE1 watched, 8 s, panic) would reboot the greenhouse controller -- in an OTA trial that retires the image.
     * So the builtin TLSF allocator gets a PSRAM overflow pool on top of its internal CONFIG_LV_MEM_SIZE_KILOBYTES
     * pool: exhaustion then costs speed, not a reset. lv_init() ran inside esp_lv_adapter_init(); the LVGL task does
     * not exist until esp_lv_adapter_start(), so no lock is needed here. Soft on failure, like everything here. */
    void *lv_extra = heap_caps_malloc(PANEL_LV_PSRAM_POOL, MALLOC_CAP_SPIRAM);
    if (lv_extra && lv_mem_add_pool(lv_extra, PANEL_LV_PSRAM_POOL)) {
        ESP_LOGI(TAG, "LVGL pool: %u KB internal + %u KB PSRAM overflow", (unsigned)CONFIG_LV_MEM_SIZE_KILOBYTES,
                 (unsigned)(PANEL_LV_PSRAM_POOL / 1024u));
    } else {
        if (lv_extra) heap_caps_free(lv_extra);
        ESP_LOGW(TAG, "LVGL PSRAM overflow pool unavailable -- LVGL has only its %u KB internal pool",
                 (unsigned)CONFIG_LV_MEM_SIZE_KILOBYTES);
    }

    bsp_lcd_handles_t h;
    memset(&h, 0, sizeof h);
    rc = bsp_display_new_with_handles(NULL, &h);   /* also inits the backlight LEDC, duty 0 */
    if (rc != ESP_OK) { ESP_LOGE(TAG, "bsp_display_new_with_handles: %s", esp_err_to_name(rc)); return -1; }

    /* The adapter's MIPI default yields every field bsp_display_lcd_init() sets (MIPI_DSI, 1024x600,
     * buffer_height 50, use_psram/ppa/double-buffer false, TRIPLE_PARTIAL); the three BSP values are restated
     * so a change to the adapter's defaults cannot silently move them. te_sync comes out disabled (gpio -1),
     * where the BSP leaves it zeroed -- the adapter reads it only in TE_SYNC mode. */
    esp_lv_adapter_rotation_t rot = (s_st.orient == PNL_ORIENT_FLIPPED) ? ESP_LV_ADAPTER_ROTATE_0
                                                                        : ESP_LV_ADAPTER_ROTATE_180;
    esp_lv_adapter_display_config_t dcfg =
        ESP_LV_ADAPTER_DISPLAY_MIPI_DEFAULT_CONFIG(h.panel, h.io, BSP_LCD_H_RES, BSP_LCD_V_RES, rot);
    dcfg.profile.buffer_height = 50;       /* the BSP's own profile: 1024x50x2 B internal draw buffer */
    dcfg.profile.use_psram     = false;
    dcfg.tear_avoid_mode       = ESP_LV_ADAPTER_TEAR_AVOID_MODE_TRIPLE_PARTIAL;
    s_disp = esp_lv_adapter_register_display(&dcfg);
    if (!s_disp) { ESP_LOGE(TAG, "esp_lv_adapter_register_display failed"); return -1; }

    rc = bsp_touch_new(NULL, &s_tp);
    if (rc == ESP_OK && s_tp) {
        /* bsp_touch_new(NULL) turns BOTH mirrors on; with the display rotated
         * 180 that flips touch twice (the spike's bug). Off, all three. */
        esp_lcd_touch_set_swap_xy(s_tp, false);
        esp_lcd_touch_set_mirror_x(s_tp, false);
        esp_lcd_touch_set_mirror_y(s_tp, false);
        esp_lv_adapter_touch_config_t tcfg = ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(s_disp, s_tp);
        tcfg.callbacks.custom_touch_read = touch_read;   /* ours: counts both rcs, clamps, maps orientation */
        s_touch = esp_lv_adapter_register_touch(&tcfg);
        if (s_touch) {
            s_st.touch_ok = 1;
        } else {
            ESP_LOGE(TAG, "esp_lv_adapter_register_touch failed -- display only");
        }
    } else {
        ESP_LOGE(TAG, "bsp_touch_new: %s -- display only (GT911 not answering?)", esp_err_to_name(rc));
        s_tp = NULL;
    }

    rc = esp_lv_adapter_start();
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "esp_lv_adapter_start: %s", esp_err_to_name(rc));
        s_disp = NULL;
        s_touch = NULL;
        s_st.touch_ok = 0;
        return -1;
    }
    s_st.lit = 1;
    s_st.up_ms = now_ms() - t0;
    return 0;
}

void panel_hw_status(panel_hw_status_t *out) {
    portENTER_CRITICAL(&s_mux);
    *out = s_st;
    portEXIT_CRITICAL(&s_mux);
}

/* s_st.lit is written once, by panel_hw_start() on the app_main task, before
 * any other task can call this; a byte load needs no critical section. */
int panel_hw_lit(void) { return s_st.lit; }

lv_display_t *panel_hw_display(void) { return s_disp; }
lv_indev_t   *panel_hw_touch(void)   { return s_st.touch_ok ? s_touch : NULL; }

int panel_hw_brightness(uint8_t pct) {
    if (!s_st.lit) return -1;
    if (pct > 100) pct = 100;
    uint32_t duty = 1023u * pct / 100u;
    ledc_channel_t ch = (ledc_channel_t)CONFIG_BSP_DISPLAY_BRIGHTNESS_LEDC_CH;
    if (ledc_set_duty(LEDC_LOW_SPEED_MODE, ch, duty) != ESP_OK) return -1;
    if (ledc_update_duty(LEDC_LOW_SPEED_MODE, ch) != ESP_OK) return -1;
    return 0;
}
