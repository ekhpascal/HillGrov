#include <string.h>
#include "unity.h"
#include "cmd_core.h"
#include "fake_clock.h"
#include "test_cmd_core_fixture.h"

/* cmd_help.c's render_usage() is deliberately non-static (not declared in
 * cmd_core.h -- it is not public API) so this test can call it directly
 * with a small, test-controlled buffer to exercise its offset clamping. */
void render_usage(const cmd_entry_t *e, uint8_t vbit, char *out, int cap);

static cmd_core_t core;
static cmd_session_t ses;
static char resp[CMD_RESP_MAX];

void setUp(void) {
    memset(&core, 0, sizeof core);
    core.table = TBL; core.table_len = TBL_N;
    core.role = CMD_ROLE_ZONE; core.zone_id = 2;
    core.now_ms = fake_clock_now;
    memset(&ses, 0, sizeof ses);
    ses.source = CMD_SRC_CLI;
    fake_clock_set(100000);
    resp[0] = '\0';
}
void tearDown(void) {}

static int run(const char *line) { return cmd_dispatch(&core, &ses, line, resp, sizeof resp); }

/* ---- HELP rendering ---- */

static void test_help_bare(void) {
    const char *hdr = "+ SET|GET <NOUN> [args] | <VERB> ZONE <z 0-8> <command> | <prefix> HELP\n";
    TEST_ASSERT_EQUAL_INT(0, run("HELP"));
    TEST_ASSERT_EQUAL_INT(0, strncmp(resp, hdr, strlen(hdr)));
    TEST_ASSERT_NULL(strstr(resp, "<shelf"));
    /* zone role: SYSTEM/SESSION/TIME/SHELF/DEBUG have visible rows; RING and
       NETWORK rows in this fixture are all master-only -> fully hidden */
    TEST_ASSERT_NOT_NULL(strstr(resp, "\n+ SYSTEM:"));
    TEST_ASSERT_NOT_NULL(strstr(resp, "\n+ SESSION:"));
    TEST_ASSERT_NOT_NULL(strstr(resp, "\n+ TIME:"));
    TEST_ASSERT_NOT_NULL(strstr(resp, "\n+ SHELF:"));
    TEST_ASSERT_NOT_NULL(strstr(resp, "\n+ DEBUG:"));
    TEST_ASSERT_NULL(strstr(resp, "\n+ RING:"));
    TEST_ASSERT_NULL(strstr(resp, "\n+ NETWORK:"));
    /* exactly one line per non-empty area: header + 5 area lines = 6 '+'-led lines */
    int plus_lines = 0;
    for (const char *p = resp; *p; p++)
        if (*p == '+' && (p == resp || p[-1] == '\n')) plus_lines++;
    TEST_ASSERT_EQUAL_INT(6, plus_lines);
}

static void test_help_set_light_exact(void) {
    TEST_ASSERT_EQUAL_INT(0, run("SET LIGHT HELP"));
    TEST_ASSERT_EQUAL_STRING(
        "+ SET LIGHT <shelf 1-4> <white 0-100> <red 0-100> <minutes 1-720>  -- manual override, then AUTO\n"
        "+ GET LIGHT <shelf 1-4>\n", resp);
}

static void test_help_set_poke_unlock_marker(void) {
    TEST_ASSERT_EQUAL_INT(0, run("SET POKE HELP"));
    TEST_ASSERT_NOT_NULL(strstr(resp, " [unlock]"));
}

static void test_help_hides_master_row_on_zone(void) {
    TEST_ASSERT_EQUAL_INT(-1, run("SET WIFI HELP"));
    TEST_ASSERT_EQUAL_STRING("ERR UNKNOWN_CMD\n", resp);
}

static void test_help_unknown_noun(void) {
    TEST_ASSERT_EQUAL_INT(-1, run("SET NOPE HELP"));
    TEST_ASSERT_EQUAL_STRING("ERR UNKNOWN_CMD\n", resp);
}

static void test_help_full_listing_under_4000_bytes(void) {
    TEST_ASSERT_EQUAL_INT(0, run("SET HELP"));
    TEST_ASSERT_TRUE(strlen(resp) < 4000);
    TEST_ASSERT_EQUAL_INT(0, run("GET HELP"));
    TEST_ASSERT_TRUE(strlen(resp) < 4000);

    /* same guard under master role, where MASTER-flagged rows (hidden on
       zone) become visible and add to the listing */
    core.role = CMD_ROLE_MASTER;
    TEST_ASSERT_EQUAL_INT(0, run("SET HELP"));
    TEST_ASSERT_TRUE(strlen(resp) < 4000);
    TEST_ASSERT_EQUAL_INT(0, run("GET HELP"));
    TEST_ASSERT_TRUE(strlen(resp) < 4000);
}

static void test_help_split_noun_row(void) {
    /* SET NODE HELP on master role: split-shape rows render the key arg
       between the two noun words (Task 7 ruling). */
    core.role = CMD_ROLE_MASTER;
    TEST_ASSERT_EQUAL_INT(0, run("SET NODE NAME HELP"));
    TEST_ASSERT_EQUAL_STRING("+ SET NODE <zone 1-8> NAME <name>\n", resp);
    TEST_ASSERT_EQUAL_INT(0, run("SET NODE MAC HELP"));
    TEST_ASSERT_EQUAL_STRING("+ SET NODE <zone 1-8> MAC <mac xx:xx:xx:xx:xx:xx>\n", resp);
}

/* ---- cmd_table_check ---- */

static void test_table_check_fixture_flags_tag_shadow(void) {
    /* The shared fixture deliberately keeps TAG (one-word) and TAG MAC
       (two-word, same noun1) so test_cmd_core.c can exercise runtime
       precedence between them. That shape is a table conflict per the
       Task 7 ruling: a one-word row whose max_args >= 2 collides with a
       same-noun1 two-word row's split/adjacent match positions. It must
       be flagged, at the later (two-word) row's index. */
    TEST_ASSERT_EQUAL_INT(TBL_N - 1, cmd_table_check(TBL, TBL_N));

    /* A copy without the TAG pair (the last two rows) is clean. */
    cmd_entry_t clean[TBL_N - 2];
    memcpy(clean, TBL, sizeof clean);
    TEST_ASSERT_EQUAL_INT(-1, cmd_table_check(clean, TBL_N - 2));
}

static void test_table_check_duplicate_row(void) {
    cmd_entry_t dup[2];
    dup[0] = TBL[0]; dup[1] = TBL[0];   /* LIGHT row, twice: same verbs/noun1/noun2 */
    TEST_ASSERT_EQUAL_INT(1, cmd_table_check(dup, 2));
}

static void test_table_check_actuator_missing_duration_arg(void) {
    static const cmd_arg_t BAD_ARGS[] = {
        {"shelf", ARG_INT, 1, 4, NULL}, {"white", ARG_INT, 0, 100, NULL},
        {"red", ARG_INT, 0, 100, NULL}, {"level", ARG_INT, 1, 720, NULL},
    };
    cmd_entry_t bad = TBL[0];           /* LIGHT: CMDF_ACTUATOR */
    bad.args = BAD_ARGS;                /* duration arg renamed "level" */
    TEST_ASSERT_EQUAL_INT(0, cmd_table_check(&bad, 1));
}

static void test_table_check_bare_combined_with_set(void) {
    cmd_entry_t bad = TBL[4];           /* REBOOT: CMDV_BARE only */
    bad.verbs = CMDV_BARE | CMDV_SET;
    TEST_ASSERT_EQUAL_INT(0, cmd_table_check(&bad, 1));
}

static void test_table_check_n_key_exceeds_max_args(void) {
    cmd_entry_t bad = TBL[0];           /* LIGHT: max_args 4 */
    bad.n_key = 5;
    TEST_ASSERT_EQUAL_INT(0, cmd_table_check(&bad, 1));
}

static void test_table_check_min_exceeds_max_args(void) {
    cmd_entry_t bad = TBL[0];           /* LIGHT: min_args 4, max_args 4 */
    bad.min_args = 5;
    TEST_ASSERT_EQUAL_INT(0, cmd_table_check(&bad, 1));
}

static void test_table_check_max_args_exceeds_cmd_max(void) {
    cmd_entry_t bad = TBL[0];           /* LIGHT: max_args 4, n_key/min_args unaffected */
    bad.max_args = CMD_MAX_ARGS + 1;
    TEST_ASSERT_EQUAL_INT(0, cmd_table_check(&bad, 1));
}

static void test_table_check_null_handler(void) {
    cmd_entry_t bad = TBL[0];
    bad.handler = NULL;
    TEST_ASSERT_EQUAL_INT(0, cmd_table_check(&bad, 1));
}

static void test_table_check_enum_null_enums(void) {
    static const cmd_arg_t BAD_ENUM_ARGS[] = { {"mode", ARG_ENUM, 0, 1, NULL} };
    cmd_entry_t bad = TBL[5];           /* POKE: SET-only, 1 ENUM arg */
    bad.args = BAD_ENUM_ARGS;
    TEST_ASSERT_EQUAL_INT(0, cmd_table_check(&bad, 1));
}

static void test_table_check_long_line_no_corruption(void) {
    /* 8 ENUM args whose rendered text runs well past render_usage's 224-byte
       internal buffer -- exercises sappend()'s offset clamping without a
       crash or out-of-bounds write, and still correctly flags the row via
       the >96-char rule (a truncated line is still > 96 chars here). */
    static const char LONGENUM[] =
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";  /* 68 chars */
    static const cmd_arg_t LONG_ARGS[8] = {
        {"a1", ARG_ENUM, 0, 0, LONGENUM}, {"a2", ARG_ENUM, 0, 0, LONGENUM},
        {"a3", ARG_ENUM, 0, 0, LONGENUM}, {"a4", ARG_ENUM, 0, 0, LONGENUM},
        {"a5", ARG_ENUM, 0, 0, LONGENUM}, {"a6", ARG_ENUM, 0, 0, LONGENUM},
        {"a7", ARG_ENUM, 0, 0, LONGENUM}, {"a8", ARG_ENUM, 0, 0, LONGENUM},
    };
    cmd_entry_t bad = { CMDV_SET, CMD_AREA_DEBUG, "LONG", NULL, LONG_ARGS, 0, 8, 8, 0, h_poke, NULL };
    TEST_ASSERT_EQUAL_INT(0, cmd_table_check(&bad, 1));   /* flagged; no crash */

    /* render directly into an undersized (64-byte) buffer: must stay in
       bounds and leave a NUL-terminated, truncated string */
    char small[64];
    memset(small, 0x7E, sizeof small);   /* sentinel fill: '~' everywhere */
    render_usage(&bad, CMDV_SET, small, (int)sizeof small);
    TEST_ASSERT_EQUAL_INT('\0', small[sizeof small - 1]);
    TEST_ASSERT_TRUE(strlen(small) < sizeof small);
}

int main(void) { UNITY_BEGIN();
    RUN_TEST(test_help_bare);
    RUN_TEST(test_help_set_light_exact);
    RUN_TEST(test_help_set_poke_unlock_marker);
    RUN_TEST(test_help_hides_master_row_on_zone);
    RUN_TEST(test_help_unknown_noun);
    RUN_TEST(test_help_full_listing_under_4000_bytes);
    RUN_TEST(test_help_split_noun_row);
    RUN_TEST(test_table_check_fixture_flags_tag_shadow);
    RUN_TEST(test_table_check_duplicate_row);
    RUN_TEST(test_table_check_actuator_missing_duration_arg);
    RUN_TEST(test_table_check_bare_combined_with_set);
    RUN_TEST(test_table_check_n_key_exceeds_max_args);
    RUN_TEST(test_table_check_min_exceeds_max_args);
    RUN_TEST(test_table_check_max_args_exceeds_cmd_max);
    RUN_TEST(test_table_check_null_handler);
    RUN_TEST(test_table_check_enum_null_enums);
    RUN_TEST(test_table_check_long_line_no_corruption);
    return UNITY_END(); }
