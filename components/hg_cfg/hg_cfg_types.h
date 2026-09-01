#pragma once
#include <stdint.h>
#include <assert.h>
#ifdef __cplusplus
extern "C" {
#endif

#define HG_MAX_SHELVES  4
#define HG_MAX_ZONES    8
#define HG_MAX_AUX      2
#define HG_NONE         0xFF
#define HG_NODE_MASTER      0
#define HG_NODE_UNASSIGNED  254
#define HG_NODE_BCAST       255

#define HG_MAGIC_HW     0x57484748u /* 'HGHW' LE */
#define HG_MAGIC_CFG    0x46434748u /* 'HGCF' LE */
#define HG_MAGIC_DAILY  0x59444748u /* 'HGDY' LE */
#define HG_HW_VER       1
#define HG_HW_VER_MIN   1
#define HG_CFG_VER      1
#define HG_CFG_VER_MIN  1

enum { HG_SRC_LOCAL = 0, HG_SRC_MASTER = 1 };

/* ---------- HARDWARE plane (Zone NVS "hw") ---------- */
typedef struct {                     /* 32 B */
    uint8_t  led_ch[2];              /* 0  PCA9685 ch WHITE, RED; HG_NONE = absent */
    uint8_t  pump_pin, fan_pin;      /* 2  PCF8575 pin; HG_NONE = absent */
    uint8_t  soil_ch[2];             /* 4  logical soil channel A,B -> soil_gpio[]; HG_NONE */
    uint8_t  led_max_pct[2];         /* 6  SAFETY duty cap 0..100 */
    uint16_t soil_dry_mv[2];         /* 8  calibration, mV */
    uint16_t soil_wet_mv[2];         /* 12 */
    uint16_t soil_min_ok_mv;         /* 16 plausibility window */
    uint16_t soil_max_ok_mv;         /* 18 */
    uint16_t pump_max_run_s;         /* 20 SAFETY 1..300 */
    uint16_t pump_max_daily_s;       /* 22 SAFETY 1..3600 */
    uint8_t  vib_ch;                 /* 24 PCA9685 channel of the shelf vibrator; HG_NONE = absent */
    uint8_t  rsvd[7];                /* 25 */
} hg_shelf_hw_t;

typedef struct { uint8_t type, pin, shelf_mask, rsvd; } hg_aux_hw_t; /* 4 B; type: 0 NONE, 1 VIBRATOR, 2 RELAY */

typedef struct {                     /* 160 B */
    uint8_t  shelf_count;            /* 0  1..4 (0 = hw fault, no outputs) */
    uint8_t  pca_addr, pcf_addr;     /* 1  0x40 / 0x20; 0 = absent */
    uint8_t  soil_backend;           /* 3  0 internal ADC, 1 ADS1115 */
    uint16_t pcf_active_low_mask;    /* 4  default 0xFFFF (PCF8575 powers up HIGH) */
    uint16_t pca_freq_hz;            /* 6  default 1000 */
    uint8_t  soil_gpio[8];           /* 8  logical ch -> GPIO {32,33,34,35,36,39,25,26} */
    hg_aux_hw_t aux[HG_MAX_AUX];     /* 16 */
    uint8_t  rsvd[8];                /* 24 */
    hg_shelf_hw_t shelf[HG_MAX_SHELVES]; /* 32..159 */
} hg_zone_hw_t;

/* ---------- LOGICAL plane (Master NVS "zcN", Zone NVS "cfg") ---------- */
typedef struct {                     /* 12 B */
    uint16_t on_min, off_min;        /* minutes since midnight */
    uint8_t  white_pct, red_pct, ramp_min, rsvd;
    uint16_t dli_x10;                /* 0 = off */
    uint8_t  rsvd2[2];
} hg_light_cfg_t;

typedef struct {                     /* 16 B */
    uint8_t  mode;                   /* 0 OFF, 1 AUTO */
    uint8_t  target_pct, hyst_pct, settle_min;
    uint16_t dose_s, min_interval_min;
    uint8_t  max_doses_day, diff_max_pct;
    uint16_t win_start_min, win_end_min; /* equal = always */
    uint8_t  rsvd[2];
} hg_water_cfg_t;

typedef struct { uint8_t mode, on_min; uint16_t period_min; } hg_fan_cfg_t;  /* 4 B; mode: 0 OFF 1 ON 2 LIGHT 3 CYCLE */
typedef struct {                     /* 16 B; per-shelf pollination */
    uint8_t  mode, intensity_pct, pulse_s, rsvd;   /* mode: 0 OFF 1 PULSE; intensity 20..100 */
    uint16_t interval_min, start_min, end_min;
    uint8_t  rsvd2[6];
} hg_vib_cfg_t;
typedef struct { uint8_t mode, pulse_s; uint16_t interval_min, start_min, end_min; } hg_aux_cfg_t; /* 8 B; mode: 0 OFF 1 PULSE */

typedef struct {                     /* 72 B */
    char     crop[16];               /* 0 */
    uint8_t  enabled, profile_id;    /* 16 */
    uint8_t  rsvd[2];                /* 18 */
    uint32_t rsvd_mask;              /* 20 */
    hg_light_cfg_t light;            /* 24 */
    hg_water_cfg_t water;            /* 36 */
    hg_fan_cfg_t   fan;              /* 52 */
    hg_vib_cfg_t   vib;              /* 56 */
} hg_shelf_cfg_t;

typedef struct {                     /* 336 B */
    uint32_t generation;             /* 0  0 = never written */
    uint8_t  source;                 /* 4  HG_SRC_* of last writer */
    uint8_t  rsvd0;                  /* 5 */
    uint16_t link_loss_timeout_s;    /* 6  10..600, default 30 */
    char     name[16];               /* 8 */
    hg_aux_cfg_t aux[HG_MAX_AUX];    /* 24 */
    uint8_t  rsvd[8];                /* 40 */
    hg_shelf_cfg_t shelf[HG_MAX_SHELVES]; /* 48..335 */
} hg_zone_cfg_t;

/* ---------- daily safety counters (RTC_NOINIT + NVS "daily") ---------- */
typedef struct {                     /* 64 B */
    uint32_t magic;                  /* HG_MAGIC_DAILY */
    uint32_t unix_time;
    uint16_t day_index;
    uint8_t  rsvd0[2];
    struct { uint16_t doses_today, pump_today_s, light_today_min, vib_today_s; } shelf[HG_MAX_SHELVES]; /* 12..43 */
    uint16_t dli_today_x10[HG_MAX_SHELVES]; /* 44..51 */
    uint8_t  rsvd[8];                /* 52 */
    uint32_t crc;                    /* 60 */
} hg_daily_t;

_Static_assert(sizeof(hg_shelf_hw_t) == 32,  "hg_shelf_hw_t");
_Static_assert(sizeof(hg_zone_hw_t)  == 160, "hg_zone_hw_t");
_Static_assert(sizeof(hg_light_cfg_t) == 12, "hg_light_cfg_t");
_Static_assert(sizeof(hg_water_cfg_t) == 16, "hg_water_cfg_t");
_Static_assert(sizeof(hg_fan_cfg_t)   == 4,  "hg_fan_cfg_t");
_Static_assert(sizeof(hg_aux_cfg_t)   == 8,  "hg_aux_cfg_t");
_Static_assert(sizeof(hg_vib_cfg_t)   == 16, "hg_vib_cfg_t");
_Static_assert(sizeof(hg_shelf_cfg_t) == 72, "hg_shelf_cfg_t");
_Static_assert(sizeof(hg_zone_cfg_t)  == 336,"hg_zone_cfg_t");
_Static_assert(sizeof(hg_daily_t)     == 64, "hg_daily_t");

#ifdef __cplusplus
}
#endif
