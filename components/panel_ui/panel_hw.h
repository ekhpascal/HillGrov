#pragma once
#include <stdint.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Display, touch and backlight for the Waveshare ESP32-P4-WIFI6-Touch-LCD-7B
 * (EK79007 1024x600 over MIPI-DSI, GT911 on I2C port 1), brought up through
 * the registry BSP 3.0.1 and esp_lvgl_adapter 0.6.4 with SOFT errors
 * everywhere: the greenhouse controller never aborts over its screen.
 * Deliberately NOT bsp_display_start()/_with_config(): those call
 * ESP_ERROR_CHECK(esp_lv_adapter_start()) unconditionally, return NULL for
 * the whole display when only touch failed, and init LEDC twice. */

#define PANEL_HW_W 1024   /* native panel size, the frame pnl_touch_map() clamps to */
#define PANEL_HW_H 600
#define PANEL_LV_PSRAM_POOL (256u * 1024u)   /* LVGL overflow pool in PSRAM, added by panel_hw_start() */

/* The adapter's LVGL worker task name, verified in esp_lvgl_adapter 0.6.4
 * (src/adapter/esp_lv_adapter.c, both xTaskCreatePinnedToCoreWithCaps calls in
 * esp_lv_adapter_start()). "Am I on the LVGL task" checks compare
 * pcTaskGetName(NULL) against this, never an assumed literal. */
#define PANEL_LVGL_TASK_NAME "lvgl"

typedef struct {
    uint8_t  lit, touch_ok, orient;
    uint32_t up_ms;                       /* bring-up time */
    uint32_t reads, read_errs, points;    /* touch reads, failed reads (either rc), reads with >= 1 point */
    int      last_err;                    /* last failing esp_err_t, 0 if none */
    uint16_t last_x, last_y;              /* last mapped point */
} panel_hw_status_t;

/* Brings the display up (and touch, if the GT911 answers). adapter task on
 * core 1 at prio 3, display profile exactly the BSP's, touch mirrors OFF on
 * the handle, and our own read callback installed (both esp_lcd_touch return
 * codes counted, point mapped by pnl_touch_map). A touch failure leaves the
 * display up with touch_ok 0. 0 display up / -1 no display. The backlight
 * stays at 0 until the caller calls panel_hw_brightness(). Call once, from
 * app_main (before the LVGL task exists). */
int  panel_hw_start(uint8_t orient);
void panel_hw_status(panel_hw_status_t *out);   /* [ANY] */
/* [ANY] 1 once panel_hw_start() has returned 0 (adapter started, display
 * registered), else 0. A one-byte read, cheap enough for every panel_lock(). */
int  panel_hw_lit(void);
lv_display_t *panel_hw_display(void);
lv_indev_t   *panel_hw_touch(void);             /* NULL when touch_ok == 0 */

/* 0..100 (clamped) via ledc_set_duty/ledc_update_duty on the BSP's own LEDC
 * channel, duty 1023*pct/100 -- never bsp_display_brightness_set(), which logs
 * at INFO on UART0, the machine-parsed CLI. 0 ok / -1 (display not up, or LEDC
 * refused). */
int  panel_hw_brightness(uint8_t pct);

#ifdef __cplusplus
}
#endif
