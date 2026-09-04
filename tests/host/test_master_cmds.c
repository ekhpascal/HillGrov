#include <string.h>
#include "unity.h"
#include "cmd_core.h"
#include "ring_proto.h"
#include "master_cmds.h"
#include "cmd_common.h"
#include "ota_trial.h"
#include "fake_node_ops.h"
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
    master_cmds_init(&FAKE_NODE_OPS);
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

    /* the real cmd_table_master.c wiring maps its Task-15 stub's permanent -1
     * to NOT_IMPLEMENTED (ruling #5) -- exercised here via fail_fw_*. */
    g_fake_nodes.fail_fw_all = 1;
    TEST_ASSERT_EQUAL_INT(-1, run("SET FW ZONES CONFIRM"));
    TEST_ASSERT_EQUAL_STRING("ERR NOT_IMPLEMENTED\n", resp);

    TEST_ASSERT_EQUAL_INT(0, run("SET FW ABORT"));
    TEST_ASSERT_EQUAL_STRING("OK FW ABORT\n", resp);
    TEST_ASSERT_EQUAL_INT(1, g_fake_nodes.fw_abort_calls);

    snprintf(g_fake_nodes.fw_status_text, sizeof g_fake_nodes.fw_status_text, "IDLE");
    TEST_ASSERT_EQUAL_INT(0, run("GET FW ZONE"));
    TEST_ASSERT_EQUAL_STRING("OK FW ZONE IDLE\n", resp);
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
    return UNITY_END(); }
