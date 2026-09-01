#include <string.h>
#include "unity.h"
#include "hg_cfg.h"

static hg_zone_hw_t hw;
static hg_zone_cfg_t cfg;
void setUp(void) { hg_defaults_hw(&hw); hg_defaults_cfg(&cfg); }
void tearDown(void) {}

static const size_t GROUP_SIZE[HG_G_COUNT] = {
    sizeof(hg_zone_cfg_t), sizeof(hg_shelf_cfg_t), sizeof(hg_light_cfg_t),
    sizeof(hg_water_cfg_t), sizeof(hg_fan_cfg_t), sizeof(hg_vib_cfg_t), sizeof(hg_aux_cfg_t),
    sizeof(hg_zone_hw_t), sizeof(hg_shelf_hw_t), sizeof(hg_shelf_hw_t) };

static size_t type_width(uint8_t t) {
    switch (t) {
    case HG_T_U16: case HG_T_HHMM: return 2;
    case HG_T_STR16: return 16;
    default: return 1;
    }
}

static void test_table_integrity(void) {
    TEST_ASSERT_GREATER_THAN_INT(40, HG_FIELD_COUNT);
    for (int i = 0; i < HG_FIELD_COUNT; i++) {
        const hg_field_t *f = &HG_FIELDS[i];
        TEST_ASSERT_LESS_THAN_UINT8(HG_G_COUNT, f->group);
        TEST_ASSERT((size_t)f->offset + type_width(f->type) <= GROUP_SIZE[f->group]);
        if (f->type == HG_T_ENUM) TEST_ASSERT_NOT_NULL(f->enums);
        if (f->type == HG_T_HHMM) { /* range implied 0..1439 */ }
        for (int j = i + 1; j < HG_FIELD_COUNT; j++)                     /* unique key per group */
            if (HG_FIELDS[j].group == f->group)
                TEST_ASSERT_NOT_EQUAL(0, strcmp(HG_FIELDS[j].key, f->key));
    }
}

static void test_set_get_roundtrip_all_rows(void) {
    char buf[24], buf2[24];
    for (int i = 0; i < HG_FIELD_COUNT; i++) {
        const hg_field_t *f = &HG_FIELDS[i];
        int idx = 0;
        TEST_ASSERT_EQUAL_INT(0, hg_field_get_text(&hw, &cfg, f->group, idx, f->key, buf, sizeof buf));
        TEST_ASSERT_EQUAL_INT(0, hg_field_set_text(&hw, &cfg, f->group, idx, f->key, buf));
        TEST_ASSERT_EQUAL_INT(0, hg_field_get_text(&hw, &cfg, f->group, idx, f->key, buf2, sizeof buf2));
        TEST_ASSERT_EQUAL_STRING(buf, buf2);
    }
}

static void test_specific_forms(void) {
    char buf[24];
    TEST_ASSERT_EQUAL_INT(0, hg_field_set_text(&hw, &cfg, HG_G_LIGHT, 1, "ON", "06:30"));
    TEST_ASSERT_EQUAL_INT(0, hg_field_get_text(&hw, &cfg, HG_G_LIGHT, 1, "on", buf, sizeof buf));
    TEST_ASSERT_EQUAL_STRING("06:30", buf);
    TEST_ASSERT_EQUAL_INT(0, hg_field_set_text(&hw, &cfg, HG_G_LIGHT, 1, "ON", "390"));   /* integer minutes ok */
    TEST_ASSERT_EQUAL_INT(0, hg_field_set_text(&hw, &cfg, HG_G_WATER, 0, "mode", "off"));
    TEST_ASSERT_EQUAL_INT(0, hg_field_get_text(&hw, &cfg, HG_G_WATER, 0, "MODE", buf, sizeof buf));
    TEST_ASSERT_EQUAL_STRING("OFF", buf);
    TEST_ASSERT_EQUAL_INT(0, hg_field_set_text(&hw, &cfg, HG_G_WATER, 0, "MODE", "1"));   /* index form */
    TEST_ASSERT_EQUAL_INT(0, hg_field_set_text(&hw, &cfg, HG_G_HWSHELF, 2, "PUMP", "NONE"));
    TEST_ASSERT_EQUAL_INT(0, hg_field_get_text(&hw, &cfg, HG_G_HWSHELF, 2, "PUMP", buf, sizeof buf));
    TEST_ASSERT_EQUAL_STRING("NONE", buf);
    TEST_ASSERT_EQUAL_UINT8(HG_NONE, hw.shelf[2].pump_pin);
    TEST_ASSERT_EQUAL_INT(0, hg_field_set_text(&hw, &cfg, HG_G_SHELF, 0, "ENABLED", "ON"));
    TEST_ASSERT_EQUAL_UINT8(1, cfg.shelf[0].enabled);
    TEST_ASSERT_EQUAL_INT(0, hg_field_set_text(&hw, &cfg, HG_G_SHELF, 0, "CROP", "Basil"));
    TEST_ASSERT_EQUAL_STRING("Basil", cfg.shelf[0].crop);
}

static void test_errors(void) {
    TEST_ASSERT_EQUAL_INT(-2, hg_field_set_text(&hw, &cfg, HG_G_WATER, 0, "DOSE_S", "301"));
    TEST_ASSERT_EQUAL_INT(-2, hg_field_set_text(&hw, &cfg, HG_G_WATER, 0, "HYST", "0"));
    TEST_ASSERT_EQUAL_INT(-1, hg_field_set_text(&hw, &cfg, HG_G_WATER, 0, "DOSE_S", "abc"));
    TEST_ASSERT_EQUAL_INT(-1, hg_field_set_text(&hw, &cfg, HG_G_LIGHT, 0, "ON", "25:00"));
    TEST_ASSERT_EQUAL_INT(-1, hg_field_set_text(&hw, &cfg, HG_G_WATER, 0, "MODE", "MAYBE"));
    TEST_ASSERT_EQUAL_INT(-3, hg_field_set_text(&hw, &cfg, HG_G_WATER, 0, "BOGUS", "1"));
    TEST_ASSERT_EQUAL_INT(-4, hg_field_set_text(&hw, &cfg, HG_G_WATER, 4, "TARGET", "50"));
    TEST_ASSERT_EQUAL_INT(-4, hg_field_set_text(&hw, &cfg, HG_G_AUX, 2, "MODE", "OFF"));
    TEST_ASSERT_EQUAL_INT(-1, hg_field_set_text(&hw, &cfg, HG_G_SHELF, 0, "CROP", "name with space"));
    TEST_ASSERT_EQUAL_INT(-1, hg_field_set_text(&hw, &cfg, HG_G_SHELF, 0, "CROP", "sixteencharslong!"));
    TEST_ASSERT_EQUAL_INT(-1, hg_group_find("NOPE"));
    TEST_ASSERT_EQUAL_INT(HG_G_WATER, hg_group_find("water"));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_table_integrity);
    RUN_TEST(test_set_get_roundtrip_all_rows);
    RUN_TEST(test_specific_forms);
    RUN_TEST(test_errors);
    return UNITY_END();
}
