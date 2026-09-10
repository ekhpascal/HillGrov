#include <string.h>
#include "unity.h"
#include "cJSON.h"
#include "hg_json.h"
#include "hg_cfg.h"
#include "hg_mcfg.h"

void setUp(void) {}
void tearDown(void) {}

static cJSON *find_by_name(cJSON *arr, const char *name) {
    for (int i = 0; i < cJSON_GetArraySize(arr); i++) {
        cJSON *o = cJSON_GetArrayItem(arr, i);
        cJSON *n = cJSON_GetObjectItemCaseSensitive(o, "name");
        if (n && n->valuestring && strcmp(n->valuestring, name) == 0) return o;
    }
    return NULL;
}

static cJSON *find_field(cJSON *fields, const char *key) {
    for (int i = 0; i < cJSON_GetArraySize(fields); i++) {
        cJSON *f = cJSON_GetArrayItem(fields, i);
        cJSON *k = cJSON_GetObjectItemCaseSensitive(f, "key");
        if (k && k->valuestring && strcmp(k->valuestring, key) == 0) return f;
    }
    return NULL;
}

static void test_schema_has_all_groups_and_types(void) {
    char buf[8192];
    int n = hg_json_schema(buf, sizeof buf);
    TEST_ASSERT_GREATER_THAN_INT(0, n);
    cJSON *root = cJSON_Parse(buf);
    TEST_ASSERT_NOT_NULL(root);

    cJSON *groups = cJSON_GetObjectItemCaseSensitive(root, "groups");
    TEST_ASSERT_EQUAL_INT(HG_G_COUNT, cJSON_GetArraySize(groups));
    for (int g = 0; g < HG_G_COUNT; g++) {
        cJSON *go = cJSON_GetArrayItem(groups, g);
        TEST_ASSERT_EQUAL_STRING(HG_GROUP_NAMES[g], cJSON_GetObjectItemCaseSensitive(go, "name")->valuestring);
    }

    cJSON *water = find_by_name(groups, "WATER");
    TEST_ASSERT_NOT_NULL(water);
    cJSON *mode = find_field(cJSON_GetObjectItemCaseSensitive(water, "fields"), "MODE");
    TEST_ASSERT_NOT_NULL(mode);
    cJSON *enums = cJSON_GetObjectItemCaseSensitive(mode, "enums");
    TEST_ASSERT_EQUAL_INT(2, cJSON_GetArraySize(enums));
    TEST_ASSERT_EQUAL_STRING("OFF", cJSON_GetArrayItem(enums, 0)->valuestring);
    TEST_ASSERT_EQUAL_STRING("AUTO", cJSON_GetArrayItem(enums, 1)->valuestring);

    cJSON *mgroups = cJSON_GetObjectItemCaseSensitive(root, "mgroups");
    TEST_ASSERT_EQUAL_INT(3, cJSON_GetArraySize(mgroups));
    TEST_ASSERT_NOT_NULL(find_by_name(mgroups, "WIFI"));
    TEST_ASSERT_NOT_NULL(find_by_name(mgroups, "TIME"));
    TEST_ASSERT_NOT_NULL(find_by_name(mgroups, "SYS"));
    TEST_ASSERT_NULL(find_by_name(mgroups, "WEB"));

    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "hw_readonly")));
    cJSON_Delete(root);
}

static void test_export_then_merge_roundtrip(void) {
    hg_zone_hw_t hw;
    hg_zone_cfg_t cfg, cfg2;
    hg_defaults_hw(&hw);
    hg_defaults_cfg(&cfg);
    hg_defaults_cfg(&cfg2);

    char buf[8192];
    int n = hg_json_export_cfg(&hw, &cfg, 12, buf, sizeof buf);
    TEST_ASSERT_GREATER_THAN_INT(0, n);

    cJSON *root = cJSON_Parse(buf);
    TEST_ASSERT_NOT_NULL(root);
    TEST_ASSERT_EQUAL_INT(12, cJSON_GetObjectItemCaseSensitive(root, "gen")->valueint);
    cJSON_Delete(root);

    char err[64] = "", warn[256] = "";
    int rc = hg_json_merge_cfg(&hw, &cfg2, buf, err, sizeof err, warn, sizeof warn);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_MEMORY(&cfg, &cfg2, sizeof cfg);
}

static void test_merge_partial_keeps_others(void) {
    hg_zone_hw_t hw;
    hg_zone_cfg_t cfg, orig;
    hg_defaults_hw(&hw);
    hg_defaults_cfg(&cfg);
    orig = cfg;

    char err[64] = "", warn[256] = "";
    int rc = hg_json_merge_cfg(&hw, &cfg, "{\"cfg\":{\"shelf\":[{},{\"WATER\":{\"TARGET\":55}}]}}",
                                err, sizeof err, warn, sizeof warn);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_STRING("", warn);
    TEST_ASSERT_EQUAL_UINT8(55, cfg.shelf[1].water.target_pct);

    cfg.shelf[1].water.target_pct = orig.shelf[1].water.target_pct;
    TEST_ASSERT_EQUAL_MEMORY(&orig, &cfg, sizeof orig);
}

static void test_merge_unknown_key_warns_not_fails(void) {
    hg_zone_hw_t hw;
    hg_zone_cfg_t cfg;
    hg_defaults_hw(&hw);
    hg_defaults_cfg(&cfg);

    char err[64] = "", warn[256] = "";
    int rc = hg_json_merge_cfg(&hw, &cfg, "{\"cfg\":{\"ZONECFG\":{\"BOGUS\":1}}}",
                                err, sizeof err, warn, sizeof warn);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_NOT_NULL(strstr(warn, "cfg.ZONECFG.BOGUS"));
}

static void test_merge_bad_value_path(void) {
    hg_zone_hw_t hw;
    hg_zone_cfg_t cfg, orig;
    hg_defaults_hw(&hw);
    hg_defaults_cfg(&cfg);
    orig = cfg;

    char err[64] = "", warn[256] = "";
    int rc = hg_json_merge_cfg(&hw, &cfg, "{\"cfg\":{\"shelf\":[{\"WATER\":{\"TARGET\":101}}]}}",
                                err, sizeof err, warn, sizeof warn);
    TEST_ASSERT_EQUAL_INT(-2, rc);
    TEST_ASSERT_EQUAL_STRING("cfg.shelf[0].WATER.TARGET", err);
    TEST_ASSERT_EQUAL_MEMORY(&orig, &cfg, sizeof orig); /* merge works on a scratch copy */
}

static void test_merge_validation_path(void) {
    hg_zone_hw_t hw;
    hg_zone_cfg_t cfg;
    hg_defaults_hw(&hw);
    hg_defaults_cfg(&cfg); /* default LIGHT.ON = 06:00 */

    char err[64] = "", warn[256] = "";
    int rc = hg_json_merge_cfg(&hw, &cfg, "{\"cfg\":{\"shelf\":[{\"LIGHT\":{\"OFF\":\"06:00\"}}]}}",
                                err, sizeof err, warn, sizeof warn);
    TEST_ASSERT_EQUAL_INT(-3, rc);
    TEST_ASSERT_EQUAL_STRING("shelf[0].light.off", err); /* hg_cfg_validate's own path, verbatim */
}

static void test_hw_keys_are_warnings(void) {
    hg_zone_hw_t hw, orig_hw;
    hg_zone_cfg_t cfg;
    hg_defaults_hw(&hw);
    hg_defaults_cfg(&cfg);
    orig_hw = hw;

    char err[64] = "", warn[256] = "";
    int rc = hg_json_merge_cfg(&hw, &cfg, "{\"hw\":{\"HW\":{\"SHELF_COUNT\":2}}}",
                                err, sizeof err, warn, sizeof warn);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_MEMORY(&orig_hw, &hw, sizeof orig_hw); /* hw is const -- never touched */
    TEST_ASSERT_NOT_NULL(strstr(warn, "hw.HW.SHELF_COUNT readonly"));
}

static void test_hhmm_bool_enum_forms(void) {
    hg_zone_hw_t hw;
    hg_zone_cfg_t cfg;
    hg_defaults_hw(&hw);
    hg_defaults_cfg(&cfg);
    char err[64], warn[256];

    int rc = hg_json_merge_cfg(&hw, &cfg, "{\"cfg\":{\"shelf\":[{\"LIGHT\":{\"ON\":\"06:30\"}}]}}",
                                err, sizeof err, warn, sizeof warn);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_UINT16(6 * 60 + 30, cfg.shelf[0].light.on_min);

    rc = hg_json_merge_cfg(&hw, &cfg, "{\"cfg\":{\"shelf\":[{\"SHELF\":{\"ENABLED\":true}}]}}",
                            err, sizeof err, warn, sizeof warn);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_UINT8(1, cfg.shelf[0].enabled);

    rc = hg_json_merge_cfg(&hw, &cfg, "{\"cfg\":{\"shelf\":[{\"WATER\":{\"MODE\":\"AUTO\"}}]}}",
                            err, sizeof err, warn, sizeof warn);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_UINT8(1, cfg.shelf[0].water.mode);

    rc = hg_json_merge_cfg(&hw, &cfg, "{\"cfg\":{\"shelf\":[{\"WATER\":{\"MODE\":\"BANANA\"}}]}}",
                            err, sizeof err, warn, sizeof warn);
    TEST_ASSERT_EQUAL_INT(-2, rc);
    TEST_ASSERT_EQUAL_STRING("cfg.shelf[0].WATER.MODE", err);

    /* a JSON number for an ENUM field is rejected, not silently accepted as an index */
    rc = hg_json_merge_cfg(&hw, &cfg, "{\"cfg\":{\"shelf\":[{\"WATER\":{\"MODE\":1}}]}}",
                            err, sizeof err, warn, sizeof warn);
    TEST_ASSERT_EQUAL_INT(-2, rc);
    TEST_ASSERT_EQUAL_STRING("cfg.shelf[0].WATER.MODE", err);
}

static void test_mcfg_export_secrets(void) {
    hg_mcfg_t m;
    hg_mcfg_defaults(&m);
    char buf[2048];

    int n = hg_json_export_mcfg(&m, 0, buf, sizeof buf);
    TEST_ASSERT_GREATER_THAN_INT(0, n);
    cJSON *root = cJSON_Parse(buf);
    cJSON *wifi = cJSON_GetObjectItemCaseSensitive(root, "WIFI");
    TEST_ASSERT_NOT_NULL(wifi);
    TEST_ASSERT_NOT_NULL(cJSON_GetObjectItemCaseSensitive(wifi, "STA_SSID"));
    TEST_ASSERT_NULL(cJSON_GetObjectItemCaseSensitive(wifi, "STA_PASS"));
    TEST_ASSERT_NULL(cJSON_GetObjectItemCaseSensitive(wifi, "AP_PASS"));
    cJSON_Delete(root);

    n = hg_json_export_mcfg(&m, 1, buf, sizeof buf);
    TEST_ASSERT_GREATER_THAN_INT(0, n);
    root = cJSON_Parse(buf);
    wifi = cJSON_GetObjectItemCaseSensitive(root, "WIFI");
    TEST_ASSERT_EQUAL_STRING("hillgrow1", cJSON_GetObjectItemCaseSensitive(wifi, "AP_PASS")->valuestring);
    cJSON_Delete(root);

    hg_json_set_tz_check(NULL);
    char err[64] = "";
    int rc = hg_json_merge_mcfg(&m, "{\"WIFI\":{\"AP_PASS\":\"short\"}}", err, sizeof err);
    TEST_ASSERT_EQUAL_INT(-2, rc);
    TEST_ASSERT_EQUAL_STRING("WIFI.AP_PASS", err);
    TEST_ASSERT_EQUAL_STRING("hillgrow1", m.ap_pass); /* untouched on failure */
}

static void test_cap_too_small(void) {
    hg_zone_hw_t hw;
    hg_zone_cfg_t cfg;
    hg_defaults_hw(&hw);
    hg_defaults_cfg(&cfg);
    char buf[64];
    TEST_ASSERT_EQUAL_INT(-1, hg_json_export_cfg(&hw, &cfg, 1, buf, sizeof buf));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_schema_has_all_groups_and_types);
    RUN_TEST(test_export_then_merge_roundtrip);
    RUN_TEST(test_merge_partial_keeps_others);
    RUN_TEST(test_merge_unknown_key_warns_not_fails);
    RUN_TEST(test_merge_bad_value_path);
    RUN_TEST(test_merge_validation_path);
    RUN_TEST(test_hw_keys_are_warnings);
    RUN_TEST(test_hhmm_bool_enum_forms);
    RUN_TEST(test_mcfg_export_secrets);
    RUN_TEST(test_cap_too_small);
    return UNITY_END();
}
