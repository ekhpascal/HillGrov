/* test_pcfg_edit.c -- the dirty set: per-field, replace-or-append, hw plane refused, blank secrets never exported. */
#include <string.h>
#include "unity.h"
#include "hg_cfg.h"
#include "hg_mcfg.h"
#include "pcfg_edit.h"

static pcfg_edits_t E;   /* ~10 KB: static, never on the stack */

static const hg_field_t *zrow(uint8_t g, const char *k) {
    for (int i = 0; i < HG_FIELD_COUNT; i++)
        if (HG_FIELDS[i].group == g && strcmp(HG_FIELDS[i].key, k) == 0) return &HG_FIELDS[i];
    TEST_FAIL_MESSAGE(k);
    return NULL;
}
static const hg_field_t *mrow(uint8_t g, const char *k) {
    for (int i = 0; i < HG_MFIELD_COUNT; i++)
        if (HG_MFIELDS[i].group == g && strcmp(HG_MFIELDS[i].key, k) == 0) return &HG_MFIELDS[i];
    TEST_FAIL_MESSAGE(k);
    return NULL;
}
void setUp(void) { pcfg_edits_reset(&E, PCFG_TABLE_ZONE, 2); }
void tearDown(void) {}

static void test_reset(void) {
    TEST_ASSERT_EQUAL_INT(0, E.n);
    TEST_ASSERT_EQUAL_INT(PCFG_TABLE_ZONE, E.table);
    TEST_ASSERT_EQUAL_UINT8(2, E.zone);
}
static void test_append_then_replace(void) {
    const hg_field_t *t = zrow(HG_G_WATER, "TARGET");
    TEST_ASSERT_EQUAL_INT(0, pcfg_edits_set(&E, HG_G_WATER, 1, t, "55"));
    TEST_ASSERT_EQUAL_INT(1, E.n);
    TEST_ASSERT_EQUAL_INT(0, pcfg_edits_set(&E, HG_G_WATER, 1, t, "56"));   /* same key: replaced */
    TEST_ASSERT_EQUAL_INT(1, E.n);
    TEST_ASSERT_EQUAL_INT(0, pcfg_edits_set(&E, HG_G_WATER, 2, t, "40"));   /* other shelf: appended */
    TEST_ASSERT_EQUAL_INT(2, E.n);
    const psvc_fedit_t *d = pcfg_edits_get(&E, HG_G_WATER, 1, t);
    TEST_ASSERT_NOT_NULL(d);
    TEST_ASSERT_EQUAL_STRING("56", d->text);
    TEST_ASSERT_EQUAL_INT8(1, d->idx);
    TEST_ASSERT_EQUAL_PTR(t, d->f);
    TEST_ASSERT_NULL(pcfg_edits_get(&E, HG_G_WATER, 3, t));
}
static void test_scope0_idx_normalised(void) {
    const hg_field_t *name = zrow(HG_G_ZONECFG, "NAME");
    TEST_ASSERT_EQUAL_INT(0, pcfg_edits_set(&E, HG_G_ZONECFG, 3, name, "north"));
    TEST_ASSERT_EQUAL_INT8(-1, E.d[0].idx);
    TEST_ASSERT_NOT_NULL(pcfg_edits_get(&E, HG_G_ZONECFG, -1, name));
    TEST_ASSERT_NOT_NULL(pcfg_edits_get(&E, HG_G_ZONECFG, 0, name));
    TEST_ASSERT_EQUAL_INT(0, pcfg_edits_set(&E, HG_G_ZONECFG, -1, name, "south"));
    TEST_ASSERT_EQUAL_INT(1, E.n);
}
static void test_hw_row_refused(void) {
    TEST_ASSERT_EQUAL_INT(-2, pcfg_edits_set(&E, HG_G_HWSHELF, 0, zrow(HG_G_HWSHELF, "PUMP"), "3"));
    TEST_ASSERT_EQUAL_INT(-2, pcfg_edits_set(&E, HG_G_HW, -1, zrow(HG_G_HW, "SHELVES"), "2"));
    TEST_ASSERT_EQUAL_INT(-2, pcfg_edits_set(&E, HG_G_CAL, 1, zrow(HG_G_CAL, "DRY_A"), "2800"));
    TEST_ASSERT_EQUAL_INT(0, E.n);
}
static void test_bad_args_and_long_text(void) {
    char big[PSVC_FEDIT_TEXT_MAX + 1];
    memset(big, 'a', sizeof big - 1);
    big[sizeof big - 1] = '\0';                                    /* 65 characters: one too many */
    const hg_field_t *crop = zrow(HG_G_SHELF, "CROP");
    TEST_ASSERT_EQUAL_INT(-1, pcfg_edits_set(&E, HG_G_SHELF, 0, crop, big));
    big[PSVC_FEDIT_TEXT_MAX - 1] = '\0';                           /* 64 characters: fits */
    TEST_ASSERT_EQUAL_INT(0, pcfg_edits_set(&E, HG_G_SHELF, 0, crop, big));
    TEST_ASSERT_EQUAL_INT(-1, pcfg_edits_set(&E, HG_G_SHELF, 0, crop, NULL));
    TEST_ASSERT_EQUAL_INT(-1, pcfg_edits_set(&E, HG_G_SHELF, 0, NULL, "x"));
    TEST_ASSERT_EQUAL_INT(-1, pcfg_edits_set(&E, HG_G_SHELF, 128, crop, "x"));
    TEST_ASSERT_EQUAL_INT(-1, pcfg_edits_set(NULL, HG_G_SHELF, 0, crop, "x"));
}
static void test_full(void) {
    const hg_field_t *t = zrow(HG_G_WATER, "TARGET");
    for (int i = 0; i < PCFG_EDIT_MAX; i++)                         /* distinct keys by idx 0..127 */
        TEST_ASSERT_EQUAL_INT(0, pcfg_edits_set(&E, HG_G_WATER, i, t, "50"));
    TEST_ASSERT_EQUAL_INT(PCFG_EDIT_MAX, E.n);
    TEST_ASSERT_EQUAL_INT(-1, pcfg_edits_set(&E, HG_G_LIGHT, 0, zrow(HG_G_LIGHT, "WHITE"), "10"));
    TEST_ASSERT_EQUAL_INT(0, pcfg_edits_set(&E, HG_G_WATER, 5, t, "51"));   /* a replace still works when full */
}
static void test_drop_keeps_order(void) {
    const hg_field_t *a = zrow(HG_G_WATER, "TARGET"), *b = zrow(HG_G_WATER, "HYST"), *c = zrow(HG_G_WATER, "DOSE_S");
    pcfg_edits_set(&E, HG_G_WATER, 0, a, "50");
    pcfg_edits_set(&E, HG_G_WATER, 0, b, "5");
    pcfg_edits_set(&E, HG_G_WATER, 0, c, "20");
    TEST_ASSERT_EQUAL_INT(0, pcfg_edits_drop(&E, HG_G_WATER, 0, b));
    TEST_ASSERT_EQUAL_INT(2, E.n);
    TEST_ASSERT_EQUAL_PTR(a, E.d[0].f);
    TEST_ASSERT_EQUAL_PTR(c, E.d[1].f);
    TEST_ASSERT_EQUAL_INT(0, E.d[2].text[0]);                       /* the vacated slot is zeroed */
    TEST_ASSERT_EQUAL_INT(-1, pcfg_edits_drop(&E, HG_G_WATER, 0, b));
}
static void test_export_drops_blank_secret_only(void) {
    pcfg_edits_reset(&E, PCFG_TABLE_MASTER, 0);
    TEST_ASSERT_EQUAL_INT(0, pcfg_edits_set(&E, HG_MG_WIFI, -1, mrow(HG_MG_WIFI, "STA_PASS"), ""));
    TEST_ASSERT_EQUAL_INT(0, pcfg_edits_set(&E, HG_MG_WIFI, -1, mrow(HG_MG_WIFI, "STA_SSID"), ""));
    TEST_ASSERT_EQUAL_INT(0, pcfg_edits_set(&E, HG_MG_SYS, -1, mrow(HG_MG_SYS, "HOSTNAME"), "gh1"));
    psvc_fedit_t out[4];
    TEST_ASSERT_EQUAL_INT(2, pcfg_edits_export(&E, out, 4));
    TEST_ASSERT_EQUAL_STRING("STA_SSID", out[0].f->key);            /* a blank SSID is a real edit: open STA off */
    TEST_ASSERT_EQUAL_STRING("", out[0].text);
    TEST_ASSERT_EQUAL_STRING("HOSTNAME", out[1].f->key);
    TEST_ASSERT_EQUAL_INT(-1, pcfg_edits_export(&E, out, 1));       /* never a partial set */
}
static void test_export_zone_keeps_blank_text(void) {
    TEST_ASSERT_EQUAL_INT(0, pcfg_edits_set(&E, HG_G_SHELF, 0, zrow(HG_G_SHELF, "CROP"), ""));
    psvc_fedit_t out[2];
    TEST_ASSERT_EQUAL_INT(1, pcfg_edits_export(&E, out, 2));
}
static void test_export_none(void) {
    psvc_fedit_t out[1];
    TEST_ASSERT_EQUAL_INT(0, pcfg_edits_export(&E, out, 1));
    TEST_ASSERT_EQUAL_INT(0, pcfg_edits_export(&E, NULL, 0));
}
static void test_wipe(void) {
    pcfg_edits_reset(&E, PCFG_TABLE_MASTER, 0);
    pcfg_edits_set(&E, HG_MG_WIFI, -1, mrow(HG_MG_WIFI, "STA_PASS"), "hunter22hunter22");
    pcfg_edits_wipe(&E);
    TEST_ASSERT_EQUAL_INT(0, E.n);
    TEST_ASSERT_EQUAL_INT(PCFG_TABLE_MASTER, E.table);
    TEST_ASSERT_EQUAL_UINT8(0, E.zone);
    for (size_t i = 0; i < sizeof E.d[0].text; i++) TEST_ASSERT_EQUAL_INT(0, E.d[0].text[i]);
    TEST_ASSERT_NULL(E.d[0].f);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_reset);
    RUN_TEST(test_append_then_replace);
    RUN_TEST(test_scope0_idx_normalised);
    RUN_TEST(test_hw_row_refused);
    RUN_TEST(test_bad_args_and_long_text);
    RUN_TEST(test_full);
    RUN_TEST(test_drop_keeps_order);
    RUN_TEST(test_export_drops_blank_secret_only);
    RUN_TEST(test_export_zone_keeps_blank_text);
    RUN_TEST(test_export_none);
    RUN_TEST(test_wipe);
    return UNITY_END();
}
