#include <stdint.h>
#include "bsp/esp-bsp.h"
#include "panel_hw.h"
#include "panel_lock.h"

bool panel_lock(uint32_t timeout_ms) {
    /* A dark panel has no lock worth taking: esp_lv_adapter_init() may have
     * created the adapter's mutex before a later bring-up step failed, and a
     * true here would license lv_* calls with no display behind them. It also
     * spares UART0 the adapter's "mutex not initialized" E line when init
     * itself failed. */
    if (!panel_hw_lit()) return false;
    /* bsp_display_lock() takes an int32_t where any negative value means "wait
     * forever": 0 is promoted to 1 (0 is "try once") and anything past
     * INT32_MAX is capped, so no caller can wrap into an unbounded wait. */
    if (timeout_ms == 0) timeout_ms = 1u;
    if (timeout_ms > (uint32_t)INT32_MAX) timeout_ms = (uint32_t)INT32_MAX;
    return bsp_display_lock((int32_t)timeout_ms);
}

void panel_unlock(void) {
    bsp_display_unlock();
}
