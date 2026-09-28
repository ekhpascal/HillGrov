#include <stdint.h>
#include "lvgl.h"
#include "pnl_dim_hold.h"

static uint32_t s_until;   /* lv_tick_get() deadline */
static uint8_t  s_on;

void pnl_dim_hold(uint32_t ms) {
    s_on = ms != 0;
    s_until = lv_tick_get() + ms;
}

int pnl_dim_held(uint32_t now_ms) {
    if (!s_on) return 0;
    if ((int32_t)(s_until - now_ms) > 0) return 1;
    s_on = 0;                  /* lapsed: stays released until the next hold, whatever the tick does after a wrap */
    return 0;
}
