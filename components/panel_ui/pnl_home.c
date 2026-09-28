#include <stdio.h>
#include <string.h>
#include "pnl_home.h"

int pnl_band_layout(int n, int width, pnl_band_t *out) {
    if (!out || n < 1 || n > HG_MAX_ZONES || width <= 0) return -1;
    int w = (width - (n - 1) * PNL_BAND_GAP) / n;
    if (w > PNL_TILE_MAX_W) w = PNL_TILE_MAX_W;
    out->tile_w = w;
    out->gap = PNL_BAND_GAP;
    out->x0 = (width - (n * w + (n - 1) * PNL_BAND_GAP)) / 2;
    out->detail = w < 140 ? PNL_TILE_MIN : (w < 200 ? PNL_TILE_MID : PNL_TILE_FULL);
    return 0;
}

int pnl_in_daily_window(int start_min, int end_min, int now_min) {
    if (start_min < end_min) return now_min >= start_min && now_min < end_min;
    if (start_min > end_min) return now_min >= start_min || now_min < end_min;
    return 0;
}

void pnl_sched_reset(pnl_sched_t *s) { memset(s, 0, sizeof *s); }

void pnl_sched_add_zone(pnl_sched_t *s, const hg_zone_cfg_t *cfg, const hg_zone_hw_t *hw_or_null) {
    int ns = hw_or_null ? hw_or_null->shelf_count : HG_MAX_SHELVES;
    if (ns > HG_MAX_SHELVES) ns = HG_MAX_SHELVES;
    for (int i = 0; i < ns; i++) {
        const hg_shelf_cfg_t *sh = &cfg->shelf[i];
        if (!sh->enabled) continue;
        if (s->n_light < PNL_SCHED_MAX) {
            s->light_on[s->n_light]  = sh->light.on_min;
            s->light_off[s->n_light] = sh->light.off_min;
            s->n_light++;
        }
        if (sh->water.mode == 1 && s->n_water < PNL_SCHED_MAX) {
            s->water_start[s->n_water] = sh->water.win_start_min;
            s->water_end[s->n_water]   = sh->water.win_end_min;
            s->n_water++;
        }
    }
}

static int until(int target, int now) { return (target - now + 1440) % 1440; }

int pnl_lights_on_now(const pnl_sched_t *s, int now) {
    for (int i = 0; i < s->n_light; i++)
        if (pnl_in_daily_window(s->light_on[i], s->light_off[i], now)) return 1;
    return 0;
}

int pnl_ctx_line(const pnl_sched_t *s, const pnl_local_t *t, char *out, size_t cap) {
    if (!t || !t->valid) return snprintf(out, cap, "Clock not set");
    int now = t->minute_of_day;

    int best = -1;
    for (int i = 0; i < s->n_light; i++) {
        if (!pnl_in_daily_window(s->light_on[i], s->light_off[i], now)) continue;
        int d = until(s->light_off[i], now);
        if (best < 0 || d < best) best = d;
    }
    if (best >= 0) {
        if (best >= 60) return snprintf(out, cap, "Lights off in %dh %dm", best / 60, best % 60);
        return snprintf(out, cap, "Lights off in %dm", best);
    }

    int on_at = -1;
    best = -1;
    for (int i = 0; i < s->n_light; i++) {
        if (s->light_on[i] == s->light_off[i]) continue;
        int d = until(s->light_on[i], now);
        if (best < 0 || d < best) { best = d; on_at = s->light_on[i]; }
    }
    if (on_at >= 0) return snprintf(out, cap, "Lights on at %02d:%02d", on_at / 60, on_at % 60);

    for (int i = 0; i < s->n_water; i++)
        if (s->water_start[i] == s->water_end[i]) return snprintf(out, cap, "Watering on demand");

    int end_at = -1;
    best = -1;
    for (int i = 0; i < s->n_water; i++) {
        if (!pnl_in_daily_window(s->water_start[i], s->water_end[i], now)) continue;
        int d = until(s->water_end[i], now);
        if (best < 0 || d < best) { best = d; end_at = s->water_end[i]; }
    }
    if (end_at >= 0) return snprintf(out, cap, "Watering window open until %02d:%02d", end_at / 60, end_at % 60);

    int start_at = -1;
    best = -1;
    for (int i = 0; i < s->n_water; i++) {
        int d = until(s->water_start[i], now);
        if (best < 0 || d < best) { best = d; start_at = s->water_start[i]; }
    }
    if (start_at >= 0) return snprintf(out, cap, "Next watering %02d:%02d", start_at / 60, start_at % 60);

    if (cap) out[0] = '\0';
    return 0;
}
