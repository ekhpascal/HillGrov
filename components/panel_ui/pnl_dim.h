#pragma once
/* Pure (host-tested, tests/host/test_pnl_dim.c): is it night, and what brightness? */
#include <stdint.h>
#include "pnl_prefs.h"   /* pnl_dim_cfg_t */
#include "pnl_home.h"    /* pnl_sched_t, pnl_lights_on_now, pnl_in_daily_window */
#include "pnl_time.h"    /* pnl_local_t */

#ifdef __cplusplus
extern "C" {
#endif

#define PNL_DIM_MIN_PCT 5   /* the backlight floor: pct is clamped to PNL_DIM_MIN_PCT..100 (a dark panel stays findable) */

typedef enum { PNL_DIM_OFF = 0, PNL_DIM_FOLLOW_LIGHTS, PNL_DIM_FIXED } pnl_dim_mode_t;
typedef struct { uint8_t night; uint8_t pct; } pnl_dim_out_t;
void pnl_dim_eval(const pnl_dim_cfg_t *c, const pnl_sched_t *s, const pnl_local_t *t, uint32_t idle_ms, pnl_dim_out_t *out);
     /* night: OFF never; FIXED: t->valid && now in [start,end) (wraps; start == end is empty -- pnl_in_daily_window,
        ruling C13); FOLLOW_LIGHTS: t->valid && s->n_light > 0 && !pnl_lights_on_now. An unset clock (!t->valid) is
        never night. pct = (night && idle_ms >= c->idle_s*1000) ? c->night_pct : c->day_pct, clamped to
        PNL_DIM_MIN_PCT..100 */

#ifdef __cplusplus
}
#endif
