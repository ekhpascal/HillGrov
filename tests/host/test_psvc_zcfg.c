/* psvc_zone_cfg_edit() is the ONE zone-config write for both faces. What this
   pins: the web's exact check order (busy before any read, then the cache),
   the hw_present rule in BOTH directions (what_we_learned: a presence signal
   must be proven with a positive assertion, not only its absence), per-field
   last-writer-wins on a fresh copy, the hardware plane refused, and the web's
   error paths and codes. */
#include <stdio.h>
#include <string.h>
#include "unity.h"
#include "hg_cfg.h"
#include "psvc_zcfg.h"
#include "fake_nmgr_cfg_api.h"

void setUp(void) { fake_nmgr_cfg_reset(); }
void tearDown(void) {}

static const hg_field_t *zrow(uint8_t group, const char *key) {
    for (int i = 0; i < HG_FIELD_COUNT; i++)
        if (HG_FIELDS[i].group == group && strcmp(HG_FIELDS[i].key, key) == 0) return &HG_FIELDS[i];
    return NULL;
}

static psvc_fedit_t zedit(uint8_t group, int idx, const char *key, const char *text) {
    psvc_fedit_t e;
    memset(&e, 0, sizeof e);
    e.group = group;
    e.idx = (int8_t)idx;
    e.f = zrow(group, key);
    snprintf(e.text, sizeof e.text, "%s", text);
    return e;
}

static psvc_rc_t edit(uint8_t zone, const psvc_fedit_t *e, int n, char *err, size_t cap) {
    psvc_fedits_t set = { e, n };
    char warn[64];
    return psvc_zone_cfg_edit(zone, psvc_zone_fields_fn, &set, err, cap, warn, sizeof warn);
}

static psvc_rc_t put_json(uint8_t zone, const char *json, char *err, size_t cap) {
    char warn[128];
    return psvc_zone_cfg_edit(zone, psvc_zone_json_fn, (void *)json, err, cap, warn, sizeof warn);
}

static void test_busy_refuses_before_any_read(void) {
    g_fnc.busy_rc = 1;
    psvc_fedit_t e = zedit(HG_G_WATER, 0, "TARGET", "55");
    char err[96] = "";
    TEST_ASSERT_EQUAL_INT(PSVC_E_BUSY, edit(2, &e, 1, err, sizeof err));
    TEST_ASSERT_EQUAL_INT(0, g_fnc.get_calls);
    TEST_ASSERT_EQUAL_INT(0, g_fnc.set_calls);
}

static void test_no_cache_is_no_cache(void) {
    g_fnc.get_rc = -1;
    psvc_fedit_t e = zedit(HG_G_WATER, 0, "TARGET", "55");
    char err[96] = "";
    TEST_ASSERT_EQUAL_INT(PSVC_E_NO_CACHE, edit(2, &e, 1, err, sizeof err));
    TEST_ASSERT_EQUAL_INT(0, g_fnc.set_calls);
}

static void test_zone_out_of_range_is_zone_unknown(void) {
    psvc_fedit_t e = zedit(HG_G_WATER, 0, "TARGET", "55");
    char err[96] = "";
    TEST_ASSERT_EQUAL_INT(PSVC_E_ZONE_UNKNOWN, edit(0, &e, 1, err, sizeof err));
    TEST_ASSERT_EQUAL_INT(PSVC_E_ZONE_UNKNOWN, edit(HG_MAX_ZONES + 1, &e, 1, err, sizeof err));
    TEST_ASSERT_EQUAL_INT(0, g_fnc.busy_calls);
}

/* hw present: dose 200 s against the zone's own pump_max_run_s 60 must be
   refused -- the flood guard is ON. */
static void test_hw_present_enforces_the_pump_limit(void) {
    g_fnc.hw_present = 1;
    psvc_fedit_t e = zedit(HG_G_WATER, 0, "DOSE_S", "200");
    char err[96] = "";
    TEST_ASSERT_EQUAL_INT(PSVC_E_VALIDATION, edit(2, &e, 1, err, sizeof err));
    TEST_ASSERT_EQUAL_STRING("shelf[0].water.dose_s", err);
    TEST_ASSERT_EQUAL_INT(0, g_fnc.set_calls);
}

/* hw absent: the same edit must NOT be refused against an all-zero profile. */
static void test_hw_absent_skips_the_hardware_checks(void) {
    g_fnc.hw_present = 0;
    psvc_fedit_t e = zedit(HG_G_WATER, 0, "DOSE_S", "200");
    char err[96] = "x";
    TEST_ASSERT_EQUAL_INT(PSVC_OK, edit(2, &e, 1, err, sizeof err));
    TEST_ASSERT_EQUAL_STRING("", err);
    TEST_ASSERT_EQUAL_INT(1, g_fnc.set_calls);
    TEST_ASSERT_EQUAL_UINT16(200, g_fnc.last_set.shelf[0].water.dose_s);
}

/* The panel displayed hyst 5; meanwhile another writer set it to 9. A save of
   TARGET alone applies to the FRESH copy, so the other writer's field survives. */
static void test_fields_apply_to_a_fresh_copy(void) {
    g_fnc.cfg.shelf[0].water.hyst_pct = 9;
    psvc_fedit_t e = zedit(HG_G_WATER, 0, "TARGET", "55");
    char err[96] = "";
    TEST_ASSERT_EQUAL_INT(PSVC_OK, edit(2, &e, 1, err, sizeof err));
    TEST_ASSERT_EQUAL_UINT8(55, g_fnc.last_set.shelf[0].water.target_pct);
    TEST_ASSERT_EQUAL_UINT8(9, g_fnc.last_set.shelf[0].water.hyst_pct);
    TEST_ASSERT_EQUAL_UINT8(2, g_fnc.last_zone);
}

static void test_hardware_plane_is_refused(void) {
    psvc_fedit_t e = zedit(HG_G_HW, -1, "SHELVES", "2");
    char err[96] = "";
    TEST_ASSERT_EQUAL_INT(PSVC_E_INVALID_FIELD, edit(2, &e, 1, err, sizeof err));
    TEST_ASSERT_EQUAL_STRING("hw.HW.SHELVES", err);
    psvc_fedit_t c = zedit(HG_G_CAL, 1, "DRY_A", "2900");
    TEST_ASSERT_EQUAL_INT(PSVC_E_INVALID_FIELD, edit(2, &c, 1, err, sizeof err));
    TEST_ASSERT_EQUAL_STRING("hw.CAL.DRY_A", err);
    TEST_ASSERT_EQUAL_INT(0, g_fnc.set_calls);
}

static void test_field_write_failure_uses_the_merge_path_shape(void) {
    psvc_fedit_t a = zedit(HG_G_WATER, 1, "TARGET", "101");
    char err[96] = "";
    TEST_ASSERT_EQUAL_INT(PSVC_E_INVALID_FIELD, edit(2, &a, 1, err, sizeof err));
    TEST_ASSERT_EQUAL_STRING("cfg.shelf[1].WATER.TARGET", err);
    psvc_fedit_t b = zedit(HG_G_ZONECFG, -1, "LINKLOSS_S", "5");
    TEST_ASSERT_EQUAL_INT(PSVC_E_INVALID_FIELD, edit(2, &b, 1, err, sizeof err));
    TEST_ASSERT_EQUAL_STRING("cfg.ZONECFG.LINKLOSS_S", err);
    psvc_fedit_t c = zedit(HG_G_AUX, 0, "MODE", "BOGUS");
    TEST_ASSERT_EQUAL_INT(PSVC_E_INVALID_FIELD, edit(2, &c, 1, err, sizeof err));
    TEST_ASSERT_EQUAL_STRING("cfg.aux[0].AUX.MODE", err);
}

static void test_set_codes_map_like_the_web(void) {
    psvc_fedit_t e = zedit(HG_G_WATER, 0, "TARGET", "55");
    char err[96];
    g_fnc.set_rc = -1; TEST_ASSERT_EQUAL_INT(PSVC_E_ZONE_UNKNOWN,    edit(2, &e, 1, err, sizeof err));
    g_fnc.set_rc = -2; TEST_ASSERT_EQUAL_INT(PSVC_E_BUSY,            edit(2, &e, 1, err, sizeof err));
    g_fnc.set_rc = -3; TEST_ASSERT_EQUAL_INT(PSVC_E_ZONE_NOT_ONLINE, edit(2, &e, 1, err, sizeof err));
}

static void test_json_paths_match_the_web(void) {
    char err[96] = "";
    TEST_ASSERT_EQUAL_INT(PSVC_E_INVALID_FIELD,
        put_json(2, "{\"cfg\":{\"shelf\":[null,{\"WATER\":{\"TARGET\":101}}]}}", err, sizeof err));
    TEST_ASSERT_EQUAL_STRING("cfg.shelf[1].WATER.TARGET", err);
    TEST_ASSERT_EQUAL_INT(PSVC_E_VALIDATION,
        put_json(2, "{\"cfg\":{\"shelf\":[null,{\"LIGHT\":{\"OFF\":\"06:00\"}}]}}", err, sizeof err));
    TEST_ASSERT_EQUAL_STRING("shelf[1].light.off", err);
    TEST_ASSERT_EQUAL_INT(PSVC_E_BAD_JSON, put_json(2, "{", err, sizeof err));
    TEST_ASSERT_EQUAL_INT(0, g_fnc.set_calls);
}

static void test_json_hw_key_is_a_warning_not_a_write(void) {
    char err[96] = "", warn[128] = "";
    TEST_ASSERT_EQUAL_INT(PSVC_OK, psvc_zone_cfg_edit(2, psvc_zone_json_fn,
        (void *)"{\"hw\":{\"HW\":{\"SHELVES\":1}}}", err, sizeof err, warn, sizeof warn));
    TEST_ASSERT_NOT_NULL(strstr(warn, "hw.HW.SHELVES readonly"));
    TEST_ASSERT_EQUAL_INT(1, g_fnc.set_calls);
}

static void test_get_ranges_and_hw_presence(void) {
    hg_zone_cfg_t cfg; hg_zone_hw_t hw; uint32_t gen = 0; int hp = -1;
    TEST_ASSERT_EQUAL_INT(PSVC_E_ZONE_UNKNOWN, psvc_zone_cfg_get(9, &cfg, &hw, &gen, &hp));
    g_fnc.hw_present = 0;
    memset(&hw, 0xAA, sizeof hw);
    TEST_ASSERT_EQUAL_INT(PSVC_OK, psvc_zone_cfg_get(3, &cfg, &hw, &gen, &hp));
    TEST_ASSERT_EQUAL_INT(0, hp);
    TEST_ASSERT_EQUAL_UINT32(7, gen);
    TEST_ASSERT_EQUAL_UINT8(0, hw.shelf_count);   /* zeroed, never stale bytes */
    g_fnc.get_rc = -1;
    TEST_ASSERT_EQUAL_INT(PSVC_E_NO_CACHE, psvc_zone_cfg_get(3, &cfg, &hw, &gen, &hp));
    g_fnc.busy_rc = 1;
    TEST_ASSERT_EQUAL_INT(1, psvc_zone_cfg_busy(3));
}

int main(void) { UNITY_BEGIN();
    RUN_TEST(test_busy_refuses_before_any_read);
    RUN_TEST(test_no_cache_is_no_cache);
    RUN_TEST(test_zone_out_of_range_is_zone_unknown);
    RUN_TEST(test_hw_present_enforces_the_pump_limit);
    RUN_TEST(test_hw_absent_skips_the_hardware_checks);
    RUN_TEST(test_fields_apply_to_a_fresh_copy);
    RUN_TEST(test_hardware_plane_is_refused);
    RUN_TEST(test_field_write_failure_uses_the_merge_path_shape);
    RUN_TEST(test_set_codes_map_like_the_web);
    RUN_TEST(test_json_paths_match_the_web);
    RUN_TEST(test_json_hw_key_is_a_warning_not_a_write);
    RUN_TEST(test_get_ranges_and_hw_presence);
    return UNITY_END(); }
