/* The panel and /api/state now share ONE gather (psvc_state_fill) and ONE
   conversion to the web's JSON block (psvc_state_to_snap). The conversion is
   pure and is pinned here to produce EXACTLY the bytes a hand-built
   snap_master_t does -- that is the web's "same bytes on the wire" guarantee
   for this extraction. The two text parsers are h_state's own, moved. */
#include <stdio.h>
#include <string.h>
#include "unity.h"
#include "cJSON.h"
#include "state_snap.h"
#include "psvc_state.h"
#include "psvc_fw.h"

void setUp(void) { psvc_fw_progress_set("", 0); }
void tearDown(void) {}

typedef struct { char buf[6144]; size_t len; } sink_t;
static int sink(void *ctx, const char *b, size_t n) {
    sink_t *s = (sink_t *)ctx;
    if (s->len + n >= sizeof s->buf) return -1;
    memcpy(s->buf + s->len, b, n);
    s->len += n;
    s->buf[s->len] = '\0';
    return 0;
}

static void test_parse_time_noted_real_shape(void) {
    char t[20], src[8];
    psvc_parse_time_noted("2026-09-23 10:00:00 NTP 12", t, src);
    TEST_ASSERT_EQUAL_STRING("2026-09-23 10:00:00", t);
    TEST_ASSERT_EQUAL_STRING("NTP", src);
    psvc_parse_time_noted("1970-01-01 00:03:28 NONE 0", t, src);
    TEST_ASSERT_EQUAL_STRING("NONE", src);
}

static void test_parse_time_noted_short_and_garbage(void) {
    char t[20], src[8];
    psvc_parse_time_noted("1970-01", t, src);
    TEST_ASSERT_EQUAL_STRING("1970-01", t);
    TEST_ASSERT_EQUAL_STRING("", src);
    psvc_parse_time_noted("", t, src);
    TEST_ASSERT_EQUAL_STRING("", t);
    TEST_ASSERT_EQUAL_STRING("", src);
    psvc_parse_time_noted(NULL, t, src);
    TEST_ASSERT_EQUAL_STRING("", t);
    psvc_parse_time_noted("2026-09-23 10:00:00 AVERYLONGSOURCE 1", t, src);
    TEST_ASSERT_EQUAL_STRING("AVERYLO", src);   /* %7s: bounded, never overflows src[8] */
}

static void test_parse_fw_info(void) {
    char slot[16], state[16], other[16];
    psvc_parse_fw_info("0.4.0 ota_0 PENDING NONE", slot, state, other);
    TEST_ASSERT_EQUAL_STRING("ota_0", slot);
    TEST_ASSERT_EQUAL_STRING("PENDING", state);
    TEST_ASSERT_EQUAL_STRING("NONE", other);
    psvc_parse_fw_info("0.4.0 ota_1", slot, state, other);
    TEST_ASSERT_EQUAL_STRING("ota_1", slot);
    TEST_ASSERT_EQUAL_STRING("", state);
    psvc_parse_fw_info("garbage", slot, state, other);
    TEST_ASSERT_EQUAL_STRING("", slot);
    psvc_parse_fw_info(NULL, slot, state, other);
    TEST_ASSERT_EQUAL_STRING("", slot);
}

static void fill_node(hg_node_t *n, uint8_t id) {
    memset(n, 0, sizeof *n);
    n->used = 1;
    n->id = id;
    snprintf(n->name, sizeof n->name, "Z%u\"q", (unsigned)id);   /* a quote: the escaper must run the same both ways */
    n->health = NODE_H_ONLINE;
    n->last_hb_ms = 1000;
    n->hb.fw_min = 1;
    n->hb.n_shelves = 1;
    n->hb.shelf[0].pct_a = 41;
}

static void test_to_snap_is_byte_identical_to_a_hand_built_block(void) {
    static psvc_state_t s;
    memset(&s, 0, sizeof s);
    snprintf(s.version, sizeof s.version, "0.5.0");
    s.uptime_s = 1234; s.heap_min_kb = 33000;
    snprintf(s.time, sizeof s.time, "2026-09-23 10:00:00");
    snprintf(s.time_src, sizeof s.time_src, "NTP");
    s.wifi.sta_up = 1;
    snprintf(s.wifi.sta_ip, sizeof s.wifi.sta_ip, "192.168.1.5");
    snprintf(s.wifi.sta_ssid, sizeof s.wifi.sta_ssid, "house");
    s.wifi.rssi = -61;
    s.wifi.sta_reason[0] = '\0';
    snprintf(s.wifi.ap_ssid, sizeof s.wifi.ap_ssid, "HillGrow");
    s.wifi.ap_clients = 2;
    snprintf(s.wifi.ap_ip, sizeof s.wifi.ap_ip, "192.168.7.7");
    snprintf(s.fw_slot, sizeof s.fw_slot, "ota_1");
    snprintf(s.fw_state, sizeof s.fw_state, "PENDING");
    snprintf(s.fw_other, sizeof s.fw_other, "NONE");
    snprintf(s.upload_kind, sizeof s.upload_kind, "zone");
    s.upload_pct = 40;
    snprintf(s.fleet_line, sizeof s.fleet_line, "2 UPDATING");
    s.alarms_active = 1; s.alarms_total = 9;
    s.web_default = 1; s.ap_default = 0;
    s.web_cmd_quarantined = 2;
    fill_node(&s.node[1], 2);
    s.cfg_sync_failed[1] = 1;
    s.ring.state = RING_ST_OK; s.ring.size = 2; s.ring.online_mask = 6;
    s.now_ms = 5000;

    snap_master_t conv;
    psvc_state_to_snap(&s, &conv);
    static sink_t a;
    memset(&a, 0, sizeof a);
    TEST_ASSERT_EQUAL_INT(0, state_snap_write(&conv, s.node, HG_MAX_ZONES, &s.ring, s.cfg_sync_failed, s.now_ms, sink, &a));

    snap_master_t m;
    memset(&m, 0, sizeof m);
    m.version = "0.5.0"; m.uptime_s = 1234; m.heap_min_kb = 33000;
    snprintf(m.time, sizeof m.time, "2026-09-23 10:00:00");
    m.time_src = "NTP";
    m.sta.up = 1;
    snprintf(m.sta.ip, sizeof m.sta.ip, "192.168.1.5");
    snprintf(m.sta.ssid, sizeof m.sta.ssid, "house");
    m.sta.rssi = -61;
    snprintf(m.ap.ssid, sizeof m.ap.ssid, "HillGrow");
    m.ap.clients = 2;
    snprintf(m.ap.ip, sizeof m.ap.ip, "192.168.7.7");
    m.fw.slot = "ota_1"; m.fw.state = "PENDING"; m.fw.other = "NONE";
    m.fw.upload_kind = "zone"; m.fw.upload_pct = 40;
    m.fleet_line = "2 UPDATING";
    m.alarms_active = 1; m.alarms_total = 9;
    m.web_default = 1; m.ap_default = 0;
    m.cmd_quarantined = 2;
    static sink_t b;
    memset(&b, 0, sizeof b);
    TEST_ASSERT_EQUAL_INT(0, state_snap_write(&m, s.node, HG_MAX_ZONES, &s.ring, s.cfg_sync_failed, s.now_ms, sink, &b));

    TEST_ASSERT_EQUAL_STRING(b.buf, a.buf);

    cJSON *root = cJSON_Parse(a.buf);
    TEST_ASSERT_NOT_NULL(root);
    cJSON *master = cJSON_GetObjectItem(root, "master");
    TEST_ASSERT_EQUAL_INT(2, cJSON_GetObjectItem(cJSON_GetObjectItem(master, "http"), "cmd_quarantined")->valueint);
    TEST_ASSERT_EQUAL_STRING("zone", cJSON_GetObjectItem(cJSON_GetObjectItem(master, "fw"), "upload_kind")->valuestring);
    TEST_ASSERT_EQUAL_STRING("PENDING", cJSON_GetObjectItem(cJSON_GetObjectItem(master, "fw"), "state")->valuestring);
    TEST_ASSERT_EQUAL_INT(1, cJSON_GetArraySize(cJSON_GetObjectItem(root, "nodes")));
    cJSON_Delete(root);
}

static void test_progress_publication_and_clamp(void) {
    const char *k = "x";
    uint8_t p = 99;
    TEST_ASSERT_EQUAL_INT(0, psvc_fw_progress(&k, &p));
    TEST_ASSERT_EQUAL_STRING("", k);
    TEST_ASSERT_EQUAL_UINT8(0, p);
    psvc_fw_progress_set("zone", 150);
    TEST_ASSERT_EQUAL_INT(1, psvc_fw_progress(&k, &p));
    TEST_ASSERT_EQUAL_STRING("zone", k);
    TEST_ASSERT_EQUAL_UINT8(100, p);
    psvc_fw_progress_set(NULL, 0);
    TEST_ASSERT_EQUAL_INT(0, psvc_fw_progress(&k, &p));
    TEST_ASSERT_EQUAL_STRING("", k);
    TEST_ASSERT_EQUAL_INT(0, psvc_fw_progress(NULL, NULL));   /* NULL outs are allowed */
}

int main(void) { UNITY_BEGIN();
    RUN_TEST(test_parse_time_noted_real_shape);
    RUN_TEST(test_parse_time_noted_short_and_garbage);
    RUN_TEST(test_parse_fw_info);
    RUN_TEST(test_to_snap_is_byte_identical_to_a_hand_built_block);
    RUN_TEST(test_progress_publication_and_clamp);
    return UNITY_END(); }
