#include <string.h>
#include "unity.h"
#include "cmd_core.h"
#include "fake_clock.h"
#include "test_cmd_core_fixture.h"

/* ---- dispatch-test-only doubles (forward/audit callbacks) ---- */
static int audit_calls, fwd_calls;
static char fwd_line[CMD_LINE_MAX]; static uint8_t fwd_zone;

static int fwd(void *ctx, uint8_t zone, const char *line, char *resp, int len) {
    (void)ctx; fwd_calls++; fwd_zone = zone;
    snprintf(fwd_line, sizeof fwd_line, "%s", line);
    return cmd_okf(resp, len, "FORWARDED");
}
static void audit(void *ctx, cmd_src_t src, const char *line) { (void)ctx; (void)src; (void)line; audit_calls++; }

static cmd_core_t core;
static cmd_session_t ses;
static char resp[CMD_RESP_MAX];

void setUp(void) {
    memset(&core, 0, sizeof core);
    core.table = TBL; core.table_len = TBL_N;
    core.role = CMD_ROLE_ZONE; core.zone_id = 2;
    core.now_ms = fake_clock_now;
    core.audit = audit; core.debug_key = "hill";
    memset(&ses, 0, sizeof ses);
    ses.source = CMD_SRC_CLI;
    h_calls = audit_calls = fwd_calls = 0;
    fake_clock_set(100000);
    resp[0] = '\0';
}
void tearDown(void) {}

static int run(const char *line) { return cmd_dispatch(&core, &ses, line, resp, sizeof resp); }

static void test_syntax_errors(void) {
    TEST_ASSERT_EQUAL_INT(-1, run(""));                TEST_ASSERT_EQUAL_STRING("ERR EMPTY\n", resp);
    TEST_ASSERT_EQUAL_INT(-1, run("   "));             TEST_ASSERT_EQUAL_STRING("ERR EMPTY\n", resp);
    char long_line[300]; memset(long_line, 'a', 299); long_line[299] = '\0';
    TEST_ASSERT_EQUAL_INT(-1, run(long_line));         TEST_ASSERT_EQUAL_STRING("ERR TOO_LONG\n", resp);
    TEST_ASSERT_EQUAL_INT(-1, run("a b c d e f g h i j k l m")); TEST_ASSERT_EQUAL_STRING("ERR BAD_ARGS\n", resp);
    TEST_ASSERT_EQUAL_INT(-1, run("FOO BAR"));         TEST_ASSERT_EQUAL_STRING("ERR UNKNOWN_CMD\n", resp);
}

static void test_match_case_and_two_word_precedence(void) {
    core.role = CMD_ROLE_MASTER;
    TEST_ASSERT_EQUAL_INT(0, run("set node 3 name Basil"));
    TEST_ASSERT_EQUAL_STRING("OK NODE 3 Basil\n", resp);        /* STR case preserved */
    TEST_ASSERT_EQUAL_INT(0, run("SET NODE 3 MAC 24:6F:28:AA:BB:02"));
    TEST_ASSERT_EQUAL_STRING("OK MAC 2402\n", resp);
}

static void test_arity_and_ranges(void) {
    TEST_ASSERT_EQUAL_INT(-1, run("SET LIGHT 1 70 45"));   TEST_ASSERT_EQUAL_STRING("ERR BAD_ARGS\n", resp);
    TEST_ASSERT_EQUAL_INT(-1, run("GET LIGHT 1 2"));       TEST_ASSERT_EQUAL_STRING("ERR BAD_ARGS\n", resp);
    TEST_ASSERT_EQUAL_INT(-1, run("SET LIGHT 5 1 1 1"));   TEST_ASSERT_EQUAL_STRING("ERR OUT_OF_RANGE\n", resp);
    TEST_ASSERT_EQUAL_INT(-1, run("SET LIGHT x 1 1 1"));   TEST_ASSERT_EQUAL_STRING("ERR BAD_ARGS\n", resp);
    TEST_ASSERT_EQUAL_INT(0,  run("SET LIGHT 3 70 45 60"));
    TEST_ASSERT_EQUAL_STRING("OK LIGHT 3 70 45 MANUAL 60\n", resp);
    TEST_ASSERT_EQUAL_INT(0,  run("GET LIGHT 3"));
    TEST_ASSERT_EQUAL_INT(-1, run("SET WHEN 25:00"));      TEST_ASSERT_EQUAL_STRING("ERR BAD_ARGS\n", resp);
    TEST_ASSERT_EQUAL_INT(0,  run("SET WHEN 06:30"));      TEST_ASSERT_EQUAL_STRING("OK WHEN 390\n", resp);
    TEST_ASSERT_EQUAL_INT(0,  run("SET WHEN 390"));
    TEST_ASSERT_EQUAL_INT(-1, run("SET NODE 3 MAC 24:6F:28:AA:BB")); /* 5 groups */
    TEST_ASSERT_EQUAL_STRING("ERR MASTER_ONLY\n", resp);   /* role gate wins before parse on zone */
    TEST_ASSERT_EQUAL_INT(0,  run("REBOOT CONFIRM"));      TEST_ASSERT_EQUAL_STRING("OK REBOOT\n", resp);
    TEST_ASSERT_EQUAL_INT(-1, run("REBOOT"));              TEST_ASSERT_EQUAL_STRING("ERR BAD_ARGS\n", resp);
    TEST_ASSERT_EQUAL_INT(-1, run("REBOOT YES"));          TEST_ASSERT_EQUAL_STRING("ERR BAD_ARGS\n", resp);
    /* Regression: enum_index's numeric fallback must not let index 0 satisfy
     * a single-entry enum ("CONFIRM") -- REBOOT 0 is not a confirmation. */
    TEST_ASSERT_EQUAL_INT(-1, run("REBOOT 0"));            TEST_ASSERT_EQUAL_STRING("ERR BAD_ARGS\n", resp);
    /* Multi-entry enums (A_MODE: OFF|ON) must keep accepting numeric indices. */
    ses.unlock_until_ms = fake_clock_now() + 1000;
    TEST_ASSERT_EQUAL_INT(0,  run("SET POKE 1"));           TEST_ASSERT_EQUAL_STRING("OK POKE\n", resp);
}

static void test_gates(void) {
    TEST_ASSERT_EQUAL_INT(-1, run("SET WIFI ON"));         TEST_ASSERT_EQUAL_STRING("ERR MASTER_ONLY\n", resp);
    core.role = CMD_ROLE_MASTER;
    TEST_ASSERT_EQUAL_INT(-1, run("SET LIGHT 1 1 1 1"));   TEST_ASSERT_EQUAL_STRING("ERR ZONE_ONLY\n", resp);
    core.role = CMD_ROLE_ZONE;
    ses.source = CMD_SRC_HTTP;
    TEST_ASSERT_EQUAL_INT(-1, run("SET ECHO ON"));         TEST_ASSERT_EQUAL_STRING("ERR NOT_LOCAL\n", resp);
    ses.source = CMD_SRC_CLI;
    TEST_ASSERT_EQUAL_INT(-1, run("SET POKE ON"));         TEST_ASSERT_EQUAL_STRING("ERR LOCKED\n", resp);
    ses.unlock_until_ms = fake_clock_now() + 1000;
    TEST_ASSERT_EQUAL_INT(0,  run("SET POKE ON"));
    TEST_ASSERT_EQUAL_UINT32(fake_clock_now() + CMD_UNLOCK_MS, ses.unlock_until_ms); /* refreshed */
    fake_clock_add(CMD_UNLOCK_MS + 1);
    TEST_ASSERT_EQUAL_INT(-1, run("SET POKE ON"));         TEST_ASSERT_EQUAL_STRING("ERR LOCKED\n", resp);
}

static void test_zone_prefix(void) {
    /* zone role, own id 2 */
    TEST_ASSERT_EQUAL_INT(0,  run("SET ZONE 2 LIGHT 1 10 10 5"));
    TEST_ASSERT_EQUAL_INT(0,  run("SET ZONE 0 LIGHT 1 10 10 5"));
    TEST_ASSERT_EQUAL_INT(-1, run("SET ZONE 3 LIGHT 1 10 10 5"));
    TEST_ASSERT_EQUAL_STRING("ERR WRONG_ZONE\n", resp);
    TEST_ASSERT_EQUAL_INT(-1, run("SET ZONE 9 LIGHT 1 1 1 1"));
    TEST_ASSERT_EQUAL_STRING("ERR OUT_OF_RANGE\n", resp);
    TEST_ASSERT_EQUAL_INT(-1, run("SET ZONE 2"));
    TEST_ASSERT_EQUAL_STRING("ERR BAD_ARGS\n", resp);
    /* master role: 0 local, others forwarded verbatim, case preserved */
    core.role = CMD_ROLE_MASTER; core.zone_id = 0;
    core.forward = fwd;
    TEST_ASSERT_EQUAL_INT(0, run("set ZONE 2 light 3 70 45 60"));
    TEST_ASSERT_EQUAL_INT(1, fwd_calls);
    TEST_ASSERT_EQUAL_UINT8(2, fwd_zone);
    TEST_ASSERT_EQUAL_STRING("set light 3 70 45 60", fwd_line);
    TEST_ASSERT_EQUAL_STRING("OK FORWARDED\n", resp);
    core.forward = NULL;                                    /* SP1 master without ring */
    TEST_ASSERT_EQUAL_INT(-1, run("SET ZONE 2 LIGHT 1 1 1 1"));
    TEST_ASSERT_EQUAL_STRING("ERR ZONE_UNKNOWN\n", resp);
}

static void test_internal_and_audit(void) {
    TEST_ASSERT_EQUAL_INT(-1, run("GET SILENT"));
    TEST_ASSERT_EQUAL_STRING("ERR INTERNAL\n", resp);
    audit_calls = 0;
    TEST_ASSERT_EQUAL_INT(0, run("SET LIGHT 1 10 10 5"));
    TEST_ASSERT_EQUAL_INT(1, audit_calls);                  /* actuator + success */
    TEST_ASSERT_EQUAL_INT(-1, run("SET LIGHT 9 10 10 5"));
    TEST_ASSERT_EQUAL_INT(1, audit_calls);                  /* refused -> no audit */
    TEST_ASSERT_EQUAL_INT(0, run("GET LIGHT 1"));
    TEST_ASSERT_EQUAL_INT(1, audit_calls);                  /* GET is not audited */
}

static void test_split_noun_single_lookahead(void) {
    /* Regression: two-word match only at noun0+2, not open-ended scan */
    /* SET TAG 3 MAC <mac> should match two-word TAG MAC (noun2 at exactly noun0+2) */
    TEST_ASSERT_EQUAL_INT(0, run("SET TAG 3 MAC 24:6F:28:AA:BB:02"));
    TEST_ASSERT_EQUAL_STRING("OK TAGMAC 3 2402\n", resp);

    /* SET TAG 3 x MAC should match one-word TAG with args "3", "x", "MAC" (no reroute) */
    TEST_ASSERT_EQUAL_INT(0, run("SET TAG 3 x MAC"));
    TEST_ASSERT_EQUAL_STRING("OK TAG 3 x MAC\n", resp);    /* q->tok[2]=="MAC" as STR arg, not noun2 */
}

int main(void) { UNITY_BEGIN();
    RUN_TEST(test_syntax_errors);
    RUN_TEST(test_match_case_and_two_word_precedence);
    RUN_TEST(test_arity_and_ranges);
    RUN_TEST(test_gates);
    RUN_TEST(test_zone_prefix);
    RUN_TEST(test_internal_and_audit);
    RUN_TEST(test_split_noun_single_lookahead);
    return UNITY_END(); }
