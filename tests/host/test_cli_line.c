#include <string.h>
#include "unity.h"
#include "cli_line.h"
#include "fake_clock.h"

static cli_line_t l;
static char eo[256];
static size_t eol;
void setUp(void) { cli_line_init(&l, 1); fake_clock_set(1000); }
void tearDown(void) {}

#define l_buf_peek() cli_line_peek(&l)

static cli_evt_t feed(const char *s) {
    cli_evt_t e = CLI_EVT_NONE;
    while (*s) e = cli_line_feed(&l, (uint8_t)*s++, fake_clock_now(), eo, sizeof eo, &eol);
    return e;
}

static void test_basic_line_and_backspace(void) {
    feed("ab");
    cli_line_feed(&l, 0x7F, fake_clock_now(), eo, sizeof eo, &eol);
    TEST_ASSERT_EQUAL_size_t(3, eol);                       /* "\b \b" */
    TEST_ASSERT_EQUAL_MEMORY("\b \b", eo, 3);
    TEST_ASSERT_EQUAL_INT(CLI_EVT_LINE, feed("c\r"));
    TEST_ASSERT_EQUAL_STRING("ac", cli_line_take(&l));
}

static void test_crlf_swallow_and_lf_alone(void) {
    TEST_ASSERT_EQUAL_INT(CLI_EVT_LINE, feed("X\r"));
    TEST_ASSERT_EQUAL_STRING("X", cli_line_take(&l));
    TEST_ASSERT_EQUAL_INT(CLI_EVT_NONE, feed("\n"));        /* LF after CR swallowed */
    TEST_ASSERT_EQUAL_INT(CLI_EVT_LINE, feed("Y\n"));
    TEST_ASSERT_EQUAL_STRING("Y", cli_line_take(&l));
    TEST_ASSERT_EQUAL_INT(CLI_EVT_LINE, feed("\r"));        /* empty line */
    TEST_ASSERT_EQUAL_STRING("", cli_line_take(&l));
}

static void test_crlf_swallow_immediate_only(void) {
    /* A stale last_cr flag must not swallow an LF that isn't immediately after the CR. */
    TEST_ASSERT_EQUAL_INT(CLI_EVT_LINE, feed("\r"));
    TEST_ASSERT_EQUAL_STRING("", cli_line_take(&l));
    TEST_ASSERT_EQUAL_INT(CLI_EVT_NONE, feed("a"));
    TEST_ASSERT_EQUAL_INT(CLI_EVT_LINE, feed("\n"));         /* not swallowed: 'a' intervened */
    TEST_ASSERT_EQUAL_STRING("a", cli_line_take(&l));

    TEST_ASSERT_EQUAL_INT(CLI_EVT_LINE, feed("\r"));
    TEST_ASSERT_EQUAL_STRING("", cli_line_take(&l));
    cli_line_feed(&l, 0x00, fake_clock_now(), eo, sizeof eo, &eol);  /* noise intervened */
    TEST_ASSERT_EQUAL_INT(CLI_EVT_LINE, feed("\n"));         /* not swallowed */
    TEST_ASSERT_EQUAL_STRING("", cli_line_take(&l));
}

static void test_history_up_down(void) {
    feed("AAA\r"); cli_line_take(&l);
    feed("BBB\r"); cli_line_take(&l);
    feed("BBB\r"); cli_line_take(&l);                        /* duplicate not stored twice */
    feed("cc");                                              /* partial, then browse */
    feed("\x1b[A");
    TEST_ASSERT_EQUAL_STRING("BBB", l_buf_peek());          /* helper below */
    feed("\x1b[A");
    TEST_ASSERT_EQUAL_STRING("AAA", l_buf_peek());
    feed("\x1b[A");                                          /* at oldest, stays */
    TEST_ASSERT_EQUAL_STRING("AAA", l_buf_peek());
    feed("\x1b[B");
    TEST_ASSERT_EQUAL_STRING("BBB", l_buf_peek());
    feed("\x1b[B");                                          /* past newest -> saved partial */
    TEST_ASSERT_EQUAL_STRING("cc", l_buf_peek());
}

static void test_too_long_and_noise(void) {
    for (int i = 0; i < 300; i++) cli_line_feed(&l, 'z', fake_clock_now(), eo, sizeof eo, &eol);
    TEST_ASSERT_EQUAL_INT(CLI_EVT_TOO_LONG, feed("\r"));
    TEST_ASSERT_EQUAL_INT(CLI_EVT_LINE, feed("Q\r"));
    TEST_ASSERT_EQUAL_STRING("Q", cli_line_take(&l));
    uint8_t junk[3] = { 0x00, 0xFF, 0x07 };
    for (int i = 0; i < 3; i++) cli_line_feed(&l, junk[i], fake_clock_now(), eo, sizeof eo, &eol);
    TEST_ASSERT_EQUAL_UINT32(3, cli_line_noise(&l));
}

static void test_machine_mode_stale_and_esc_noise(void) {
    cli_line_init(&l, 0);
    feed("par");
    fake_clock_add(30001);
    TEST_ASSERT_EQUAL_INT(CLI_EVT_LINE, feed("Z\r"));        /* stale partial dropped first */
    TEST_ASSERT_EQUAL_STRING("Z", cli_line_take(&l));
    TEST_ASSERT_EQUAL_UINT32(1, cli_line_stale(&l));
    feed("\x1b");
    TEST_ASSERT_EQUAL_UINT32(1, cli_line_noise(&l));
    cli_line_set_echo(&l, 1);                                /* switch to human mode */
    feed("\x1b[A");                                          /* "Z" must not be recallable: */
    TEST_ASSERT_EQUAL_STRING("", l_buf_peek());              /* it was received in machine mode */
}

static void test_ctrl_c(void) {
    feed("junk");
    cli_line_feed(&l, 0x03, fake_clock_now(), eo, sizeof eo, &eol);
    TEST_ASSERT_EQUAL_MEMORY("^C\r\n", eo, 4);
    TEST_ASSERT_EQUAL_INT(CLI_EVT_LINE, feed("k\r"));
    TEST_ASSERT_EQUAL_STRING("k", cli_line_take(&l));
}

int main(void) { UNITY_BEGIN();
    RUN_TEST(test_basic_line_and_backspace);
    RUN_TEST(test_crlf_swallow_and_lf_alone);
    RUN_TEST(test_crlf_swallow_immediate_only);
    RUN_TEST(test_history_up_down);
    RUN_TEST(test_too_long_and_noise);
    RUN_TEST(test_machine_mode_stale_and_esc_noise);
    RUN_TEST(test_ctrl_c);
    return UNITY_END(); }
