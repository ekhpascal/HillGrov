#include "unity.h"
#include "app_version.h"

void setUp(void) {}
void tearDown(void) {}

static void test_parse_valid(void) {
    app_version_t v;
    TEST_ASSERT_EQUAL_INT(0, app_version_parse("0.1.0", &v));
    TEST_ASSERT_EQUAL_UINT8(0, v.major);
    TEST_ASSERT_EQUAL_UINT8(1, v.minor);
    TEST_ASSERT_EQUAL_UINT8(0, v.patch);
    TEST_ASSERT_EQUAL_INT(0, app_version_parse("255.255.255", &v));
    TEST_ASSERT_EQUAL_UINT8(255, v.patch);
}

static void test_parse_invalid(void) {
    app_version_t v;
    TEST_ASSERT_EQUAL_INT(-1, app_version_parse("", &v));
    TEST_ASSERT_EQUAL_INT(-1, app_version_parse("1.2", &v));
    TEST_ASSERT_EQUAL_INT(-1, app_version_parse("1.2.3.4", &v));
    TEST_ASSERT_EQUAL_INT(-1, app_version_parse("1.2.x", &v));
    TEST_ASSERT_EQUAL_INT(-1, app_version_parse("256.0.0", &v));
    TEST_ASSERT_EQUAL_INT(-1, app_version_parse("1.2.3-rc1", &v));
    TEST_ASSERT_EQUAL_INT(-1, app_version_parse(" 1.2.3", &v));
}

static void test_cmp(void) {
    app_version_t a, b;
    app_version_parse("1.2.3", &a);
    app_version_parse("1.2.4", &b);
    TEST_ASSERT_EQUAL_INT(-1, app_version_cmp(&a, &b));
    TEST_ASSERT_EQUAL_INT(1, app_version_cmp(&b, &a));
    TEST_ASSERT_EQUAL_INT(0, app_version_cmp(&a, &a));
    app_version_parse("2.0.0", &b);
    TEST_ASSERT_EQUAL_INT(-1, app_version_cmp(&a, &b));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_parse_valid);
    RUN_TEST(test_parse_invalid);
    RUN_TEST(test_cmp);
    return UNITY_END();
}
