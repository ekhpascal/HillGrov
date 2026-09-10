#include <string.h>
#include "unity.h"
#include "hg_mcfg.h"
#include "hg_blob.h"

static int tz_ok(const char *tz) { return strncmp(tz, "CET", 3) == 0 ? 0 : -1; }

void setUp(void) {}
void tearDown(void) {}

static void test_defaults_valid_and_flagged(void) {
    hg_mcfg_t m; hg_mcfg_defaults(&m); char err[32];
    TEST_ASSERT_EQUAL_STRING("HillGrow", m.ap_ssid);
    TEST_ASSERT_EQUAL_STRING("hillgrow1", m.ap_pass);
    TEST_ASSERT_EQUAL_STRING("CET-1CEST,M3.5.0,M10.5.0/3", m.tz);
    TEST_ASSERT_EQUAL_UINT8(MCFG_F_WEB_DEFAULT | MCFG_F_AP_DEFAULT, m.flags);
    TEST_ASSERT_EQUAL_INT(0, hg_mcfg_validate(&m, tz_ok, err, sizeof err));
}

static void test_validate_paths(void) {
    hg_mcfg_t m; char err[32];
    hg_mcfg_defaults(&m); strcpy(m.ap_pass, "short");
    TEST_ASSERT_EQUAL_INT(-1, hg_mcfg_validate(&m, tz_ok, err, sizeof err)); TEST_ASSERT_EQUAL_STRING("WIFI.AP_PASS", err);
    hg_mcfg_defaults(&m); strcpy(m.tz, "Nope");
    TEST_ASSERT_EQUAL_INT(-1, hg_mcfg_validate(&m, tz_ok, err, sizeof err)); TEST_ASSERT_EQUAL_STRING("TIME.TZ", err);
    hg_mcfg_defaults(&m); strcpy(m.hostname, "Bad Host");
    TEST_ASSERT_EQUAL_INT(-1, hg_mcfg_validate(&m, tz_ok, err, sizeof err)); TEST_ASSERT_EQUAL_STRING("SYS.HOSTNAME", err);
    hg_mcfg_defaults(&m); strcpy(m.sta_ssid, "Home"); strcpy(m.sta_pass, "");   /* open STA allowed */
    TEST_ASSERT_EQUAL_INT(0, hg_mcfg_validate(&m, tz_ok, err, sizeof err));
}

/* Controller ruling: a NULL tzck (Task 3, before Task 7 wires up the real
 * checker) must accept any TZ string rather than reject every validate call. */
static void test_validate_null_tzck_accepts_any_tz(void) {
    hg_mcfg_t m; char err[32];
    hg_mcfg_defaults(&m);
    TEST_ASSERT_EQUAL_INT(0, hg_mcfg_validate(&m, NULL, err, sizeof err));
    strcpy(m.tz, "Not/AValidZone");
    TEST_ASSERT_EQUAL_INT(0, hg_mcfg_validate(&m, NULL, err, sizeof err));
}

static void test_pack_unpack_roundtrip(void) {
    hg_mcfg_t m, o; uint8_t buf[512]; uint32_t gen = 0;
    hg_mcfg_defaults(&m); strcpy(m.sta_ssid, "Home"); m.flags = 0;
    size_t n = hg_mcfg_pack(&m, 7, buf, sizeof buf);
    TEST_ASSERT_EQUAL_size_t(16 + sizeof(hg_mcfg_t), n);
    TEST_ASSERT_EQUAL_INT(0, hg_mcfg_unpack(buf, n, &o, &gen));
    TEST_ASSERT_EQUAL_UINT32(7, gen); TEST_ASSERT_EQUAL_MEMORY(&m, &o, sizeof m);
    buf[20] ^= 1;   /* corrupt payload -> CRC */
    hg_mcfg_defaults(&o); TEST_ASSERT_EQUAL_INT(-1, hg_mcfg_unpack(buf, n, &o, &gen));
    TEST_ASSERT_EQUAL_STRING("HillGrow", o.ap_ssid);   /* untouched on failure */
}

static void test_field_table_write_read(void) {
    hg_mcfg_t m; hg_mcfg_defaults(&m); char out[64];
    const hg_field_t *f = NULL;
    for (int i = 0; i < HG_MFIELD_COUNT; i++) if (strcmp(HG_MFIELDS[i].key, "STA_SSID") == 0) f = &HG_MFIELDS[i];
    TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_EQUAL_INT(0, hg_field_write(f, &m, "MyWifi"));
    TEST_ASSERT_EQUAL_STRING("MyWifi", m.sta_ssid);
    TEST_ASSERT_EQUAL_INT(0, hg_field_read(f, &m, out, sizeof out)); TEST_ASSERT_EQUAL_STRING("MyWifi", out);
    TEST_ASSERT_EQUAL_INT(-2, hg_field_write(f, &m, "123456789012345678901234567890123"));   /* 33 chars > max 32 */
    TEST_ASSERT_EQUAL_INT(1, hg_mcfg_is_secret(&HG_MFIELDS[1]));   /* STA_PASS */
}

static void test_sizeof_struct(void) { TEST_ASSERT_EQUAL_size_t(368, sizeof(hg_mcfg_t)); }

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_sizeof_struct);
    RUN_TEST(test_defaults_valid_and_flagged);
    RUN_TEST(test_validate_paths);
    RUN_TEST(test_validate_null_tzck_accepts_any_tz);
    RUN_TEST(test_pack_unpack_roundtrip);
    RUN_TEST(test_field_table_write_read);
    return UNITY_END();
}
