#include <stdio.h>
#include <string.h>
#include "unity.h"
#include "alarm_mgr.h"
#include "fake_clock.h"
#include "cJSON.h"

void setUp(void) {
    fake_clock_set(100);
    alarm_mgr_init(fake_clock_now);
}
void tearDown(void) {}

/* The real SP3 bench sequence from the brief: active count walks 1,2,2,1,0;
 * 5 events recorded; JSON parses with events[0] (newest) == "RING 0 CLOSED". */
static void test_bench_sequence_active_set_and_events(void) {
    alarm_mgr_sink(NULL, "NOTIFY NODE 2 DEGRADED\n");
    TEST_ASSERT_EQUAL_INT(1, alarm_mgr_active_count());

    alarm_mgr_sink(NULL, "NOTIFY RING 0 OPEN Z2 dead or wire Z2->Z1\n");
    TEST_ASSERT_EQUAL_INT(2, alarm_mgr_active_count());

    alarm_mgr_sink(NULL, "NOTIFY NODE 2 OFFLINE\n");
    TEST_ASSERT_EQUAL_INT(2, alarm_mgr_active_count());   /* same key "NODE 2", still active */

    alarm_mgr_sink(NULL, "NOTIFY NODE 2 ONLINE\n");
    TEST_ASSERT_EQUAL_INT(1, alarm_mgr_active_count());

    alarm_mgr_sink(NULL, "NOTIFY RING 0 CLOSED\n");
    TEST_ASSERT_EQUAL_INT(0, alarm_mgr_active_count());

    TEST_ASSERT_EQUAL_INT(5, alarm_mgr_total());

    char buf[8192];
    int n = alarm_mgr_json(buf, sizeof buf);
    TEST_ASSERT_GREATER_THAN_INT(0, n);
    cJSON *root = cJSON_Parse(buf);
    TEST_ASSERT_NOT_NULL(root);

    cJSON *events = cJSON_GetObjectItem(root, "events");
    TEST_ASSERT_TRUE(cJSON_IsArray(events));
    TEST_ASSERT_EQUAL_INT(5, cJSON_GetArraySize(events));
    cJSON *e0 = cJSON_GetArrayItem(events, 0);
    TEST_ASSERT_EQUAL_STRING("RING 0 CLOSED", cJSON_GetObjectItem(e0, "text")->valuestring);

    cJSON *active = cJSON_GetObjectItem(root, "active");
    TEST_ASSERT_TRUE(cJSON_IsArray(active));
    TEST_ASSERT_EQUAL_INT(0, cJSON_GetArraySize(active));

    cJSON_Delete(root);
}

/* since_s tracks the START of a key's active streak, not the latest state
 * change within it: DEGRADED at t=100 then OFFLINE (still active) at t=150
 * keeps since_s == 100. */
static void test_active_json_shape_and_since_s(void) {
    fake_clock_set(100);
    alarm_mgr_sink(NULL, "NOTIFY NODE 2 DEGRADED\n");
    fake_clock_set(150);
    alarm_mgr_sink(NULL, "NOTIFY NODE 2 OFFLINE\n");

    char buf[4096];
    int n = alarm_mgr_json(buf, sizeof buf);
    TEST_ASSERT_GREATER_THAN_INT(0, n);
    cJSON *root = cJSON_Parse(buf);
    TEST_ASSERT_NOT_NULL(root);

    cJSON *active = cJSON_GetObjectItem(root, "active");
    TEST_ASSERT_EQUAL_INT(1, cJSON_GetArraySize(active));
    cJSON *a0 = cJSON_GetArrayItem(active, 0);
    TEST_ASSERT_EQUAL_STRING("NODE 2", cJSON_GetObjectItem(a0, "key")->valuestring);
    TEST_ASSERT_EQUAL_STRING("NODE 2 OFFLINE", cJSON_GetObjectItem(a0, "text")->valuestring);
    TEST_ASSERT_EQUAL_INT(100, cJSON_GetObjectItem(a0, "since_s")->valueint);

    cJSON_Delete(root);
}

/* 70 lines into a 64-slot ring: total keeps counting every line ever seen,
 * the exported "events" array is capped at AM_EVENTS with the oldest 6 dropped. */
static void test_ring_wrap_keeps_newest_64(void) {
    char line[64];
    for (int i = 1; i <= 70; i++) {
        snprintf(line, sizeof line, "NOTIFY CMD 0 SEQ %d\n", i);
        alarm_mgr_sink(NULL, line);
    }
    TEST_ASSERT_EQUAL_INT(70, alarm_mgr_total());
    TEST_ASSERT_EQUAL_INT(0, alarm_mgr_active_count());   /* CMD is event-only */

    char buf[16384];
    int n = alarm_mgr_json(buf, sizeof buf);
    TEST_ASSERT_GREATER_THAN_INT(0, n);
    cJSON *root = cJSON_Parse(buf);
    TEST_ASSERT_NOT_NULL(root);

    cJSON *events = cJSON_GetObjectItem(root, "events");
    TEST_ASSERT_EQUAL_INT(AM_EVENTS, cJSON_GetArraySize(events));
    cJSON *newest = cJSON_GetArrayItem(events, 0);
    TEST_ASSERT_EQUAL_STRING("CMD 0 SEQ 70", cJSON_GetObjectItem(newest, "text")->valuestring);
    cJSON *oldest = cJSON_GetArrayItem(events, AM_EVENTS - 1);
    TEST_ASSERT_EQUAL_STRING("CMD 0 SEQ 7", cJSON_GetObjectItem(oldest, "text")->valuestring);

    cJSON_Delete(root);
}

static void test_boot_is_event_only(void) {
    alarm_mgr_sink(NULL, "NOTIFY BOOT 2 0.1.0 SW\n");
    TEST_ASSERT_EQUAL_INT(0, alarm_mgr_active_count());
    TEST_ASSERT_EQUAL_INT(1, alarm_mgr_total());

    char buf[2048];
    TEST_ASSERT_GREATER_THAN_INT(0, alarm_mgr_json(buf, sizeof buf));
    cJSON *root = cJSON_Parse(buf);
    cJSON *e0 = cJSON_GetArrayItem(cJSON_GetObjectItem(root, "events"), 0);
    TEST_ASSERT_EQUAL_STRING("BOOT 2 0.1.0 SW", cJSON_GetObjectItem(e0, "text")->valuestring);
    cJSON_Delete(root);
}

/* NTF_WIFI (new in this task) is event-only, same as BOOT/CMD. */
static void test_wifi_is_event_only(void) {
    alarm_mgr_sink(NULL, "NOTIFY WIFI 0 STA UP 192.168.1.42\n");
    TEST_ASSERT_EQUAL_INT(0, alarm_mgr_active_count());
    TEST_ASSERT_EQUAL_INT(1, alarm_mgr_total());
}

static void test_malformed_no_node_ignored(void) {
    alarm_mgr_sink(NULL, "NOTIFY NODE\n");
    TEST_ASSERT_EQUAL_INT(0, alarm_mgr_total());
    TEST_ASSERT_EQUAL_INT(0, alarm_mgr_active_count());
}

static void test_malformed_unknown_type_ignored(void) {
    alarm_mgr_sink(NULL, "NOTIFY NOPE 2 WHATEVER\n");
    TEST_ASSERT_EQUAL_INT(0, alarm_mgr_total());
}

/* Prefix match on W_/F_ fault tokens: any first word starting with those two
 * chars activates the key regardless of what follows; OK clears it. */
static void test_fault_prefix_activates_and_ok_clears(void) {
    alarm_mgr_sink(NULL, "NOTIFY RING 0 W_LINK_LOST master silent 5 s\n");
    TEST_ASSERT_EQUAL_INT(1, alarm_mgr_active_count());
    alarm_mgr_sink(NULL, "NOTIFY RING 0 OK\n");
    TEST_ASSERT_EQUAL_INT(0, alarm_mgr_active_count());
}

/* The active set is keyed on <= 16 distinct "<TYPE> <node>" keys; a 17th+
 * distinct key is safely dropped (events keep recording regardless). */
static void test_active_set_caps_at_16_keys(void) {
    char line[64];
    for (int i = 0; i < 20; i++) {
        snprintf(line, sizeof line, "NOTIFY WATER %d FAILED\n", i);
        alarm_mgr_sink(NULL, line);
    }
    TEST_ASSERT_EQUAL_INT(16, alarm_mgr_active_count());
    TEST_ASSERT_EQUAL_INT(20, alarm_mgr_total());
}

int main(void) { UNITY_BEGIN();
    RUN_TEST(test_bench_sequence_active_set_and_events);
    RUN_TEST(test_active_json_shape_and_since_s);
    RUN_TEST(test_ring_wrap_keeps_newest_64);
    RUN_TEST(test_boot_is_event_only);
    RUN_TEST(test_wifi_is_event_only);
    RUN_TEST(test_malformed_no_node_ignored);
    RUN_TEST(test_malformed_unknown_type_ignored);
    RUN_TEST(test_fault_prefix_activates_and_ok_clears);
    RUN_TEST(test_active_set_caps_at_16_keys);
    return UNITY_END(); }
