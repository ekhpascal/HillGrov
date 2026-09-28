#include <stdio.h>
#include <string.h>
#include "unity.h"
#include "hg_image.h"

/* hg_image identifies an app image from its first 112 bytes BEFORE anything is erased (recovery design 4.2): image magic,
   chip id, app-descriptor magic, project name. esp_ota_end() checks chip and checksums but not the project, so this is
   the only thing that stops a zone image landing in a master slot. The project-name constants are checked against the
   CMakeLists.txt that defines each project (recovery design 5.7), so a rename cannot drift silently. */

static uint8_t g_b[HG_IMG_ID_BYTES];

void setUp(void) {}
void tearDown(void) {}

static void mk(uint16_t chip, const char *proj, const char *ver) {
    memset(g_b, 0x5A, sizeof g_b);
    g_b[0] = HG_IMG_MAGIC;
    g_b[12] = (uint8_t)chip;
    g_b[13] = (uint8_t)(chip >> 8);
    uint32_t m = HG_IMG_DESC_MAGIC;
    g_b[32] = (uint8_t)m; g_b[33] = (uint8_t)(m >> 8); g_b[34] = (uint8_t)(m >> 16); g_b[35] = (uint8_t)(m >> 24);
    memset(g_b + 48, 0, 32);
    memcpy(g_b + 48, ver, strlen(ver));
    memset(g_b + 80, 0, 32);
    size_t n = strlen(proj);
    memcpy(g_b + 80, proj, n > 32 ? 32 : n);
}

static void test_p4_master(void) {
    hg_image_id_t id;
    mk(HG_CHIP_ESP32P4, HG_PROJ_MASTER, "1.4.0");
    TEST_ASSERT_EQUAL_INT(0, hg_image_parse(g_b, sizeof g_b, &id));
    TEST_ASSERT_EQUAL_HEX16(0x0012, id.chip_id);
    TEST_ASSERT_EQUAL_STRING("hillgrow_master", id.project);
    TEST_ASSERT_EQUAL_STRING("1.4.0", id.version);
    TEST_ASSERT_EQUAL_INT(1, hg_image_is(g_b, sizeof g_b, HG_CHIP_ESP32P4, HG_PROJ_MASTER));
}

static void test_esp32_master_is_not_a_p4_master(void) {
    mk(HG_CHIP_ESP32, HG_PROJ_MASTER, "1.4.0");
    TEST_ASSERT_EQUAL_INT(0, hg_image_is(g_b, sizeof g_b, HG_CHIP_ESP32P4, HG_PROJ_MASTER));
    TEST_ASSERT_EQUAL_INT(1, hg_image_is(g_b, sizeof g_b, HG_CHIP_ESP32, HG_PROJ_MASTER));
}

static void test_zone_image(void) {
    mk(HG_CHIP_ESP32, HG_PROJ_ZONE, "1.4.0");
    TEST_ASSERT_EQUAL_INT(1, hg_image_is(g_b, sizeof g_b, HG_CHIP_ESP32, HG_PROJ_ZONE));
    TEST_ASSERT_EQUAL_INT(0, hg_image_is(g_b, sizeof g_b, HG_CHIP_ESP32, HG_PROJ_MASTER));
}

static void test_c6_radio_image(void) {
    mk(HG_CHIP_ESP32C6, HG_PROJ_CP, "3.0.7");
    TEST_ASSERT_EQUAL_INT(1, hg_image_is(g_b, sizeof g_b, HG_CHIP_ESP32C6, HG_PROJ_CP));
}

static void test_wrong_image_magic(void) {
    hg_image_id_t id;
    mk(HG_CHIP_ESP32P4, HG_PROJ_MASTER, "1.4.0");
    g_b[0] = 0xE8;
    TEST_ASSERT_EQUAL_INT(-2, hg_image_parse(g_b, sizeof g_b, &id));
    TEST_ASSERT_EQUAL_INT(0, hg_image_is(g_b, sizeof g_b, HG_CHIP_ESP32P4, HG_PROJ_MASTER));
}

static void test_wrong_descriptor_magic(void) {
    hg_image_id_t id;
    mk(HG_CHIP_ESP32P4, HG_PROJ_MASTER, "1.4.0");
    g_b[33] ^= 0x01;
    TEST_ASSERT_EQUAL_INT(-3, hg_image_parse(g_b, sizeof g_b, &id));
}

static void test_truncated_at_111(void) {
    hg_image_id_t id;
    mk(HG_CHIP_ESP32P4, HG_PROJ_MASTER, "1.4.0");
    TEST_ASSERT_EQUAL_INT(-1, hg_image_parse(g_b, 111, &id));
    TEST_ASSERT_EQUAL_INT(0, hg_image_is(g_b, 111, HG_CHIP_ESP32P4, HG_PROJ_MASTER));
}

static void test_32_char_project_without_nul(void) {
    hg_image_id_t id;
    char p32[33];
    memset(p32, 'a', 32); p32[32] = '\0';
    mk(HG_CHIP_ESP32P4, p32, "1");
    TEST_ASSERT_EQUAL_INT(0, hg_image_parse(g_b, sizeof g_b, &id));
    TEST_ASSERT_EQUAL_size_t(32, strlen(id.project));
    TEST_ASSERT_EQUAL_INT(1, hg_image_is(g_b, sizeof g_b, HG_CHIP_ESP32P4, p32));
    p32[31] = '\0';                                             /* a 31-char prefix is not the same project */
    TEST_ASSERT_EQUAL_INT(0, hg_image_is(g_b, sizeof g_b, HG_CHIP_ESP32P4, p32));
}

static void test_non_printables_become_dots(void) {
    hg_image_id_t id;
    mk(HG_CHIP_ESP32P4, "ab\x01" "c", "v\x7f");
    TEST_ASSERT_EQUAL_INT(0, hg_image_parse(g_b, sizeof g_b, &id));
    TEST_ASSERT_EQUAL_STRING("ab.c", id.project);
    TEST_ASSERT_EQUAL_STRING("v.", id.version);
}

static void test_null_arguments(void) {
    mk(HG_CHIP_ESP32P4, HG_PROJ_MASTER, "1");
    TEST_ASSERT_EQUAL_INT(-1, hg_image_parse(NULL, 200, NULL));
    TEST_ASSERT_EQUAL_INT(0, hg_image_parse(g_b, sizeof g_b, NULL));       /* out may be NULL: validity only */
    TEST_ASSERT_EQUAL_INT(0, hg_image_is(NULL, 200, HG_CHIP_ESP32P4, HG_PROJ_MASTER));
    TEST_ASSERT_EQUAL_INT(0, hg_image_is(g_b, sizeof g_b, HG_CHIP_ESP32P4, NULL));
}

static int file_has(const char *path, const char *needle) {
    static char buf[16384];
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = '\0';
    return strstr(buf, needle) != NULL;
}

static void test_project_names_match_their_cmakelists(void) {
    TEST_ASSERT_TRUE_MESSAGE(file_has(HG_MASTER_CMAKELISTS, "project(" HG_PROJ_MASTER ")"), HG_MASTER_CMAKELISTS);
    TEST_ASSERT_TRUE_MESSAGE(file_has(HG_ZONE_CMAKELISTS, "project(" HG_PROJ_ZONE ")"), HG_ZONE_CMAKELISTS);
    TEST_ASSERT_TRUE_MESSAGE(file_has(HG_COPROC_CMAKELISTS, "project(" HG_PROJ_CP ")"), HG_COPROC_CMAKELISTS);
    /* HG_PROJ_RESCUE_P4: rescue_p4/CMakeLists.txt does not exist yet; the recovery plan adds its line here when it lands. */
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_p4_master);
    RUN_TEST(test_esp32_master_is_not_a_p4_master);
    RUN_TEST(test_zone_image);
    RUN_TEST(test_c6_radio_image);
    RUN_TEST(test_wrong_image_magic);
    RUN_TEST(test_wrong_descriptor_magic);
    RUN_TEST(test_truncated_at_111);
    RUN_TEST(test_32_char_project_without_nul);
    RUN_TEST(test_non_printables_become_dots);
    RUN_TEST(test_null_arguments);
    RUN_TEST(test_project_names_match_their_cmakelists);
    return UNITY_END();
}
