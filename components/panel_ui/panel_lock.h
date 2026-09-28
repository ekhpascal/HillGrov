#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The display lock, never ignored. A lv_* call is made in exactly two places:
 * inside an lv_timer/event callback on the LVGL task (the adapter's recursive
 * lock is already held there), or after panel_lock() returned true. Ignoring
 * a failed take panics the board (esp_lv_adapter_lock(751)), and
 * bsp_display_lock(0) means "try ONCE", not "forever" -- so 0 is promoted to 1.
 * Nothing blocking is ever called while it is held; the worker and the poller
 * never take it. panel_lock() is false, without waiting, while the panel is
 * not lit (panel_hw_lit() == 0).
 *
 * A take that times out makes the adapter log E "Failed to acquire LVGL lock"
 * (tag "esp_lvgl:adapter") on UART0. That tag keeps its default level on
 * purpose: every caller takes the lock at boot with 2000 ms, so a timeout is a
 * real fault worth its line; silencing it would need ESP_LOG_NONE, which also
 * hides the adapter's bring-up errors; and uart_test.py already drops log
 * lines (^[IWEDV] \(n\)). */
#define PANEL_LOCK_MS 200u

bool panel_lock(uint32_t timeout_ms);   /* false -> the caller makes NO lv_* call */
void panel_unlock(void);

#ifdef __cplusplus
}
#endif
