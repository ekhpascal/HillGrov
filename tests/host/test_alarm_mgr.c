#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "unity.h"
#include "alarm_mgr.h"
#include "fake_clock.h"
#include "cJSON.h"

void setUp(void) {
    fake_clock_set(100);
    alarm_mgr_init(fake_clock_now);
}
void tearDown(void) {
    alarm_mgr_set_lock(NULL, NULL);   /* the hooks are process-wide: never leak one test's into the next */
    cJSON_InitHooks(NULL);
}

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

/* Fleet FW lines nest a per-zone status after "ZONE <n>" (node_mgr_fleet.c:
 * "NOTIFY FW 0 ZONE 2 UPDATING" / "... DONE 0.1.0" / "... UPDATE_FAILED
 * <reason>"). The active-set key for these is "FW <n>" (the affected zone),
 * not "FW <node>" (the broadcasting node, here 0), and the word checked
 * against ACT_WORDS/CLR_WORDS is the one after "ZONE <n>". */
static void test_fw_zone_prefixed_lines_key_by_zone(void) {
    alarm_mgr_sink(NULL, "NOTIFY FW 0 ZONE 2 UPDATING\n");
    TEST_ASSERT_EQUAL_INT(1, alarm_mgr_active_count());

    char buf[2048];
    TEST_ASSERT_GREATER_THAN_INT(0, alarm_mgr_json(buf, sizeof buf));
    cJSON *root = cJSON_Parse(buf);
    TEST_ASSERT_NOT_NULL(root);
    cJSON *a0 = cJSON_GetArrayItem(cJSON_GetObjectItem(root, "active"), 0);
    TEST_ASSERT_EQUAL_STRING("FW 2", cJSON_GetObjectItem(a0, "key")->valuestring);
    cJSON_Delete(root);

    alarm_mgr_sink(NULL, "NOTIFY FW 0 ZONE 2 DONE 0.1.0\n");
    TEST_ASSERT_EQUAL_INT(0, alarm_mgr_active_count());

    alarm_mgr_sink(NULL, "NOTIFY FW 0 ZONE 2 UPDATE_FAILED PULL\n");
    TEST_ASSERT_EQUAL_INT(1, alarm_mgr_active_count());
}

/* The "ZONE <n> <state>" peel is gated on type == NTF_FW: a RING (or any
 * non-FW) line that merely happens to start with "ZONE 2 ..." must NOT be
 * re-keyed by the embedded zone number -- its state word stays "ZONE",
 * which matches neither ACT_WORDS nor CLR_WORDS, so "OPEN" is never
 * consulted and nothing activates under "RING 0" or "RING 2". An FW line
 * with the identical "ZONE <n> <state>" shape still peels and keys "FW 2". */
static void test_zone_peel_gated_to_fw_only(void) {
    alarm_mgr_sink(NULL, "NOTIFY RING 0 ZONE 2 OPEN\n");
    TEST_ASSERT_EQUAL_INT(0, alarm_mgr_active_count());

    alarm_mgr_sink(NULL, "NOTIFY FW 0 ZONE 2 UPDATING\n");
    TEST_ASSERT_EQUAL_INT(1, alarm_mgr_active_count());

    char buf[2048];
    TEST_ASSERT_GREATER_THAN_INT(0, alarm_mgr_json(buf, sizeof buf));
    cJSON *root = cJSON_Parse(buf);
    TEST_ASSERT_NOT_NULL(root);
    cJSON *a0 = cJSON_GetArrayItem(cJSON_GetObjectItem(root, "active"), 0);
    TEST_ASSERT_EQUAL_STRING("FW 2", cJSON_GetObjectItem(a0, "key")->valuestring);
    cJSON_Delete(root);
}

/* A non-numeric token after "ZONE" falls back to the plain rule: "ZONE"
 * itself becomes the state word, which matches neither ACT_WORDS nor
 * CLR_WORDS, so the line is still recorded as an event but never touches
 * the active set. */
static void test_fw_zone_nonnumeric_falls_back_to_plain_rule(void) {
    alarm_mgr_sink(NULL, "NOTIFY FW 0 ZONE X UPDATING\n");
    TEST_ASSERT_EQUAL_INT(0, alarm_mgr_active_count());
    TEST_ASSERT_EQUAL_INT(1, alarm_mgr_total());
}

static void test_node_over_255_ignored(void) {
    alarm_mgr_sink(NULL, "NOTIFY NODE 256 DEGRADED\n");
    TEST_ASSERT_EQUAL_INT(0, alarm_mgr_total());
    TEST_ASSERT_EQUAL_INT(0, alarm_mgr_active_count());
}

/* A line well over NTF_LINE_MAX (128) is still handled safely: the sink's
 * bounded work buffer truncates it before parsing, the event is still
 * recorded, and its text is capped at 71 chars (am_event_t.text[72]). */
static void test_long_line_truncated_safely(void) {
    char line[400];
    int pfx = snprintf(line, sizeof line, "NOTIFY CMD 0 ");
    memset(line + pfx, 'x', sizeof(line) - (size_t)pfx - 2);
    line[sizeof(line) - 2] = '\n';
    line[sizeof(line) - 1] = '\0';

    alarm_mgr_sink(NULL, line);
    TEST_ASSERT_EQUAL_INT(1, alarm_mgr_total());

    char buf[4096];
    TEST_ASSERT_GREATER_THAN_INT(0, alarm_mgr_json(buf, sizeof buf));
    cJSON *root = cJSON_Parse(buf);
    TEST_ASSERT_NOT_NULL(root);
    cJSON *e0 = cJSON_GetArrayItem(cJSON_GetObjectItem(root, "events"), 0);
    const char *text = cJSON_GetObjectItem(e0, "text")->valuestring;
    TEST_ASSERT_EQUAL_size_t(71, strlen(text));
    TEST_ASSERT_EQUAL_INT(0, strncmp(text, "CMD 0 ", 6));
    cJSON_Delete(root);
}

/* ---- panel plan Task 15: lock hooks + the struct snapshot ---- */

static int s_lock_n, s_unlock_n, s_depth, s_max_depth, s_malloc_held;
static void fake_lock(void)   { s_lock_n++; s_depth++; if (s_depth > s_max_depth) s_max_depth = s_depth; }
static void fake_unlock(void) { s_unlock_n++; s_depth--; }
static void *count_malloc(size_t n) { if (s_depth > 0) s_malloc_held++; return malloc(n); }
static void count_free(void *p) { free(p); }
static void hooks_reset(void) { s_lock_n = s_unlock_n = s_depth = s_max_depth = s_malloc_held = 0; }

/* The struct snapshot says exactly what /api/alarms says: same active set in
   the same order, same events newest first, same total -- after the ring has
   wrapped (70 events into 64 slots). */
static void test_copy_matches_json_after_the_ring_wraps(void) {
    char line[64];
    for (int i = 0; i < 70; i++) {
        fake_clock_set(100 + (uint32_t)i);
        snprintf(line, sizeof line, "NOTIFY NODE %d %s\n", 1 + (i % 3), (i % 2) ? "DEGRADED" : "ONLINE");
        alarm_mgr_sink(NULL, line);
    }
    static am_snapshot_t snap;
    alarm_mgr_copy(&snap);
    TEST_ASSERT_EQUAL_UINT32(70, snap.total);
    TEST_ASSERT_EQUAL_INT(AM_EVENTS, snap.n_events);
    TEST_ASSERT_EQUAL_STRING("NODE 1 DEGRADED", snap.events[0].text);   /* i = 69, the newest */
    TEST_ASSERT_EQUAL_INT(2, snap.n_active);                            /* NODE 1 and NODE 2 end DEGRADED, NODE 3 ONLINE */

    static char buf[8192];
    TEST_ASSERT_GREATER_THAN_INT(0, alarm_mgr_json(buf, sizeof buf));
    cJSON *root = cJSON_Parse(buf);
    TEST_ASSERT_NOT_NULL(root);
    cJSON *events = cJSON_GetObjectItem(root, "events");
    TEST_ASSERT_EQUAL_INT(snap.n_events, cJSON_GetArraySize(events));
    for (int i = 0; i < snap.n_events; i++) {
        cJSON *e = cJSON_GetArrayItem(events, i);
        TEST_ASSERT_EQUAL_STRING(snap.events[i].text, cJSON_GetObjectItem(e, "text")->valuestring);
        TEST_ASSERT_EQUAL_UINT32(snap.events[i].at_s, (uint32_t)cJSON_GetObjectItem(e, "at_s")->valuedouble);
    }
    cJSON *active = cJSON_GetObjectItem(root, "active");
    TEST_ASSERT_EQUAL_INT(snap.n_active, cJSON_GetArraySize(active));
    for (int i = 0; i < snap.n_active; i++) {
        cJSON *a = cJSON_GetArrayItem(active, i);
        TEST_ASSERT_EQUAL_STRING(snap.active[i].key, cJSON_GetObjectItem(a, "key")->valuestring);
        TEST_ASSERT_EQUAL_STRING(snap.active[i].text, cJSON_GetObjectItem(a, "text")->valuestring);
        TEST_ASSERT_EQUAL_UINT32(snap.active[i].since_s, (uint32_t)cJSON_GetObjectItem(a, "since_s")->valuedouble);
    }
    TEST_ASSERT_EQUAL_INT((int)snap.total, alarm_mgr_total());
    cJSON_Delete(root);
}

/* Every entry point takes the injected lock exactly once, never nested, and
   always releases it; a malformed line is rejected before any lock. */
static void test_lock_hooks_balance_on_every_entry_point(void) {
    hooks_reset();
    alarm_mgr_set_lock(fake_lock, fake_unlock);
    alarm_mgr_sink(NULL, "NOTIFY NODE 2 DEGRADED\n");
    (void)alarm_mgr_active_count();
    (void)alarm_mgr_total();
    static am_snapshot_t snap;
    alarm_mgr_copy(&snap);
    static char buf[4096];
    (void)alarm_mgr_json(buf, sizeof buf);
    alarm_mgr_sink(NULL, "NOTIFY BOOT 0 0.5.0 POWERON\n");   /* event-only: still one locked mutation */
    alarm_mgr_sink(NULL, "garbage");                          /* malformed: no lock at all */
    TEST_ASSERT_EQUAL_INT(6, s_lock_n);                       /* sink, count, total, copy, json (its copy), sink */
    TEST_ASSERT_EQUAL_INT(s_lock_n, s_unlock_n);
    TEST_ASSERT_EQUAL_INT(1, s_max_depth);
    TEST_ASSERT_EQUAL_INT(0, s_depth);
}

/* The lock covers the copy only: every cJSON allocation during
   alarm_mgr_json happens with no lock held. */
static void test_json_formats_outside_the_lock(void) {
    alarm_mgr_sink(NULL, "NOTIFY NODE 2 DEGRADED\n");
    alarm_mgr_sink(NULL, "NOTIFY RING 0 OPEN Z2 dead or wire Z2->Z1\n");
    hooks_reset();
    alarm_mgr_set_lock(fake_lock, fake_unlock);
    cJSON_Hooks h = { count_malloc, count_free };
    cJSON_InitHooks(&h);
    static char buf[4096];
    TEST_ASSERT_GREATER_THAN_INT(0, alarm_mgr_json(buf, sizeof buf));
    TEST_ASSERT_EQUAL_INT(1, s_lock_n);
    TEST_ASSERT_EQUAL_INT(0, s_malloc_held);
}

/* The -1 path (here: an output buffer too small for the document) leaves the
   lock balanced and taken exactly once -- the heap snapshot is freed on it
   too. (The snapshot's own malloc cannot be failed from here: it is plain
   malloc, which cJSON's hooks do not reach.) */
static void test_json_failure_returns_minus_one_lock_balanced(void) {
    alarm_mgr_sink(NULL, "NOTIFY NODE 2 DEGRADED\n");
    hooks_reset();
    alarm_mgr_set_lock(fake_lock, fake_unlock);
    char tiny[8];
    TEST_ASSERT_EQUAL_INT(-1, alarm_mgr_json(tiny, sizeof tiny));
    TEST_ASSERT_EQUAL_INT(1, s_lock_n);
    TEST_ASSERT_EQUAL_INT(s_lock_n, s_unlock_n);
    TEST_ASSERT_EQUAL_INT(0, s_depth);
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
    RUN_TEST(test_fw_zone_prefixed_lines_key_by_zone);
    RUN_TEST(test_zone_peel_gated_to_fw_only);
    RUN_TEST(test_fw_zone_nonnumeric_falls_back_to_plain_rule);
    RUN_TEST(test_node_over_255_ignored);
    RUN_TEST(test_long_line_truncated_safely);
    RUN_TEST(test_copy_matches_json_after_the_ring_wraps);
    RUN_TEST(test_lock_hooks_balance_on_every_entry_point);
    RUN_TEST(test_json_formats_outside_the_lock);
    RUN_TEST(test_json_failure_returns_minus_one_lock_balanced);
    return UNITY_END(); }
