#include <string.h>
#include "unity.h"
#include "cmd_core.h"
#include "hg_cfg.h"
#include "hg_model.h"
#include "zone_cmds.h"
#include "cmd_common.h"
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

/* zone/main/cmd_table_zone.c memcpy's CMD_COMMON_ROWS + ZONE_CMD_ROWS into one
 * production dispatch table; each set passes cmd_table_check on its own
 * (test_table_valid above, and test_cmd_common's own test_table_valid), but
 * neither of those catches a noun collision that only exists across the two
 * sets. Mirror the merge exactly so that gap is host-validated. */
static void test_merged_table_valid(void) {
    cmd_entry_t merged[64];
    int total = CMD_COMMON_ROWS_N + ZONE_CMD_ROWS_N;
    TEST_ASSERT_TRUE(total <= 64);
    memcpy(merged, CMD_COMMON_ROWS, (size_t)CMD_COMMON_ROWS_N * sizeof(cmd_entry_t));
    memcpy(merged + CMD_COMMON_ROWS_N, ZONE_CMD_ROWS, (size_t)ZONE_CMD_ROWS_N * sizeof(cmd_entry_t));
    TEST_ASSERT_EQUAL_INT(CMD_COMMON_ROWS_N + ZONE_CMD_ROWS_N, total);
    TEST_ASSERT_EQUAL_INT(-1, cmd_table_check(merged, total));
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

/* Drives enough non-default fields across all 4 shelves that the replayable
 * GET CONFIG dump exceeds CMD_RESP_MAX (spec 4.6): the reply must fit the
 * lines it can and say how many it dropped, not silently truncate one mid-line. */
static void test_config_dump_truncates_with_more(void) {
    ses.unlock_until_ms = fake_clock_now() + 1000;   /* HW/HWSHELF rows are UNLOCK-gated */

    TEST_ASSERT_EQUAL_INT(0, run("SET ZONECFG NAME BigZoneMegaLong"));
    TEST_ASSERT_EQUAL_INT(0, run("SET ZONECFG LINKLOSS_S 45"));
    TEST_ASSERT_EQUAL_INT(0, run("SET HW PCA_ADDR 65"));
    TEST_ASSERT_EQUAL_INT(0, run("SET HW PCF_ADDR 33"));
    TEST_ASSERT_EQUAL_INT(0, run("SET HW SOIL_BACKEND ADS1115"));
    TEST_ASSERT_EQUAL_INT(0, run("SET HW PCF_ACTLOW 4095"));
    TEST_ASSERT_EQUAL_INT(0, run("SET HW PCA_HZ 1200"));
    for (int a = 1; a <= 2; a++) {
        char cmd[48];
        snprintf(cmd, sizeof cmd, "SET AUX %d MODE PULSE", a);        TEST_ASSERT_EQUAL_INT(0, run(cmd));
        snprintf(cmd, sizeof cmd, "SET AUX %d PULSE_S %d", a, 2 + a); TEST_ASSERT_EQUAL_INT(0, run(cmd));
        snprintf(cmd, sizeof cmd, "SET AUX %d INTERVAL_MIN %d", a, 8 + a); TEST_ASSERT_EQUAL_INT(0, run(cmd));
        snprintf(cmd, sizeof cmd, "SET AUX %d START 0%d:00", a, a);   TEST_ASSERT_EQUAL_INT(0, run(cmd));
        snprintf(cmd, sizeof cmd, "SET AUX %d END 0%d:00", a, a + 2); TEST_ASSERT_EQUAL_INT(0, run(cmd));
    }

    for (int s = 1; s <= 4; s++) {
        char cmd[48];
        snprintf(cmd, sizeof cmd, "SET SHELF %d CROP CropForShelfX%d", s, s); TEST_ASSERT_EQUAL_INT(0, run(cmd));
        snprintf(cmd, sizeof cmd, "SET SHELF %d ENABLED 1", s);           TEST_ASSERT_EQUAL_INT(0, run(cmd));
        snprintf(cmd, sizeof cmd, "SET SHELF %d PROFILE %d", s, s);       TEST_ASSERT_EQUAL_INT(0, run(cmd));

        snprintf(cmd, sizeof cmd, "SET LIGHT %d ON 05:%02d", s, s);       TEST_ASSERT_EQUAL_INT(0, run(cmd));
        snprintf(cmd, sizeof cmd, "SET LIGHT %d OFF 20:%02d", s, s);      TEST_ASSERT_EQUAL_INT(0, run(cmd));
        snprintf(cmd, sizeof cmd, "SET LIGHT %d WHITE %d", s, 60 + s);    TEST_ASSERT_EQUAL_INT(0, run(cmd));
        snprintf(cmd, sizeof cmd, "SET LIGHT %d RED %d", s, 40 + s);      TEST_ASSERT_EQUAL_INT(0, run(cmd));
        snprintf(cmd, sizeof cmd, "SET LIGHT %d RAMP_MIN %d", s, 5 + s);  TEST_ASSERT_EQUAL_INT(0, run(cmd));
        snprintf(cmd, sizeof cmd, "SET LIGHT %d DLI %d", s, 400 + s);     TEST_ASSERT_EQUAL_INT(0, run(cmd));

        snprintf(cmd, sizeof cmd, "SET WATER %d MODE OFF", s);            TEST_ASSERT_EQUAL_INT(0, run(cmd));
        snprintf(cmd, sizeof cmd, "SET WATER %d TARGET %d", s, 50 + s);   TEST_ASSERT_EQUAL_INT(0, run(cmd));
        snprintf(cmd, sizeof cmd, "SET WATER %d HYST %d", s, 8 + s);      TEST_ASSERT_EQUAL_INT(0, run(cmd));
        snprintf(cmd, sizeof cmd, "SET WATER %d SETTLE_MIN %d", s, 12 + s); TEST_ASSERT_EQUAL_INT(0, run(cmd));
        snprintf(cmd, sizeof cmd, "SET WATER %d DOSE_S %d", s, 25 + s);   TEST_ASSERT_EQUAL_INT(0, run(cmd));
        snprintf(cmd, sizeof cmd, "SET WATER %d INTERVAL_MIN %d", s, 130 + s); TEST_ASSERT_EQUAL_INT(0, run(cmd));
        snprintf(cmd, sizeof cmd, "SET WATER %d MAX_DOSES %d", s, 7);     TEST_ASSERT_EQUAL_INT(0, run(cmd));
        snprintf(cmd, sizeof cmd, "SET WATER %d DIFF_MAX %d", s, 18 + s); TEST_ASSERT_EQUAL_INT(0, run(cmd));
        snprintf(cmd, sizeof cmd, "SET WATER %d WIN_START %d", s, 100 + s); TEST_ASSERT_EQUAL_INT(0, run(cmd));
        snprintf(cmd, sizeof cmd, "SET WATER %d WIN_END %d", s, 200 + s); TEST_ASSERT_EQUAL_INT(0, run(cmd));

        snprintf(cmd, sizeof cmd, "SET FAN %d MODE ON", s);               TEST_ASSERT_EQUAL_INT(0, run(cmd));
        snprintf(cmd, sizeof cmd, "SET FAN %d ON_MIN %d", s, 20 + s);     TEST_ASSERT_EQUAL_INT(0, run(cmd));
        snprintf(cmd, sizeof cmd, "SET FAN %d PERIOD_MIN %d", s, 90 + s); TEST_ASSERT_EQUAL_INT(0, run(cmd));

        snprintf(cmd, sizeof cmd, "SET VIB %d MODE PULSE", s);            TEST_ASSERT_EQUAL_INT(0, run(cmd));
        snprintf(cmd, sizeof cmd, "SET VIB %d INTENSITY %d", s, 70 + s);  TEST_ASSERT_EQUAL_INT(0, run(cmd));
        snprintf(cmd, sizeof cmd, "SET VIB %d PULSE_S %d", s, 8 + s);     TEST_ASSERT_EQUAL_INT(0, run(cmd));
        snprintf(cmd, sizeof cmd, "SET VIB %d INTERVAL_MIN %d", s, 140 + s); TEST_ASSERT_EQUAL_INT(0, run(cmd));
        snprintf(cmd, sizeof cmd, "SET VIB %d START %02d:00", s, 5 + s);  TEST_ASSERT_EQUAL_INT(0, run(cmd));
        snprintf(cmd, sizeof cmd, "SET VIB %d END %02d:00", s, 18 + s);   TEST_ASSERT_EQUAL_INT(0, run(cmd));

        snprintf(cmd, sizeof cmd, "SET CAL %d A DRY %d", s, 2810 + s);    TEST_ASSERT_EQUAL_INT(0, run(cmd));
        snprintf(cmd, sizeof cmd, "SET CAL %d B DRY %d", s, 2810 + s);    TEST_ASSERT_EQUAL_INT(0, run(cmd));
        snprintf(cmd, sizeof cmd, "SET CAL %d A WET %d", s, 1310 + s);    TEST_ASSERT_EQUAL_INT(0, run(cmd));
        snprintf(cmd, sizeof cmd, "SET CAL %d B WET %d", s, 1310 + s);    TEST_ASSERT_EQUAL_INT(0, run(cmd));

        snprintf(cmd, sizeof cmd, "SET HWSHELF %d LED_MAX_W %d", s, 80 + s); TEST_ASSERT_EQUAL_INT(0, run(cmd));
        snprintf(cmd, sizeof cmd, "SET HWSHELF %d LED_MAX_R %d", s, 60 + s); TEST_ASSERT_EQUAL_INT(0, run(cmd));
        snprintf(cmd, sizeof cmd, "SET HWSHELF %d PUMP_MAX_RUN_S %d", s, 90 + s); TEST_ASSERT_EQUAL_INT(0, run(cmd));
        snprintf(cmd, sizeof cmd, "SET HWSHELF %d PUMP_MAX_DAILY_S %d", s, 700 + s); TEST_ASSERT_EQUAL_INT(0, run(cmd));
    }

    TEST_ASSERT_EQUAL_INT(0, run("GET CONFIG"));

    size_t len = strlen(resp);
    TEST_ASSERT_TRUE(len < CMD_RESP_MAX);

    int n = -1;
    TEST_ASSERT_EQUAL_INT(1, sscanf(resp, "OK CONFIG %d", &n));
    TEST_ASSERT_TRUE(n > 0);

    /* Walk every line: count the delivered "  SET ..." replay lines, and
     * remember the last line seen so it can be checked against "  MORE : k". */
    char dump[CMD_RESP_MAX];
    strcpy(dump, resp);
    int delivered = 0;
    char last[64] = "";
    char *line = strtok(dump, "\n");
    while (line) {
        if (strncmp(line, "  SET", 5) == 0) delivered++;
        snprintf(last, sizeof last, "%s", line);
        line = strtok(NULL, "\n");
    }

    int more_k = -1;
    TEST_ASSERT_EQUAL_INT(1, sscanf(last, "  MORE : %d", &more_k));
    TEST_ASSERT_TRUE(more_k > 0);
    TEST_ASSERT_EQUAL_INT(n, delivered + more_k);
}

int main(void) { UNITY_BEGIN();
    RUN_TEST(test_table_valid);
    RUN_TEST(test_merged_table_valid);
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
    RUN_TEST(test_config_dump_truncates_with_more);
    return UNITY_END(); }
