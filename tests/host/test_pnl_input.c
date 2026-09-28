/* test_pnl_input.c -- what each keyboard class lets through, text bounds, and the web's MAC rule. */
#include <string.h>
#include "unity.h"
#include "pnl_input.h"

void setUp(void) {}
void tearDown(void) {}

static void test_class_boundaries(void) {
    TEST_ASSERT_EQUAL_INT(1, pnl_kb_accepts(PCFG_KB_TEXT, ' '));
    TEST_ASSERT_EQUAL_INT(0, pnl_kb_accepts(PCFG_KB_TEXT_NOSPACE, ' '));
    TEST_ASSERT_EQUAL_INT(1, pnl_kb_accepts(PCFG_KB_TEXT, '~'));
    TEST_ASSERT_EQUAL_INT(1, pnl_kb_accepts(PCFG_KB_TEXT_NOSPACE, '~'));
    TEST_ASSERT_EQUAL_INT(1, pnl_kb_accepts(PCFG_KB_TEXT_NOSPACE, '!'));
    TEST_ASSERT_EQUAL_INT(0, pnl_kb_accepts(PCFG_KB_TEXT, 0x7F));
    TEST_ASSERT_EQUAL_INT(0, pnl_kb_accepts(PCFG_KB_TEXT, 0x1F));
    TEST_ASSERT_EQUAL_INT(0, pnl_kb_accepts(PCFG_KB_TEXT, '\n'));
    TEST_ASSERT_EQUAL_INT(0, pnl_kb_accepts(PCFG_KB_TEXT, (char)0xC3));   /* UTF-8 lead byte */
    TEST_ASSERT_EQUAL_INT(1, pnl_kb_accepts(PCFG_KB_NUMERIC, '0'));
    TEST_ASSERT_EQUAL_INT(1, pnl_kb_accepts(PCFG_KB_NUMERIC, '9'));
    TEST_ASSERT_EQUAL_INT(0, pnl_kb_accepts(PCFG_KB_NUMERIC, '-'));
    TEST_ASSERT_EQUAL_INT(0, pnl_kb_accepts(PCFG_KB_NUMERIC, '.'));
    TEST_ASSERT_EQUAL_INT(0, pnl_kb_accepts(PCFG_KB_NUMERIC, '+'));
}
static void test_hostname_class(void) {
    TEST_ASSERT_EQUAL_INT(1, pnl_kb_accepts(PCFG_KB_HOSTNAME, 'a'));
    TEST_ASSERT_EQUAL_INT(1, pnl_kb_accepts(PCFG_KB_HOSTNAME, 'z'));
    TEST_ASSERT_EQUAL_INT(1, pnl_kb_accepts(PCFG_KB_HOSTNAME, '0'));
    TEST_ASSERT_EQUAL_INT(1, pnl_kb_accepts(PCFG_KB_HOSTNAME, '-'));
    TEST_ASSERT_EQUAL_INT(0, pnl_kb_accepts(PCFG_KB_HOSTNAME, 'A'));   /* upper case refused (hg_mcfg.c:74-78) */
    TEST_ASSERT_EQUAL_INT(0, pnl_kb_accepts(PCFG_KB_HOSTNAME, '_'));
    TEST_ASSERT_EQUAL_INT(0, pnl_kb_accepts(PCFG_KB_HOSTNAME, '.'));
}
static void test_hex_class_and_none(void) {
    TEST_ASSERT_EQUAL_INT(1, pnl_kb_accepts(PCFG_KB_HEX, 'f'));
    TEST_ASSERT_EQUAL_INT(1, pnl_kb_accepts(PCFG_KB_HEX, 'F'));
    TEST_ASSERT_EQUAL_INT(1, pnl_kb_accepts(PCFG_KB_HEX, ':'));
    TEST_ASSERT_EQUAL_INT(0, pnl_kb_accepts(PCFG_KB_HEX, 'g'));
    TEST_ASSERT_EQUAL_INT(0, pnl_kb_accepts(PCFG_KB_HEX, '-'));
    TEST_ASSERT_EQUAL_INT(0, pnl_kb_accepts(PCFG_KB_NONE, 'a'));
}
static void test_text_bounds(void) {
    TEST_ASSERT_EQUAL_INT(0, pnl_text_ok(PCFG_KB_TEXT_NOSPACE, "", 1, 15));
    TEST_ASSERT_EQUAL_INT(1, pnl_text_ok(PCFG_KB_TEXT_NOSPACE, "", 0, 15));
    TEST_ASSERT_EQUAL_INT(1, pnl_text_ok(PCFG_KB_TEXT_NOSPACE, "123456789012345", 1, 15));
    TEST_ASSERT_EQUAL_INT(0, pnl_text_ok(PCFG_KB_TEXT_NOSPACE, "1234567890123456", 1, 15));
    TEST_ASSERT_EQUAL_INT(0, pnl_text_ok(PCFG_KB_TEXT_NOSPACE, "a b", 1, 15));
    TEST_ASSERT_EQUAL_INT(1, pnl_text_ok(PCFG_KB_TEXT, "a b", 1, 15));
    TEST_ASSERT_EQUAL_INT(0, pnl_text_ok(PCFG_KB_HOSTNAME, "Green", 1, 23));
    TEST_ASSERT_EQUAL_INT(1, pnl_text_ok(PCFG_KB_HOSTNAME, "green-1", 1, 23));
    TEST_ASSERT_EQUAL_INT(0, pnl_text_ok(PCFG_KB_TEXT, NULL, 0, 15));
}
static void test_mac(void) {
    uint8_t m[6] = { 0 };
    static const uint8_t want[6] = { 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x0F };
    TEST_ASSERT_EQUAL_INT(0, pnl_mac_parse("aa:bb:cc:dd:ee:0f", m));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(want, m, 6);
    TEST_ASSERT_EQUAL_INT(0, pnl_mac_parse("AA:BB:CC:DD:EE:0F", m));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(want, m, 6);
    memset(m, 0x11, sizeof m);
    TEST_ASSERT_EQUAL_INT(-1, pnl_mac_parse("aa:bb:cc:dd:ee", m));          /* short */
    TEST_ASSERT_EQUAL_INT(-1, pnl_mac_parse("aa:bb:cc:dd:ee:ff:", m));      /* trailing colon */
    TEST_ASSERT_EQUAL_INT(-1, pnl_mac_parse("aa-bb-cc-dd-ee-ff", m));
    TEST_ASSERT_EQUAL_INT(-1, pnl_mac_parse("aa:bb:cc:dd:ee:fg", m));
    TEST_ASSERT_EQUAL_INT(-1, pnl_mac_parse("a:bb:cc:dd:ee:ff0", m));
    TEST_ASSERT_EQUAL_INT(-1, pnl_mac_parse(NULL, m));
    TEST_ASSERT_EQUAL_UINT8(0x11, m[0]);                                     /* untouched on failure */
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_class_boundaries);
    RUN_TEST(test_hostname_class);
    RUN_TEST(test_hex_class_and_none);
    RUN_TEST(test_text_bounds);
    RUN_TEST(test_mac);
    return UNITY_END();
}
