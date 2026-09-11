#include <string.h>
#include "unity.h"
#include "cmd_core.h"
#include "ring_proto.h"
#include "master_cmds.h"
#include "cmd_common.h"
#include "ota_trial.h"
#include "fake_node_ops.h"
#include "fake_net_ops.h"
#include "fake_clock.h"

/* trial_cmds.c (linked in below for OTA_TRIAL_ROWS/N in test_merged_table_valid)
 * calls this from its handler; that test only runs cmd_table_check, never
 * cmd_dispatch, on the OTA row, so this exists solely to satisfy the linker
 * (mirrors test_zone_cmds.c's own stub). */
int ota_trial_confirm(void) { return -1; }

static cmd_core_t    core;
static cmd_session_t ses;
static char          resp[CMD_RESP_MAX];

static const uint8_t MAC1[6] = { 0x24, 0x6F, 0x28, 0xAA, 0xBB, 0x01 };
static const uint8_t MAC2[6] = { 0x24, 0x6F, 0x28, 0xAA, 0xBB, 0x02 };

static void set_node(int slot, uint8_t id, const char *name, const uint8_t mac[6],
                      node_health_t health, uint8_t maj, uint8_t min_, uint8_t patch, uint32_t gen) {
    hg_node_t *nd = &g_fake_nodes.nodes[slot];
    memset(nd, 0, sizeof *nd);
    nd->used = 1; nd->id = id;
    snprintf(nd->name, sizeof nd->name, "%s", name);
    memcpy(nd->mac, mac, 6);
    nd->health = health;
    nd->hops = id;
    nd->link_flags = 0x07;
    nd->last_hb_ms = 100000;
    nd->hb.fw_maj = maj; nd->hb.fw_min = min_; nd->hb.fw_patch = patch;
    nd->hb.cfg_gen = gen;
    nd->hb.rx_crc_err = 1; nd->hb.rx_uart_err = 2; nd->hb.rx_drop = 3; nd->hb.fwd_count = 4;
    nd->hb.min_free_heap_kb = 180;
    nd->seq_drop_tally = 7;
}

static void two_node_table(void) {
    set_node(0, 1, "Basil", MAC1, NODE_H_ONLINE,   0, 1, 0, 5);
    set_node(1, 2, "Mint",  MAC2, NODE_H_DEGRADED, 0, 1, 1, 9);
}

void setUp(void) {
    memset(&core, 0, sizeof core);
    core.table = MASTER_CMD_ROWS; core.table_len = MASTER_CMD_ROWS_N;
    core.role = CMD_ROLE_MASTER; core.zone_id = 0;
    core.now_ms = fake_clock_now;
    memset(&ses, 0, sizeof ses);
    ses.source = CMD_SRC_CLI;
    fake_clock_set(200000);
    fake_node_ops_reset();
    fake_net_ops_reset();
    master_cmds_init2(&FAKE_NODE_OPS, &FAKE_NET_OPS);
    resp[0] = '\0';
}
void tearDown(void) {}

static int run(const char *line) { return cmd_dispatch(&core, &ses, line, resp, sizeof resp); }

static void test_table_valid(void) {
    TEST_ASSERT_EQUAL_INT(-1, cmd_table_check(MASTER_CMD_ROWS, MASTER_CMD_ROWS_N));
}

/* master/main/cmd_table_master.c memcpy's CMD_COMMON_ROWS + MASTER_CMD_ROWS +
 * OTA_TRIAL_ROWS into one production dispatch table (mirrors master_table());
 * mirror that merge exactly so a cross-set noun collision is host-validated,
 * the same pattern test_zone_cmds.c uses for the zone merge. */
static void test_merged_table_valid(void) {
    cmd_entry_t merged[64];
    int total = CMD_COMMON_ROWS_N + MASTER_CMD_ROWS_N + OTA_TRIAL_ROWS_N;
    TEST_ASSERT_TRUE(total <= 64);
    memcpy(merged, CMD_COMMON_ROWS, (size_t)CMD_COMMON_ROWS_N * sizeof(cmd_entry_t));
    memcpy(merged + CMD_COMMON_ROWS_N, MASTER_CMD_ROWS, (size_t)MASTER_CMD_ROWS_N * sizeof(cmd_entry_t));
    memcpy(merged + CMD_COMMON_ROWS_N + MASTER_CMD_ROWS_N, OTA_TRIAL_ROWS,
           (size_t)OTA_TRIAL_ROWS_N * sizeof(cmd_entry_t));
    TEST_ASSERT_EQUAL_INT(CMD_COMMON_ROWS_N + MASTER_CMD_ROWS_N + OTA_TRIAL_ROWS_N, total);
    TEST_ASSERT_EQUAL_INT(-1, cmd_table_check(merged, total));
}

static void test_get_nodes(void) {
    two_node_table();
    TEST_ASSERT_EQUAL_INT(0, run("GET NODES"));
    TEST_ASSERT_EQUAL_STRING(
        "OK NODES 2\n"
        "  Z1 : Basil 24:6f:28:aa:bb:01 ONLINE fw 0.1.0 gen 5\n"
        "  Z2 : Mint 24:6f:28:aa:bb:02 DEGRADED fw 0.1.1 gen 9\n",
        resp);
}

static void test_get_ring_idle(void) {
    g_fake_nodes.ring_status.state = RING_ST_IDLE;
    g_fake_nodes.ring_status.size = 0;
    g_fake_nodes.ring_status.online_mask = 0;
    g_fake_nodes.time_valid = 0;
    TEST_ASSERT_EQUAL_INT(0, run("GET RING"));
    TEST_ASSERT_EQUAL_STRING(
        "OK RING IDLE SIZE 0 ONLINE 0x0000 TIME NONE\n"
        "  RxCrcErr : 0\n"
        "  RxUartErr : 0\n"
        "  RxDrop : 0\n"
        "  Fwd : 0\n",
        resp);
}

static void test_get_ring_open_blame_and_counters(void) {
    two_node_table();
    g_fake_nodes.ring_status.state = RING_ST_OPEN;
    g_fake_nodes.ring_status.size = 2;
    g_fake_nodes.ring_status.online_mask = 0x0002;
    snprintf(g_fake_nodes.ring_status.blame, sizeof g_fake_nodes.ring_status.blame, "Z2 dead or wire Z1->Z2");
    g_fake_nodes.time_valid = 1;
    TEST_ASSERT_EQUAL_INT(0, run("GET RING"));
    TEST_ASSERT_EQUAL_STRING(
        "OK RING OPEN SIZE 2 ONLINE 0x0002 TIME VALID\n"
        "  Blame : Z2 dead or wire Z1->Z2\n"
        "  RxCrcErr : 2\n"
        "  RxUartErr : 4\n"
        "  RxDrop : 6\n"
        "  Fwd : 8\n",
        resp);
}

static void test_get_node_detail(void) {
    two_node_table();
    TEST_ASSERT_EQUAL_INT(0, run("GET NODE 1"));
    TEST_ASSERT_NOT_NULL(strstr(resp, "OK NODE 1 Basil\n"));
    TEST_ASSERT_NOT_NULL(strstr(resp, "  MAC : 24:6f:28:aa:bb:01\n"));
    TEST_ASSERT_NOT_NULL(strstr(resp, "  Health : ONLINE\n"));
    TEST_ASSERT_NOT_NULL(strstr(resp, "  SeqDrops : 7\n"));
    TEST_ASSERT_NOT_NULL(strstr(resp, "  CfgSync : OK\n"));

    g_fake_nodes.cfg_sync_failed[1] = 1;
    TEST_ASSERT_EQUAL_INT(0, run("GET NODE 1"));
    TEST_ASSERT_NOT_NULL(strstr(resp, "  CfgSync : FAILED\n"));
}

static void test_get_node_unknown(void) {
    two_node_table();
    TEST_ASSERT_EQUAL_INT(-1, run("GET NODE 5"));
    TEST_ASSERT_EQUAL_STRING("ERR ZONE_UNKNOWN\n", resp);
}

static void test_get_unassigned(void) {
    memcpy(g_fake_nodes.unassigned_macs[0], MAC1, 6);
    g_fake_nodes.unassigned_n = 1;
    TEST_ASSERT_EQUAL_INT(0, run("GET UNASSIGNED"));
    TEST_ASSERT_EQUAL_STRING("OK UNASSIGNED 1\n  24:6f:28:aa:bb:01\n", resp);
}

static void test_set_node_name(void) {
    TEST_ASSERT_EQUAL_INT(0, run("SET NODE 2 NAME Basil"));
    TEST_ASSERT_EQUAL_STRING("OK NODE 2 NAME Basil\n", resp);
    TEST_ASSERT_EQUAL_INT(1, g_fake_nodes.set_name_calls);
    TEST_ASSERT_EQUAL_UINT8(2, g_fake_nodes.set_name_zone);
    TEST_ASSERT_EQUAL_STRING("Basil", g_fake_nodes.set_name_name);
}

static void test_set_node_name_too_long(void) {
    TEST_ASSERT_EQUAL_INT(-1, run("SET NODE 2 NAME toolongname16chars"));
    TEST_ASSERT_EQUAL_STRING("ERR BAD_ARGS\n", resp);
}

static void test_set_node_name_unknown_zone(void) {
    g_fake_nodes.fail_set_name = 1;
    TEST_ASSERT_EQUAL_INT(-1, run("SET NODE 2 NAME Basil"));
    TEST_ASSERT_EQUAL_STRING("ERR ZONE_UNKNOWN\n", resp);
}

static void test_clear_node(void) {
    TEST_ASSERT_EQUAL_INT(0, run("CLEAR NODE 2 CONFIRM"));
    TEST_ASSERT_EQUAL_STRING("OK NODE 2 CLEARED\n", resp);
    TEST_ASSERT_EQUAL_INT(1, g_fake_nodes.clear_calls);
    TEST_ASSERT_EQUAL_UINT8(2, g_fake_nodes.clear_zone);
}

static void test_clear_node_without_confirm(void) {
    TEST_ASSERT_EQUAL_INT(-1, run("CLEAR NODE 2"));
    TEST_ASSERT_EQUAL_STRING("ERR BAD_ARGS\n", resp);
}

static void test_ring_trace(void) {
    TEST_ASSERT_EQUAL_INT(0, run("SET RING TRACE ON"));
    TEST_ASSERT_EQUAL_STRING("OK RING TRACE ON\n", resp);
    TEST_ASSERT_EQUAL_INT(1, g_fake_nodes.trace_calls);
    TEST_ASSERT_EQUAL_INT(1, g_fake_nodes.trace_on);

    TEST_ASSERT_EQUAL_INT(0, run("SET RING TRACE OFF"));
    TEST_ASSERT_EQUAL_STRING("OK RING TRACE OFF\n", resp);
    TEST_ASSERT_EQUAL_INT(0, g_fake_nodes.trace_on);
}

static void test_fw_zone_rows(void) {
    TEST_ASSERT_EQUAL_INT(0, run("SET FW ZONE 2"));
    TEST_ASSERT_EQUAL_STRING("OK QUEUED\n", resp);
    TEST_ASSERT_EQUAL_UINT8(2, g_fake_nodes.fw_zone_arg);

    /* fix round ruling: real fw_zone/fw_all rc -1 (invalid zone / no
     * assigned zones) maps to ERR ZONE_UNKNOWN -- exercised here via the
     * fake's canned rc fields. */
    g_fake_nodes.fw_all_rc = -1;
    TEST_ASSERT_EQUAL_INT(-1, run("SET FW ZONES CONFIRM"));
    TEST_ASSERT_EQUAL_STRING("ERR ZONE_UNKNOWN\n", resp);

    /* -2 (a sequence is already running) maps to ERR FW_BUSY -- the
     * busy-path test the fix round asked for. */
    g_fake_nodes.fw_zone_rc = -2;
    TEST_ASSERT_EQUAL_INT(-1, run("SET FW ZONE 3"));
    TEST_ASSERT_EQUAL_STRING("ERR FW_BUSY\n", resp);
    g_fake_nodes.fw_all_rc = -2;
    TEST_ASSERT_EQUAL_INT(-1, run("SET FW ZONES CONFIRM"));
    TEST_ASSERT_EQUAL_STRING("ERR FW_BUSY\n", resp);

    /* fw_abort's only failure (nothing running) maps to ERR NOT_READY. */
    g_fake_nodes.fw_abort_rc = -1;
    TEST_ASSERT_EQUAL_INT(-1, run("SET FW ABORT"));
    TEST_ASSERT_EQUAL_STRING("ERR NOT_READY\n", resp);

    g_fake_nodes.fw_abort_rc = 0;
    TEST_ASSERT_EQUAL_INT(0, run("SET FW ABORT"));
    TEST_ASSERT_EQUAL_STRING("OK FW ABORT\n", resp);
    TEST_ASSERT_EQUAL_INT(2, g_fake_nodes.fw_abort_calls);

    snprintf(g_fake_nodes.fw_status_text, sizeof g_fake_nodes.fw_status_text, "IDLE");
    TEST_ASSERT_EQUAL_INT(0, run("GET FW ZONE"));
    TEST_ASSERT_EQUAL_STRING("OK FW ZONE IDLE\n", resp);
}

/* ---------------- NET/TIME rows (Task 8) ---------------- */

static void wifi_fixture_up(void) {
    g_fake_net.status.sta_up = 1;
    snprintf(g_fake_net.status.sta_ip, sizeof g_fake_net.status.sta_ip, "192.168.1.42");
    snprintf(g_fake_net.status.sta_ssid, sizeof g_fake_net.status.sta_ssid, "Home");
    g_fake_net.status.rssi = -57;
    g_fake_net.status.ap_clients = 2;
    snprintf(g_fake_net.status.ap_ip, sizeof g_fake_net.status.ap_ip, "192.168.7.7");
    snprintf(g_fake_net.status.ap_ssid, sizeof g_fake_net.status.ap_ssid, "HillGrow");
}

static void test_get_wifi_down(void) {
    snprintf(g_fake_net.status.ap_ssid, sizeof g_fake_net.status.ap_ssid, "HillGrow");
    snprintf(g_fake_net.status.ap_ip, sizeof g_fake_net.status.ap_ip, "192.168.7.7");
    TEST_ASSERT_EQUAL_INT(0, run("GET WIFI"));
    TEST_ASSERT_EQUAL_STRING(
        "OK WIFI STA DOWN - AP HillGrow 0\n"
        "  StaSsid : -\n"
        "  StaReason : -\n"
        "  Rssi : -\n",
        resp);
    TEST_ASSERT_EQUAL_INT(1, g_fake_net.wifi_status_calls);
}

static void test_get_wifi_up(void) {
    wifi_fixture_up();
    TEST_ASSERT_EQUAL_INT(0, run("GET WIFI"));
    TEST_ASSERT_EQUAL_STRING(
        "OK WIFI STA UP 192.168.1.42 AP HillGrow 2\n"
        "  StaSsid : Home\n"
        "  StaReason : -\n"
        "  Rssi : -57\n",
        resp);
}

static void test_get_wifi_down_with_reason(void) {
    snprintf(g_fake_net.status.ap_ssid, sizeof g_fake_net.status.ap_ssid, "HillGrow");
    snprintf(g_fake_net.status.sta_ssid, sizeof g_fake_net.status.sta_ssid, "Home");
    snprintf(g_fake_net.status.sta_reason, sizeof g_fake_net.status.sta_reason, "AUTH_FAIL");
    TEST_ASSERT_EQUAL_INT(0, run("GET WIFI"));
    TEST_ASSERT_EQUAL_STRING(
        "OK WIFI STA DOWN - AP HillGrow 0\n"
        "  StaSsid : Home\n"
        "  StaReason : AUTH_FAIL\n"
        "  Rssi : -\n",
        resp);
}

static void test_set_wifi_sta(void) {
    TEST_ASSERT_EQUAL_INT(0, run("SET WIFI STA Home pass1234"));
    TEST_ASSERT_EQUAL_STRING("OK WIFI STA Home\n", resp);
    TEST_ASSERT_EQUAL_INT(1, g_fake_net.set_sta_calls);
    TEST_ASSERT_EQUAL_STRING("Home", g_fake_net.set_sta_ssid);
    TEST_ASSERT_EQUAL_STRING("pass1234", g_fake_net.set_sta_pass);
}

/* "-" is the documented stand-in for an open network: the CLI tokenizer has
 * no way to express an empty token, so the row maps it to "". */
static void test_set_wifi_sta_open(void) {
    TEST_ASSERT_EQUAL_INT(0, run("SET WIFI STA Home -"));
    TEST_ASSERT_EQUAL_STRING("OK WIFI STA Home\n", resp);
    TEST_ASSERT_EQUAL_INT(1, g_fake_net.set_sta_calls);
    TEST_ASSERT_EQUAL_STRING("Home", g_fake_net.set_sta_ssid);
    TEST_ASSERT_EQUAL_STRING("", g_fake_net.set_sta_pass);
}

/* "SET WIFI STA - -" is the only way to unconfigure the STA from the CLI, so
 * the dash mapping has to apply to the SSID slot as well as the password. */
static void test_set_wifi_sta_cleared(void) {
    TEST_ASSERT_EQUAL_INT(0, run("SET WIFI STA - -"));
    TEST_ASSERT_EQUAL_STRING("OK WIFI STA -\n", resp);
    TEST_ASSERT_EQUAL_INT(1, g_fake_net.set_sta_calls);
    TEST_ASSERT_EQUAL_STRING("", g_fake_net.set_sta_ssid);
    TEST_ASSERT_EQUAL_STRING("", g_fake_net.set_sta_pass);
}

static void test_set_wifi_sta_invalid(void) {
    g_fake_net.set_sta_rc = -1;
    TEST_ASSERT_EQUAL_INT(-1, run("SET WIFI STA Home short"));
    TEST_ASSERT_EQUAL_STRING("ERR INVALID\n", resp);
}

static void test_set_wifi_ap(void) {
    TEST_ASSERT_EQUAL_INT(0, run("SET WIFI AP GrowAP hillgrow1"));
    TEST_ASSERT_EQUAL_STRING("OK WIFI AP GrowAP\n", resp);
    TEST_ASSERT_EQUAL_INT(1, g_fake_net.set_ap_calls);
    TEST_ASSERT_EQUAL_STRING("GrowAP", g_fake_net.set_ap_ssid);
    TEST_ASSERT_EQUAL_STRING("hillgrow1", g_fake_net.set_ap_pass);

    /* An open AP is not a thing here (hg_mcfg_validate wants ap_pass 8..63):
     * the "-" shorthand still passes "" through and the commit rejects it,
     * which the row reports as ERR INVALID. */
    g_fake_net.set_ap_rc = -1;
    TEST_ASSERT_EQUAL_INT(-1, run("SET WIFI AP GrowAP -"));
    TEST_ASSERT_EQUAL_STRING("ERR INVALID\n", resp);
    TEST_ASSERT_EQUAL_STRING("", g_fake_net.set_ap_pass);
}

static void test_set_web_password(void) {
    TEST_ASSERT_EQUAL_INT(0, run("SET WEB PASSWORD sekret12"));
    TEST_ASSERT_EQUAL_STRING("OK WEB PASSWORD\n", resp);
    TEST_ASSERT_EQUAL_INT(1, g_fake_net.set_pw_calls);
    TEST_ASSERT_EQUAL_STRING("sekret12", g_fake_net.set_pw_pw);
}

static void test_set_web_password_invalid(void) {
    g_fake_net.set_pw_rc = -1;
    TEST_ASSERT_EQUAL_INT(-1, run("SET WEB PASSWORD abc"));
    TEST_ASSERT_EQUAL_STRING("ERR INVALID\n", resp);
    TEST_ASSERT_EQUAL_STRING("abc", g_fake_net.set_pw_pw);
}

static void test_tz_rows(void) {
    snprintf(g_fake_net.mcfg.tz, sizeof g_fake_net.mcfg.tz, "CET-1CEST,M3.5.0,M10.5.0/3");
    TEST_ASSERT_EQUAL_INT(0, run("GET TZ"));
    TEST_ASSERT_EQUAL_STRING("OK TZ CET-1CEST,M3.5.0,M10.5.0/3\n", resp);
    TEST_ASSERT_EQUAL_INT(1, g_fake_net.get_mcfg_calls);

    TEST_ASSERT_EQUAL_INT(0, run("SET TZ EST5EDT,M3.2.0,M11.1.0"));
    TEST_ASSERT_EQUAL_STRING("OK TZ EST5EDT,M3.2.0,M11.1.0\n", resp);
    TEST_ASSERT_EQUAL_INT(1, g_fake_net.set_tz_calls);
    TEST_ASSERT_EQUAL_STRING("EST5EDT,M3.2.0,M11.1.0", g_fake_net.set_tz_tz);

    /* mcfg_commit validates TZ through time_core's tz_check, so an
     * unparseable string comes back as -1 -> ERR INVALID. */
    g_fake_net.set_tz_rc = -1;
    TEST_ASSERT_EQUAL_INT(-1, run("SET TZ nonsense"));
    TEST_ASSERT_EQUAL_STRING("ERR INVALID\n", resp);
}

static void test_set_node_mac(void) {
    TEST_ASSERT_EQUAL_INT(0, run("SET NODE 3 MAC 24:6f:28:aa:bb:03"));
    TEST_ASSERT_EQUAL_STRING("OK NODE 3 MAC 24:6f:28:aa:bb:03\n", resp);
    TEST_ASSERT_EQUAL_INT(1, g_fake_net.seed_mac_calls);
    TEST_ASSERT_EQUAL_UINT8(3, g_fake_net.seed_mac_zone);
    static const uint8_t want[6] = { 0x24, 0x6F, 0x28, 0xAA, 0xBB, 0x03 };
    TEST_ASSERT_EQUAL_UINT8_ARRAY(want, g_fake_net.seed_mac_mac, 6);
}

/* zone is an ARG_INT 1..HG_MAX_ZONES, so the table range rejects 9 before the
 * handler ever runs (node_mgr is never reached). */
static void test_set_node_mac_out_of_range(void) {
    TEST_ASSERT_EQUAL_INT(-1, run("SET NODE 9 MAC 24:6f:28:aa:bb:09"));
    TEST_ASSERT_EQUAL_STRING("ERR OUT_OF_RANGE\n", resp);
    TEST_ASSERT_EQUAL_INT(0, g_fake_net.seed_mac_calls);
}

static void test_set_node_mac_bad_mac(void) {
    TEST_ASSERT_EQUAL_INT(-1, run("SET NODE 3 MAC nope"));
    TEST_ASSERT_EQUAL_STRING("ERR BAD_ARGS\n", resp);
    TEST_ASSERT_EQUAL_INT(0, g_fake_net.seed_mac_calls);
}

/* Task 9 has not landed yet, so net_ops_master.c's seed_mac is a -1 stub;
 * that rc must read as ERR ZONE_UNKNOWN, not ERR INVALID -- it is the same
 * failure SET NODE <z> NAME already reports for an unknown zone. */
static void test_set_node_mac_unknown_zone(void) {
    g_fake_net.seed_mac_rc = -1;
    TEST_ASSERT_EQUAL_INT(-1, run("SET NODE 5 MAC 24:6f:28:aa:bb:05"));
    TEST_ASSERT_EQUAL_STRING("ERR ZONE_UNKNOWN\n", resp);
    TEST_ASSERT_EQUAL_INT(1, g_fake_net.seed_mac_calls);
}

/* rc -2 from any op that persists something is "valid but not stored" (NVS,
 * a commit-mutex timeout, an unavailable hash) -- a different thing to tell
 * an operator than ERR INVALID, so it gets its own token. */
static void test_net_rows_storage_failure(void) {
    g_fake_net.set_sta_rc = -2;
    TEST_ASSERT_EQUAL_INT(-1, run("SET WIFI STA Home pass1234"));
    TEST_ASSERT_EQUAL_STRING("ERR STORAGE\n", resp);

    g_fake_net.set_ap_rc = -2;
    TEST_ASSERT_EQUAL_INT(-1, run("SET WIFI AP GrowAP hillgrow1"));
    TEST_ASSERT_EQUAL_STRING("ERR STORAGE\n", resp);

    g_fake_net.set_pw_rc = -2;
    TEST_ASSERT_EQUAL_INT(-1, run("SET WEB PASSWORD sekret12"));
    TEST_ASSERT_EQUAL_STRING("ERR STORAGE\n", resp);

    g_fake_net.set_tz_rc = -2;
    TEST_ASSERT_EQUAL_INT(-1, run("SET TZ EST5EDT,M3.2.0,M11.1.0"));
    TEST_ASSERT_EQUAL_STRING("ERR STORAGE\n", resp);

    /* -1 must still be ERR INVALID on every one of them: the rcs are not
     * interchangeable. */
    g_fake_net.set_sta_rc = -1;
    TEST_ASSERT_EQUAL_INT(-1, run("SET WIFI STA Home pass1234"));
    TEST_ASSERT_EQUAL_STRING("ERR INVALID\n", resp);
    g_fake_net.set_tz_rc = -1;
    TEST_ASSERT_EQUAL_INT(-1, run("SET TZ EST5EDT,M3.2.0,M11.1.0"));
    TEST_ASSERT_EQUAL_STRING("ERR INVALID\n", resp);
}

/* -3 is "this board cannot hash a password at all" -- no storage fault, no bad
 * input, nothing to retry. Only set_web_password can produce it, but the
 * mapping is shared, so the other rows are pinned too. */
static void test_net_rows_internal_failure(void) {
    g_fake_net.set_pw_rc = -3;
    TEST_ASSERT_EQUAL_INT(-1, run("SET WEB PASSWORD sekret12"));
    TEST_ASSERT_EQUAL_STRING("ERR INTERNAL\n", resp);

    g_fake_net.set_sta_rc = -3;
    TEST_ASSERT_EQUAL_INT(-1, run("SET WIFI STA Home pass1234"));
    TEST_ASSERT_EQUAL_STRING("ERR INTERNAL\n", resp);

    /* -2 must NOT be swept into INTERNAL: storage keeps its own token. */
    g_fake_net.set_pw_rc = -2;
    TEST_ASSERT_EQUAL_INT(-1, run("SET WEB PASSWORD sekret12"));
    TEST_ASSERT_EQUAL_STRING("ERR STORAGE\n", resp);
}

/* seed_mac has no persistence path of its own, so it keeps the node_ops
 * convention: any non-zero rc is an unusable zone. */
static void test_set_node_mac_storage_rc_is_zone_unknown(void) {
    g_fake_net.seed_mac_rc = -2;
    TEST_ASSERT_EQUAL_INT(-1, run("SET NODE 5 MAC 24:6f:28:aa:bb:05"));
    TEST_ASSERT_EQUAL_STRING("ERR ZONE_UNKNOWN\n", resp);
}

/* master_cmds_init() (the SP3 entry point) leaves net NULL: every NET/TIME
 * row must then answer ERR INTERNAL rather than dereference it. */
static void test_net_rows_without_ops(void) {
    master_cmds_init(&FAKE_NODE_OPS);
    TEST_ASSERT_EQUAL_INT(-1, run("GET WIFI"));
    TEST_ASSERT_EQUAL_STRING("ERR INTERNAL\n", resp);
    TEST_ASSERT_EQUAL_INT(-1, run("SET WIFI STA Home pass1234"));
    TEST_ASSERT_EQUAL_STRING("ERR INTERNAL\n", resp);
    TEST_ASSERT_EQUAL_INT(-1, run("SET WIFI AP GrowAP hillgrow1"));
    TEST_ASSERT_EQUAL_STRING("ERR INTERNAL\n", resp);
    TEST_ASSERT_EQUAL_INT(-1, run("SET WEB PASSWORD sekret12"));
    TEST_ASSERT_EQUAL_STRING("ERR INTERNAL\n", resp);
    TEST_ASSERT_EQUAL_INT(-1, run("GET TZ"));
    TEST_ASSERT_EQUAL_STRING("ERR INTERNAL\n", resp);
    TEST_ASSERT_EQUAL_INT(-1, run("SET TZ EST5EDT,M3.2.0,M11.1.0"));
    TEST_ASSERT_EQUAL_STRING("ERR INTERNAL\n", resp);
    TEST_ASSERT_EQUAL_INT(-1, run("SET NODE 3 MAC 24:6f:28:aa:bb:03"));
    TEST_ASSERT_EQUAL_STRING("ERR INTERNAL\n", resp);
    TEST_ASSERT_EQUAL_INT(0, g_fake_net.set_sta_calls + g_fake_net.seed_mac_calls);
}

/* The NETWORK/TIME area index lines must name the new nouns, and each row
 * must render its own usage under <NOUN> HELP. */
static void test_help_lists_net_rows(void) {
    TEST_ASSERT_EQUAL_INT(0, run("HELP"));
    TEST_ASSERT_NOT_NULL(strstr(resp, "+ NETWORK: "));
    TEST_ASSERT_NOT_NULL(strstr(resp, "WIFI STA"));
    TEST_ASSERT_NOT_NULL(strstr(resp, "WIFI AP"));
    TEST_ASSERT_NOT_NULL(strstr(resp, "WEB PASSWORD"));
    TEST_ASSERT_NOT_NULL(strstr(resp, "NODE MAC"));
    TEST_ASSERT_NOT_NULL(strstr(resp, "TZ"));

    TEST_ASSERT_EQUAL_INT(0, run("SET WIFI HELP"));
    TEST_ASSERT_NOT_NULL(strstr(resp, "+ SET WIFI STA <ssid> <pass>"));
    TEST_ASSERT_NOT_NULL(strstr(resp, "+ SET WIFI AP <ssid> <pass>"));

    TEST_ASSERT_EQUAL_INT(0, run("GET WIFI HELP"));
    TEST_ASSERT_NOT_NULL(strstr(resp, "+ GET WIFI"));

    TEST_ASSERT_EQUAL_INT(0, run("SET TZ HELP"));
    TEST_ASSERT_NOT_NULL(strstr(resp, "+ SET TZ <posix>"));
    TEST_ASSERT_NOT_NULL(strstr(resp, "+ GET TZ"));

    TEST_ASSERT_EQUAL_INT(0, run("SET WEB HELP"));
    TEST_ASSERT_NOT_NULL(strstr(resp, "+ SET WEB PASSWORD <password>"));

    TEST_ASSERT_EQUAL_INT(0, run("SET NODE HELP"));
    TEST_ASSERT_NOT_NULL(strstr(resp, "+ SET NODE <zone 1-8> MAC <mac xx:xx:xx:xx:xx:xx>"));
}

int main(void) { UNITY_BEGIN();
    RUN_TEST(test_table_valid);
    RUN_TEST(test_merged_table_valid);
    RUN_TEST(test_get_nodes);
    RUN_TEST(test_get_ring_idle);
    RUN_TEST(test_get_ring_open_blame_and_counters);
    RUN_TEST(test_get_node_detail);
    RUN_TEST(test_get_node_unknown);
    RUN_TEST(test_get_unassigned);
    RUN_TEST(test_set_node_name);
    RUN_TEST(test_set_node_name_too_long);
    RUN_TEST(test_set_node_name_unknown_zone);
    RUN_TEST(test_clear_node);
    RUN_TEST(test_clear_node_without_confirm);
    RUN_TEST(test_ring_trace);
    RUN_TEST(test_fw_zone_rows);
    RUN_TEST(test_get_wifi_down);
    RUN_TEST(test_get_wifi_up);
    RUN_TEST(test_get_wifi_down_with_reason);
    RUN_TEST(test_set_wifi_sta);
    RUN_TEST(test_set_wifi_sta_open);
    RUN_TEST(test_set_wifi_sta_cleared);
    RUN_TEST(test_set_wifi_sta_invalid);
    RUN_TEST(test_set_wifi_ap);
    RUN_TEST(test_set_web_password);
    RUN_TEST(test_set_web_password_invalid);
    RUN_TEST(test_tz_rows);
    RUN_TEST(test_set_node_mac);
    RUN_TEST(test_set_node_mac_out_of_range);
    RUN_TEST(test_set_node_mac_bad_mac);
    RUN_TEST(test_set_node_mac_unknown_zone);
    RUN_TEST(test_net_rows_storage_failure);
    RUN_TEST(test_net_rows_internal_failure);
    RUN_TEST(test_set_node_mac_storage_rc_is_zone_unknown);
    RUN_TEST(test_net_rows_without_ops);
    RUN_TEST(test_help_lists_net_rows);
    return UNITY_END(); }
