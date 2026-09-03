#include <string.h>
#include "unity.h"
#include "cmd_core.h"
#include "ota_trial.h"
#include "fake_clock.h"

/* ---- pure trial_eval() criteria matrix ---- */

static trial_probes_t base_probes(void) {
    trial_probes_t p;
    memset(&p, 0, sizeof p);
    p.cfg_loaded = 1;
    p.drivers_ok = 1;
    p.ticks = TRIAL_MIN_TICKS;
    p.twdt_ok = 1;
    p.min_heap_kb = TRIAL_MIN_HEAP_KB + 1;
    return p;
}

static void test_bench_pass_at_60s(void) {
    trial_probes_t p = base_probes();
    TEST_ASSERT_EQUAL_INT(TRIAL_PASS, trial_eval(&p, 0, 0, TRIAL_BENCH_WINDOW_MS));
}

static void test_bench_pending_at_59_9s(void) {
    trial_probes_t p = base_probes();
    TEST_ASSERT_EQUAL_INT(TRIAL_PENDING, trial_eval(&p, 0, 0, TRIAL_BENCH_WINDOW_MS - 100));
}

static void test_fleet_pass_instant_with_frame(void) {
    trial_probes_t p = base_probes();
    p.master_frame_seen = 1;
    /* base met, frame seen, well inside the fleet window -- no time-based gate */
    TEST_ASSERT_EQUAL_INT(TRIAL_PASS, trial_eval(&p, 1, 0, 5000));
}

static void test_fleet_pending_without_frame_before_window(void) {
    trial_probes_t p = base_probes();
    TEST_ASSERT_EQUAL_INT(TRIAL_PENDING, trial_eval(&p, 1, 0, 5000));
}

static void test_fleet_fail_at_180s_without_frame(void) {
    trial_probes_t p = base_probes();
    TEST_ASSERT_EQUAL_INT(TRIAL_FAIL, trial_eval(&p, 1, 0, TRIAL_FLEET_WINDOW_MS));
}

static void test_confirm_short_circuits_base_unmet(void) {
    trial_probes_t p;
    memset(&p, 0, sizeof p);   /* nothing met at all */
    p.confirmed = 1;
    TEST_ASSERT_EQUAL_INT(TRIAL_PASS, trial_eval(&p, 1, 0, 0));
    TEST_ASSERT_EQUAL_INT(TRIAL_PASS, trial_eval(&p, 0, 0, 0));
}

/* Each base criterion individually gates PASS -- bench mode (expect_link=0) so
 * a stalled trial can only read PENDING, never FAIL, isolating the gate. */
static void test_base_gate_cfg_loaded(void) {
    trial_probes_t p = base_probes(); p.cfg_loaded = 0;
    TEST_ASSERT_EQUAL_INT(TRIAL_PENDING, trial_eval(&p, 0, 0, TRIAL_BENCH_WINDOW_MS));
}
static void test_base_gate_drivers_ok(void) {
    trial_probes_t p = base_probes(); p.drivers_ok = 0;
    TEST_ASSERT_EQUAL_INT(TRIAL_PENDING, trial_eval(&p, 0, 0, TRIAL_BENCH_WINDOW_MS));
}
static void test_base_gate_ticks(void) {
    trial_probes_t p = base_probes(); p.ticks = TRIAL_MIN_TICKS - 1;
    TEST_ASSERT_EQUAL_INT(TRIAL_PENDING, trial_eval(&p, 0, 0, TRIAL_BENCH_WINDOW_MS));
}
static void test_base_gate_twdt_ok(void) {
    trial_probes_t p = base_probes(); p.twdt_ok = 0;
    TEST_ASSERT_EQUAL_INT(TRIAL_PENDING, trial_eval(&p, 0, 0, TRIAL_BENCH_WINDOW_MS));
}
static void test_base_gate_min_heap(void) {
    trial_probes_t p = base_probes(); p.min_heap_kb = TRIAL_MIN_HEAP_KB;   /* boundary: '>', not '>=' */
    TEST_ASSERT_EQUAL_INT(TRIAL_PENDING, trial_eval(&p, 0, 0, TRIAL_BENCH_WINDOW_MS));
}

/* ---- SET OTA CONFIRM row grammar ---- */

/* ota_trial.c (the real target-side implementation) is excluded from this host
 * link; trial_cmds.c's h_confirm() still needs something to call, so this is
 * the test double for the target hook. */
static int s_confirm_rc = -1;
int ota_trial_confirm(void) { return s_confirm_rc; }

static cmd_core_t    core;
static cmd_session_t ses;
static char          resp[CMD_RESP_MAX];

static int run(const char *line) { return cmd_dispatch(&core, &ses, line, resp, sizeof resp); }

void setUp(void) {
    memset(&core, 0, sizeof core);
    core.table = OTA_TRIAL_ROWS;
    core.table_len = OTA_TRIAL_ROWS_N;
    core.role = CMD_ROLE_ZONE;
    core.zone_id = 1;
    core.now_ms = fake_clock_now;
    memset(&ses, 0, sizeof ses);
    ses.source = CMD_SRC_CLI;
    fake_clock_set(100000);
    s_confirm_rc = -1;
    resp[0] = '\0';
}
void tearDown(void) {}

static void test_table_valid(void) {
    TEST_ASSERT_EQUAL_INT(-1, cmd_table_check(OTA_TRIAL_ROWS, OTA_TRIAL_ROWS_N));
}

static void test_confirm_grammar_rejects_numeric(void) {
    /* single-entry enum ("CONFIRM") rejects the numeric fallback per SP1's fix --
     * "SET OTA 0" must not be treated as a confirmation. */
    TEST_ASSERT_EQUAL_INT(-1, run("SET OTA 0"));
    TEST_ASSERT_EQUAL_STRING("ERR BAD_ARGS\n", resp);
}

static void test_confirm_not_ready(void) {
    s_confirm_rc = -1;
    TEST_ASSERT_EQUAL_INT(-1, run("SET OTA CONFIRM"));
    TEST_ASSERT_EQUAL_STRING("ERR NOT_READY\n", resp);
}

static void test_confirm_ok(void) {
    s_confirm_rc = 0;
    TEST_ASSERT_EQUAL_INT(0, run("SET OTA CONFIRM"));
    TEST_ASSERT_EQUAL_STRING("OK OTA CONFIRM\n", resp);
}

int main(void) { UNITY_BEGIN();
    RUN_TEST(test_bench_pass_at_60s);
    RUN_TEST(test_bench_pending_at_59_9s);
    RUN_TEST(test_fleet_pass_instant_with_frame);
    RUN_TEST(test_fleet_pending_without_frame_before_window);
    RUN_TEST(test_fleet_fail_at_180s_without_frame);
    RUN_TEST(test_confirm_short_circuits_base_unmet);
    RUN_TEST(test_base_gate_cfg_loaded);
    RUN_TEST(test_base_gate_drivers_ok);
    RUN_TEST(test_base_gate_ticks);
    RUN_TEST(test_base_gate_twdt_ok);
    RUN_TEST(test_base_gate_min_heap);
    RUN_TEST(test_table_valid);
    RUN_TEST(test_confirm_grammar_rejects_numeric);
    RUN_TEST(test_confirm_not_ready);
    RUN_TEST(test_confirm_ok);
    return UNITY_END(); }
