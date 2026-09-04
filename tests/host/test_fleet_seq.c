#include <string.h>
#include "unity.h"
#include "ring_proto.h"
#include "node_mgr_internal.h"

/* Pure fleet_seq.c host tests (Task 15): fake events drive fleet_start/
 * fleet_tick/fleet_on_ack/fleet_note_submitted directly -- no IDF, no
 * node_mgr glue linked in (this links only fleet_seq.c, see CMakeLists). */

static const uint8_t FW_A[3] = { 1, 2, 3 };
static const uint8_t FW_B[3] = { 1, 2, 4 };

void setUp(void) {}
void tearDown(void) {}

/* Drives a zone through PRECHECK(pass)->FA_START->submitted ok. Returns the
   seq fleet_note_submitted recorded (always 1 in these tests). */
static void begin_zone(fleet_t *s, uint32_t now, const uint8_t pre_fw[3], uint32_t pre_uptime, uint32_t pre_cfg_gen) {
    fleet_act_t act;
    TEST_ASSERT_EQUAL_INT(1, fleet_tick(s, now, 1, 1, 0, NULL, 0, 0, &act));
    TEST_ASSERT_EQUAL_INT(FA_START, act.kind);
    TEST_ASSERT_EQUAL_INT(0, fleet_note_submitted(s, 1, 1, now, pre_fw, pre_uptime, pre_cfg_gen, &act));
}

/* ---- single zone: PRECHECK failures ---- */

static void test_precheck_fails_bad_image(void) {
    fleet_t s; fleet_init(&s);
    uint8_t zones[1] = { 3 };
    TEST_ASSERT_EQUAL_INT(0, fleet_start(&s, zones, 1));

    fleet_act_t act;
    TEST_ASSERT_EQUAL_INT(1, fleet_tick(&s, 1000, /*image_ok*/0, /*online*/1, 0, NULL, 0, 0, &act));
    TEST_ASSERT_EQUAL_INT(FA_FAILED, act.kind);
    TEST_ASSERT_EQUAL_UINT8(3, act.zone);
    TEST_ASSERT_EQUAL_INT(0, fleet_tick(&s, 2000, 1, 1, 0, NULL, 0, 0, &act));   /* sequence is over */
}

static void test_precheck_fails_target_offline(void) {
    fleet_t s; fleet_init(&s);
    uint8_t zones[1] = { 2 };
    fleet_start(&s, zones, 1);

    fleet_act_t act;
    TEST_ASSERT_EQUAL_INT(1, fleet_tick(&s, 1000, 1, /*online*/0, 0, NULL, 0, 0, &act));
    TEST_ASSERT_EQUAL_INT(FA_FAILED, act.kind);
    TEST_ASSERT_EQUAL_UINT8(2, act.zone);
}

/* ---- single zone: submit (tracker) failure ---- */

static void test_submit_failure_fails_zone(void) {
    fleet_t s; fleet_init(&s);
    uint8_t zones[1] = { 4 };
    fleet_start(&s, zones, 1);

    fleet_act_t act;
    TEST_ASSERT_EQUAL_INT(1, fleet_tick(&s, 1000, 1, 1, 0, NULL, 0, 0, &act));
    TEST_ASSERT_EQUAL_INT(FA_START, act.kind);

    /* glue's nmgr_submit() reported failure (tracker full/busy) */
    TEST_ASSERT_EQUAL_INT(1, fleet_note_submitted(&s, /*submitted*/0, 0, 1000, FW_A, 100, 5, &act));
    TEST_ASSERT_EQUAL_INT(FA_FAILED, act.kind);
    TEST_ASSERT_EQUAL_UINT8(4, act.zone);
}

/* ---- single zone: ACK failure (ZONE_TIMEOUT/ZONE_UNKNOWN) ---- */

static void test_ack_failure_fails_zone(void) {
    fleet_t s; fleet_init(&s);
    uint8_t zones[1] = { 1 };
    fleet_start(&s, zones, 1);
    begin_zone(&s, 1000, FW_A, 500, 9);

    fleet_act_t act;
    TEST_ASSERT_EQUAL_INT(1, fleet_on_ack(&s, 1, /*ok*/0, &act));
    TEST_ASSERT_EQUAL_INT(FA_FAILED, act.kind);
    TEST_ASSERT_EQUAL_UINT8(1, act.zone);
}

/* a stale/foreign seq must not touch the sequence */
static void test_ack_stale_seq_ignored(void) {
    fleet_t s; fleet_init(&s);
    uint8_t zones[1] = { 1 };
    fleet_start(&s, zones, 1);
    begin_zone(&s, 1000, FW_A, 500, 9);

    fleet_act_t act;
    TEST_ASSERT_EQUAL_INT(0, fleet_on_ack(&s, 99, 1, &act));   /* not our seq */
    TEST_ASSERT_EQUAL_INT(1, s.active);
}

/* ---- single zone: WAIT_HB success paths ---- */

static void test_wait_hb_success_on_fw_change(void) {
    fleet_t s; fleet_init(&s);
    uint8_t zones[1] = { 5 };
    fleet_start(&s, zones, 1);
    begin_zone(&s, 1000, FW_A, 500, 9);
    fleet_act_t act;
    fleet_on_ack(&s, 1, 1, &act);   /* -> WAIT_HB */

    /* still the old fw -> not done yet */
    TEST_ASSERT_EQUAL_INT(0, fleet_tick(&s, 2000, 1, 1, 1, FW_A, 500, 9, &act));

    /* new fw triple arrives */
    TEST_ASSERT_EQUAL_INT(1, fleet_tick(&s, 3000, 1, 1, 1, FW_B, 10, 9, &act));
    TEST_ASSERT_EQUAL_INT(FA_DONE, act.kind);
    TEST_ASSERT_EQUAL_UINT8(5, act.zone);
    TEST_ASSERT_EQUAL_UINT8(1, act.fw[0]); TEST_ASSERT_EQUAL_UINT8(2, act.fw[1]); TEST_ASSERT_EQUAL_UINT8(4, act.fw[2]);
    TEST_ASSERT_EQUAL_INT(0, s.active);   /* single zone: sequence ends */
}

static void test_wait_hb_success_on_uptime_reset_same_fw(void) {
    fleet_t s; fleet_init(&s);
    uint8_t zones[1] = { 6 };
    fleet_start(&s, zones, 1);
    begin_zone(&s, 1000, FW_A, 50000, 9);   /* pre_uptime 50000 s, cfg gen 9 */
    fleet_act_t act;
    fleet_on_ack(&s, 1, 1, &act);

    /* same fw triple, uptime NOT reset yet -> not done */
    TEST_ASSERT_EQUAL_INT(0, fleet_tick(&s, 2000, 1, 1, 1, FW_A, 50010, 9, &act));

    /* same fw triple, uptime reset (reboot) + cfg gen unchanged -> done */
    TEST_ASSERT_EQUAL_INT(1, fleet_tick(&s, 3000, 1, 1, 1, FW_A, 12, 9, &act));
    TEST_ASSERT_EQUAL_INT(FA_DONE, act.kind);
}

static void test_wait_hb_uptime_reset_but_cfg_changed_not_success(void) {
    fleet_t s; fleet_init(&s);
    uint8_t zones[1] = { 6 };
    fleet_start(&s, zones, 1);
    begin_zone(&s, 1000, FW_A, 50000, 9);
    fleet_act_t act;
    fleet_on_ack(&s, 1, 1, &act);

    /* uptime reset but cfg gen differs -- not the "cfg intact" case: not done */
    TEST_ASSERT_EQUAL_INT(0, fleet_tick(&s, 2000, 1, 1, 1, FW_A, 5, 10, &act));
}

/* a zone cleared mid-WAIT_HB (hb_valid=0) must never look like success */
static void test_wait_hb_invalid_hb_never_succeeds(void) {
    fleet_t s; fleet_init(&s);
    uint8_t zones[1] = { 6 };
    fleet_start(&s, zones, 1);
    begin_zone(&s, 1000, FW_A, 500, 9);
    fleet_act_t act;
    fleet_on_ack(&s, 1, 1, &act);

    TEST_ASSERT_EQUAL_INT(0, fleet_tick(&s, 2000, 1, 0, /*hb_valid*/0, FW_B, 0, 0, &act));
}

/* ---- single zone: WAIT_HB timeout ---- */

static void test_wait_hb_timeout_fails(void) {
    fleet_t s; fleet_init(&s);
    uint8_t zones[1] = { 7 };
    fleet_start(&s, zones, 1);
    begin_zone(&s, 1000, FW_A, 500, 9);   /* deadline = 1000 + 180000 = 181000 */
    fleet_act_t act;
    fleet_on_ack(&s, 1, 1, &act);

    TEST_ASSERT_EQUAL_INT(0, fleet_tick(&s, 180999, 1, 1, 1, FW_A, 500, 9, &act));   /* not yet */
    TEST_ASSERT_EQUAL_INT(1, fleet_tick(&s, 181000, 1, 1, 1, FW_A, 500, 9, &act));   /* deadline reached */
    TEST_ASSERT_EQUAL_INT(FA_FAILED, act.kind);
    TEST_ASSERT_EQUAL_UINT8(7, act.zone);
}

/* ---- multi-zone fw_all(): ascending, stop on first failure ---- */

static void test_multi_zone_advances_on_success(void) {
    fleet_t s; fleet_init(&s);
    uint8_t zones[3] = { 1, 3, 5 };
    fleet_start(&s, zones, 3);

    begin_zone(&s, 1000, FW_A, 500, 9);
    fleet_act_t act;
    fleet_on_ack(&s, 1, 1, &act);
    TEST_ASSERT_EQUAL_INT(1, fleet_tick(&s, 2000, 1, 1, 1, FW_B, 10, 9, &act));
    TEST_ASSERT_EQUAL_INT(FA_DONE, act.kind);
    TEST_ASSERT_EQUAL_UINT8(1, act.zone);
    TEST_ASSERT_EQUAL_INT(1, s.active);   /* more zones queued */

    /* next zone's PRECHECK runs on the very next tick */
    TEST_ASSERT_EQUAL_INT(1, fleet_tick(&s, 2100, 1, 1, 0, NULL, 0, 0, &act));
    TEST_ASSERT_EQUAL_INT(FA_START, act.kind);
    TEST_ASSERT_EQUAL_UINT8(3, act.zone);
}

static void test_multi_zone_stops_on_first_failure(void) {
    fleet_t s; fleet_init(&s);
    uint8_t zones[3] = { 1, 3, 5 };
    fleet_start(&s, zones, 3);

    fleet_act_t act;
    /* zone 1 fails PRECHECK (not online) */
    TEST_ASSERT_EQUAL_INT(1, fleet_tick(&s, 1000, 1, 0, 0, NULL, 0, 0, &act));
    TEST_ASSERT_EQUAL_INT(FA_FAILED, act.kind);
    TEST_ASSERT_EQUAL_UINT8(1, act.zone);
    TEST_ASSERT_EQUAL_INT(0, s.active);   /* zones 3 and 5 never run */

    TEST_ASSERT_EQUAL_INT(0, fleet_tick(&s, 2000, 1, 1, 0, NULL, 0, 0, &act));
}

/* ---- fleet_start rejects a second concurrent run ---- */

static void test_start_rejects_while_active(void) {
    fleet_t s; fleet_init(&s);
    uint8_t zones[1] = { 1 };
    TEST_ASSERT_EQUAL_INT(0, fleet_start(&s, zones, 1));
    TEST_ASSERT_EQUAL_INT(-1, fleet_start(&s, zones, 1));   /* FW_BUSY-shaped: rejected outright */
}

/* ---- fleet_cancel: PRECHECK stops immediately; in-flight finishes on its own ---- */

static void test_cancel_during_precheck_stops_immediately(void) {
    fleet_t s; fleet_init(&s);
    uint8_t zones[2] = { 1, 2 };
    fleet_start(&s, zones, 2);
    fleet_cancel(&s);
    TEST_ASSERT_EQUAL_INT(0, s.active);
}

static void test_cancel_during_wait_hb_lets_current_zone_finish_then_stops(void) {
    fleet_t s; fleet_init(&s);
    uint8_t zones[2] = { 1, 2 };
    fleet_start(&s, zones, 2);
    begin_zone(&s, 1000, FW_A, 500, 9);
    fleet_act_t act;
    fleet_on_ack(&s, 1, 1, &act);

    fleet_cancel(&s);
    TEST_ASSERT_EQUAL_INT(1, s.active);   /* zone 1 still finishing */

    TEST_ASSERT_EQUAL_INT(1, fleet_tick(&s, 2000, 1, 1, 1, FW_B, 10, 9, &act));
    TEST_ASSERT_EQUAL_INT(FA_DONE, act.kind);
    TEST_ASSERT_EQUAL_UINT8(1, act.zone);
    TEST_ASSERT_EQUAL_INT(0, s.active);   /* cancelled: zone 2 never starts */
}

/* ---- fleet_status ---- */

static void test_status_rendering(void) {
    fleet_t s; fleet_init(&s);
    char buf[32];
    fleet_status(&s, buf, sizeof buf);
    TEST_ASSERT_EQUAL_STRING("IDLE", buf);

    uint8_t zones[1] = { 4 };
    fleet_start(&s, zones, 1);
    fleet_status(&s, buf, sizeof buf);
    TEST_ASSERT_EQUAL_STRING("ZONE 4 PRECHECK", buf);

    begin_zone(&s, 1000, FW_A, 500, 9);
    fleet_status(&s, buf, sizeof buf);
    TEST_ASSERT_EQUAL_STRING("ZONE 4 UPDATING", buf);

    fleet_act_t act;
    fleet_on_ack(&s, 1, 1, &act);
    fleet_status(&s, buf, sizeof buf);
    TEST_ASSERT_EQUAL_STRING("ZONE 4 WAIT_HB", buf);
}

int main(void) { UNITY_BEGIN();
    RUN_TEST(test_precheck_fails_bad_image);
    RUN_TEST(test_precheck_fails_target_offline);
    RUN_TEST(test_submit_failure_fails_zone);
    RUN_TEST(test_ack_failure_fails_zone);
    RUN_TEST(test_ack_stale_seq_ignored);
    RUN_TEST(test_wait_hb_success_on_fw_change);
    RUN_TEST(test_wait_hb_success_on_uptime_reset_same_fw);
    RUN_TEST(test_wait_hb_uptime_reset_but_cfg_changed_not_success);
    RUN_TEST(test_wait_hb_invalid_hb_never_succeeds);
    RUN_TEST(test_wait_hb_timeout_fails);
    RUN_TEST(test_multi_zone_advances_on_success);
    RUN_TEST(test_multi_zone_stops_on_first_failure);
    RUN_TEST(test_start_rejects_while_active);
    RUN_TEST(test_cancel_during_precheck_stops_immediately);
    RUN_TEST(test_cancel_during_wait_hb_lets_current_zone_finish_then_stops);
    RUN_TEST(test_status_rendering);
    return UNITY_END(); }
