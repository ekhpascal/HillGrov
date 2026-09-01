#include <string.h>
#include "unity.h"
#include "hg_cfg.h"

static hg_zone_hw_t hw;
static hg_zone_cfg_t cfg;
static char err[48];
void setUp(void) { hg_defaults_hw(&hw); hg_defaults_cfg(&cfg); err[0] = '\0'; }
void tearDown(void) {}

static void test_defaults_are_valid(void) {
    TEST_ASSERT_EQUAL_INT(0, hg_hw_validate(&hw, err, sizeof err));
    TEST_ASSERT_EQUAL_INT(0, hg_cfg_validate(&cfg, &hw, err, sizeof err));
    TEST_ASSERT_EQUAL_INT(0, hg_cfg_validate(&cfg, NULL, err, sizeof err));
}

static void test_hw_rejects(void) {
    hw.shelf_count = 5;
    TEST_ASSERT_EQUAL_INT(-1, hg_hw_validate(&hw, err, sizeof err));
    TEST_ASSERT_EQUAL_STRING("hw.shelves", err);
    hg_defaults_hw(&hw);
    hw.shelf[1].led_ch[0] = hw.shelf[0].led_ch[1];             /* duplicate PCA channel */
    TEST_ASSERT_EQUAL_INT(-1, hg_hw_validate(&hw, err, sizeof err));
    TEST_ASSERT_EQUAL_STRING("shelf[1].hwshelf.led_w", err);
    hg_defaults_hw(&hw);
    hw.shelf[3].pump_pin = hw.shelf[0].fan_pin;                /* duplicate PCF pin */
    TEST_ASSERT_EQUAL_INT(-1, hg_hw_validate(&hw, err, sizeof err));
    TEST_ASSERT_EQUAL_STRING("shelf[3].hwshelf.pump", err);
    hg_defaults_hw(&hw);
    hw.shelf[2].soil_ch[1] = hw.shelf[2].soil_ch[0];           /* duplicate soil channel */
    TEST_ASSERT_EQUAL_INT(-1, hg_hw_validate(&hw, err, sizeof err));
    TEST_ASSERT_EQUAL_STRING("shelf[2].hwshelf.soil_b", err);
    hg_defaults_hw(&hw);
    hw.shelf[0].soil_min_ok_mv = 3300; hw.shelf[0].soil_max_ok_mv = 300;
    TEST_ASSERT_EQUAL_INT(-1, hg_hw_validate(&hw, err, sizeof err));
    TEST_ASSERT_EQUAL_STRING("shelf[0].cal.min_ok", err);
    hg_defaults_hw(&hw);
    hw.shelf[0].soil_dry_mv[0] = 1000;                          /* dry <= wet */
    TEST_ASSERT_EQUAL_INT(-1, hg_hw_validate(&hw, err, sizeof err));
    TEST_ASSERT_EQUAL_STRING("shelf[0].cal.dry_a", err);
    hg_defaults_hw(&hw);
    hw.shelf[0].pump_max_run_s = 0;                             /* table range via blob path */
    TEST_ASSERT_EQUAL_INT(-1, hg_hw_validate(&hw, err, sizeof err));
    TEST_ASSERT_EQUAL_STRING("shelf[0].hwshelf.pump_max_run_s", err);
    /* HG_NONE duplicates are allowed */
    hg_defaults_hw(&hw);
    hw.shelf[0].pump_pin = HG_NONE; hw.shelf[1].pump_pin = HG_NONE;
    TEST_ASSERT_EQUAL_INT(0, hg_hw_validate(&hw, err, sizeof err));
}

static void test_cfg_rejects(void) {
    cfg.shelf[1].light.off_min = cfg.shelf[1].light.on_min;
    TEST_ASSERT_EQUAL_INT(-1, hg_cfg_validate(&cfg, &hw, err, sizeof err));
    TEST_ASSERT_EQUAL_STRING("shelf[1].light.off", err);
    hg_defaults_cfg(&cfg);
    hw.shelf_count = 2; cfg.shelf[3].enabled = 1;
    TEST_ASSERT_EQUAL_INT(-1, hg_cfg_validate(&cfg, &hw, err, sizeof err));
    TEST_ASSERT_EQUAL_STRING("shelf[3].enabled", err);
    TEST_ASSERT_EQUAL_INT(0, hg_cfg_validate(&cfg, NULL, err, sizeof err)); /* no hw context -> not checkable */
    hg_defaults_hw(&hw); hg_defaults_cfg(&cfg);
    cfg.shelf[0].water.dose_s = 100; hw.shelf[0].pump_max_run_s = 60;
    TEST_ASSERT_EQUAL_INT(-1, hg_cfg_validate(&cfg, &hw, err, sizeof err));
    TEST_ASSERT_EQUAL_STRING("shelf[0].water.dose_s", err);
    hg_defaults_cfg(&cfg);
    cfg.shelf[0].water.max_doses_day = 24; cfg.shelf[0].water.dose_s = 30; /* 24*30=720 > 600 daily */
    TEST_ASSERT_EQUAL_INT(-1, hg_cfg_validate(&cfg, &hw, err, sizeof err));
    TEST_ASSERT_EQUAL_STRING("shelf[0].water.max_doses", err);
    hg_defaults_cfg(&cfg);
    cfg.shelf[2].water.target_pct = 200;                        /* raw blob out of range */
    TEST_ASSERT_EQUAL_INT(-1, hg_cfg_validate(&cfg, &hw, err, sizeof err));
    TEST_ASSERT_EQUAL_STRING("shelf[2].water.target", err);
    hg_defaults_cfg(&cfg);
    cfg.name[0] = 0x07;
    TEST_ASSERT_EQUAL_INT(-1, hg_cfg_validate(&cfg, &hw, err, sizeof err));
    TEST_ASSERT_EQUAL_STRING("zonecfg.name", err);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_defaults_are_valid);
    RUN_TEST(test_hw_rejects);
    RUN_TEST(test_cfg_rejects);
    return UNITY_END();
}
