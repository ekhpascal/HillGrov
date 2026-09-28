/* psvc_mcfg_edit() is the ONE master-config read-modify-write, for the web's
   zone-0 PUT (JSON) and the panel's Config editor (field edits) alike. What is
   pinned here is what a second copy got wrong before: every refusal names the
   field and commits nothing, a blank secret is "unchanged" (never a wipe of the
   house Wi-Fi password), contention is BUSY rather than STORAGE for the panel
   (D10), apply runs exactly once and only after a commit, and a zone-0 edit
   leaves MCFG_F_AP_DEFAULT alone (D9). */
#include <stdio.h>
#include <string.h>
#include "unity.h"
#include "psvc_mcfg.h"
#include "mcfg_ops.h"
#include "mcfg_store.h"
#include "fake_mcfg_store.h"
#include "fake_apply.h"

void setUp(void) {
    mcfg_store_init();                    /* defaults: hostname "hillgrow", AP HillGrow/hillgrow1, both DEFAULT flags */
    fake_mcfg_store_force_storage_fail(0);
    fake_apply_reset();
    mcfg_ops_init();
}
void tearDown(void) { fake_mcfg_store_force_storage_fail(0); }

static const hg_field_t *mrow(const char *key) {
    for (int i = 0; i < HG_MFIELD_COUNT; i++)
        if (strcmp(HG_MFIELDS[i].key, key) == 0) return &HG_MFIELDS[i];
    return NULL;
}

static psvc_fedit_t medit(const char *key, const char *text) {
    psvc_fedit_t e;
    memset(&e, 0, sizeof e);
    e.f = mrow(key);
    e.group = e.f ? e.f->group : 0;
    e.idx = -1;
    snprintf(e.text, sizeof e.text, "%s", text);
    return e;
}

static psvc_rc_t edit_fields(const psvc_fedit_t *e, int n, char *err, size_t cap) {
    psvc_fedits_t set = { e, n };
    return psvc_mcfg_edit(psvc_mcfg_fields_fn, &set, PSVC_LOCK_PANEL_MS, "TEST FIELDS", err, cap);
}

static psvc_rc_t edit_json(const char *json, uint32_t lock_ms, char *err, size_t cap) {
    return psvc_mcfg_edit(psvc_mcfg_json_fn, (void *)json, lock_ms, "TEST JSON", err, cap);
}

static void test_fields_edit_commits_and_applies_once(void) {
    psvc_fedit_t e = medit("HOSTNAME", "greenhouse-1");
    char err[64] = "x";
    TEST_ASSERT_EQUAL_INT(PSVC_OK, edit_fields(&e, 1, err, sizeof err));
    TEST_ASSERT_EQUAL_STRING("", err);
    TEST_ASSERT_EQUAL_STRING("greenhouse-1", mcfg_get()->hostname);
    TEST_ASSERT_EQUAL_UINT32(1, mcfg_gen());
    TEST_ASSERT_EQUAL_INT(1, fake_apply_wifi_n);
    TEST_ASSERT_EQUAL_INT(1, fake_apply_time_n);
}

static void test_blank_secret_means_unchanged(void) {
    psvc_fedit_t first[2] = { medit("STA_SSID", "house"), medit("STA_PASS", "housepass1") };
    char err[64];
    TEST_ASSERT_EQUAL_INT(PSVC_OK, edit_fields(first, 2, err, sizeof err));
    psvc_fedit_t second[2] = { medit("STA_SSID", "house2"), medit("STA_PASS", "") };
    TEST_ASSERT_EQUAL_INT(PSVC_OK, edit_fields(second, 2, err, sizeof err));
    TEST_ASSERT_EQUAL_STRING("house2", mcfg_get()->sta_ssid);
    TEST_ASSERT_EQUAL_STRING("housepass1", mcfg_get()->sta_pass);   /* never wiped by a blank */
}

static void test_overlong_hostname_is_invalid_field_and_commits_nothing(void) {
    psvc_fedit_t e = medit("HOSTNAME", "abcdefghijklmnopqrstuvwx");   /* 24 chars > row max 23 */
    char err[64] = "";
    TEST_ASSERT_EQUAL_INT(PSVC_E_INVALID_FIELD, edit_fields(&e, 1, err, sizeof err));
    TEST_ASSERT_EQUAL_STRING("SYS.HOSTNAME", err);
    TEST_ASSERT_EQUAL_UINT32(0, mcfg_gen());
    TEST_ASSERT_EQUAL_STRING("hillgrow", mcfg_get()->hostname);
    TEST_ASSERT_EQUAL_INT(0, fake_apply_wifi_n);
    TEST_ASSERT_EQUAL_INT(0, fake_apply_time_n);
}

static void test_bad_hostname_is_validation_with_path(void) {
    psvc_fedit_t e = medit("HOSTNAME", "Bad Name");   /* writes fine, fails [a-z0-9-] */
    char err[64] = "";
    TEST_ASSERT_EQUAL_INT(PSVC_E_VALIDATION, edit_fields(&e, 1, err, sizeof err));
    TEST_ASSERT_EQUAL_STRING("SYS.HOSTNAME", err);
    TEST_ASSERT_EQUAL_UINT32(0, mcfg_gen());
}

static void test_bad_tz_is_validation_through_tz_check(void) {
    psvc_fedit_t e = medit("TZ", "garbage!!");
    char err[64] = "";
    TEST_ASSERT_EQUAL_INT(PSVC_E_VALIDATION, edit_fields(&e, 1, err, sizeof err));
    TEST_ASSERT_EQUAL_STRING("TIME.TZ", err);
}

static void test_json_malformed_is_bad_json(void) {
    char err[64] = "";
    TEST_ASSERT_EQUAL_INT(PSVC_E_BAD_JSON, edit_json("{", PSVC_LOCK_WEB_MS, err, sizeof err));
    TEST_ASSERT_EQUAL_UINT32(0, mcfg_gen());
    TEST_ASSERT_EQUAL_INT(0, fake_apply_wifi_n);
}

static void test_json_short_ap_pass_is_invalid_field(void) {
    char err[64] = "";
    TEST_ASSERT_EQUAL_INT(PSVC_E_INVALID_FIELD,
                          edit_json("{\"WIFI\":{\"AP_PASS\":\"short\"}}", PSVC_LOCK_WEB_MS, err, sizeof err));
    TEST_ASSERT_EQUAL_STRING("WIFI.AP_PASS", err);
}

static void test_json_good_body_commits(void) {
    char err[64] = "";
    TEST_ASSERT_EQUAL_INT(PSVC_OK,
                          edit_json("{\"TIME\":{\"NTP\":\"time.google.com\"}}", PSVC_LOCK_WEB_MS, err, sizeof err));
    TEST_ASSERT_EQUAL_STRING("time.google.com", mcfg_get()->ntp);
    TEST_ASSERT_EQUAL_INT(1, fake_apply_wifi_n);   /* the web's zone-0 PUT always applied both (http_api_cfg.c:244-245) */
    TEST_ASSERT_EQUAL_INT(1, fake_apply_time_n);
}

static void test_held_lock_is_busy(void) {
    TEST_ASSERT_EQUAL_INT(0, mcfg_ops_lock(10));
    char err[64] = "";
    TEST_ASSERT_EQUAL_INT(PSVC_E_BUSY,
                          edit_json("{\"TIME\":{\"NTP\":\"time.google.com\"}}", 100, err, sizeof err));
    mcfg_ops_unlock();
    TEST_ASSERT_EQUAL_STRING("pool.ntp.org", mcfg_get()->ntp);
    TEST_ASSERT_EQUAL_INT(0, fake_apply_wifi_n);
}

static void test_storage_failure_is_storage_and_does_not_apply(void) {
    fake_mcfg_store_force_storage_fail(1);
    psvc_fedit_t e = medit("HOSTNAME", "greenhouse-2");
    char err[64] = "";
    TEST_ASSERT_EQUAL_INT(PSVC_E_STORAGE, edit_fields(&e, 1, err, sizeof err));
    TEST_ASSERT_EQUAL_STRING("", err);
    TEST_ASSERT_EQUAL_INT(0, fake_apply_wifi_n);
    TEST_ASSERT_EQUAL_INT(0, fake_apply_time_n);
}

static void test_ap_pass_edit_leaves_ap_default(void) {
    TEST_ASSERT_TRUE(mcfg_get()->flags & MCFG_F_AP_DEFAULT);
    psvc_fedit_t e = medit("AP_PASS", "newpass123");
    char err[64] = "";
    TEST_ASSERT_EQUAL_INT(PSVC_OK, edit_fields(&e, 1, err, sizeof err));
    TEST_ASSERT_EQUAL_STRING("newpass123", mcfg_get()->ap_pass);
    TEST_ASSERT_TRUE(mcfg_get()->flags & MCFG_F_AP_DEFAULT);   /* D9: web parity, unchanged */
}

static void test_get_is_a_copy_of_the_live_config(void) {
    hg_mcfg_t m;
    memset(&m, 0xAA, sizeof m);
    psvc_mcfg_get(&m);
    TEST_ASSERT_EQUAL_MEMORY(mcfg_get(), &m, sizeof m);
}

int main(void) { UNITY_BEGIN();
    RUN_TEST(test_fields_edit_commits_and_applies_once);
    RUN_TEST(test_blank_secret_means_unchanged);
    RUN_TEST(test_overlong_hostname_is_invalid_field_and_commits_nothing);
    RUN_TEST(test_bad_hostname_is_validation_with_path);
    RUN_TEST(test_bad_tz_is_validation_through_tz_check);
    RUN_TEST(test_json_malformed_is_bad_json);
    RUN_TEST(test_json_short_ap_pass_is_invalid_field);
    RUN_TEST(test_json_good_body_commits);
    RUN_TEST(test_held_lock_is_busy);
    RUN_TEST(test_storage_failure_is_storage_and_does_not_apply);
    RUN_TEST(test_ap_pass_edit_leaves_ap_default);
    RUN_TEST(test_get_is_a_copy_of_the_live_config);
    return UNITY_END(); }
