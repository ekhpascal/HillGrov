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

static void test_cfg_src_transitions(void) {
    /* defaults: fresh init, generation still 0 -> DEFAULTS */
    TEST_ASSERT_EQUAL_UINT8(0, hg_model_cfg_src());

    /* local edit bumps generation via hg_model_edit -> LOCAL */
    uint8_t v = 60;
    TEST_ASSERT_EQUAL_INT(0, hg_model_edit(ed_set_target, &v, err, sizeof err));
    TEST_ASSERT_EQUAL_UINT8(1, hg_model_cfg_src());

    /* ring/master push (full-plane replace, gen adopted as-is) -> MASTER */
    hg_zone_cfg_t pushed;
    hg_defaults_cfg(&pushed);
    pushed.generation = 42;
    pushed.shelf[0].water.target_pct = 77;
    hg_model_apply_cfg(&pushed);
    TEST_ASSERT_EQUAL_UINT8(2, hg_model_cfg_src());

    hg_zone_cfg_t out;
    hg_model_snapshot_cfg(&out, NULL);
    TEST_ASSERT_EQUAL_UINT32(42, out.generation);              /* adopted as-is, not gen_next()'d */
    TEST_ASSERT_EQUAL_UINT8(77, out.shelf[0].water.target_pct);
    TEST_ASSERT_EQUAL_UINT8(HG_CH_CFG, hg_model_dirty_mask());  /* marked dirty for hg_store */
}

static void test_cfg_info_and_hw_crc(void) {
    uint32_t gen1 = 99, crc1 = 0, crc2 = 0;
    hg_model_cfg_info(&gen1, &crc1);
    TEST_ASSERT_EQUAL_UINT32(0, gen1);           /* fresh init: generation 0 */
    TEST_ASSERT_NOT_EQUAL(0, crc1);              /* CRC of the wrapped defaults plane is non-zero */

    uint8_t v = 60;
    hg_model_edit(ed_set_target, &v, err, sizeof err);
    uint32_t gen2;
    hg_model_cfg_info(&gen2, &crc2);
    TEST_ASSERT_EQUAL_UINT32(1, gen2);
    TEST_ASSERT_NOT_EQUAL(crc1, crc2);           /* content changed -> different fingerprint */

    uint32_t hwcrc1 = 0, hwcrc2 = 0;
    hg_model_hw_crc(&hwcrc1);
    uint8_t pin = 9;
    hg_model_edit(ed_pump_pin, &pin, err, sizeof err);
    hg_model_hw_crc(&hwcrc2);
    TEST_ASSERT_NOT_EQUAL(hwcrc1, hwcrc2);
}

static void test_hw_crc_payload_only_stable_across_reboot(void) {
    hg_zone_hw_t hw;
    hg_model_snapshot_hw(&hw);              /* current (default) content */
    uint32_t crc_before = 0;
    hg_model_hw_crc(&crc_before);

    /* Simulate a reboot: hg_model_init() resets all RAM state, including the
       internal envelope-generation counter (s_hw_gen -> 0); hg_model_boot_load
       then restores byte-identical content, exactly as hg_store_init() does
       from NVS on every real boot. hw_crc must be unaffected -- payload-only,
       envelope (and any generation value) excluded. */
    hg_model_init();
    hg_model_boot_load(&hw, NULL);
    uint32_t crc_after_reboot = 0;
    hg_model_hw_crc(&crc_after_reboot);
    TEST_ASSERT_EQUAL_UINT32(crc_before, crc_after_reboot);

    /* A real content edit DOES change it. */
    uint8_t pin = 9;
    TEST_ASSERT_EQUAL_INT(0, hg_model_edit(ed_pump_pin, &pin, err, sizeof err));
    uint32_t crc_edited = 0;
    hg_model_hw_crc(&crc_edited);
    TEST_ASSERT_NOT_EQUAL(crc_after_reboot, crc_edited);
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
    RUN_TEST(test_cfg_src_transitions);
    RUN_TEST(test_cfg_info_and_hw_crc);
    RUN_TEST(test_hw_crc_payload_only_stable_across_reboot);
    RUN_TEST(test_boot_load);
    return UNITY_END(); }
