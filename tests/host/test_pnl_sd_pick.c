#include <string.h>
#include "unity.h"
#include "hg_image.h"
#include "pnl_sd_pick.h"

/* Which .bin files on a card can be installed where. Classification reads only the 112-byte header through hg_image, so
   an unrelated .bin, a truncated file and an image for the wrong chip are all visible but never installable. */

void setUp(void) {}
void tearDown(void) {}

static void mk(pnl_sd_file_t *f, const char *name, uint16_t chip, const char *proj, const char *ver, uint32_t size) {
    memset(f, 0, sizeof *f);
    strcpy(f->name, name);
    strcpy(f->path, "/sdcard/");
    strcat(f->path, name);
    f->size = size;
    f->hdr[0] = HG_IMG_MAGIC;
    f->hdr[12] = (uint8_t)chip; f->hdr[13] = (uint8_t)(chip >> 8);
    f->hdr[32] = 0x32; f->hdr[33] = 0x54; f->hdr[34] = 0xCD; f->hdr[35] = 0xAB;
    memcpy(f->hdr + 48, ver, strlen(ver));
    memcpy(f->hdr + 80, proj, strlen(proj));
    f->hdr_len = HG_IMG_ID_BYTES;
}

static void test_bin_names(void) {
    TEST_ASSERT_EQUAL_INT(1, pnl_sd_is_bin_name("hillgrow_master.bin"));
    TEST_ASSERT_EQUAL_INT(1, pnl_sd_is_bin_name("ZONE.BIN"));
    TEST_ASSERT_EQUAL_INT(1, pnl_sd_is_bin_name("a.Bin"));
    TEST_ASSERT_EQUAL_INT(0, pnl_sd_is_bin_name("notes.bin.txt"));
    TEST_ASSERT_EQUAL_INT(0, pnl_sd_is_bin_name("bin"));
    TEST_ASSERT_EQUAL_INT(0, pnl_sd_is_bin_name(NULL));
}

static void test_classify_master_zone_radio(void) {
    pnl_sd_file_t f;
    char v[33];
    mk(&f, "m.bin", HG_CHIP_ESP32P4, HG_PROJ_MASTER, "1.5.0", 900000);
    TEST_ASSERT_EQUAL_INT(PNL_FW_MASTER, pnl_sd_classify(&f, HG_CHIP_ESP32P4, v));
    TEST_ASSERT_EQUAL_STRING("1.5.0", v);
    mk(&f, "z.bin", HG_CHIP_ESP32, HG_PROJ_ZONE, "1.5.0", 700000);
    TEST_ASSERT_EQUAL_INT(PNL_FW_ZONE, pnl_sd_classify(&f, HG_CHIP_ESP32P4, v));
    mk(&f, "c6.bin", HG_CHIP_ESP32C6, HG_PROJ_CP, "3.0.7", 1100000);
    TEST_ASSERT_EQUAL_INT(PNL_FW_RADIO, pnl_sd_classify(&f, HG_CHIP_ESP32P4, v));
}

static void test_classify_wrong_chip(void) {
    pnl_sd_file_t f;
    mk(&f, "old_master.bin", HG_CHIP_ESP32, HG_PROJ_MASTER, "1.3.0", 900000);   /* the DevKitC master image */
    TEST_ASSERT_EQUAL_INT(PNL_FW_WRONG_CHIP, pnl_sd_classify(&f, HG_CHIP_ESP32P4, NULL));
    mk(&f, "p4zone.bin", HG_CHIP_ESP32P4, HG_PROJ_ZONE, "1.3.0", 900000);
    TEST_ASSERT_EQUAL_INT(PNL_FW_WRONG_CHIP, pnl_sd_classify(&f, HG_CHIP_ESP32P4, NULL));
}

static void test_classify_unknowns(void) {
    pnl_sd_file_t f;
    char v[33] = "junk";
    mk(&f, "other.bin", HG_CHIP_ESP32, "some_other_app", "9", 1000);
    TEST_ASSERT_EQUAL_INT(PNL_FW_UNKNOWN, pnl_sd_classify(&f, HG_CHIP_ESP32P4, v));
    mk(&f, "empty.bin", HG_CHIP_ESP32P4, HG_PROJ_MASTER, "1.5.0", 0);             /* a 0-byte file with a header? no */
    TEST_ASSERT_EQUAL_INT(PNL_FW_UNKNOWN, pnl_sd_classify(&f, HG_CHIP_ESP32P4, v));
    mk(&f, "short.bin", HG_CHIP_ESP32P4, HG_PROJ_MASTER, "1.5.0", 50);
    f.hdr_len = 50;
    TEST_ASSERT_EQUAL_INT(PNL_FW_UNKNOWN, pnl_sd_classify(&f, HG_CHIP_ESP32P4, v));
    memset(f.hdr, 0xFF, sizeof f.hdr);
    f.hdr_len = HG_IMG_ID_BYTES;
    f.size = 5000;
    TEST_ASSERT_EQUAL_INT(PNL_FW_UNKNOWN, pnl_sd_classify(&f, HG_CHIP_ESP32P4, v));
    TEST_ASSERT_EQUAL_STRING("", v);                                              /* version cleared when unknown */
    TEST_ASSERT_EQUAL_INT(PNL_FW_UNKNOWN, pnl_sd_classify(NULL, HG_CHIP_ESP32P4, v));
}

static void test_sort_by_class_then_name(void) {
    pnl_sd_file_t v[5];
    pnl_fw_class_t c[5] = { PNL_FW_UNKNOWN, PNL_FW_ZONE, PNL_FW_WRONG_CHIP, PNL_FW_MASTER, PNL_FW_ZONE };
    const char *names[5] = { "x.bin", "zb.bin", "w.bin", "m.bin", "za.bin" };
    for (int i = 0; i < 5; i++) { memset(&v[i], 0, sizeof v[i]); strcpy(v[i].name, names[i]); }
    pnl_fw_class_t radio = PNL_FW_RADIO;
    (void)radio;
    pnl_sd_sort(v, c, 5);
    TEST_ASSERT_EQUAL_STRING("m.bin", v[0].name);  TEST_ASSERT_EQUAL_INT(PNL_FW_MASTER, c[0]);
    TEST_ASSERT_EQUAL_STRING("za.bin", v[1].name); TEST_ASSERT_EQUAL_INT(PNL_FW_ZONE, c[1]);
    TEST_ASSERT_EQUAL_STRING("zb.bin", v[2].name); TEST_ASSERT_EQUAL_INT(PNL_FW_ZONE, c[2]);
    TEST_ASSERT_EQUAL_STRING("w.bin", v[3].name);  TEST_ASSERT_EQUAL_INT(PNL_FW_WRONG_CHIP, c[3]);
    TEST_ASSERT_EQUAL_STRING("x.bin", v[4].name);  TEST_ASSERT_EQUAL_INT(PNL_FW_UNKNOWN, c[4]);
}

static void test_sort_puts_radio_between_zone_and_wrong_chip(void) {
    pnl_sd_file_t v[3];
    pnl_fw_class_t c[3] = { PNL_FW_WRONG_CHIP, PNL_FW_RADIO, PNL_FW_ZONE };
    for (int i = 0; i < 3; i++) { memset(&v[i], 0, sizeof v[i]); strcpy(v[i].name, "a.bin"); }
    pnl_sd_sort(v, c, 3);
    TEST_ASSERT_EQUAL_INT(PNL_FW_ZONE, c[0]);
    TEST_ASSERT_EQUAL_INT(PNL_FW_RADIO, c[1]);
    TEST_ASSERT_EQUAL_INT(PNL_FW_WRONG_CHIP, c[2]);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_bin_names);
    RUN_TEST(test_classify_master_zone_radio);
    RUN_TEST(test_classify_wrong_chip);
    RUN_TEST(test_classify_unknowns);
    RUN_TEST(test_sort_by_class_then_name);
    RUN_TEST(test_sort_puts_radio_between_zone_and_wrong_chip);
    return UNITY_END();
}
