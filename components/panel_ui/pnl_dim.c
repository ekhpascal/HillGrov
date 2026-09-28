#include "pnl_dim.h"

void pnl_dim_eval(const pnl_dim_cfg_t *c, const pnl_sched_t *s, const pnl_local_t *t, uint32_t idle_ms, pnl_dim_out_t *out) {
    if (!c || !out) return;
    uint8_t night = 0;
    if (t && t->valid) {                       /* the clock rule: an unset clock never dims by schedule */
        if (c->mode == PNL_DIM_FIXED)
            night = (uint8_t)pnl_in_daily_window(c->fixed_start_min, c->fixed_end_min, t->minute_of_day);
        else if (c->mode == PNL_DIM_FOLLOW_LIGHTS)
            night = (uint8_t)(s && s->n_light > 0 && !pnl_lights_on_now(s, t->minute_of_day));
    }
    uint8_t pct = (night && idle_ms >= (uint32_t)c->idle_s * 1000u) ? c->night_pct : c->day_pct;
    if (pct < PNL_DIM_MIN_PCT) pct = PNL_DIM_MIN_PCT;
    if (pct > 100) pct = 100;
    out->night = night;
    out->pct = pct;
}
