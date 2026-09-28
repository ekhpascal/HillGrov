#include <stdint.h>
#include "bsp/esp-bsp.h"
#include "panel_lock.h"

bool panel_lock(uint32_t timeout_ms) {
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
