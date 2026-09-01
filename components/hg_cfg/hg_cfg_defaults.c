#include <string.h>
#include "hg_cfg.h"

void hg_defaults_hw(hg_zone_hw_t *hw) {
    static const uint8_t soil_gpio[8] = { 32, 33, 34, 35, 36, 39, 25, 26 };
    memset(hw, 0, sizeof *hw);
    hw->shelf_count = 4;
    hw->pca_addr = 0x40;
    hw->pcf_addr = 0x20;
    hw->soil_backend = 0;
    hw->pcf_active_low_mask = 0xFFFF;
    hw->pca_freq_hz = 1000;
    memcpy(hw->soil_gpio, soil_gpio, 8);
    for (int i = 0; i < HG_MAX_AUX; i++) { hw->aux[i].type = 0; hw->aux[i].pin = HG_NONE; }
    for (int i = 0; i < HG_MAX_SHELVES; i++) {
        hg_shelf_hw_t *s = &hw->shelf[i];
        s->led_ch[0] = (uint8_t)(2 * i);
        s->led_ch[1] = (uint8_t)(2 * i + 1);
        s->pump_pin  = (uint8_t)i;
        s->fan_pin   = (uint8_t)(4 + i);
        s->soil_ch[0] = (uint8_t)(2 * i);
        s->soil_ch[1] = (uint8_t)(2 * i + 1);
        s->led_max_pct[0] = s->led_max_pct[1] = 100;
        s->soil_dry_mv[0] = s->soil_dry_mv[1] = 2800;
        s->soil_wet_mv[0] = s->soil_wet_mv[1] = 1300;
        s->soil_min_ok_mv = 300;
        s->soil_max_ok_mv = 3200;
        s->pump_max_run_s   = 60;
        s->pump_max_daily_s = 600;
        s->vib_ch = (uint8_t)(8 + i);
    }
}

void hg_defaults_cfg(hg_zone_cfg_t *cfg) {
    memset(cfg, 0, sizeof *cfg);
    cfg->source = HG_SRC_LOCAL;
    cfg->link_loss_timeout_s = 30;
    strcpy(cfg->name, "zone");
    for (int i = 0; i < HG_MAX_AUX; i++) {
        cfg->aux[i].mode = 0;
        cfg->aux[i].pulse_s = 1;
        cfg->aux[i].interval_min = 5;
        cfg->aux[i].start_min = 0;
        cfg->aux[i].end_min = 0;
    }
    for (int i = 0; i < HG_MAX_SHELVES; i++) {
        hg_shelf_cfg_t *s = &cfg->shelf[i];
        s->enabled = 0;
        s->light.on_min = 6 * 60;
        s->light.off_min = 22 * 60;
        s->light.white_pct = 50;
        s->light.red_pct = 30;
        s->water.mode = 1;
        s->water.target_pct = 45;
        s->water.hyst_pct = 5;
        s->water.settle_min = 10;
        s->water.dose_s = 20;
        s->water.min_interval_min = 120;
        s->water.max_doses_day = 6;
        s->water.diff_max_pct = 15;
        s->fan.mode = 3;
        s->fan.on_min = 15;
        s->fan.period_min = 60;
        s->vib.mode = 0;
        s->vib.intensity_pct = 60;
        s->vib.pulse_s = 5;
        s->vib.interval_min = 120;
        s->vib.start_min = 10 * 60;
        s->vib.end_min = 16 * 60;
    }
}
