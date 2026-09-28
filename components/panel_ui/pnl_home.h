#pragma once
#include <stddef.h>
#include <stdint.h>
#include "hg_cfg_types.h"   /* HG_MAX_ZONES, HG_MAX_SHELVES, hg_zone_cfg_t, hg_zone_hw_t */
#include "pnl_time.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Home-screen maths (pure). */

typedef enum { PNL_TILE_MIN = 0, PNL_TILE_MID, PNL_TILE_FULL } pnl_tile_detail_t;
     /* MIN: id + dot + soil; MID: name + dot + soil; FULL: name + dot + soil + light */
typedef struct { int tile_w, gap, x0; pnl_tile_detail_t detail; } pnl_band_t;

#define PNL_BAND_WIDTH 976   /* 1024 - 2 x 24 margin */
#define PNL_BAND_GAP   8
#define PNL_TILE_MAX_W 240

/* 0 / -1 (n outside 1..HG_MAX_ZONES or width <= 0). tile_w =
 * min(PNL_TILE_MAX_W, (width - (n-1)*gap) / n), the row centred (x0);
 * tile_w < 140 -> MIN, < 200 -> MID, else FULL (8 tiles -> 115 px MIN;
 * 1 tile -> 240 px FULL). */
int pnl_band_layout(int n_tiles, int width, pnl_band_t *out);

/* Controller ruling C13: the ONE "is time t inside a daily on/off window"
 * test in the panel -- exported here (rather than kept static) because
 * Task 27's night dimming reuses it for the light schedule instead of
 * writing a second copy. 1 when now_min falls in [start_min, end_min):
 * start<end is a normal same-day window; start>end wraps past midnight
 * (on<=now<1440 or 0<=now<off); start==end is NEVER inside -- a caller that
 * wants "always open" for an equal start/end (e.g. watering's "equal =
 * always", hg_water_cfg_t) checks that case itself before calling this, the
 * way pnl_ctx_line() does below. */
int pnl_in_daily_window(int start_min, int end_min, int now_min);

#define PNL_SCHED_MAX (HG_MAX_ZONES * HG_MAX_SHELVES)
typedef struct {
    uint16_t light_on[PNL_SCHED_MAX], light_off[PNL_SCHED_MAX]; int n_light;   /* enabled shelves of enrolled zones */
    uint16_t water_start[PNL_SCHED_MAX], water_end[PNL_SCHED_MAX]; int n_water;/* WATER.MODE AUTO shelves */
} pnl_sched_t;

void pnl_sched_reset(pnl_sched_t *s);
/* Adds shelves i < (hw ? hw->shelf_count : HG_MAX_SHELVES) with enabled != 0;
 * a shelf's watering window only when WATER.MODE is AUTO (1). Bounded by
 * PNL_SCHED_MAX. */
void pnl_sched_add_zone(pnl_sched_t *s, const hg_zone_cfg_t *cfg, const hg_zone_hw_t *hw_or_null);

/* 1 when any light is on at minute_of_day: on<off: on<=now<off; on>off wraps
 * midnight; on==off (the validator forbids it) never. */
int  pnl_lights_on_now(const pnl_sched_t *s, int minute_of_day);

/* !valid -> "Clock not set"; a light on -> "Lights off in 2h 14m" (earliest
 * off; "Lights off in 14m" under an hour); else a light schedule -> "Lights on
 * at 06:00" (soonest on); else watering: "Watering on demand" when a window
 * has start == end (always open), "Watering window open until 18:00" inside a
 * window, else "Next watering 06:00"; else "" (returns 0). */
int  pnl_ctx_line(const pnl_sched_t *s, const pnl_local_t *t, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
