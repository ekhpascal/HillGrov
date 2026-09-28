/* test_pcfg_gen.c -- the config-widget generator: given a field row, which widget kind, bounds,
 * read-only flag and text form come out (panel spec "Testing" 1). The 35 cases of map-cfg §8. */
#include <stdio.h>
#include <string.h>
#include "unity.h"
#include "hg_cfg.h"
#include "hg_mcfg.h"
#include "pcfg_gen.h"
#include "pcfg_pres.h"

static hg_zone_hw_t  hw;
static hg_zone_cfg_t cfg;
void setUp(void) { hg_defaults_hw(&hw); hg_defaults_cfg(&cfg); }
void tearDown(void) {}

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
static pcfg_spec_t zspec(uint8_t g, const char *k) {
    pcfg_spec_t s;
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, pcfg_spec_for(PCFG_TABLE_ZONE, zrow(g, k), &s), k);
    return s;
}
static pcfg_spec_t mspec(uint8_t g, const char *k) {
    pcfg_spec_t s;
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, pcfg_spec_for(PCFG_TABLE_MASTER, mrow(g, k), &s), k);
    return s;
}
static const char *fmt(const pcfg_spec_t *s, const char *raw) {
    static char b[48];
    pcfg_format(s, raw, b, sizeof b);
    return b;
}
static void assert_ascii(const char *s) {
    TEST_ASSERT_NOT_NULL(s);
    for (const char *p = s; *p; p++)
        TEST_ASSERT_TRUE_MESSAGE((unsigned char)*p >= 0x20 && (unsigned char)*p <= 0x7E, s);
}

/* 1 */
static void test_every_zone_row_generates(void) {
    for (int i = 0; i < HG_FIELD_COUNT; i++) {
        const hg_field_t *f = &HG_FIELDS[i];
        pcfg_spec_t s;
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, pcfg_spec_for(PCFG_TABLE_ZONE, f, &s), f->key);
        TEST_ASSERT_TRUE_MESSAGE(s.edit_kind < PCFG_K_READONLY, f->key);    /* a real editor for every row */
        TEST_ASSERT_TRUE_MESSAGE(s.min <= s.max, f->key);
        TEST_ASSERT_EQUAL_INT_MESSAGE(hg_group_is_hw(f->group), s.readonly, f->key);
        TEST_ASSERT_EQUAL_INT_MESSAGE(s.readonly ? PCFG_K_READONLY : s.edit_kind, s.kind, f->key);
        TEST_ASSERT_NOT_NULL(s.label);
        TEST_ASSERT_NOT_NULL(s.unit);
    }
}
/* 2 */
static void test_every_master_row_text_or_secret(void) {
    for (int i = 0; i < HG_MFIELD_COUNT; i++) {
        pcfg_spec_t s;
        TEST_ASSERT_EQUAL_INT(0, pcfg_spec_for(PCFG_TABLE_MASTER, &HG_MFIELDS[i], &s));
        TEST_ASSERT_EQUAL_INT(0, s.readonly);
        TEST_ASSERT_TRUE_MESSAGE(s.kind == PCFG_K_TEXT || s.kind == PCFG_K_SECRET, HG_MFIELDS[i].key);
        TEST_ASSERT_EQUAL_INT(hg_mcfg_is_secret(&HG_MFIELDS[i]) ? PCFG_K_SECRET : PCFG_K_TEXT, s.kind);
    }
}
/* 3: the "field added later" tripwire */
static int pres_count(pcfg_table_t t, uint8_t g, const char *k) {
    int n = 0;
    for (int i = 0; i < PCFG_PRES_COUNT; i++)
        if (PCFG_PRES[i].table == t && PCFG_PRES[i].group == g && strcmp(PCFG_PRES[i].key, k) == 0) n++;
    return n;
}
static void test_presentation_covers_every_row_exactly_once(void) {
    for (int i = 0; i < HG_FIELD_COUNT; i++)
        TEST_ASSERT_EQUAL_INT_MESSAGE(1, pres_count(PCFG_TABLE_ZONE, HG_FIELDS[i].group, HG_FIELDS[i].key), HG_FIELDS[i].key);
    for (int i = 0; i < HG_MFIELD_COUNT; i++)
        TEST_ASSERT_EQUAL_INT_MESSAGE(1, pres_count(PCFG_TABLE_MASTER, HG_MFIELDS[i].group, HG_MFIELDS[i].key), HG_MFIELDS[i].key);
    for (int p = 0; p < PCFG_PRES_COUNT; p++) {          /* no orphan: every entry names a real row */
        const pcfg_pres_t *e = &PCFG_PRES[p];
        const hg_field_t *tab = e->table == PCFG_TABLE_MASTER ? HG_MFIELDS : HG_FIELDS;
        int n = e->table == PCFG_TABLE_MASTER ? HG_MFIELD_COUNT : HG_FIELD_COUNT, found = 0;
        for (int i = 0; i < n; i++) if (tab[i].group == e->group && strcmp(tab[i].key, e->key) == 0) found++;
        TEST_ASSERT_EQUAL_INT_MESSAGE(1, found, e->key);
        TEST_ASSERT_TRUE_MESSAGE(e->label && e->label[0], e->key);
        assert_ascii(e->label);
        assert_ascii(e->unit ? e->unit : "");
    }
    TEST_ASSERT_EQUAL_INT(HG_FIELD_COUNT + HG_MFIELD_COUNT, PCFG_PRES_COUNT);
    TEST_ASSERT_NULL(pcfg_pres_find(PCFG_TABLE_ZONE, HG_G_WATER, "NO_SUCH_KEY"));
}
/* 4: pins today's tables -- update deliberately when a row is added */
static void test_counts(void) {
    int ro = 0, ed = 0, sec = 0;
    for (int i = 0; i < HG_FIELD_COUNT; i++) {
        pcfg_spec_t s; pcfg_spec_for(PCFG_TABLE_ZONE, &HG_FIELDS[i], &s);
        if (s.kind == PCFG_K_READONLY) ro++; else ed++;
    }
    for (int i = 0; i < HG_MFIELD_COUNT; i++) {
        pcfg_spec_t s; pcfg_spec_for(PCFG_TABLE_MASTER, &HG_MFIELDS[i], &s);
        if (s.kind == PCFG_K_SECRET) sec++;
    }
    TEST_ASSERT_EQUAL_INT(23, ro);
    TEST_ASSERT_EQUAL_INT(35, ed);
    TEST_ASSERT_EQUAL_INT(2, sec);
}
/* 5 */
static void test_enabled_is_switch(void) {
    pcfg_spec_t s = zspec(HG_G_SHELF, "ENABLED");
    TEST_ASSERT_EQUAL_INT(PCFG_K_SWITCH, s.kind);
    TEST_ASSERT_EQUAL_INT(PCFG_FMT_BOOL, s.fmt);
    TEST_ASSERT_EQUAL_STRING("on", fmt(&s, "1"));
    TEST_ASSERT_EQUAL_STRING("off", fmt(&s, "0"));
}
/* 6 */
static void test_water_mode_segmented(void) {
    pcfg_spec_t s = zspec(HG_G_WATER, "MODE");
    TEST_ASSERT_EQUAL_INT(PCFG_K_SEGMENTED, s.kind);
    TEST_ASSERT_EQUAL_UINT8(2, s.n_opts);
    TEST_ASSERT_EQUAL_STRING("OFF", s.opts[0]);
    TEST_ASSERT_EQUAL_STRING("AUTO", s.opts[1]);
    TEST_ASSERT_EQUAL_INT32(0, s.min);
    TEST_ASSERT_EQUAL_INT32(1, s.max);
}
/* 7 */
static void test_fan_mode_four_options(void) {
    pcfg_spec_t s = zspec(HG_G_FAN, "MODE");
    TEST_ASSERT_EQUAL_INT(PCFG_K_SEGMENTED, s.kind);
    TEST_ASSERT_EQUAL_UINT8(4, s.n_opts);
    TEST_ASSERT_EQUAL_STRING("CYCLE", s.opts[3]);
}
/* 8 */
static void test_five_option_enum_is_roller(void) {
    static const hg_field_t syn5 = { HG_G_WATER, "SYN5", 0, HG_T_ENUM, 0, 4, "A|B|C|D|E" };
    pcfg_spec_t s;
    TEST_ASSERT_EQUAL_INT(0, pcfg_spec_for(PCFG_TABLE_ZONE, &syn5, &s));
    TEST_ASSERT_EQUAL_INT(PCFG_K_ROLLER, s.kind);
    TEST_ASSERT_EQUAL_INT(PCFG_ROLL_ENUM, s.roller);
    TEST_ASSERT_EQUAL_UINT8(5, s.n_opts);
    TEST_ASSERT_EQUAL_STRING("SYN5", s.label);          /* no presentation entry: the key */
}
/* 9 */
static void test_soil_backend_readonly_segmented(void) {
    pcfg_spec_t s = zspec(HG_G_HW, "SOIL_BACKEND");
    TEST_ASSERT_EQUAL_INT(PCFG_K_READONLY, s.kind);
    TEST_ASSERT_EQUAL_INT(PCFG_K_SEGMENTED, s.edit_kind);
    TEST_ASSERT_EQUAL_INT(PCFG_FMT_ENUM, s.fmt);
    TEST_ASSERT_EQUAL_UINT8(1, s.readonly);
    TEST_ASSERT_EQUAL_STRING("ADS1115", fmt(&s, "ADS1115"));
}
/* 10 */
static void test_hhmm_roller(void) {
    pcfg_spec_t s = zspec(HG_G_LIGHT, "ON");
    TEST_ASSERT_EQUAL_INT(PCFG_K_ROLLER, s.kind);
    TEST_ASSERT_EQUAL_INT(PCFG_ROLL_HHMM, s.roller);
    TEST_ASSERT_EQUAL_INT32(0, s.min);
    TEST_ASSERT_EQUAL_INT32(1439, s.max);
    TEST_ASSERT_EQUAL_INT32(1, s.step);
    TEST_ASSERT_EQUAL_STRING("06:30", fmt(&s, "390"));
    TEST_ASSERT_EQUAL_STRING("06:30", fmt(&s, "06:30"));
}
/* 11-14 */
static void test_stepper_bounds(void) {
    pcfg_spec_t s = zspec(HG_G_WATER, "DOSE_S");
    TEST_ASSERT_EQUAL_INT(PCFG_K_STEPPER, s.kind);
    TEST_ASSERT_EQUAL_INT32(1, s.min);
    TEST_ASSERT_EQUAL_INT32(300, s.max);
    TEST_ASSERT_EQUAL_INT(PCFG_KB_NUMERIC, s.keyboard);
    TEST_ASSERT_EQUAL_STRING("s", s.unit);
    s = zspec(HG_G_WATER, "HYST");
    TEST_ASSERT_EQUAL_INT32(1, s.min);                  /* a non-zero min is kept */
    TEST_ASSERT_EQUAL_INT32(30, s.max);
    TEST_ASSERT_EQUAL_INT32(0, s.big_step);
    s = zspec(HG_G_VIB, "INTENSITY");
    TEST_ASSERT_EQUAL_INT32(20, s.min);
    TEST_ASSERT_EQUAL_INT32(100, s.max);
    TEST_ASSERT_EQUAL_STRING("%", s.unit);
    s = zspec(HG_G_WATER, "INTERVAL_MIN");
    TEST_ASSERT_EQUAL_INT32(10, s.min);
    TEST_ASSERT_EQUAL_INT32(1440, s.max);
    TEST_ASSERT_EQUAL_INT32(10, s.big_step);
    TEST_ASSERT_EQUAL_UINT8(4, s.max_len);              /* keypad: at most "1440" */
}
/* 15 */
static void test_dli_scaled(void) {
    pcfg_spec_t s = zspec(HG_G_LIGHT, "DLI");
    TEST_ASSERT_EQUAL_INT16(10, s.scale_div);
    TEST_ASSERT_EQUAL_INT(PCFG_FMT_SCALED, s.fmt);
    TEST_ASSERT_EQUAL_STRING("12.5", fmt(&s, "125"));
    TEST_ASSERT_EQUAL_STRING("off", fmt(&s, "0"));
    TEST_ASSERT_EQUAL_STRING("mol/m2/d", s.unit);
}
/* 16 */
static void test_step_clamps(void) {
    pcfg_spec_t s = zspec(HG_G_WATER, "DOSE_S");
    TEST_ASSERT_EQUAL_INT32(300, pcfg_step(&s, 300, +1, 0));
    TEST_ASSERT_EQUAL_INT32(1, pcfg_step(&s, 1, -1, 0));
    TEST_ASSERT_EQUAL_INT32(1, pcfg_step(&s, 5, -1, 1));
    TEST_ASSERT_EQUAL_INT32(21, pcfg_step(&s, 20, +1, 0));
    s = zspec(HG_G_WATER, "INTERVAL_MIN");
    TEST_ASSERT_EQUAL_INT32(1440, pcfg_step(&s, 1435, +1, 1));
    TEST_ASSERT_EQUAL_INT32(130, pcfg_step(&s, 120, +1, 1));
    TEST_ASSERT_EQUAL_INT32(120, pcfg_step(&s, 120, 0, 0));
}
/* 17 */
static void test_pin_none(void) {
    pcfg_spec_t s = zspec(HG_G_HWSHELF, "PUMP");
    TEST_ASSERT_EQUAL_INT(PCFG_K_READONLY, s.kind);
    TEST_ASSERT_EQUAL_INT(PCFG_K_ROLLER, s.edit_kind);
    TEST_ASSERT_EQUAL_INT(PCFG_ROLL_PIN, s.roller);
    TEST_ASSERT_EQUAL_INT32(255, s.none_value);
    TEST_ASSERT_EQUAL_STRING("none", fmt(&s, "NONE"));
    TEST_ASSERT_EQUAL_STRING("3", fmt(&s, "3"));
    int32_t v;
    TEST_ASSERT_EQUAL_INT(0, pcfg_parse_raw(&s, "NONE", &v));
    TEST_ASSERT_EQUAL_INT32(255, v);
}
/* 18-19 */
static void test_hex(void) {
    pcfg_spec_t s = zspec(HG_G_HW, "PCA_ADDR");
    TEST_ASSERT_EQUAL_INT(PCFG_K_READONLY, s.kind);
    TEST_ASSERT_EQUAL_INT(PCFG_FMT_HEX, s.fmt);
    TEST_ASSERT_EQUAL_STRING("0x40", fmt(&s, "64"));
    s = zspec(HG_G_HW, "PCF_ACTLOW");
    TEST_ASSERT_EQUAL_INT(PCFG_FMT_HEX, s.fmt);
    TEST_ASSERT_EQUAL_STRING("0xFFFF", fmt(&s, "65535"));
    TEST_ASSERT_EQUAL_STRING("0x0000", fmt(&s, "0"));
}
/* 20-25 */
static void test_text_rows(void) {
    pcfg_spec_t s = zspec(HG_G_ZONECFG, "NAME");
    TEST_ASSERT_EQUAL_INT(PCFG_K_TEXT, s.kind);
    TEST_ASSERT_EQUAL_INT(PCFG_KB_TEXT_NOSPACE, s.keyboard);
    TEST_ASSERT_EQUAL_UINT8(15, s.max_len);
    TEST_ASSERT_EQUAL_UINT8(1, s.min_len);
    s = zspec(HG_G_SHELF, "CROP");
    TEST_ASSERT_EQUAL_INT(PCFG_KB_TEXT_NOSPACE, s.keyboard);
    TEST_ASSERT_EQUAL_UINT8(15, s.max_len);
    TEST_ASSERT_EQUAL_UINT8(0, s.min_len);
    s = mspec(HG_MG_WIFI, "STA_SSID");
    TEST_ASSERT_EQUAL_INT(PCFG_K_TEXT, s.kind);
    TEST_ASSERT_EQUAL_INT(PCFG_KB_TEXT, s.keyboard);
    TEST_ASSERT_EQUAL_UINT8(32, s.max_len);
    TEST_ASSERT_EQUAL_UINT8(0, s.min_len);
    s = mspec(HG_MG_WIFI, "AP_SSID");
    TEST_ASSERT_EQUAL_UINT8(32, s.max_len);
    TEST_ASSERT_EQUAL_UINT8(1, s.min_len);
    s = mspec(HG_MG_WIFI, "STA_PASS");
    TEST_ASSERT_EQUAL_INT(PCFG_K_SECRET, s.kind);
    TEST_ASSERT_EQUAL_UINT8(63, s.max_len);             /* the validator's 63, not the row's 64 (D12) */
    s = mspec(HG_MG_WIFI, "AP_PASS");
    TEST_ASSERT_EQUAL_INT(PCFG_K_SECRET, s.kind);
    TEST_ASSERT_EQUAL_UINT8(63, s.max_len);
    s = mspec(HG_MG_TIME, "TZ");
    TEST_ASSERT_EQUAL_INT(PCFG_K_TEXT, s.kind);
    TEST_ASSERT_EQUAL_UINT8(47, s.max_len);
    TEST_ASSERT_EQUAL_INT(PCFG_KB_TEXT_NOSPACE, s.keyboard);
    s = mspec(HG_MG_TIME, "NTP");
    TEST_ASSERT_EQUAL_INT(PCFG_KB_TEXT_NOSPACE, s.keyboard);
    TEST_ASSERT_EQUAL_UINT8(1, s.min_len);
    s = mspec(HG_MG_SYS, "HOSTNAME");
    TEST_ASSERT_EQUAL_INT(PCFG_KB_HOSTNAME, s.keyboard);
    TEST_ASSERT_EQUAL_UINT8(23, s.max_len);
    TEST_ASSERT_EQUAL_INT32(-1, pcfg_parse_raw(&s, "hillgrow", &(int32_t){0}));   /* text carries no number */
}
/* 26 */
static void test_unknown_ftype_renders(void) {
    static const hg_field_t syn99 = { HG_G_ZONECFG, "SYN99", 0, 99, 0, 1, NULL };
    pcfg_spec_t s;
    TEST_ASSERT_EQUAL_INT(-1, pcfg_spec_for(PCFG_TABLE_ZONE, &syn99, &s));
    TEST_ASSERT_EQUAL_INT(PCFG_K_READONLY, s.kind);
    TEST_ASSERT_EQUAL_INT(PCFG_FMT_TEXT, s.fmt);
    TEST_ASSERT_EQUAL_STRING("SYN99", s.label);
    TEST_ASSERT_EQUAL_INT(-1, pcfg_spec_for(PCFG_TABLE_ZONE, NULL, &s));
    TEST_ASSERT_EQUAL_INT(PCFG_K_READONLY, s.kind);
    TEST_ASSERT_EQUAL_INT(-1, pcfg_spec_for(PCFG_TABLE_ZONE, &syn99, NULL));
}
/* 27: the same uint8_t group means different things in the two tables */
static void test_group_id_resolved_per_table(void) {
    pcfg_spec_t m, z;
    TEST_ASSERT_NOT_NULL(pcfg_pres_find(PCFG_TABLE_MASTER, HG_MG_WIFI, "STA_SSID"));
    TEST_ASSERT_NULL(pcfg_pres_find(PCFG_TABLE_ZONE, HG_G_ZONECFG, "STA_SSID"));
    const hg_field_t *host = mrow(HG_MG_SYS, "HOSTNAME");     /* group 3: SYS here, WATER in the zone table */
    TEST_ASSERT_EQUAL_INT(0, pcfg_spec_for(PCFG_TABLE_MASTER, host, &m));
    TEST_ASSERT_EQUAL_INT(0, pcfg_spec_for(PCFG_TABLE_ZONE, host, &z));
    TEST_ASSERT_EQUAL_INT(PCFG_KB_HOSTNAME, m.keyboard);
    TEST_ASSERT_EQUAL_INT(PCFG_KB_TEXT, z.keyboard);          /* zone table: no entry, generator defaults */
    TEST_ASSERT_EQUAL_STRING("HOSTNAME", z.label);
    const hg_field_t *shelves = zrow(HG_G_HW, "SHELVES");     /* group 7: HW here, no such master group */
    TEST_ASSERT_EQUAL_INT(0, pcfg_spec_for(PCFG_TABLE_ZONE, shelves, &z));
    TEST_ASSERT_EQUAL_INT(PCFG_K_READONLY, z.kind);
    TEST_ASSERT_EQUAL_INT(-1, pcfg_spec_for(PCFG_TABLE_MASTER, shelves, &m));
    for (int i = 0; i < HG_MFIELD_COUNT; i++) {
        TEST_ASSERT_EQUAL_INT(0, pcfg_spec_for(PCFG_TABLE_MASTER, &HG_MFIELDS[i], &m));
        TEST_ASSERT_EQUAL_UINT8(0, m.readonly);
    }
}
/* 28 */
static void test_tighten_dose(void) {
    const hg_field_t *dose = zrow(HG_G_WATER, "DOSE_S");
    pcfg_spec_t s = zspec(HG_G_WATER, "DOSE_S");
    hw.shelf[1].pump_max_run_s = 60;
    pcfg_tighten(&s, dose, 1, &hw);
    TEST_ASSERT_EQUAL_INT32(60, s.max);
    s = zspec(HG_G_WATER, "DOSE_S");
    pcfg_tighten(&s, dose, 1, NULL);
    TEST_ASSERT_EQUAL_INT32(300, s.max);
    hw.shelf[1].pump_max_run_s = 500;
    pcfg_tighten(&s, dose, 1, &hw);
    TEST_ASSERT_EQUAL_INT32(300, s.max);                /* never widens */
    pcfg_spec_t t = zspec(HG_G_WATER, "TARGET");
    hw.shelf[1].pump_max_run_s = 60;
    pcfg_tighten(&t, zrow(HG_G_WATER, "TARGET"), 1, &hw);
    TEST_ASSERT_EQUAL_INT32(100, t.max);                /* other rows untouched */
}
/* 29-33 plus the other path shapes the panel receives */
static void locate_ok(pcfg_table_t t, const char *path, uint8_t g, int ix, const hg_field_t *row) {
    uint8_t og = 0xAA; int oi = 99; const hg_field_t *orow = NULL;
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, pcfg_locate(t, path, &og, &oi, &orow), path);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(g, og, path);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ix, oi, path);
    TEST_ASSERT_EQUAL_PTR_MESSAGE(row, orow, path);
}
static void locate_bad(pcfg_table_t t, const char *path) {
    uint8_t og; int oi; const hg_field_t *orow;
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, pcfg_locate(t, path, &og, &oi, &orow), path);
}
static void test_locate_shapes(void) {
    locate_ok(PCFG_TABLE_ZONE, "cfg.shelf[1].WATER.TARGET", HG_G_WATER, 1, zrow(HG_G_WATER, "TARGET"));
    locate_ok(PCFG_TABLE_ZONE, "shelf[2].light.off", HG_G_LIGHT, 2, zrow(HG_G_LIGHT, "OFF"));
    locate_ok(PCFG_TABLE_ZONE, "zonecfg.name", HG_G_ZONECFG, -1, zrow(HG_G_ZONECFG, "NAME"));
    locate_ok(PCFG_TABLE_ZONE, "cfg.ZONECFG.NAME", HG_G_ZONECFG, -1, zrow(HG_G_ZONECFG, "NAME"));
    locate_ok(PCFG_TABLE_ZONE, "shelf[0].enabled", HG_G_SHELF, 0, zrow(HG_G_SHELF, "ENABLED"));
    locate_ok(PCFG_TABLE_ZONE, "cfg.aux[0].AUX.MODE", HG_G_AUX, 0, zrow(HG_G_AUX, "MODE"));
    locate_ok(PCFG_TABLE_ZONE, "aux.pulse_s", HG_G_AUX, -1, zrow(HG_G_AUX, "PULSE_S"));
    locate_ok(PCFG_TABLE_ZONE, "shelf[0].water.dose_s", HG_G_WATER, 0, zrow(HG_G_WATER, "DOSE_S"));
    locate_ok(PCFG_TABLE_MASTER, "WIFI.AP_PASS", HG_MG_WIFI, -1, mrow(HG_MG_WIFI, "AP_PASS"));
    locate_ok(PCFG_TABLE_MASTER, "SYS.HOSTNAME", HG_MG_SYS, -1, mrow(HG_MG_SYS, "HOSTNAME"));
    locate_bad(PCFG_TABLE_ZONE, "hw.aux_pin");                  /* no row carries it: caller shows a banner */
    locate_bad(PCFG_TABLE_ZONE, "");
    locate_bad(PCFG_TABLE_ZONE, "cfg.shelf[4].WATER.TARGET");
    locate_bad(PCFG_TABLE_ZONE, "cfg.aux[2].AUX.MODE");
    locate_bad(PCFG_TABLE_ZONE, "shelf[1].AUX.MODE");
    locate_bad(PCFG_TABLE_ZONE, "cfg.WATER.BOGUS");
    locate_bad(PCFG_TABLE_ZONE, "WIFI.AP_PASS");
    locate_bad(PCFG_TABLE_MASTER, "BOGUS.KEY");
    locate_bad(PCFG_TABLE_MASTER, "WIFI");
    TEST_ASSERT_EQUAL_INT(-1, pcfg_locate(PCFG_TABLE_ZONE, NULL, &(uint8_t){0}, &(int){0}, &(const hg_field_t *){NULL}));
}
/* 34: the generator never produces a value the writer refuses */
static void test_round_trip_every_editable_row(void) {
    int checked = 0;
    for (int i = 0; i < HG_FIELD_COUNT; i++) {
        const hg_field_t *f = &HG_FIELDS[i];
        pcfg_spec_t s; char raw[32], t[32]; int32_t v;
        TEST_ASSERT_EQUAL_INT(0, pcfg_spec_for(PCFG_TABLE_ZONE, f, &s));
        if (s.readonly) continue;
        void *base = hg_field_base(f->group, 0, &hw, &cfg);
        TEST_ASSERT_NOT_NULL(base);
        TEST_ASSERT_EQUAL_INT(0, hg_field_read(f, base, raw, sizeof raw));
        if (s.kind == PCFG_K_TEXT) {
            TEST_ASSERT_EQUAL_INT_MESSAGE(0, hg_field_write(f, base, raw), f->key);
            checked++;
            continue;
        }
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, pcfg_parse_raw(&s, raw, &v), f->key);
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, pcfg_raw_text(&s, v, t, sizeof t), f->key);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(raw, t, f->key);
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, hg_field_write(f, base, t), f->key);
        checked++;
    }
    TEST_ASSERT_EQUAL_INT(35, checked);
}
/* 35: bounds agree with the writer -- every reachable value accepted, min-1 / max+1 refused */
static void test_numeric_bounds_agree_with_writer(void) {
    for (int i = 0; i < HG_FIELD_COUNT; i++) {
        const hg_field_t *f = &HG_FIELDS[i];
        if (f->type != HG_T_U8 && f->type != HG_T_U16) continue;
        pcfg_spec_t s; char t[16];
        TEST_ASSERT_EQUAL_INT(0, pcfg_spec_for(PCFG_TABLE_ZONE, f, &s));
        void *base = hg_field_base(f->group, 0, &hw, &cfg);
        TEST_ASSERT_NOT_NULL(base);
        TEST_ASSERT_EQUAL_INT(0, pcfg_raw_text(&s, s.min - 1, t, sizeof t));
        TEST_ASSERT_EQUAL_INT_MESSAGE(-2, hg_field_write(f, base, t), f->key);
        TEST_ASSERT_EQUAL_INT(0, pcfg_raw_text(&s, s.max + 1, t, sizeof t));
        TEST_ASSERT_EQUAL_INT_MESSAGE(-2, hg_field_write(f, base, t), f->key);
        int32_t v = s.min;
        for (int guard = 0; ; guard++) {
            TEST_ASSERT_TRUE(guard < 70000);
            TEST_ASSERT_EQUAL_INT(0, pcfg_raw_text(&s, v, t, sizeof t));
            TEST_ASSERT_EQUAL_INT_MESSAGE(0, hg_field_write(f, base, t), f->key);
            if (v == s.max) break;
            int32_t nv = pcfg_step(&s, v, +1, 1);
            TEST_ASSERT_TRUE_MESSAGE(nv > v, f->key);
            v = nv;
        }
    }
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_every_zone_row_generates);
    RUN_TEST(test_every_master_row_text_or_secret);
    RUN_TEST(test_presentation_covers_every_row_exactly_once);
    RUN_TEST(test_counts);
    RUN_TEST(test_enabled_is_switch);
    RUN_TEST(test_water_mode_segmented);
    RUN_TEST(test_fan_mode_four_options);
    RUN_TEST(test_five_option_enum_is_roller);
    RUN_TEST(test_soil_backend_readonly_segmented);
    RUN_TEST(test_hhmm_roller);
    RUN_TEST(test_stepper_bounds);
    RUN_TEST(test_dli_scaled);
    RUN_TEST(test_step_clamps);
    RUN_TEST(test_pin_none);
    RUN_TEST(test_hex);
    RUN_TEST(test_text_rows);
    RUN_TEST(test_unknown_ftype_renders);
    RUN_TEST(test_group_id_resolved_per_table);
    RUN_TEST(test_tighten_dose);
    RUN_TEST(test_locate_shapes);
    RUN_TEST(test_round_trip_every_editable_row);
    RUN_TEST(test_numeric_bounds_agree_with_writer);
    return UNITY_END();
}
