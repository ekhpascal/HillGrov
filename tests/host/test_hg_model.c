#include <string.h>
#include "unity.h"
#include "hg_model.h"

void setUp(void) { hg_model_init(); }
void tearDown(void) {}
static char err[48];

static uint32_t ed_set_target(hg_zone_hw_t *hw, hg_zone_cfg_t *cfg, void *arg) {
    (void)hw; cfg->shelf[0].water.target_pct = *(uint8_t *)arg; return HG_CH_CFG;
}
static uint32_t ed_bad_dose(hg_zone_hw_t *hw, hg_zone_cfg_t *cfg, void *arg) {
    (void)hw; (void)arg; cfg->shelf[1].water.dose_s = 299; return HG_CH_CFG; /* > pump_max_run_s 60 */
}
static uint32_t ed_pump_pin(hg_zone_hw_t *hw, hg_zone_cfg_t *cfg, void *arg) {
    (void)cfg; hw->shelf[0].pump_pin = *(uint8_t *)arg; return HG_CH_HW;
}
static uint32_t ed_cal(hg_zone_hw_t *hw, hg_zone_cfg_t *cfg, void *arg) {
    (void)cfg; (void)arg; hw->shelf[0].soil_dry_mv[0] = 2900; return HG_CH_HW_LIVE;
}

static void test_edit_commit_and_gen(void) {
    uint8_t v = 60;
    TEST_ASSERT_EQUAL_INT(0, hg_model_edit(ed_set_target, &v, err, sizeof err));
    hg_zone_cfg_t c; uint32_t seq;
    hg_model_snapshot_cfg(&c, &seq);
    TEST_ASSERT_EQUAL_UINT8(60, c.shelf[0].water.target_pct);
    TEST_ASSERT_EQUAL_UINT32(1, c.generation);
    TEST_ASSERT_EQUAL_UINT8(HG_SRC_LOCAL, c.source);
    TEST_ASSERT_EQUAL_UINT32(1, seq);
    TEST_ASSERT_EQUAL_UINT8(HG_CH_CFG, hg_model_dirty_mask());
}

static void test_invalid_edit_changes_nothing(void) {
    TEST_ASSERT_EQUAL_INT(-2, hg_model_edit(ed_bad_dose, NULL, err, sizeof err));
    TEST_ASSERT_EQUAL_STRING("shelf[1].water.dose_s", err);
    hg_zone_cfg_t c;
    hg_model_snapshot_cfg(&c, NULL);
    TEST_ASSERT_EQUAL_UINT16(20, c.shelf[1].water.dose_s);
    TEST_ASSERT_EQUAL_UINT32(0, c.generation);
    TEST_ASSERT_EQUAL_UINT8(0, hg_model_dirty_mask());
}

static void test_hw_edit_restart_pending_cal_not(void) {
    uint8_t pin = 9;
    TEST_ASSERT_EQUAL_INT(0, hg_model_edit(ed_pump_pin, &pin, err, sizeof err));
    TEST_ASSERT_EQUAL_INT(1, hg_model_restart_pending());
    TEST_ASSERT_EQUAL_UINT8(HG_CH_HW, hg_model_dirty_mask() & HG_CH_HW);
    hg_model_init();
    TEST_ASSERT_EQUAL_INT(0, hg_model_edit(ed_cal, NULL, err, sizeof err));
    TEST_ASSERT_EQUAL_INT(0, hg_model_restart_pending());
    TEST_ASSERT_EQUAL_UINT8(HG_CH_HW, hg_model_dirty_mask()); /* still persisted as hw */
}

static void test_take_dirty(void) {
    uint8_t v = 50, pin = 9;
    hg_model_edit(ed_set_target, &v, err, sizeof err);   /* cfg edit #1: generation=1, seq=1 */
    hg_model_edit(ed_pump_pin, &pin, err, sizeof err);   /* hw edit: seq=2, cfg.generation untouched */
    v = 51;
    hg_model_edit(ed_set_target, &v, err, sizeof err);   /* cfg edit #2: generation=2, seq=3 */
    hg_zone_cfg_t staged; uint32_t gen = 0;
    TEST_ASSERT_EQUAL_UINT32(sizeof(hg_zone_cfg_t), hg_model_take_dirty(HG_CH_CFG, &staged, &gen));
    TEST_ASSERT_EQUAL_UINT32(2, gen); /* == cfg.generation, not seq (3) */
    TEST_ASSERT_EQUAL_UINT8(51, staged.shelf[0].water.target_pct);
    TEST_ASSERT_EQUAL_UINT32(0, hg_model_take_dirty(HG_CH_CFG, &staged, &gen)); /* cleared */
}

static void test_take_dirty_hw(void) {
    TEST_ASSERT_EQUAL_INT(0, hg_model_edit(ed_cal, NULL, err, sizeof err));      /* HW_LIVE commit: hw_gen=1 */
    uint8_t v = 70;
    TEST_ASSERT_EQUAL_INT(0, hg_model_edit(ed_set_target, &v, err, sizeof err)); /* interleaved cfg edit */
    uint8_t pin = 9;
    TEST_ASSERT_EQUAL_INT(0, hg_model_edit(ed_pump_pin, &pin, err, sizeof err)); /* HW commit: hw_gen=2 */

    hg_zone_hw_t staged_hw; uint32_t gen = 0;
    TEST_ASSERT_EQUAL_UINT32(sizeof(hg_zone_hw_t), hg_model_take_dirty(HG_CH_HW, &staged_hw, &gen));
    TEST_ASSERT_EQUAL_UINT32(2, gen); /* one increment per commit that touched hw */
    TEST_ASSERT_EQUAL_UINT16(2900, staged_hw.shelf[0].soil_dry_mv[0]); /* cal edit */
    TEST_ASSERT_EQUAL_UINT8(9, staged_hw.shelf[0].pump_pin);           /* pump-pin edit */
    TEST_ASSERT_EQUAL_UINT32(0, hg_model_take_dirty(HG_CH_HW, &staged_hw, &gen)); /* cleared */
    TEST_ASSERT_EQUAL_UINT8(HG_CH_CFG, hg_model_dirty_mask()); /* CFG bit from interleaved edit untouched */
}

static void test_boot_load(void) {
    hg_zone_cfg_t c; hg_zone_hw_t h;
    hg_defaults_cfg(&c); hg_defaults_hw(&h);
    c.generation = 7; c.shelf[2].water.target_pct = 33; h.shelf[0].pump_pin = 11;
    hg_model_boot_load(&h, &c);
    hg_zone_cfg_t out; hg_model_snapshot_cfg(&out, NULL);
    TEST_ASSERT_EQUAL_UINT32(7, out.generation);
    TEST_ASSERT_EQUAL_UINT8(33, out.shelf[2].water.target_pct);
    TEST_ASSERT_EQUAL_UINT8(0, hg_model_dirty_mask());        /* boot load is clean */
}

int main(void) { UNITY_BEGIN();
    RUN_TEST(test_edit_commit_and_gen);
    RUN_TEST(test_invalid_edit_changes_nothing);
    RUN_TEST(test_hw_edit_restart_pending_cal_not);
    RUN_TEST(test_take_dirty);
    RUN_TEST(test_take_dirty_hw);
    RUN_TEST(test_boot_load);
    return UNITY_END(); }
