/* The P4 partition table is hand-maintained and every mistake in it is a
   brick-or-silent-truncation class bug: an app slot that is not 64 KB aligned
   will not boot, an overlap corrupts whichever neighbour is written second, and
   a renamed data partition makes esp_partition_find_first() return NULL at
   runtime (fw_srv looks up "zone_fw" by name, Task 6 looks up "cp_fw"). Parsing
   the real CSV keeps this honest -- a test over a hard-coded copy of the table
   would pass while the shipped file was wrong. */
#include "unity.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

typedef struct { char name[24], type[8]; unsigned long off, size; } part_t;

void setUp(void) {}
void tearDown(void) {}

static int load(part_t *p, int cap) {
    FILE *f = fopen(HG_PARTITIONS_P4_CSV, "r");
    TEST_ASSERT_NOT_NULL_MESSAGE(f, "cannot open partitions_p4.csv");
    char line[256];
    int n = 0;
    while (fgets(line, sizeof line, f) && n < cap) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
        char nm[24], ty[8], sub[16], o[16], sz[16];
        if (sscanf(line, "%23[^,], %7[^,], %15[^,], %15[^,], %15[^,\n]",
                   nm, ty, sub, o, sz) != 5) continue;
        /* trim trailing spaces the CSV uses for column alignment */
        for (char *e = nm + strlen(nm) - 1; e > nm && *e == ' '; e--) *e = 0;
        for (char *e = ty + strlen(ty) - 1; e > ty && *e == ' '; e--) *e = 0;
        snprintf(p[n].name, sizeof p[n].name, "%s", nm);
        snprintf(p[n].type, sizeof p[n].type, "%s", ty);
        p[n].off  = strtoul(o, NULL, 0);
        p[n].size = strtoul(sz, NULL, 0);
        n++;
    }
    fclose(f);
    return n;
}

static const part_t *find(const part_t *p, int n, const char *name) {
    for (int i = 0; i < n; i++) if (strcmp(p[i].name, name) == 0) return &p[i];
    return NULL;
}

void test_p4_table_has_the_partitions_the_firmware_looks_up_by_name(void) {
    part_t p[16];
    int n = load(p, 16);
    TEST_ASSERT_NOT_NULL_MESSAGE(find(p, n, "factory"), "factory (rescue) missing");
    TEST_ASSERT_NOT_NULL_MESSAGE(find(p, n, "ota_0"),   "ota_0 missing");
    TEST_ASSERT_NOT_NULL_MESSAGE(find(p, n, "ota_1"),   "ota_1 missing");
    TEST_ASSERT_NOT_NULL_MESSAGE(find(p, n, "zone_fw"), "zone_fw missing (fw_srv finds it by name)");
    TEST_ASSERT_NOT_NULL_MESSAGE(find(p, n, "cp_fw"),   "cp_fw missing (C6 image)");
}

void test_p4_app_partitions_are_64k_aligned(void) {
    part_t p[16];
    int n = load(p, 16);
    for (int i = 0; i < n; i++) {
        if (strcmp(p[i].type, "app") != 0) continue;
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, p[i].off % 0x10000, p[i].name);
    }
}

void test_p4_partitions_do_not_overlap_and_clear_the_table(void) {
    part_t p[16];
    int n = load(p, 16);
    for (int i = 0; i < n; i++) {
        /* table at 0x8000 on the P4; 0x9000 is the first byte a partition may use */
        TEST_ASSERT_TRUE_MESSAGE(p[i].off >= 0x9000, p[i].name);
        for (int j = i + 1; j < n; j++) {
            unsigned long ae = p[i].off + p[i].size, be = p[j].off + p[j].size;
            TEST_ASSERT_TRUE_MESSAGE(p[i].off >= be || p[j].off >= ae, p[i].name);
        }
    }
}

void test_p4_app_slots_are_large_enough_for_the_master_plus_the_panel_ui(void) {
    part_t p[16];
    int n = load(p, 16);
    /* The ESP32 master is already 966 KB and the panel adds LVGL, a large
       subsetted font and an image decoder. 2 MB (the ESP32 slot size) leaves too
       little; require 4 MB so an OTA cannot be blocked by a slot ceiling. */
    TEST_ASSERT_TRUE_MESSAGE(find(p, n, "ota_0")->size >= 0x400000, "ota_0 under 4 MB");
    TEST_ASSERT_TRUE_MESSAGE(find(p, n, "ota_1")->size >= 0x400000, "ota_1 under 4 MB");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(find(p, n, "ota_0")->size,
                                     find(p, n, "ota_1")->size,
                                     "OTA slots must match in size");
}

void test_p4_cp_fw_holds_the_coprocessor_image(void) {
    part_t p[16];
    int n = load(p, 16);
    /* The built CP image is 1 145 984 B and its own OTA layout allows 1.75 MB
       per slot, so give it 1.5 MB of headroom rather than a snug fit. */
    TEST_ASSERT_TRUE_MESSAGE(find(p, n, "cp_fw")->size >= 0x180000, "cp_fw under 1.5 MB");
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_p4_table_has_the_partitions_the_firmware_looks_up_by_name);
    RUN_TEST(test_p4_app_partitions_are_64k_aligned);
    RUN_TEST(test_p4_partitions_do_not_overlap_and_clear_the_table);
    RUN_TEST(test_p4_app_slots_are_large_enough_for_the_master_plus_the_panel_ui);
    RUN_TEST(test_p4_cp_fw_holds_the_coprocessor_image);
    return UNITY_END();
}
