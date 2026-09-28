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
 * never take it. */
#define PANEL_LOCK_MS 200u

bool panel_lock(uint32_t timeout_ms);   /* false -> the caller makes NO lv_* call */
void panel_unlock(void);

#ifdef __cplusplus
}
#endif
