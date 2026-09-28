#include <stdio.h>
#include <string.h>
#include "unity.h"
#include "pnl_console.h"

/* pnl_console is pure: the zone console's model. The web's console (app.js:582-590, 1647-1682) is the reference:
   forward rewrite "VERB rest" -> "VERB ZONE <z> rest" unless token 2 is already ZONE, a 20-entry sent/reply log,
   history of the ORIGINAL lines walked with prev/next, and a wipe for the idle operator-state rule (D18). */

static char g_store[PNL_CON_LOG][CMD_RESP_MAX];
static pnl_console_t g_c;

void setUp(void) { pnl_con_init(&g_c, g_store); }
void tearDown(void) {}

static void fill(char *s, char ch, int n) { memset(s, ch, (size_t)n); s[n] = '\0'; }

static int all_zero(const void *p, size_t n) {
    const unsigned char *b = (const unsigned char *)p;
    for (size_t i = 0; i < n; i++) if (b[i]) return 0;
    return 1;
}
/* bytes after the terminator are zero: a shorter string written over a longer one leaves no tail behind (C3) */
static int tail_zero(const char *s, size_t cap) {
    size_t n = strlen(s);
    return all_zero(s + n, cap - n);
}

static void test_forward_plain(void) {
    char out[CMD_LINE_MAX];
    TEST_ASSERT_EQUAL_INT(0, pnl_con_forward("GET WATER 1", 2, out, sizeof out));
    TEST_ASSERT_EQUAL_STRING("GET ZONE 2 WATER 1", out);
}

static void test_forward_already_addressed_is_unchanged(void) {
    char out[CMD_LINE_MAX];
    TEST_ASSERT_EQUAL_INT(0, pnl_con_forward("SET ZONE 2 WATER 1 TARGET 55", 3, out, sizeof out));
    TEST_ASSERT_EQUAL_STRING("SET ZONE 2 WATER 1 TARGET 55", out);
    TEST_ASSERT_EQUAL_INT(0, pnl_con_forward("get zone 3 id", 2, out, sizeof out));   /* case-insensitive, like app.js */
    TEST_ASSERT_EQUAL_STRING("get zone 3 id", out);
}

static void test_forward_single_token(void) {
    char out[CMD_LINE_MAX];
    TEST_ASSERT_EQUAL_INT(0, pnl_con_forward("GET", 5, out, sizeof out));
    TEST_ASSERT_EQUAL_STRING("GET ZONE 5", out);
}

static void test_forward_collapses_whitespace(void) {
    char out[CMD_LINE_MAX];
    TEST_ASSERT_EQUAL_INT(0, pnl_con_forward("GET   WATER\t1", 2, out, sizeof out));
    TEST_ASSERT_EQUAL_STRING("GET ZONE 2 WATER 1", out);
}

static void test_forward_overflow_limit(void) {
    char line[256], out[CMD_LINE_MAX];
    memcpy(line, "GET ", 4); fill(line + 4, 'A', 176);          /* 180 chars -> "GET ZONE 2 " + 176 = 187: fits */
    TEST_ASSERT_EQUAL_INT(0, pnl_con_forward(line, 2, out, sizeof out));
    TEST_ASSERT_EQUAL_size_t(187, strlen(out));
    memcpy(line, "GET ", 4); fill(line + 4, 'A', 184);          /* 188 chars -> 195: over CMD_LINE_MAX-1 */
    TEST_ASSERT_EQUAL_INT(-1, pnl_con_forward(line, 2, out, sizeof out));
}

static void test_line_ok_bounds(void) {
    char s[256];
    TEST_ASSERT_EQUAL_INT(0, pnl_con_line_ok(""));
    TEST_ASSERT_EQUAL_INT(0, pnl_con_line_ok("   \r\n"));
    fill(s, 'x', 191); TEST_ASSERT_EQUAL_INT(1, pnl_con_line_ok(s));
    fill(s, 'x', 192); TEST_ASSERT_EQUAL_INT(0, pnl_con_line_ok(s));
    fill(s, 'x', 191); strcat(s, "  \r\n"); TEST_ASSERT_EQUAL_INT(1, pnl_con_line_ok(s));  /* trailing trimmed first */
    TEST_ASSERT_EQUAL_INT(0, pnl_con_line_ok("\r\n"));                                  /* only whitespace: empty */
    TEST_ASSERT_EQUAL_INT(0, pnl_con_line_ok(" \t\r\n\t "));
    s[0] = '\t'; s[1] = ' '; fill(s + 2, 'x', 191); strcat(s, "\t");                   /* both ends, like .trim() */
    TEST_ASSERT_EQUAL_INT(1, pnl_con_line_ok(s));
}

static void test_span_trims_both_ends(void) {
    size_t n = 99;
    const char *p = pnl_con_span(" \tGET ID\t\r\n", &n);
    TEST_ASSERT_EQUAL_size_t(6, n);
    TEST_ASSERT_EQUAL_INT(0, strncmp(p, "GET ID", 6));
    p = pnl_con_span("\r\n", &n);
    TEST_ASSERT_EQUAL_size_t(0, n);
    TEST_ASSERT_NOT_NULL(p);
    p = pnl_con_span(NULL, &n);
    TEST_ASSERT_EQUAL_size_t(0, n);
    TEST_ASSERT_NOT_NULL(p);
    p = pnl_con_span("GET  ID", &n);                                                   /* inner whitespace is kept */
    TEST_ASSERT_EQUAL_size_t(7, n);
}

static void test_log_ring_wraps_at_21(void) {
    char sent[16];
    for (int i = 0; i < 21; i++) {
        snprintf(sent, sizeof sent, "L%d", i);
        pnl_con_entry_t *e = pnl_con_push(&g_c, sent);
        TEST_ASSERT_NOT_NULL(e);
        pnl_con_reply(e, "OK\n");
    }
    TEST_ASSERT_EQUAL_INT(PNL_CON_LOG, g_c.n_log);
    TEST_ASSERT_EQUAL_STRING("L1", pnl_con_log_at(&g_c, 0)->sent);      /* L0 fell off */
    TEST_ASSERT_EQUAL_STRING("L20", pnl_con_log_at(&g_c, 19)->sent);
    TEST_ASSERT_NULL(pnl_con_log_at(&g_c, 20));
    TEST_ASSERT_NULL(pnl_con_log_at(&g_c, -1));
}

static void test_push_never_overwrites_a_pending_entry(void) {
    pnl_con_entry_t *first = pnl_con_push(&g_c, "FIRST");               /* left pending: the worker owns it */
    for (int i = 1; i < PNL_CON_LOG; i++) pnl_con_reply(pnl_con_push(&g_c, "X"), "OK\n");
    TEST_ASSERT_NULL(pnl_con_push(&g_c, "WOULD-WRAP-ONTO-FIRST"));
    TEST_ASSERT_EQUAL_STRING("FIRST", first->sent);
    TEST_ASSERT_EQUAL_UINT8(1, first->pending);
}

static void test_reply_is_verbatim_and_clipped(void) {
    static char big[5000];
    pnl_con_entry_t *e = pnl_con_push(&g_c, "GET ID");
    TEST_ASSERT_EQUAL_UINT8(1, e->pending);
    pnl_con_reply(e, "OK ID MASTER 0\n");
    TEST_ASSERT_EQUAL_STRING("OK ID MASTER 0\n", e->reply);
    TEST_ASSERT_EQUAL_UINT8(0, e->pending);
    fill(big, 'r', 4999);
    pnl_con_reply(e, big);
    TEST_ASSERT_EQUAL_size_t(CMD_RESP_MAX - 1, strlen(e->reply));
    pnl_con_reply(e, "OK\n");                                             /* shorter over longer: no tail left */
    TEST_ASSERT_EQUAL_STRING("OK\n", e->reply);
    TEST_ASSERT_TRUE(tail_zero(e->reply, CMD_RESP_MAX));
}

static void test_reuse_leaves_no_tail(void) {
    pnl_con_entry_t *first = pnl_con_push(&g_c, "SET WIFI STA house secretpass");
    pnl_con_reply(first, "OK WIFI STA house secretpass\n");
    for (int i = 1; i < PNL_CON_LOG; i++) pnl_con_reply(pnl_con_push(&g_c, "X"), "OK\n");
    pnl_con_entry_t *again = pnl_con_push(&g_c, "GET ID");                /* wraps onto the first entry */
    TEST_ASSERT_EQUAL_PTR(first, again);
    TEST_ASSERT_TRUE(tail_zero(again->sent, sizeof again->sent));
    TEST_ASSERT_TRUE(all_zero(g_store[0], CMD_RESP_MAX));                 /* pending: the old reply is gone already */
    pnl_con_reply(again, "OK\n");
    TEST_ASSERT_TRUE(tail_zero(g_store[0], CMD_RESP_MAX));

    for (int i = 0; i < PNL_CON_HIST; i++) pnl_con_hist_add(&g_c, "SET WIFI STA house secretpass");
    pnl_con_hist_add(&g_c, "GET ID");                                     /* shifts, then writes the last slot */
    TEST_ASSERT_EQUAL_STRING("GET ID", g_c.hist[PNL_CON_HIST - 1]);
    TEST_ASSERT_TRUE(tail_zero(g_c.hist[PNL_CON_HIST - 1], CMD_LINE_MAX));
}

static void test_history_prev_next_ends(void) {
    TEST_ASSERT_NULL(pnl_con_hist_prev(&g_c));                           /* empty: nothing to recall */
    pnl_con_hist_add(&g_c, "GET ID");
    pnl_con_hist_add(&g_c, "GET WATER 1");
    TEST_ASSERT_EQUAL_STRING("GET WATER 1", pnl_con_hist_prev(&g_c));
    TEST_ASSERT_EQUAL_STRING("GET ID", pnl_con_hist_prev(&g_c));
    TEST_ASSERT_EQUAL_STRING("GET ID", pnl_con_hist_prev(&g_c));         /* stops at the oldest */
    TEST_ASSERT_EQUAL_STRING("GET WATER 1", pnl_con_hist_next(&g_c));
    TEST_ASSERT_EQUAL_STRING("", pnl_con_hist_next(&g_c));               /* past the newest */
    TEST_ASSERT_EQUAL_STRING("", pnl_con_hist_next(&g_c));
}

static void test_history_keeps_the_newest_20(void) {
    char s[16];
    for (int i = 0; i < 21; i++) { snprintf(s, sizeof s, "H%d", i); pnl_con_hist_add(&g_c, s); }
    TEST_ASSERT_EQUAL_INT(PNL_CON_HIST, g_c.n_hist);
    TEST_ASSERT_EQUAL_STRING("H1", g_c.hist[0]);
    TEST_ASSERT_EQUAL_STRING("H20", g_c.hist[PNL_CON_HIST - 1]);
}

static void test_wipe_clears_everything_idle(void) {
    pnl_con_reply(pnl_con_push(&g_c, "SET WIFI STA house secretpass"), "OK WIFI STA house secretpass\n");
    pnl_con_hist_add(&g_c, "SET WIFI STA house secretpass");
    strcpy(g_c.draft, "SET WIFI STA other pw");
    g_c.forward = 1;
    pnl_con_wipe(&g_c);
    TEST_ASSERT_EQUAL_INT(0, g_c.n_log);
    TEST_ASSERT_EQUAL_INT(0, g_c.n_hist);
    TEST_ASSERT_EQUAL_UINT8(0, g_c.forward);
    TEST_ASSERT_EQUAL_STRING("", g_c.draft);
    for (int i = 0; i < PNL_CON_LOG; i++) {
        TEST_ASSERT_EQUAL_STRING("", g_c.log[i].sent);
        TEST_ASSERT_EQUAL_STRING("", g_c.log[i].reply);
    }
    TEST_ASSERT_TRUE(all_zero(g_c.log[0].sent, sizeof g_c.log[0].sent));   /* every byte, not just the first */
    TEST_ASSERT_TRUE(all_zero(g_c.hist[0], sizeof g_c.hist[0]));
    TEST_ASSERT_TRUE(all_zero(g_store[0], CMD_RESP_MAX));
    TEST_ASSERT_TRUE(all_zero(g_c.draft, sizeof g_c.draft));
}

static void test_wipe_leaves_a_pending_entry_to_the_worker(void) {
    pnl_con_entry_t *e = pnl_con_push(&g_c, "GET ZONE 2 WATER 1");
    pnl_con_wipe(&g_c);
    TEST_ASSERT_EQUAL_STRING("GET ZONE 2 WATER 1", e->sent);
    TEST_ASSERT_EQUAL_UINT8(1, e->pending);
    pnl_con_reply(e, "OK\n");
    TEST_ASSERT_EQUAL_STRING("OK\n", e->reply);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_forward_plain);
    RUN_TEST(test_forward_already_addressed_is_unchanged);
    RUN_TEST(test_forward_single_token);
    RUN_TEST(test_forward_collapses_whitespace);
    RUN_TEST(test_forward_overflow_limit);
    RUN_TEST(test_line_ok_bounds);
    RUN_TEST(test_span_trims_both_ends);
    RUN_TEST(test_log_ring_wraps_at_21);
    RUN_TEST(test_push_never_overwrites_a_pending_entry);
    RUN_TEST(test_reply_is_verbatim_and_clipped);
    RUN_TEST(test_reuse_leaves_no_tail);
    RUN_TEST(test_history_prev_next_ends);
    RUN_TEST(test_history_keeps_the_newest_20);
    RUN_TEST(test_wipe_clears_everything_idle);
    RUN_TEST(test_wipe_leaves_a_pending_entry_to_the_worker);
    return UNITY_END();
}
