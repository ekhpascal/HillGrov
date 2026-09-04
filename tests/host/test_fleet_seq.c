#include <string.h>
#include "unity.h"
#include "ring_proto.h"
#include "node_mgr_internal.h"

/* Pure fleet_seq.c host tests (Task 15 + fix round 1): fake events drive
   fleet_start/fleet_tick/fleet_on_ack/fleet_note_submitted directly -- no
   IDF, no node_mgr glue linked in (this links only fleet_seq.c, see
   CMakeLists). */

static const uint8_t FW_A[3] = { 1, 2, 3 };
static const uint8_t FW_B[3] = { 1, 2, 4 };

void setUp(void) {}
void tearDown(void) {}

/* Drives a zone through PRECHECK(pass)->FA_START->submitted ok, recording
   ack_ms = 1000 (fleet_on_ack's now_ms) as the WAIT_HB freshness baseline. */
static void begin_zone(fleet_t *s, uint32_t now, const uint8_t pre_fw[3]) {
    fleet_act_t act;
    TEST_ASSERT_EQUAL_INT(1, fleet_tick(s, now, 1, 1, 0, NULL, 0, 0, &act));
    TEST_ASSERT_EQUAL_INT(FA_START, act.kind);
    TEST_ASSERT_EQUAL_INT(0, fleet_note_submitted(s, 1, 1, now, pre_fw, &act));
    TEST_ASSERT_EQUAL_INT(0, fleet_on_ack(s, 1, 1, now, &act));   /* -> WAIT_HB, ack_ms = now */
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
    TEST_ASSERT_EQUAL_INT(1, fleet_note_submitted(&s, /*submitted*/0, 0, 1000, FW_A, &act));
    TEST_ASSERT_EQUAL_INT(FA_FAILED, act.kind);
    TEST_ASSERT_EQUAL_UINT8(4, act.zone);
}

/* ---- single zone: ACK failure (ZONE_TIMEOUT/ZONE_UNKNOWN) ---- */

static void test_ack_failure_fails_zone(void) {
    fleet_t s; fleet_init(&s);
    uint8_t zones[1] = { 1 };
    fleet_start(&s, zones, 1);
    fleet_act_t act;
    TEST_ASSERT_EQUAL_INT(1, fleet_tick(&s, 1000, 1, 1, 0, NULL, 0, 0, &act));
    TEST_ASSERT_EQUAL_INT(0, fleet_note_submitted(&s, 1, 1, 1000, FW_A, &act));

    TEST_ASSERT_EQUAL_INT(1, fleet_on_ack(&s, 1, /*ok*/0, 1000, &act));
    TEST_ASSERT_EQUAL_INT(FA_FAILED, act.kind);
    TEST_ASSERT_EQUAL_UINT8(1, act.zone);
}

/* a stale/foreign seq must not touch the sequence */
static void test_ack_stale_seq_ignored(void) {
    fleet_t s; fleet_init(&s);
    uint8_t zones[1] = { 1 };
    fleet_start(&s, zones, 1);
    fleet_act_t act;
    fleet_tick(&s, 1000, 1, 1, 0, NULL, 0, 0, &act);
    fleet_note_submitted(&s, 1, 1, 1000, FW_A, &act);

    TEST_ASSERT_EQUAL_INT(0, fleet_on_ack(&s, 99, 1, 1000, &act));   /* not our seq */
    TEST_ASSERT_EQUAL_INT(1, s.active);
}

/* ---- single zone: WAIT_HB success paths ---- */

static void test_wait_hb_success_on_fw_change(void) {
    fleet_t s; fleet_init(&s);
    uint8_t zones[1] = { 5 };
    fleet_start(&s, zones, 1);
    begin_zone(&s, 1000, FW_A);   /* ack_ms = 1000 */
    fleet_act_t act;

    /* still the old fw, and not fresh (arrived at/ before the ack) -> not done yet */
    TEST_ASSERT_EQUAL_INT(0, fleet_tick(&s, 2000, 1, 1, 1, FW_A, 500, 1000, &act));

    /* new fw triple, arrived AFTER the ack */
    TEST_ASSERT_EQUAL_INT(1, fleet_tick(&s, 3000, 1, 1, 1, FW_B, 500, 2500, &act));
    TEST_ASSERT_EQUAL_INT(FA_DONE, act.kind);
    TEST_ASSERT_EQUAL_UINT8(5, act.zone);
    TEST_ASSERT_EQUAL_UINT8(1, act.fw[0]); TEST_ASSERT_EQUAL_UINT8(2, act.fw[1]); TEST_ASSERT_EQUAL_UINT8(4, act.fw[2]);
    TEST_ASSERT_EQUAL_INT(0, s.active);   /* single zone: sequence ends */
}

/* Fix round ruling: same-version deploy (T16 bench reflashes the identical
   build, so the fw triple never changes) -- success must come from a
   FRESH low-uptime HB alone, without needing the fw triple to differ. */
static void test_wait_hb_success_same_version_fresh_low_uptime(void) {
    fleet_t s; fleet_init(&s);
    uint8_t zones[1] = { 6 };
    fleet_start(&s, zones, 1);
    begin_zone(&s, 1000, FW_A);   /* ack_ms = 1000 */
    fleet_act_t act;

    /* fresh HB, same fw, but uptime still >= 60 s -- not done yet */
    TEST_ASSERT_EQUAL_INT(0, fleet_tick(&s, 2000, 1, 1, 1, FW_A, 61, 1500, &act));

    /* fresh HB (arrived after ack_ms=1000), same fw, uptime < 60 s -> done */
    TEST_ASSERT_EQUAL_INT(1, fleet_tick(&s, 3000, 1, 1, 1, FW_A, 8, 2500, &act));
    TEST_ASSERT_EQUAL_INT(FA_DONE, act.kind);
    TEST_ASSERT_EQUAL_UINT8(6, act.zone);
}

/* The freshness gate this predicate depends on: a low-uptime HB that never
   actually arrived after the ack (still the stale pre-update snapshot --
   e.g. the zone had already rebooted for an unrelated reason moments
   before this update was even triggered) must NOT read as success. */
static void test_wait_hb_stale_hb_not_fresh_never_succeeds(void) {
    fleet_t s; fleet_init(&s);
    uint8_t zones[1] = { 6 };
    fleet_start(&s, zones, 1);
    begin_zone(&s, 1000, FW_A);   /* ack_ms = 1000 */
    fleet_act_t act;

    /* same fw, uptime already < 60 -- but arrived_ms == ack_ms (not fresh) */
    TEST_ASSERT_EQUAL_INT(0, fleet_tick(&s, 2000, 1, 1, 1, FW_A, 8, 1000, &act));
    /* arrived_ms BEFORE ack_ms (even more clearly stale) */
    TEST_ASSERT_EQUAL_INT(0, fleet_tick(&s, 2100, 1, 1, 1, FW_A, 8, 500, &act));
}

/* a zone cleared mid-WAIT_HB (hb_valid=0) must never look like success */
static void test_wait_hb_invalid_hb_never_succeeds(void) {
    fleet_t s; fleet_init(&s);
    uint8_t zones[1] = { 6 };
    fleet_start(&s, zones, 1);
    begin_zone(&s, 1000, FW_A);
    fleet_act_t act;

    TEST_ASSERT_EQUAL_INT(0, fleet_tick(&s, 2000, 1, 0, /*hb_valid*/0, FW_B, 5, 2500, &act));
}

/* ---- single zone: WAIT_HB timeout ---- */

static void test_wait_hb_timeout_fails(void) {
    fleet_t s; fleet_init(&s);
    uint8_t zones[1] = { 7 };
    fleet_start(&s, zones, 1);
    begin_zone(&s, 1000, FW_A);   /* deadline = 1000 + 180000 = 181000 */
    fleet_act_t act;

    TEST_ASSERT_EQUAL_INT(0, fleet_tick(&s, 180999, 1, 1, 1, FW_A, 500, 1000, &act));   /* not yet */
    TEST_ASSERT_EQUAL_INT(1, fleet_tick(&s, 181000, 1, 1, 1, FW_A, 500, 1000, &act));   /* deadline reached */
    TEST_ASSERT_EQUAL_INT(FA_FAILED, act.kind);
    TEST_ASSERT_EQUAL_UINT8(7, act.zone);
}

/* ---- multi-zone fw_all(): ascending, stop on first failure ---- */

static void test_multi_zone_advances_on_success(void) {
    fleet_t s; fleet_init(&s);
    uint8_t zones[3] = { 1, 3, 5 };
    fleet_start(&s, zones, 3);

    begin_zone(&s, 1000, FW_A);
    fleet_act_t act;
    TEST_ASSERT_EQUAL_INT(1, fleet_tick(&s, 2000, 1, 1, 1, FW_B, 10, 1500, &act));
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

/* ---- fleet_start rejects a second concurrent run (fix round: -2, not -1) ---- */

static void test_start_rejects_while_active_with_busy_rc(void) {
    fleet_t s; fleet_init(&s);
    uint8_t zones[1] = { 1 };
    TEST_ASSERT_EQUAL_INT(0, fleet_start(&s, zones, 1));
    TEST_ASSERT_EQUAL_INT(-2, fleet_start(&s, zones, 1));   /* busy, distinguishable from invalid */
}

static void test_start_rejects_invalid_zone_count(void) {
    fleet_t s; fleet_init(&s);
    uint8_t zones[1] = { 1 };
    TEST_ASSERT_EQUAL_INT(-1, fleet_start(&s, zones, 0));            /* invalid: no zones */
    TEST_ASSERT_EQUAL_INT(-1, fleet_start(&s, zones, HG_MAX_ZONES + 1)); /* invalid: too many */
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
    begin_zone(&s, 1000, FW_A);

    fleet_cancel(&s);
    TEST_ASSERT_EQUAL_INT(1, s.active);   /* zone 1 still finishing */

    fleet_act_t act;
    TEST_ASSERT_EQUAL_INT(1, fleet_tick(&s, 2000, 1, 1, 1, FW_B, 10, 1500, &act));
    TEST_ASSERT_EQUAL_INT(FA_DONE, act.kind);
    TEST_ASSERT_EQUAL_UINT8(1, act.zone);
    TEST_ASSERT_EQUAL_INT(0, s.active);   /* cancelled: zone 2 never starts */
}

/* ---- fleet_status (fix round: no leading "ZONE" word -- caller prefixes) ---- */

static void test_status_rendering(void) {
    fleet_t s; fleet_init(&s);
    char buf[32];
    fleet_status(&s, buf, sizeof buf);
    TEST_ASSERT_EQUAL_STRING("IDLE", buf);

    uint8_t zones[1] = { 4 };
    fleet_start(&s, zones, 1);
    fleet_status(&s, buf, sizeof buf);
    TEST_ASSERT_EQUAL_STRING("4 PRECHECK", buf);

    fleet_act_t act;
    fleet_tick(&s, 1000, 1, 1, 0, NULL, 0, 0, &act);
    fleet_note_submitted(&s, 1, 1, 1000, FW_A, &act);
    fleet_status(&s, buf, sizeof buf);
    TEST_ASSERT_EQUAL_STRING("4 UPDATING", buf);

    fleet_on_ack(&s, 1, 1, 1000, &act);
    fleet_status(&s, buf, sizeof buf);
    TEST_ASSERT_EQUAL_STRING("4 WAIT_HB", buf);
}

int main(void) { UNITY_BEGIN();
    RUN_TEST(test_precheck_fails_bad_image);
    RUN_TEST(test_precheck_fails_target_offline);
    RUN_TEST(test_submit_failure_fails_zone);
    RUN_TEST(test_ack_failure_fails_zone);
    RUN_TEST(test_ack_stale_seq_ignored);
    RUN_TEST(test_wait_hb_success_on_fw_change);
    RUN_TEST(test_wait_hb_success_same_version_fresh_low_uptime);
    RUN_TEST(test_wait_hb_stale_hb_not_fresh_never_succeeds);
    RUN_TEST(test_wait_hb_invalid_hb_never_succeeds);
    RUN_TEST(test_wait_hb_timeout_fails);
    RUN_TEST(test_multi_zone_advances_on_success);
    RUN_TEST(test_multi_zone_stops_on_first_failure);
    RUN_TEST(test_start_rejects_while_active_with_busy_rc);
    RUN_TEST(test_start_rejects_invalid_zone_count);
    RUN_TEST(test_cancel_during_precheck_stops_immediately);
    RUN_TEST(test_cancel_during_wait_hb_lets_current_zone_finish_then_stops);
    RUN_TEST(test_status_rendering);
    return UNITY_END(); }
