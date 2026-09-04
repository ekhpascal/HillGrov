#include <string.h>
#include "unity.h"
#include "ring_proto.h"

/* ========== Timeout formula: clamp(200 + 40*ring_size, 400, 900) ========== */

static void test_ack_timeout_formula_clamp_points(void) {
    TEST_ASSERT_EQUAL_UINT32(400, ring_ack_timeout_ms(0));
    TEST_ASSERT_EQUAL_UINT32(400, ring_ack_timeout_ms(1));
    TEST_ASSERT_EQUAL_UINT32(400, ring_ack_timeout_ms(2));
    TEST_ASSERT_EQUAL_UINT32(400, ring_ack_timeout_ms(3));
    TEST_ASSERT_EQUAL_UINT32(400, ring_ack_timeout_ms(4));
    TEST_ASSERT_EQUAL_UINT32(400, ring_ack_timeout_ms(5));   /* 200+40*5 = 400, boundary */
    TEST_ASSERT_EQUAL_UINT32(440, ring_ack_timeout_ms(6));
    TEST_ASSERT_EQUAL_UINT32(480, ring_ack_timeout_ms(7));
    TEST_ASSERT_EQUAL_UINT32(520, ring_ack_timeout_ms(8));
    TEST_ASSERT_EQUAL_UINT32(880, ring_ack_timeout_ms(17));
    TEST_ASSERT_EQUAL_UINT32(900, ring_ack_timeout_ms(18));  /* 200+40*18 = 920, clamps to 900 */
    TEST_ASSERT_EQUAL_UINT32(900, ring_ack_timeout_ms(19));
    TEST_ASSERT_EQUAL_UINT32(900, ring_ack_timeout_ms(255));
}

/* ========== Single outstanding in-flight entry ring-wide ========== */

static void test_single_inflight_two_cmds(void) {
    ring_trk_t t; ring_trk_init(&t);
    ring_hdr_t h1 = { .src = 0, .dst = 1, .type = RING_T_CMD, .flags = RING_F_ACK_REQ, .ttl = 16, .len = 4, .seq = 1 };
    ring_hdr_t h2 = { .src = 0, .dst = 2, .type = RING_T_CMD, .flags = RING_F_ACK_REQ, .ttl = 16, .len = 4, .seq = 2 };
    TEST_ASSERT_EQUAL_INT(0, ring_trk_submit(&t, &h1, (const uint8_t *)"ping", 0));
    TEST_ASSERT_EQUAL_INT(0, ring_trk_submit(&t, &h2, (const uint8_t *)"ping", 0));

    ring_trk_ev_t ev;
    TEST_ASSERT_EQUAL_INT(1, ring_trk_tick(&t, 0, 8, &ev));
    TEST_ASSERT_EQUAL_INT(RING_TRK_EV_SEND, ev.kind);
    TEST_ASSERT_EQUAL_UINT16(1, ev.seq);

    /* second entry must not send while the first is in flight and not yet timed out */
    TEST_ASSERT_EQUAL_INT(0, ring_trk_tick(&t, 1, 8, &ev));

    TEST_ASSERT_EQUAL_INT(1, ring_trk_ack(&t, 1, 0, "OK", 2, &ev));
    TEST_ASSERT_EQUAL_INT(RING_TRK_EV_DONE, ev.kind);
    TEST_ASSERT_EQUAL_UINT16(1, ev.seq);
    TEST_ASSERT_EQUAL_STRING("OK", ev.detail);

    TEST_ASSERT_EQUAL_INT(1, ring_trk_tick(&t, 1, 8, &ev));
    TEST_ASSERT_EQUAL_INT(RING_TRK_EV_SEND, ev.kind);
    TEST_ASSERT_EQUAL_UINT16(2, ev.seq);
}

/* ========== Priority insert ========== */

static void test_priority_insert_cmd_before_cfg_commit(void) {
    ring_trk_t t; ring_trk_init(&t);
    ring_hdr_t hc = { .src = 0, .dst = 1, .type = RING_T_CFG_COMMIT, .flags = RING_F_ACK_REQ, .ttl = 16, .len = 0, .seq = 10 };
    ring_hdr_t hm = { .src = 0, .dst = 2, .type = RING_T_CMD, .flags = RING_F_ACK_REQ, .ttl = 16, .len = 0, .seq = 11 };
    TEST_ASSERT_EQUAL_INT(0, ring_trk_submit(&t, &hc, NULL, 0));
    TEST_ASSERT_EQUAL_INT(0, ring_trk_submit(&t, &hm, NULL, 0));

    ring_trk_ev_t ev;
    TEST_ASSERT_EQUAL_INT(1, ring_trk_tick(&t, 0, 8, &ev));
    TEST_ASSERT_EQUAL_INT(RING_TRK_EV_SEND, ev.kind);
    TEST_ASSERT_EQUAL_UINT16(11, ev.seq);   /* CMD jumped ahead of the queued CFG_COMMIT */
}

static void test_priority_insert_fw_update_before_cfg_get(void) {
    ring_trk_t t; ring_trk_init(&t);
    ring_hdr_t hg = { .src = 0, .dst = 1, .type = RING_T_CFG_GET, .flags = RING_F_ACK_REQ, .ttl = 16, .len = 0, .seq = 12 };
    ring_hdr_t hf = { .src = 0, .dst = 2, .type = RING_T_FW_UPDATE, .flags = RING_F_ACK_REQ, .ttl = 16, .len = 0, .seq = 13 };
    TEST_ASSERT_EQUAL_INT(0, ring_trk_submit(&t, &hg, NULL, 0));
    TEST_ASSERT_EQUAL_INT(0, ring_trk_submit(&t, &hf, NULL, 0));

    ring_trk_ev_t ev;
    TEST_ASSERT_EQUAL_INT(1, ring_trk_tick(&t, 0, 8, &ev));
    TEST_ASSERT_EQUAL_UINT16(13, ev.seq);   /* FW_UPDATE jumped ahead of the queued CFG_GET */
}

static void test_priority_insert_never_preempts_inflight(void) {
    ring_trk_t t; ring_trk_init(&t);
    ring_hdr_t hc = { .src = 0, .dst = 1, .type = RING_T_CFG_COMMIT, .flags = RING_F_ACK_REQ, .ttl = 16, .len = 0, .seq = 20 };
    TEST_ASSERT_EQUAL_INT(0, ring_trk_submit(&t, &hc, NULL, 0));

    ring_trk_ev_t ev;
    TEST_ASSERT_EQUAL_INT(1, ring_trk_tick(&t, 0, 8, &ev));   /* CFG_COMMIT now in flight */
    TEST_ASSERT_EQUAL_UINT16(20, ev.seq);

    ring_hdr_t hm = { .src = 0, .dst = 2, .type = RING_T_CMD, .flags = RING_F_ACK_REQ, .ttl = 16, .len = 0, .seq = 21 };
    TEST_ASSERT_EQUAL_INT(0, ring_trk_submit(&t, &hm, NULL, 0));

    /* still nothing to do: the in-flight CFG_COMMIT has not timed out, and the CMD must not preempt it */
    TEST_ASSERT_EQUAL_INT(0, ring_trk_tick(&t, 1, 8, &ev));

    TEST_ASSERT_EQUAL_INT(1, ring_trk_ack(&t, 20, 0, "", 0, &ev));

    TEST_ASSERT_EQUAL_INT(1, ring_trk_tick(&t, 1, 8, &ev));
    TEST_ASSERT_EQUAL_INT(RING_TRK_EV_SEND, ev.kind);
    TEST_ASSERT_EQUAL_UINT16(21, ev.seq);
}

static void test_priority_insert_multiple_cmds_group_before_cfg(void) {
    ring_trk_t t; ring_trk_init(&t);
    ring_hdr_t h1 = { .src = 0, .dst = 1, .type = RING_T_CMD,        .flags = RING_F_ACK_REQ, .ttl = 16, .len = 0, .seq = 30 };
    ring_hdr_t hc = { .src = 0, .dst = 1, .type = RING_T_CFG_COMMIT, .flags = RING_F_ACK_REQ, .ttl = 16, .len = 0, .seq = 31 };
    ring_hdr_t h2 = { .src = 0, .dst = 2, .type = RING_T_CMD,        .flags = RING_F_ACK_REQ, .ttl = 16, .len = 0, .seq = 32 };
    TEST_ASSERT_EQUAL_INT(0, ring_trk_submit(&t, &h1, NULL, 0));
    TEST_ASSERT_EQUAL_INT(0, ring_trk_submit(&t, &hc, NULL, 0));
    TEST_ASSERT_EQUAL_INT(0, ring_trk_submit(&t, &h2, NULL, 0));

    ring_trk_ev_t ev;
    TEST_ASSERT_EQUAL_INT(1, ring_trk_tick(&t, 0, 8, &ev));
    TEST_ASSERT_EQUAL_UINT16(30, ev.seq);
    TEST_ASSERT_EQUAL_INT(1, ring_trk_ack(&t, 30, 0, "", 0, &ev));

    TEST_ASSERT_EQUAL_INT(1, ring_trk_tick(&t, 0, 8, &ev));
    TEST_ASSERT_EQUAL_UINT16(32, ev.seq);   /* second CMD sends before the CFG_COMMIT */
    TEST_ASSERT_EQUAL_INT(1, ring_trk_ack(&t, 32, 0, "", 0, &ev));

    TEST_ASSERT_EQUAL_INT(1, ring_trk_tick(&t, 0, 8, &ev));
    TEST_ASSERT_EQUAL_UINT16(31, ev.seq);   /* CFG_COMMIT sends last */
}

/* ========== Retry ladder: 3 attempts total, byte-identical wire, FAIL after 3rd timeout ========== */

static void test_retry_ladder_and_timeout_fail(void) {
    ring_trk_t t; ring_trk_init(&t);
    ring_hdr_t h = { .src = 0, .dst = 3, .type = RING_T_CMD, .flags = RING_F_ACK_REQ, .ttl = 16, .len = 4, .seq = 40 };
    TEST_ASSERT_EQUAL_INT(0, ring_trk_submit(&t, &h, (const uint8_t *)"ping", 0));

    const uint8_t ring_size = 5;   /* ring_ack_timeout_ms(5) == 400 */
    ring_trk_ev_t ev;
    uint8_t wire1[RING_WIRE_MAX], wire2[RING_WIRE_MAX], wire3[RING_WIRE_MAX];
    uint16_t len1, len2, len3;

    TEST_ASSERT_EQUAL_INT(1, ring_trk_tick(&t, 0, ring_size, &ev));      /* attempt 1: initial send */
    TEST_ASSERT_EQUAL_INT(RING_TRK_EV_SEND, ev.kind);
    len1 = ev.wire_len; memcpy(wire1, ev.wire, len1);

    TEST_ASSERT_EQUAL_INT(0, ring_trk_tick(&t, 399, ring_size, &ev));    /* not yet timed out */

    TEST_ASSERT_EQUAL_INT(1, ring_trk_tick(&t, 400, ring_size, &ev));    /* attempt 2: resend */
    TEST_ASSERT_EQUAL_INT(RING_TRK_EV_SEND, ev.kind);
    len2 = ev.wire_len; memcpy(wire2, ev.wire, len2);

    TEST_ASSERT_EQUAL_INT(0, ring_trk_tick(&t, 799, ring_size, &ev));

    TEST_ASSERT_EQUAL_INT(1, ring_trk_tick(&t, 800, ring_size, &ev));    /* attempt 3: resend */
    TEST_ASSERT_EQUAL_INT(RING_TRK_EV_SEND, ev.kind);
    len3 = ev.wire_len; memcpy(wire3, ev.wire, len3);

    TEST_ASSERT_EQUAL_UINT16(len1, len2);
    TEST_ASSERT_EQUAL_UINT16(len1, len3);
    TEST_ASSERT_EQUAL_MEMORY(wire1, wire2, len1);
    TEST_ASSERT_EQUAL_MEMORY(wire1, wire3, len1);

    TEST_ASSERT_EQUAL_INT(0, ring_trk_tick(&t, 1199, ring_size, &ev));

    TEST_ASSERT_EQUAL_INT(1, ring_trk_tick(&t, 1200, ring_size, &ev));   /* 3rd attempt's timeout expires -> FAIL */
    TEST_ASSERT_EQUAL_INT(RING_TRK_EV_FAIL, ev.kind);
    TEST_ASSERT_EQUAL_STRING("ZONE_TIMEOUT", ev.fail_token);
    TEST_ASSERT_EQUAL_UINT16(40, ev.seq);

    TEST_ASSERT_EQUAL_INT(0, ring_trk_tick(&t, 1200, ring_size, &ev));   /* queue now empty */
}

static void test_late_ack_after_fail_returns_zero(void) {
    ring_trk_t t; ring_trk_init(&t);
    ring_hdr_t h = { .src = 0, .dst = 3, .type = RING_T_CMD, .flags = RING_F_ACK_REQ, .ttl = 16, .len = 0, .seq = 50 };
    TEST_ASSERT_EQUAL_INT(0, ring_trk_submit(&t, &h, NULL, 0));

    ring_trk_ev_t ev;
    ring_trk_tick(&t, 0, 5, &ev);      /* attempt 1 */
    ring_trk_tick(&t, 400, 5, &ev);    /* attempt 2 */
    ring_trk_tick(&t, 800, 5, &ev);    /* attempt 3 */
    TEST_ASSERT_EQUAL_INT(1, ring_trk_tick(&t, 1200, 5, &ev));
    TEST_ASSERT_EQUAL_INT(RING_TRK_EV_FAIL, ev.kind);

    /* late ACK for the seq that already failed: no entry left to match, no event */
    TEST_ASSERT_EQUAL_INT(0, ring_trk_ack(&t, 50, 0, "late", 4, &ev));
}

static void test_ack_wrong_seq_returns_zero(void) {
    ring_trk_t t; ring_trk_init(&t);
    ring_hdr_t h = { .src = 0, .dst = 3, .type = RING_T_CMD, .flags = RING_F_ACK_REQ, .ttl = 16, .len = 0, .seq = 55 };
    ring_trk_submit(&t, &h, NULL, 0);
    ring_trk_ev_t ev;
    ring_trk_tick(&t, 0, 5, &ev);
    TEST_ASSERT_EQUAL_INT(0, ring_trk_ack(&t, 9999, 0, "", 0, &ev));
}

/* ========== UNCLAIMED ========== */

static void test_unclaimed_immediate_fail_and_next_proceeds(void) {
    ring_trk_t t; ring_trk_init(&t);
    ring_hdr_t h1 = { .src = 0, .dst = 4, .type = RING_T_CMD, .flags = RING_F_ACK_REQ, .ttl = 16, .len = 0, .seq = 60 };
    ring_hdr_t h2 = { .src = 0, .dst = 5, .type = RING_T_CMD, .flags = RING_F_ACK_REQ, .ttl = 16, .len = 0, .seq = 61 };
    TEST_ASSERT_EQUAL_INT(0, ring_trk_submit(&t, &h1, NULL, 0));
    TEST_ASSERT_EQUAL_INT(0, ring_trk_submit(&t, &h2, NULL, 0));

    ring_trk_ev_t ev;
    TEST_ASSERT_EQUAL_INT(1, ring_trk_tick(&t, 0, 5, &ev));   /* h1 in flight */
    TEST_ASSERT_EQUAL_UINT16(60, ev.seq);

    /* mismatched seq/dst: not the in-flight entry, no-op */
    ring_hdr_t mismatch = { .src = 0, .dst = 9, .type = RING_T_CMD, .flags = 0, .ttl = 1, .len = 0, .seq = 999 };
    TEST_ASSERT_EQUAL_INT(0, ring_trk_unclaimed(&t, &mismatch, &ev));

    ring_hdr_t returned = { .src = 0, .dst = 4, .type = RING_T_CMD, .flags = RING_F_ACK_REQ, .ttl = 1, .len = 0, .seq = 60 };
    TEST_ASSERT_EQUAL_INT(1, ring_trk_unclaimed(&t, &returned, &ev));
    TEST_ASSERT_EQUAL_INT(RING_TRK_EV_FAIL, ev.kind);
    TEST_ASSERT_EQUAL_STRING("ZONE_UNKNOWN", ev.fail_token);
    TEST_ASSERT_EQUAL_UINT16(60, ev.seq);

    TEST_ASSERT_EQUAL_INT(1, ring_trk_tick(&t, 0, 5, &ev));
    TEST_ASSERT_EQUAL_INT(RING_TRK_EV_SEND, ev.kind);
    TEST_ASSERT_EQUAL_UINT16(61, ev.seq);
}

static void test_unclaimed_before_any_send_is_noop(void) {
    ring_trk_t t; ring_trk_init(&t);
    ring_hdr_t h1 = { .src = 0, .dst = 4, .type = RING_T_CMD, .flags = RING_F_ACK_REQ, .ttl = 16, .len = 0, .seq = 65 };
    TEST_ASSERT_EQUAL_INT(0, ring_trk_submit(&t, &h1, NULL, 0));

    /* nothing sent yet (attempts == 0): even a matching seq/dst is not "in flight" */
    ring_trk_ev_t ev;
    TEST_ASSERT_EQUAL_INT(0, ring_trk_unclaimed(&t, &h1, &ev));
}

/* ========== Queue full ========== */

static void test_queue_full_returns_minus1(void) {
    ring_trk_t t; ring_trk_init(&t);
    for (int i = 0; i < RING_TRK_DEPTH; i++) {
        ring_hdr_t h = { .src = 0, .dst = 1, .type = RING_T_CFG_GET, .flags = RING_F_ACK_REQ,
                          .ttl = 16, .len = 0, .seq = (uint16_t)(100 + i) };
        TEST_ASSERT_EQUAL_INT(0, ring_trk_submit(&t, &h, NULL, 0));
    }
    ring_hdr_t hx = { .src = 0, .dst = 1, .type = RING_T_CFG_GET, .flags = RING_F_ACK_REQ, .ttl = 16, .len = 0, .seq = 999 };
    TEST_ASSERT_EQUAL_INT(-1, ring_trk_submit(&t, &hx, NULL, 0));

    /* also full for a priority-inserted CMD -- depth is a hard cap regardless of insert position */
    ring_hdr_t hcmd = { .src = 0, .dst = 1, .type = RING_T_CMD, .flags = RING_F_ACK_REQ, .ttl = 16, .len = 0, .seq = 998 };
    TEST_ASSERT_EQUAL_INT(-1, ring_trk_submit(&t, &hcmd, NULL, 0));
}

/* ========== Detail preserved verbatim, <= 125 bytes + NUL ========== */

static void test_ack_detail_preserved_at_125_bytes(void) {
    ring_trk_t t; ring_trk_init(&t);
    ring_hdr_t h = { .src = 0, .dst = 1, .type = RING_T_CMD, .flags = RING_F_ACK_REQ, .ttl = 16, .len = 0, .seq = 70 };
    ring_trk_submit(&t, &h, NULL, 0);
    ring_trk_ev_t ev;
    ring_trk_tick(&t, 0, 5, &ev);

    char msg[125];
    memset(msg, 'A', sizeof msg);
    TEST_ASSERT_EQUAL_INT(1, ring_trk_ack(&t, 70, 3, msg, sizeof msg, &ev));
    TEST_ASSERT_EQUAL_UINT32(125, (uint32_t)strlen(ev.detail));
    TEST_ASSERT_EQUAL_UINT8('A', (uint8_t)ev.detail[124]);
    TEST_ASSERT_EQUAL_UINT8(0, (uint8_t)ev.detail[125]);
    TEST_ASSERT_EQUAL_UINT8(3, ev.status);
}

static void test_ack_detail_truncated_beyond_125_bytes(void) {
    ring_trk_t t; ring_trk_init(&t);
    ring_hdr_t h = { .src = 0, .dst = 1, .type = RING_T_CMD, .flags = RING_F_ACK_REQ, .ttl = 16, .len = 0, .seq = 71 };
    ring_trk_submit(&t, &h, NULL, 0);
    ring_trk_ev_t ev;
    ring_trk_tick(&t, 0, 5, &ev);

    char msg[200];
    memset(msg, 'B', sizeof msg);
    TEST_ASSERT_EQUAL_INT(1, ring_trk_ack(&t, 71, 0, msg, sizeof msg, &ev));
    TEST_ASSERT_EQUAL_UINT32(125, (uint32_t)strlen(ev.detail));
}

/* ========== submit() encodes once; the SEND wire matches a direct ring_frame_encode() call ========== */

static void test_send_wire_matches_direct_encode(void) {
    ring_trk_t t; ring_trk_init(&t);
    ring_hdr_t h = { .src = 0, .dst = 2, .type = RING_T_CMD, .flags = RING_F_ACK_REQ, .ttl = 16, .len = 6, .seq = 1042 };
    TEST_ASSERT_EQUAL_INT(0, ring_trk_submit(&t, &h, (const uint8_t *)"GET ID", 0));

    uint8_t expect[RING_WIRE_MAX];
    int elen = ring_frame_encode(&h, (const uint8_t *)"GET ID", expect, sizeof expect);
    TEST_ASSERT_TRUE(elen > 0);

    ring_trk_ev_t ev;
    TEST_ASSERT_EQUAL_INT(1, ring_trk_tick(&t, 0, 5, &ev));
    TEST_ASSERT_EQUAL_INT(elen, (int)ev.wire_len);
    TEST_ASSERT_EQUAL_MEMORY(expect, ev.wire, (size_t)elen);
    TEST_ASSERT_EQUAL_UINT8(2, ev.dst);
    TEST_ASSERT_EQUAL_UINT8(RING_T_CMD, ev.type);
}

/* ========== cancel(): withdraws a QUEUED entry, never an in-flight one ========== */

static void test_cancel_queued_entry_removed_order_kept(void) {
    ring_trk_t t; ring_trk_init(&t);
    /* head in flight (a CMD), then two queued CFG entries -- exactly the §4.4
       revert-race shape: the COMMIT sits behind the forwarded CMD. */
    ring_hdr_t hm = { .src = 0, .dst = 1, .type = RING_T_CMD,        .flags = RING_F_ACK_REQ, .ttl = 16, .len = 0, .seq = 80 };
    ring_hdr_t hc = { .src = 0, .dst = 2, .type = RING_T_CFG_COMMIT, .flags = RING_F_ACK_REQ, .ttl = 16, .len = 0, .seq = 81 };
    ring_hdr_t hg = { .src = 0, .dst = 3, .type = RING_T_CFG_GET,    .flags = RING_F_ACK_REQ, .ttl = 16, .len = 0, .seq = 82 };
    ring_trk_submit(&t, &hm, NULL, 0);
    ring_trk_submit(&t, &hc, NULL, 0);
    ring_trk_submit(&t, &hg, NULL, 0);

    ring_trk_ev_t ev;
    TEST_ASSERT_EQUAL_INT(1, ring_trk_tick(&t, 0, 8, &ev));      /* CMD 80 goes out */
    TEST_ASSERT_EQUAL_UINT16(80, ev.seq);

    TEST_ASSERT_EQUAL_INT(0, ring_trk_cancel(&t, 81));           /* withdraw the queued COMMIT */
    TEST_ASSERT_EQUAL_UINT8(2, t.n);

    TEST_ASSERT_EQUAL_INT(1, ring_trk_ack(&t, 80, 0, "OK", 2, &ev));
    TEST_ASSERT_EQUAL_INT(1, ring_trk_tick(&t, 1, 8, &ev));      /* next is 82, not the cancelled 81 */
    TEST_ASSERT_EQUAL_UINT16(82, ev.seq);
}

static void test_cancel_inflight_entry_refused(void) {
    ring_trk_t t; ring_trk_init(&t);
    ring_hdr_t hc = { .src = 0, .dst = 2, .type = RING_T_CFG_COMMIT, .flags = RING_F_ACK_REQ, .ttl = 16, .len = 0, .seq = 90 };
    ring_trk_submit(&t, &hc, NULL, 0);
    ring_trk_ev_t ev;
    TEST_ASSERT_EQUAL_INT(1, ring_trk_tick(&t, 0, 8, &ev));      /* now in flight */

    TEST_ASSERT_EQUAL_INT(-1, ring_trk_cancel(&t, 90));
    TEST_ASSERT_EQUAL_UINT8(1, t.n);                              /* untouched: its ACK is still coming */
    TEST_ASSERT_EQUAL_INT(1, ring_trk_ack(&t, 90, 0, "OK", 2, &ev));
}

static void test_cancel_unknown_seq_returns_minus1(void) {
    ring_trk_t t; ring_trk_init(&t);
    TEST_ASSERT_EQUAL_INT(-1, ring_trk_cancel(&t, 7));            /* empty queue */
    ring_hdr_t hc = { .src = 0, .dst = 2, .type = RING_T_CFG_COMMIT, .flags = RING_F_ACK_REQ, .ttl = 16, .len = 0, .seq = 91 };
    ring_trk_submit(&t, &hc, NULL, 0);
    TEST_ASSERT_EQUAL_INT(-1, ring_trk_cancel(&t, 7));
    TEST_ASSERT_EQUAL_UINT8(1, t.n);
}

void setUp(void) {}
void tearDown(void) {}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_ack_timeout_formula_clamp_points);
    RUN_TEST(test_single_inflight_two_cmds);
    RUN_TEST(test_priority_insert_cmd_before_cfg_commit);
    RUN_TEST(test_priority_insert_fw_update_before_cfg_get);
    RUN_TEST(test_priority_insert_never_preempts_inflight);
    RUN_TEST(test_priority_insert_multiple_cmds_group_before_cfg);
    RUN_TEST(test_retry_ladder_and_timeout_fail);
    RUN_TEST(test_late_ack_after_fail_returns_zero);
    RUN_TEST(test_ack_wrong_seq_returns_zero);
    RUN_TEST(test_unclaimed_immediate_fail_and_next_proceeds);
    RUN_TEST(test_unclaimed_before_any_send_is_noop);
    RUN_TEST(test_queue_full_returns_minus1);
    RUN_TEST(test_ack_detail_preserved_at_125_bytes);
    RUN_TEST(test_ack_detail_truncated_beyond_125_bytes);
    RUN_TEST(test_send_wire_matches_direct_encode);
    RUN_TEST(test_cancel_queued_entry_removed_order_kept);
    RUN_TEST(test_cancel_inflight_entry_refused);
    RUN_TEST(test_cancel_unknown_seq_returns_minus1);
    return UNITY_END();
}
