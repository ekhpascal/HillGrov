#include <string.h>
#include "unity.h"
#include "cmd_core.h"
#include "notify.h"
#include "cmd_common.h"
#include "fake_app_if.h"
#include "fake_clock.h"

static cmd_core_t    core;
static cmd_session_t ses;
static char          resp[CMD_RESP_MAX];

void setUp(void) {
    memset(&core, 0, sizeof core);
    core.table = CMD_COMMON_ROWS;
    core.table_len = CMD_COMMON_ROWS_N;
    core.role = CMD_ROLE_ZONE;
    core.zone_id = 2;
    core.now_ms = fake_clock_now;
    core.debug_key = "letmein";
    memset(&ses, 0, sizeof ses);
    ses.source = CMD_SRC_CLI;
    fake_clock_set(100000);
    fake_app_if_reset();
    cmd_common_init(&FAKE_APP_IF);
    resp[0] = '\0';
}
void tearDown(void) {}

static int run(const char *line) { return cmd_dispatch(&core, &ses, line, resp, sizeof resp); }

static void test_table_valid(void) {
    TEST_ASSERT_EQUAL_INT(-1, cmd_table_check(CMD_COMMON_ROWS, CMD_COMMON_ROWS_N));
}

static void test_get_id(void) {
    TEST_ASSERT_EQUAL_INT(0, run("GET ID"));
    TEST_ASSERT_EQUAL_STRING("OK ID ZONE 24:6f:28:aa:bb:02 2 -\n", resp);
}

static void test_get_version(void) {
    TEST_ASSERT_EQUAL_INT(0, run("GET VERSION"));
    TEST_ASSERT_EQUAL_STRING("OK VERSION 0.1.0 ota_0 VALID NONE\n", resp);
}

static void test_get_fw(void) {
    TEST_ASSERT_EQUAL_INT(0, run("GET FW"));
    TEST_ASSERT_EQUAL_STRING("OK FW 0.1.0 ota_0 VALID NONE\n", resp);
}

static void test_get_status(void) {
    TEST_ASSERT_EQUAL_INT(0, run("GET STATUS"));
    TEST_ASSERT_EQUAL_INT(0, strncmp(resp, "OK STATUS", strlen("OK STATUS")));
    TEST_ASSERT_NOT_NULL(strstr(resp, "  Uptime : 812s"));
}

static void test_echo_session(void) {
    TEST_ASSERT_EQUAL_INT(0, run("GET ECHO"));
    TEST_ASSERT_EQUAL_STRING("OK ECHO OFF\n", resp);

    TEST_ASSERT_EQUAL_INT(0, run("SET ECHO ON"));
    TEST_ASSERT_EQUAL_STRING("OK ECHO ON\n", resp);
    TEST_ASSERT_EQUAL_UINT8(1, ses.echo);

    TEST_ASSERT_EQUAL_INT(0, run("GET ECHO"));
    TEST_ASSERT_EQUAL_STRING("OK ECHO ON\n", resp);

    ses.source = CMD_SRC_HTTP;
    TEST_ASSERT_EQUAL_INT(-1, run("SET ECHO OFF"));
    TEST_ASSERT_EQUAL_STRING("ERR NOT_LOCAL\n", resp);
}

static void test_notify_session(void) {
    TEST_ASSERT_EQUAL_INT(0, run("SET NOTIFY ALARM ON"));
    TEST_ASSERT_EQUAL_UINT16(NTF_MASK(NTF_ALARM), ses.notify_mask);
    TEST_ASSERT_EQUAL_INT(0, run("GET NOTIFY"));
    TEST_ASSERT_NOT_NULL(strstr(resp, "ALARM=1"));

    TEST_ASSERT_EQUAL_INT(0, run("SET NOTIFY ALL OFF"));
    TEST_ASSERT_EQUAL_UINT16(0, ses.notify_mask);
    TEST_ASSERT_EQUAL_INT(0, run("GET NOTIFY"));
    TEST_ASSERT_NOT_NULL(strstr(resp, "BOOT=0"));

    ses.source = CMD_SRC_HTTP;
    TEST_ASSERT_EQUAL_INT(-1, run("SET NOTIFY ALL ON"));
    TEST_ASSERT_EQUAL_STRING("ERR NOT_LOCAL\n", resp);
}

static void test_log_session(void) {
    TEST_ASSERT_EQUAL_INT(0, run("SET LOG DEBUG"));
    TEST_ASSERT_EQUAL_STRING("OK LOG DEBUG\n", resp);

    TEST_ASSERT_EQUAL_INT(0, run("GET LOG"));
    TEST_ASSERT_EQUAL_STRING("OK LOG DEBUG\n", resp);

    TEST_ASSERT_EQUAL_INT(0, run("SET LOG INFO mytag"));
    TEST_ASSERT_EQUAL_STRING("OK LOG INFO\n", resp);
    TEST_ASSERT_EQUAL_STRING("mytag", g_fake_app.log_tag);

    ses.source = CMD_SRC_HTTP;
    TEST_ASSERT_EQUAL_INT(-1, run("GET LOG"));
    TEST_ASSERT_EQUAL_STRING("ERR NOT_LOCAL\n", resp);
}

static void test_debug_enable_unlocks_and_expires(void) {
    TEST_ASSERT_EQUAL_INT(-1, run("SET FW ROLLBACK CONFIRM"));
    TEST_ASSERT_EQUAL_STRING("ERR LOCKED\n", resp);

    TEST_ASSERT_EQUAL_INT(-1, run("DEBUG ENABLE wrongkey"));
    TEST_ASSERT_EQUAL_STRING("ERR AUTH_FAILED\n", resp);

    TEST_ASSERT_EQUAL_INT(0, run("DEBUG ENABLE letmein"));
    TEST_ASSERT_EQUAL_STRING("OK DEBUG ENABLE 600\n", resp);
    TEST_ASSERT_EQUAL_UINT32(fake_clock_now() + CMD_UNLOCK_MS, ses.unlock_until_ms);

    TEST_ASSERT_EQUAL_INT(0, run("GET DEBUG"));
    TEST_ASSERT_EQUAL_STRING("OK DEBUG 600\n", resp);

    TEST_ASSERT_EQUAL_INT(0, run("SET FW ROLLBACK CONFIRM"));
    TEST_ASSERT_EQUAL_STRING("OK FW ROLLBACK\n", resp);
    TEST_ASSERT_EQUAL_INT(1, g_fake_app.fw_rollback_calls);

    g_fake_app.fail_fw_rollback = 1;
    TEST_ASSERT_EQUAL_INT(-1, run("SET FW ROLLBACK CONFIRM"));
    TEST_ASSERT_EQUAL_STRING("ERR INTERNAL\n", resp);
    g_fake_app.fail_fw_rollback = 0;

    fake_clock_add(600001);
    TEST_ASSERT_EQUAL_INT(0, run("GET DEBUG"));
    TEST_ASSERT_EQUAL_STRING("OK DEBUG 0\n", resp);

    TEST_ASSERT_EQUAL_INT(-1, run("SET FW ROLLBACK CONFIRM"));
    TEST_ASSERT_EQUAL_STRING("ERR LOCKED\n", resp);
}

static void test_debug_disable(void) {
    TEST_ASSERT_EQUAL_INT(0, run("DEBUG ENABLE letmein"));
    TEST_ASSERT_EQUAL_INT(0, run("DEBUG DISABLE"));
    TEST_ASSERT_EQUAL_STRING("OK DEBUG DISABLE\n", resp);
    TEST_ASSERT_EQUAL_INT(-1, run("SET FW ROLLBACK CONFIRM"));
    TEST_ASSERT_EQUAL_STRING("ERR LOCKED\n", resp);
}

static void test_time(void) {
    TEST_ASSERT_EQUAL_INT(0, run("GET TIME"));
    TEST_ASSERT_EQUAL_STRING("OK TIME 2026-08-31 12:00:00 RTC 5\n", resp);

    TEST_ASSERT_EQUAL_INT(0, run("SET TIME 2026-08-31 14:03:22"));
    TEST_ASSERT_EQUAL_STRING("OK TIME 2026-08-31 14:03:22\n", resp);
    TEST_ASSERT_EQUAL_INT(2026, g_fake_app.ts_y);
    TEST_ASSERT_EQUAL_INT(8, g_fake_app.ts_mo);
    TEST_ASSERT_EQUAL_INT(31, g_fake_app.ts_d);
    TEST_ASSERT_EQUAL_INT(14, g_fake_app.ts_h);
    TEST_ASSERT_EQUAL_INT(3, g_fake_app.ts_mi);
    TEST_ASSERT_EQUAL_INT(22, g_fake_app.ts_s);

    TEST_ASSERT_EQUAL_INT(-1, run("SET TIME 20260831 14:03:22"));
    TEST_ASSERT_EQUAL_STRING("ERR BAD_ARGS\n", resp);

    /* trailing garbage after a full match: sscanf's conversion count alone
     * would accept these (it stops silently at the bad character) */
    TEST_ASSERT_EQUAL_INT(-1, run("SET TIME 2026-08-1x 14:03:22"));
    TEST_ASSERT_EQUAL_STRING("ERR BAD_ARGS\n", resp);

    TEST_ASSERT_EQUAL_INT(-1, run("SET TIME 2026-08-31 14:03:2x"));
    TEST_ASSERT_EQUAL_STRING("ERR BAD_ARGS\n", resp);
}

static void test_save(void) {
    TEST_ASSERT_EQUAL_INT(0, run("SAVE"));
    TEST_ASSERT_EQUAL_STRING("OK SAVE\n", resp);

    g_fake_app.fail_save = 1;
    TEST_ASSERT_EQUAL_INT(-1, run("SAVE"));
    TEST_ASSERT_EQUAL_STRING("ERR NVS_WRITE\n", resp);
}

static void test_reboot(void) {
    TEST_ASSERT_EQUAL_INT(-1, run("REBOOT"));
    TEST_ASSERT_EQUAL_STRING("ERR BAD_ARGS\n", resp);

    TEST_ASSERT_EQUAL_INT(0, run("REBOOT CONFIRM"));
    TEST_ASSERT_EQUAL_STRING("OK REBOOT\n", resp);
    TEST_ASSERT_EQUAL_INT(1, g_fake_app.reboot_calls);
}

static void test_factory_reset(void) {
    TEST_ASSERT_EQUAL_INT(0, run("FACTORY RESET CONFIRM"));
    TEST_ASSERT_EQUAL_STRING("OK FACTORY RESET\n", resp);
    TEST_ASSERT_EQUAL_INT(1, g_fake_app.factory_reset_calls);

    g_fake_app.fail_factory_reset = 1;
    TEST_ASSERT_EQUAL_INT(-1, run("FACTORY RESET CONFIRM"));
    TEST_ASSERT_EQUAL_STRING("ERR INTERNAL\n", resp);
}

static void test_fw_update(void) {
    TEST_ASSERT_EQUAL_INT(-1, run("SET FW UPDATE a b c"));
    TEST_ASSERT_EQUAL_STRING("ERR LOCKED\n", resp);

    ses.unlock_until_ms = fake_clock_now() + 1000;
    ses.source = CMD_SRC_HTTP;
    TEST_ASSERT_EQUAL_INT(-1, run("SET FW UPDATE a b c"));
    TEST_ASSERT_EQUAL_STRING("ERR NOT_LOCAL\n", resp);

    ses.source = CMD_SRC_CLI;
    TEST_ASSERT_EQUAL_INT(0, run("SET FW UPDATE ssid1 pass1 http://x"));
    TEST_ASSERT_EQUAL_STRING("OK FW UPDATE\n", resp);
    TEST_ASSERT_EQUAL_STRING("ssid1", g_fake_app.fu_ssid);
    TEST_ASSERT_EQUAL_STRING("pass1", g_fake_app.fu_pass);
    TEST_ASSERT_EQUAL_STRING("http://x", g_fake_app.fu_url);

    g_fake_app.fail_fw_update = 1;
    TEST_ASSERT_EQUAL_INT(-1, run("SET FW UPDATE ssid1 pass1 http://x"));
    TEST_ASSERT_EQUAL_STRING("ERR INTERNAL\n", resp);
}

/* url's ARG_STR max mirrors hg_handover_t.url[64]'s capacity (63 chars +
 * NUL); a 64-char url must be rejected at dispatch, not silently truncated
 * into an unfetchable rescue-handover record. */
static void test_fw_update_url_too_long(void) {
    ses.unlock_until_ms = fake_clock_now() + 1000;
    char url[65];
    memset(url, 'x', 64);
    url[64] = '\0';
    char line[256];
    snprintf(line, sizeof line, "SET FW UPDATE ssid1 pass1 %s", url);
    TEST_ASSERT_EQUAL_INT(-1, run(line));
    TEST_ASSERT_EQUAL_STRING("ERR BAD_ARGS\n", resp);
}

int main(void) { UNITY_BEGIN();
    RUN_TEST(test_table_valid);
    RUN_TEST(test_get_id);
    RUN_TEST(test_get_version);
    RUN_TEST(test_get_fw);
    RUN_TEST(test_get_status);
    RUN_TEST(test_echo_session);
    RUN_TEST(test_notify_session);
    RUN_TEST(test_log_session);
    RUN_TEST(test_debug_enable_unlocks_and_expires);
    RUN_TEST(test_debug_disable);
    RUN_TEST(test_time);
    RUN_TEST(test_save);
    RUN_TEST(test_reboot);
    RUN_TEST(test_factory_reset);
    RUN_TEST(test_fw_update);
    RUN_TEST(test_fw_update_url_too_long);
    return UNITY_END(); }
