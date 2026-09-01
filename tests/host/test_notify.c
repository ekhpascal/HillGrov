#include <string.h>
#include "unity.h"
#include "notify.h"
#include "fake_clock.h"

typedef struct { char lines[8][NTF_LINE_MAX]; int count; } cap_t;

static cap_t capA, capB;

static void cap_sink(void *ctx, const char *line) {
    cap_t *c = (cap_t *)ctx;
    if (c->count >= 8) return;
    size_t n = strlen(line);
    if (n >= NTF_LINE_MAX) n = NTF_LINE_MAX - 1;
    memcpy(c->lines[c->count], line, n);
    c->lines[c->count][n] = '\0';
    c->count++;
}

void setUp(void) {
    fake_clock_set(1000);
    notify_init(fake_clock_now, 2);
    memset(&capA, 0, sizeof capA);
    memset(&capB, 0, sizeof capB);
}
void tearDown(void) {}

static void test_line_format_exact(void) {
    notify_add_sink(cap_sink, &capA, NTF_MASK_ALL);
    notify_emit(NTF_WATER, 1, "%d START %d MANUAL", 1, 15);
    TEST_ASSERT_EQUAL_INT(1, capA.count);
    TEST_ASSERT_EQUAL_STRING("NOTIFY WATER 2 1 START 15 MANUAL\n", capA.lines[0]);
}

static void test_soil_rate_limit_per_idx(void) {
    notify_add_sink(cap_sink, &capA, NTF_MASK_ALL);
    notify_emit(NTF_SOIL, 1, "%d", 50);
    notify_emit(NTF_SOIL, 1, "%d", 51);     /* suppressed: same idx within 60s */
    notify_emit(NTF_SOIL, 2, "%d", 52);     /* different idx: passes */
    TEST_ASSERT_EQUAL_INT(2, capA.count);
    TEST_ASSERT_EQUAL_STRING("NOTIFY SOIL 2 50\n", capA.lines[0]);
    TEST_ASSERT_EQUAL_STRING("NOTIFY SOIL 2 52\n", capA.lines[1]);
}

static void test_alarm_rate_limit_and_recover(void) {
    notify_add_sink(cap_sink, &capA, NTF_MASK_ALL);
    notify_emit(NTF_ALARM, 0, "trip");
    notify_emit(NTF_ALARM, 0, "trip");      /* suppressed: <1000ms since last */
    TEST_ASSERT_EQUAL_INT(1, capA.count);
    fake_clock_add(1000);
    notify_emit(NTF_ALARM, 0, "trip");      /* passes: interval elapsed */
    TEST_ASSERT_EQUAL_INT(2, capA.count);
}

static void test_sink_mask_filters(void) {
    notify_add_sink(cap_sink, &capA, (uint16_t)(NTF_MASK_ALL & ~NTF_MASK(NTF_WATER)));
    notify_add_sink(cap_sink, &capB, NTF_MASK_ALL);
    notify_emit(NTF_WATER, 0, "hi");
    TEST_ASSERT_EQUAL_INT(0, capA.count);
    TEST_ASSERT_EQUAL_INT(1, capB.count);
}

static void test_parse(void) {
    TEST_ASSERT_EQUAL_INT(NTF_COUNT, notify_parse("all"));
    TEST_ASSERT_EQUAL_INT(NTF_SOIL, notify_parse("soil"));
    TEST_ASSERT_EQUAL_INT(-1, notify_parse("nope"));
}

static void test_truncation(void) {
    notify_add_sink(cap_sink, &capA, NTF_MASK_ALL);
    char payload[201];
    memset(payload, 'x', 200);
    payload[200] = '\0';
    notify_emit(NTF_CMD, 0, "%s", payload);
    TEST_ASSERT_EQUAL_INT(1, capA.count);
    size_t len = strlen(capA.lines[0]);
    TEST_ASSERT_EQUAL_size_t((size_t)(NTF_LINE_MAX - 1), len);
    TEST_ASSERT_EQUAL_CHAR('\n', capA.lines[0][len - 1]);
}

static void test_boot_once(void) {
    notify_add_sink(cap_sink, &capA, NTF_MASK_ALL);
    notify_emit(NTF_BOOT, 0, "up");
    notify_emit(NTF_BOOT, 0, "up");         /* suppressed: once per boot */
    TEST_ASSERT_EQUAL_INT(1, capA.count);
}

int main(void) { UNITY_BEGIN();
    RUN_TEST(test_line_format_exact);
    RUN_TEST(test_soil_rate_limit_per_idx);
    RUN_TEST(test_alarm_rate_limit_and_recover);
    RUN_TEST(test_sink_mask_filters);
    RUN_TEST(test_parse);
    RUN_TEST(test_truncation);
    RUN_TEST(test_boot_once);
    return UNITY_END(); }
