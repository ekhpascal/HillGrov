#include <string.h>
#include "unity.h"
#include "cmd_core.h"
#include "hg_cfg.h"
#include "hg_model.h"
#include "zone_cmds.h"
#include "fake_clock.h"

static cmd_core_t    core;
static cmd_session_t ses;
static char          resp[CMD_RESP_MAX];

void setUp(void) {
    memset(&core, 0, sizeof core);
    core.table = ZONE_CMD_ROWS;
    core.table_len = ZONE_CMD_ROWS_N;
    core.role = CMD_ROLE_ZONE;
    core.zone_id = 1;
    core.now_ms = fake_clock_now;
    memset(&ses, 0, sizeof ses);
    ses.source = CMD_SRC_CLI;
    fake_clock_set(100000);
    hg_model_init();
    resp[0] = '\0';
}
void tearDown(void) {}

static int run(const char *line) { return cmd_dispatch(&core, &ses, line, resp, sizeof resp); }

static void test_table_valid(void) {
    TEST_ASSERT_EQUAL_INT(-1, cmd_table_check(ZONE_CMD_ROWS, ZONE_CMD_ROWS_N));
}

static void test_water_set_get(void) {
    TEST_ASSERT_EQUAL_INT(0, run("SET WATER 1 TARGET 60"));
    TEST_ASSERT_EQUAL_STRING("OK WATER 1 TARGET 60\n", resp);
    hg_zone_cfg_t cfg;
    hg_model_snapshot_cfg(&cfg, NULL);
    TEST_ASSERT_EQUAL_UINT8(60, cfg.shelf[0].water.target_pct);

    TEST_ASSERT_EQUAL_INT(0, run("GET WATER 1"));
    TEST_ASSERT_NOT_NULL(strstr(resp, "  Target : 60"));
}

static void test_water_errors(void) {
    TEST_ASSERT_EQUAL_INT(-1, run("SET WATER 1 TARGET 101"));
    TEST_ASSERT_EQUAL_STRING("ERR OUT_OF_RANGE\n", resp);

    TEST_ASSERT_EQUAL_INT(-1, run("SET WATER 1 BOGUS 1"));
    TEST_ASSERT_EQUAL_STRING("ERR INVALID_FIELD\n", resp);

    TEST_ASSERT_EQUAL_INT(-1, run("SET WATER 5 TARGET 60"));
    TEST_ASSERT_EQUAL_STRING("ERR OUT_OF_RANGE\n", resp);
}

static void test_light_hhmm(void) {
    TEST_ASSERT_EQUAL_INT(0, run("SET LIGHT 1 ON 06:30"));
    TEST_ASSERT_EQUAL_STRING("OK LIGHT 1 ON 06:30\n", resp);
}

static void test_shelf_crop_case(void) {
    TEST_ASSERT_EQUAL_INT(0, run("SET SHELF 1 CROP Basil"));
    TEST_ASSERT_EQUAL_STRING("OK SHELF 1 CROP Basil\n", resp);
    hg_zone_cfg_t cfg;
    hg_model_snapshot_cfg(&cfg, NULL);
    TEST_ASSERT_EQUAL_STRING("Basil", cfg.shelf[0].crop);
}

static void test_hw_shelves_locked_then_unlock(void) {
    TEST_ASSERT_EQUAL_INT(-1, run("SET HW SHELVES 3"));
    TEST_ASSERT_EQUAL_STRING("ERR LOCKED\n", resp);

    ses.unlock_until_ms = fake_clock_now() + 1000;
    TEST_ASSERT_EQUAL_INT(0, run("SET HW SHELVES 3"));
    TEST_ASSERT_EQUAL_STRING("OK HW SHELVES 3\n", resp);
    TEST_ASSERT_EQUAL_INT(1, hg_model_restart_pending());
}

static void test_shelves_alias(void) {
    /* SET SHELVES <n> is the alias row; must gate and echo like SET HW SHELVES */
    TEST_ASSERT_EQUAL_INT(-1, run("SET SHELVES 3"));
    TEST_ASSERT_EQUAL_STRING("ERR LOCKED\n", resp);

    ses.unlock_until_ms = fake_clock_now() + 1000;
    TEST_ASSERT_EQUAL_INT(0, run("SET SHELVES 3"));
    TEST_ASSERT_EQUAL_STRING("OK HW SHELVES 3\n", resp);
}

static void test_cal_set(void) {
    TEST_ASSERT_EQUAL_INT(0, run("SET CAL 1 A DRY 2850"));
    TEST_ASSERT_EQUAL_STRING("OK CAL 1 A DRY 2850\n", resp);
    hg_zone_hw_t hw;
    hg_model_snapshot_hw(&hw);
    TEST_ASSERT_EQUAL_UINT16(2850, hw.shelf[0].soil_dry_mv[0]);
    TEST_ASSERT_EQUAL_INT(0, hg_model_restart_pending());
}

static void test_zonecfg_name(void) {
    TEST_ASSERT_EQUAL_INT(0, run("SET ZONECFG NAME Herbs"));
    TEST_ASSERT_EQUAL_STRING("OK ZONECFG NAME Herbs\n", resp);
}

static void test_config_dump_replay(void) {
    TEST_ASSERT_EQUAL_INT(0, run("SET ZONECFG NAME Herbs"));
    TEST_ASSERT_EQUAL_INT(0, run("SET WATER 1 TARGET 60"));
    TEST_ASSERT_EQUAL_INT(0, run("SET LIGHT 1 ON 06:30"));

    TEST_ASSERT_EQUAL_INT(0, run("GET CONFIG"));
    TEST_ASSERT_EQUAL_INT(0, strncmp(resp, "OK CONFIG 3", strlen("OK CONFIG 3")));

    char dump[CMD_RESP_MAX];
    strcpy(dump, resp);

    hg_model_init();   /* fresh model to replay into */
    int replayed = 0;
    char *line = strtok(dump, "\n");
    while (line) {
        if (line[0] == ' ') {
            const char *p = line;
            while (*p == ' ') p++;
            TEST_ASSERT_EQUAL_INT(0, run(p));
            replayed++;
        }
        line = strtok(NULL, "\n");
    }
    TEST_ASSERT_EQUAL_INT(3, replayed);

    hg_zone_cfg_t cfg;
    hg_model_snapshot_cfg(&cfg, NULL);
    TEST_ASSERT_EQUAL_STRING("Herbs", cfg.name);
    TEST_ASSERT_EQUAL_UINT8(60, cfg.shelf[0].water.target_pct);
    TEST_ASSERT_EQUAL_UINT16(390, cfg.shelf[0].light.on_min);
}

static void test_dose_s_cross_field_invalid(void) {
    TEST_ASSERT_EQUAL_INT(-1, run("SET WATER 1 DOSE_S 100"));
    TEST_ASSERT_EQUAL_STRING("ERR INVALID_FIELD\n  Field : shelf[0].water.dose_s\n", resp);
}

int main(void) { UNITY_BEGIN();
    RUN_TEST(test_table_valid);
    RUN_TEST(test_water_set_get);
    RUN_TEST(test_water_errors);
    RUN_TEST(test_light_hhmm);
    RUN_TEST(test_shelf_crop_case);
    RUN_TEST(test_hw_shelves_locked_then_unlock);
    RUN_TEST(test_shelves_alias);
    RUN_TEST(test_cal_set);
    RUN_TEST(test_zonecfg_name);
    RUN_TEST(test_config_dump_replay);
    RUN_TEST(test_dose_s_cross_field_invalid);
    return UNITY_END(); }
