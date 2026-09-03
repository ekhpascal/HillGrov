#include <string.h>
#include "unity.h"
#include "ring_proto.h"

/* ========== Zone Node Tests ========== */

static void test_zone_src_equals_my_id_drop_self(void) {
    uint8_t my_mac[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
    ring_hdr_t h = {.src = 5, .dst = 2, .type = RING_T_NOTIFY, .flags = 0, .ttl = 10, .len = 0, .seq = 100};
    uint8_t payload[1] = {0};
    ring_rt_t result = ring_route(0, 5, my_mac, &h, payload);
    TEST_ASSERT_EQUAL_INT(RING_RT_DROP_SELF, result);
}

static void test_zone_dst_equals_my_id_consume(void) {
    uint8_t my_mac[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
    ring_hdr_t h = {.src = 1, .dst = 5, .type = RING_T_CMD, .flags = 0, .ttl = 10, .len = 0, .seq = 101};
    uint8_t payload[1] = {0};
    ring_rt_t result = ring_route(0, 5, my_mac, &h, payload);
    TEST_ASSERT_EQUAL_INT(RING_RT_CONSUME, result);
}

static void test_zone_dst_broadcast_consume_fwd(void) {
    uint8_t my_mac[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
    ring_hdr_t h = {.src = 1, .dst = RING_ID_BCAST, .type = RING_T_NOTIFY, .flags = 0, .ttl = 10, .len = 0, .seq = 102};
    uint8_t payload[1] = {0};
    ring_rt_t result = ring_route(0, 5, my_mac, &h, payload);
    TEST_ASSERT_EQUAL_INT(RING_RT_CONSUME_FWD, result);
}

static void test_zone_dst_unassigned_assign_id_mac_match_consume(void) {
    uint8_t my_mac[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
    /* Payload: MAC address that matches */
    uint8_t payload[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
    ring_hdr_t h = {.src = 0, .dst = RING_ID_UNASSIGNED, .type = RING_T_ASSIGN_ID, .flags = 0, .ttl = 10, .len = 6, .seq = 103};
    ring_rt_t result = ring_route(0, 5, my_mac, &h, payload);
    TEST_ASSERT_EQUAL_INT(RING_RT_CONSUME, result);
}

static void test_zone_dst_unassigned_assign_id_mac_mismatch_forward(void) {
    uint8_t my_mac[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
    /* Payload: MAC address that doesn't match */
    uint8_t payload[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    ring_hdr_t h = {.src = 0, .dst = RING_ID_UNASSIGNED, .type = RING_T_ASSIGN_ID, .flags = 0, .ttl = 10, .len = 6, .seq = 104};
    ring_rt_t result = ring_route(0, 5, my_mac, &h, payload);
    TEST_ASSERT_EQUAL_INT(RING_RT_FORWARD, result);
}

static void test_zone_dst_unassigned_not_assign_id_forward(void) {
    uint8_t my_mac[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
    uint8_t payload[1] = {0};
    ring_hdr_t h = {.src = 0, .dst = RING_ID_UNASSIGNED, .type = RING_T_HEARTBEAT, .flags = 0, .ttl = 10, .len = 0, .seq = 105};
    ring_rt_t result = ring_route(0, 5, my_mac, &h, payload);
    TEST_ASSERT_EQUAL_INT(RING_RT_FORWARD, result);
}

static void test_zone_else_ttl_greater_than_1_forward(void) {
    uint8_t my_mac[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
    ring_hdr_t h = {.src = 1, .dst = 3, .type = RING_T_NOTIFY, .flags = 0, .ttl = 10, .len = 0, .seq = 106};
    uint8_t payload[1] = {0};
    ring_rt_t result = ring_route(0, 5, my_mac, &h, payload);
    TEST_ASSERT_EQUAL_INT(RING_RT_FORWARD, result);
}

static void test_zone_else_ttl_equals_1_drop(void) {
    uint8_t my_mac[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
    ring_hdr_t h = {.src = 1, .dst = 3, .type = RING_T_NOTIFY, .flags = 0, .ttl = 1, .len = 0, .seq = 107};
    uint8_t payload[1] = {0};
    ring_rt_t result = ring_route(0, 5, my_mac, &h, payload);
    TEST_ASSERT_EQUAL_INT(RING_RT_DROP, result);
}

static void test_zone_else_ttl_equals_0_drop(void) {
    uint8_t my_mac[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
    ring_hdr_t h = {.src = 1, .dst = 3, .type = RING_T_NOTIFY, .flags = 0, .ttl = 0, .len = 0, .seq = 108};
    uint8_t payload[1] = {0};
    ring_rt_t result = ring_route(0, 5, my_mac, &h, payload);
    TEST_ASSERT_EQUAL_INT(RING_RT_DROP, result);
}

/* ========== Master Node Tests ========== */

static void test_master_src_equals_master_drop_self(void) {
    uint8_t my_mac[6] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    ring_hdr_t h = {.src = RING_ID_MASTER, .dst = 2, .type = RING_T_HEARTBEAT, .flags = 0, .ttl = 10, .len = 0, .seq = 200};
    uint8_t payload[1] = {0};
    ring_rt_t result = ring_route(1, RING_ID_MASTER, my_mac, &h, payload);
    TEST_ASSERT_EQUAL_INT(RING_RT_DROP_SELF, result);
}

static void test_master_dst_equals_zone_consume(void) {
    uint8_t my_mac[6] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    ring_hdr_t h = {.src = 2, .dst = RING_ID_MASTER, .type = RING_T_HEARTBEAT, .flags = 0, .ttl = 10, .len = 0, .seq = 201};
    uint8_t payload[1] = {0};
    ring_rt_t result = ring_route(1, RING_ID_MASTER, my_mac, &h, payload);
    TEST_ASSERT_EQUAL_INT(RING_RT_CONSUME, result);
}

static void test_master_dst_broadcast_consume(void) {
    uint8_t my_mac[6] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    ring_hdr_t h = {.src = 2, .dst = RING_ID_BCAST, .type = RING_T_NOTIFY, .flags = 0, .ttl = 10, .len = 0, .seq = 202};
    uint8_t payload[1] = {0};
    ring_rt_t result = ring_route(1, RING_ID_MASTER, my_mac, &h, payload);
    TEST_ASSERT_EQUAL_INT(RING_RT_CONSUME, result);
}

static void test_master_never_forwards_dst_zone(void) {
    uint8_t my_mac[6] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    ring_hdr_t h = {.src = 2, .dst = 3, .type = RING_T_NOTIFY, .flags = 0, .ttl = 10, .len = 0, .seq = 203};
    uint8_t payload[1] = {0};
    ring_rt_t result = ring_route(1, RING_ID_MASTER, my_mac, &h, payload);
    TEST_ASSERT_EQUAL_INT(RING_RT_CONSUME, result);
}

static void test_master_never_forwards_dst_unassigned(void) {
    uint8_t my_mac[6] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    ring_hdr_t h = {.src = 2, .dst = RING_ID_UNASSIGNED, .type = RING_T_NOTIFY, .flags = 0, .ttl = 10, .len = 0, .seq = 204};
    uint8_t payload[1] = {0};
    ring_rt_t result = ring_route(1, RING_ID_MASTER, my_mac, &h, payload);
    TEST_ASSERT_EQUAL_INT(RING_RT_CONSUME, result);
}

/* ========== Duplicate Cache Tests ========== */

static void test_dup_init_clears_cache(void) {
    ring_dup_t c;
    memset(&c, 0xFF, sizeof c);
    ring_dup_init(&c);
    TEST_ASSERT_EQUAL_UINT16(0, c.seq);
    TEST_ASSERT_EQUAL_UINT8(0, c.state);
    TEST_ASSERT_EQUAL_UINT8(0, c.status);
    TEST_ASSERT_EQUAL_UINT32(0, c.t_ms);
}

static void test_dup_check_first_seq_returns_exec(void) {
    ring_dup_t c;
    ring_dup_init(&c);
    int result = ring_dup_check(&c, 100, 1000);
    TEST_ASSERT_EQUAL_INT(RING_DUP_EXEC, result);
}

static void test_dup_check_new_seq_returns_exec(void) {
    ring_dup_t c;
    ring_dup_init(&c);
    ring_dup_start(&c, 100, 1000);
    ring_dup_done(&c, 100, 0, "");
    int result = ring_dup_check(&c, 101, 1000);
    TEST_ASSERT_EQUAL_INT(RING_DUP_EXEC, result);
}

static void test_dup_check_same_seq_in_progress_absorb(void) {
    ring_dup_t c;
    ring_dup_init(&c);
    ring_dup_start(&c, 100, 1000);
    int result = ring_dup_check(&c, 100, 1000);
    TEST_ASSERT_EQUAL_INT(RING_DUP_ABSORB, result);
}

static void test_dup_check_same_seq_done_within_window_replay(void) {
    ring_dup_t c;
    ring_dup_init(&c);
    ring_dup_start(&c, 100, 1000);
    ring_dup_done(&c, 100, 0, "test");
    int result = ring_dup_check(&c, 100, 2000);
    TEST_ASSERT_EQUAL_INT(RING_DUP_REPLAY, result);
}

static void test_dup_check_same_seq_done_outside_window_exec(void) {
    ring_dup_t c;
    ring_dup_init(&c);
    ring_dup_start(&c, 100, 1000);
    ring_dup_done(&c, 100, 0, "test");
    int result = ring_dup_check(&c, 100, 4001);
    TEST_ASSERT_EQUAL_INT(RING_DUP_EXEC, result);
}

static void test_dup_start_marks_in_progress(void) {
    ring_dup_t c;
    ring_dup_init(&c);
    ring_dup_start(&c, 42, 5000);
    TEST_ASSERT_EQUAL_UINT16(42, c.seq);
    TEST_ASSERT_EQUAL_UINT8(RING_DUP_ABSORB, c.state);  /* IN_PROGRESS = ABSORB state */
    TEST_ASSERT_EQUAL_UINT32(5000, c.t_ms);
}

static void test_dup_done_marks_complete(void) {
    ring_dup_t c;
    ring_dup_init(&c);
    ring_dup_start(&c, 42, 5000);
    ring_dup_done(&c, 42, 5, "operation succeeded");
    TEST_ASSERT_EQUAL_UINT8(RING_DUP_REPLAY, c.state);  /* DONE = REPLAY state */
    TEST_ASSERT_EQUAL_UINT8(5, c.status);
    TEST_ASSERT_EQUAL_INT(0, strcmp(c.detail, "operation succeeded"));
}

static void test_dup_detail_preserved_exactly(void) {
    ring_dup_t c;
    ring_dup_init(&c);
    const char *detail_msg = "zone 3 reports sensor timeout on channel 2";
    ring_dup_start(&c, 50, 2000);
    ring_dup_done(&c, 50, 1, detail_msg);
    TEST_ASSERT_EQUAL_MEMORY(detail_msg, c.detail, strlen(detail_msg) + 1);
}

static void test_dup_new_seq_overwrites_old(void) {
    ring_dup_t c;
    ring_dup_init(&c);
    ring_dup_start(&c, 100, 1000);
    ring_dup_done(&c, 100, 0, "old entry");
    ring_dup_start(&c, 200, 2000);
    TEST_ASSERT_EQUAL_UINT16(200, c.seq);
    TEST_ASSERT_EQUAL_UINT32(2000, c.t_ms);
    TEST_ASSERT_EQUAL_UINT8(RING_DUP_ABSORB, c.state);
}

static void test_dup_cache_single_slot_only(void) {
    ring_dup_t c;
    ring_dup_init(&c);
    /* Start with seq 100, finish it */
    ring_dup_start(&c, 100, 1000);
    ring_dup_done(&c, 100, 0, "first");
    /* Start with seq 200, should overwrite */
    ring_dup_start(&c, 200, 2000);
    ring_dup_done(&c, 200, 0, "second");
    /* Check seq 100 is gone (returns EXEC, not REPLAY) */
    int result = ring_dup_check(&c, 100, 2500);
    TEST_ASSERT_EQUAL_INT(RING_DUP_EXEC, result);
}

void setUp(void) {}
void tearDown(void) {}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_zone_src_equals_my_id_drop_self);
    RUN_TEST(test_zone_dst_equals_my_id_consume);
    RUN_TEST(test_zone_dst_broadcast_consume_fwd);
    RUN_TEST(test_zone_dst_unassigned_assign_id_mac_match_consume);
    RUN_TEST(test_zone_dst_unassigned_assign_id_mac_mismatch_forward);
    RUN_TEST(test_zone_dst_unassigned_not_assign_id_forward);
    RUN_TEST(test_zone_else_ttl_greater_than_1_forward);
    RUN_TEST(test_zone_else_ttl_equals_1_drop);
    RUN_TEST(test_zone_else_ttl_equals_0_drop);
    RUN_TEST(test_master_src_equals_master_drop_self);
    RUN_TEST(test_master_dst_equals_zone_consume);
    RUN_TEST(test_master_dst_broadcast_consume);
    RUN_TEST(test_master_never_forwards_dst_zone);
    RUN_TEST(test_master_never_forwards_dst_unassigned);
    RUN_TEST(test_dup_init_clears_cache);
    RUN_TEST(test_dup_check_first_seq_returns_exec);
    RUN_TEST(test_dup_check_new_seq_returns_exec);
    RUN_TEST(test_dup_check_same_seq_in_progress_absorb);
    RUN_TEST(test_dup_check_same_seq_done_within_window_replay);
    RUN_TEST(test_dup_check_same_seq_done_outside_window_exec);
    RUN_TEST(test_dup_start_marks_in_progress);
    RUN_TEST(test_dup_done_marks_complete);
    RUN_TEST(test_dup_detail_preserved_exactly);
    RUN_TEST(test_dup_new_seq_overwrites_old);
    RUN_TEST(test_dup_cache_single_slot_only);
    return UNITY_END();
}
