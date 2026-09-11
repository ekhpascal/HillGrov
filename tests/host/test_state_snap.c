#include <stdio.h>
#include <string.h>
#include "unity.h"
#include "cJSON.h"
#include "state_snap.h"

/* ---- fixture: reuses test_master_cmds.c's set_node shape (hg_node_t is
 * filled directly here -- state_snap_write takes the table by pointer, no
 * injected node_ops_t, so no fake layer is needed). */

static const uint8_t MAC1[6] = { 0x24, 0x6F, 0x28, 0xAA, 0xBB, 0x01 };
static const uint8_t MAC2[6] = { 0x24, 0x6F, 0x28, 0xAA, 0xBB, 0x02 };

static void set_node(hg_node_t *nd, uint8_t id, const char *name, const uint8_t mac[6], node_health_t health) {
    memset(nd, 0, sizeof *nd);
    nd->used = 1;
    nd->id = id;
    snprintf(nd->name, sizeof nd->name, "%s", name);
    memcpy(nd->mac, mac, 6);
    nd->health = health;
    nd->hops = id;
    nd->link_flags = 0x07;
    nd->last_hb_ms = 195000;
    nd->hb.fw_maj = 0; nd->hb.fw_min = 1; nd->hb.fw_patch = 0;
    nd->hb.cfg_gen = 13;
    nd->hb.uptime_s = 3600;
    nd->hb.min_free_heap_kb = 180;
    nd->hb.reset_reason = 1;
    nd->hb.mode = 0;
}

static snap_master_t default_master(void) {
    snap_master_t m;
    memset(&m, 0, sizeof m);
    m.version = "0.4.0";
    m.uptime_s = 12345;
    m.heap_min_kb = 180;
    snprintf(m.time, sizeof m.time, "2026-09-11 10:00:00");
    m.time_src = "NTP";
    m.sta.up = 1;
    snprintf(m.sta.ip, sizeof m.sta.ip, "192.168.1.50");
    snprintf(m.sta.ssid, sizeof m.sta.ssid, "HomeWiFi");
    m.sta.rssi = -55;
    m.sta.reason[0] = '\0';
    snprintf(m.ap.ssid, sizeof m.ap.ssid, "HillGrow-AP");
    m.ap.clients = 0;
    snprintf(m.ap.ip, sizeof m.ap.ip, "192.168.4.1");
    m.fw.slot = "A"; m.fw.state = "OK"; m.fw.other = "B: OK"; m.fw.upload_kind = "";
    m.fw.upload_pct = 0;
    m.fleet_line = "IDLE";
    m.alarms_active = 1;
    m.alarms_total = 3;
    m.web_default = 1;
    m.ap_default = 0;
    return m;
}

/* ---- collector: gathers every w() call into one buffer, and enforces the
 * "never more than 1 KB per write call" contract on every call, for every
 * test that uses it (brief step 1: "each write call <= 1024 bytes -- assert
 * in the collector"). */

typedef struct {
    char   buf[6144];      /* "6 KB buffer" per the brief */
    size_t len;
    int    calls;
    size_t max_call_n;
    int    fail_at;        /* 0 = never fail; else the 1-based call to abort on */
} collector_t;

static void collector_init(collector_t *c, int fail_at) {
    memset(c, 0, sizeof *c);
    c->fail_at = fail_at;
}

static int collect(void *ctx, const char *b, size_t n) {
    collector_t *c = (collector_t *)ctx;
    c->calls++;
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(1024, n);       /* the streaming contract */
    if (n > c->max_call_n) c->max_call_n = n;
    if (c->fail_at && c->calls == c->fail_at) return -1;
    TEST_ASSERT_TRUE(c->len + n < sizeof c->buf);
    memcpy(c->buf + c->len, b, n);
    c->len += n;
    c->buf[c->len] = '\0';
    return 0;
}

void setUp(void) {}
void tearDown(void) {}

/* ---- brief's case: two-node fixture, cJSON-parse the collected document,
 * check the master/nodes/ring shapes the brief calls out, including
 * link_stale (OFFLINE) and cfg_sync (driven off the cfg_sync_failed array,
 * a controller-ruling addition documented in state_snap.h). */
static void test_two_node_snapshot_shapes_and_values(void) {
    hg_node_t tab[2];
    set_node(&tab[0], 1, "Basil", MAC1, NODE_H_ONLINE);
    set_node(&tab[1], 2, "Mint",  MAC2, NODE_H_OFFLINE);

    ring_status_t rs;
    memset(&rs, 0, sizeof rs);
    rs.state = RING_ST_OK;
    rs.size = 2;
    rs.online_mask = 6;
    rs.blame[0] = '\0';

    snap_master_t m = default_master();

    uint8_t cfgf[HG_MAX_ZONES] = { 0 };
    cfgf[1] = 1;   /* zone id 2 (index 1) has a failed CFG sync */

    collector_t c;
    collector_init(&c, 0);
    int rc = state_snap_write(&m, tab, 2, &rs, cfgf, 200000, collect, &c);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_INT(4, c.calls);   /* master+open, node0, node1, ring+close */

    cJSON *root = cJSON_Parse(c.buf);
    TEST_ASSERT_NOT_NULL(root);

    cJSON *master = cJSON_GetObjectItem(root, "master");
    TEST_ASSERT_NOT_NULL(master);
    cJSON *sta = cJSON_GetObjectItem(cJSON_GetObjectItem(master, "wifi"), "sta");
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(sta, "up")));
    TEST_ASSERT_EQUAL_STRING("192.168.1.50", cJSON_GetObjectItem(sta, "ip")->valuestring);
    TEST_ASSERT_EQUAL_STRING("IDLE", cJSON_GetObjectItem(master, "fleet")->valuestring);
    cJSON *alarms = cJSON_GetObjectItem(master, "alarms");
    TEST_ASSERT_EQUAL_INT(1, cJSON_GetObjectItem(alarms, "active")->valueint);
    TEST_ASSERT_EQUAL_INT(3, cJSON_GetObjectItem(alarms, "total")->valueint);
    cJSON *defaults = cJSON_GetObjectItem(master, "defaults");
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(defaults, "web")));
    TEST_ASSERT_TRUE(cJSON_IsFalse(cJSON_GetObjectItem(defaults, "ap")));

    cJSON *nodes = cJSON_GetObjectItem(root, "nodes");
    TEST_ASSERT_TRUE(cJSON_IsArray(nodes));
    TEST_ASSERT_EQUAL_INT(2, cJSON_GetArraySize(nodes));

    cJSON *n0 = cJSON_GetArrayItem(nodes, 0);
    TEST_ASSERT_EQUAL_STRING("Basil", cJSON_GetObjectItem(n0, "name")->valuestring);
    TEST_ASSERT_EQUAL_STRING("24:6f:28:aa:bb:01", cJSON_GetObjectItem(n0, "mac")->valuestring);
    TEST_ASSERT_EQUAL_STRING("ONLINE", cJSON_GetObjectItem(n0, "health")->valuestring);
    TEST_ASSERT_FALSE(cJSON_IsTrue(cJSON_GetObjectItem(n0, "link_stale")));
    TEST_ASSERT_EQUAL_STRING("OK", cJSON_GetObjectItem(n0, "cfg_sync")->valuestring);
    TEST_ASSERT_EQUAL_INT(5, cJSON_GetObjectItem(n0, "hb_age_s")->valueint);   /* 200000-195000 ms */

    cJSON *n1 = cJSON_GetArrayItem(nodes, 1);
    TEST_ASSERT_EQUAL_STRING("Mint", cJSON_GetObjectItem(n1, "name")->valuestring);
    TEST_ASSERT_EQUAL_STRING("OFFLINE", cJSON_GetObjectItem(n1, "health")->valuestring);
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(n1, "link_stale")));     /* SP3 carry */
    TEST_ASSERT_EQUAL_STRING("FAILED", cJSON_GetObjectItem(n1, "cfg_sync")->valuestring);
    TEST_ASSERT_EQUAL_STRING("0.1.0", cJSON_GetObjectItem(n1, "fw")->valuestring);
    TEST_ASSERT_EQUAL_INT(13, cJSON_GetObjectItem(n1, "gen")->valueint);
    TEST_ASSERT_EQUAL_STRING("0x0", cJSON_GetObjectItem(n1, "faults")->valuestring);
    TEST_ASSERT_TRUE(cJSON_IsArray(cJSON_GetObjectItem(n1, "shelves")));
    TEST_ASSERT_EQUAL_INT(0, cJSON_GetArraySize(cJSON_GetObjectItem(n1, "shelves")));

    cJSON *ring = cJSON_GetObjectItem(root, "ring");
    TEST_ASSERT_EQUAL_STRING("OK", cJSON_GetObjectItem(ring, "state")->valuestring);
    TEST_ASSERT_EQUAL_INT(2, cJSON_GetObjectItem(ring, "size")->valueint);
    TEST_ASSERT_EQUAL_INT(6, cJSON_GetObjectItem(ring, "online")->valueint);
    TEST_ASSERT_EQUAL_STRING("", cJSON_GetObjectItem(ring, "blame")->valuestring);

    cJSON_Delete(root);
}

/* ---- escaping + worst-case node size: a 15-char name made entirely of
 * quotes and backslashes (each doubles on escape), plus 4 shelves (the max)
 * with maximal field widths. Confirms the node object round-trips through
 * cJSON to the exact original name, and that it fits by construction (no -1,
 * and -- via the collector -- every write call still <= 1024 bytes). */
static void test_escaping_and_max_size_node_fits(void) {
    char nasty[16];
    for (int i = 0; i < 15; i++) nasty[i] = (i % 2 == 0) ? '"' : '\\';
    nasty[15] = '\0';

    hg_node_t nd;
    set_node(&nd, 3, nasty, MAC1, NODE_H_ONLINE);
    nd.hb.cfg_gen = 4294967295u;
    nd.hops = 255;
    nd.link_flags = 255;
    nd.last_hb_ms = 0;
    nd.hb.uptime_s = 4294967295u;
    nd.hb.min_free_heap_kb = 65535;
    nd.hb.reset_reason = 255;
    nd.hb.active_faults = 0xFFFFFFFFFFFFFFFFull;
    nd.hb.mode = 255;
    nd.hb.n_shelves = 4;
    for (int i = 0; i < 4; i++) {
        hg_hb_shelf_t *s = &nd.hb.shelf[i];
        s->pct_a = 255; s->pct_b = 255; s->white = 255; s->red = 255;
        s->out_flags = 255; s->pump_today_s = 65535;
    }

    ring_status_t rs;
    memset(&rs, 0, sizeof rs);
    rs.state = RING_ST_IDLE;

    snap_master_t m = default_master();

    collector_t c;
    collector_init(&c, 0);
    int rc = state_snap_write(&m, &nd, 1, &rs, NULL, 4000000000u, collect, &c);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_LESS_THAN_UINT32(1024, c.max_call_n);   /* fits with room to spare */

    cJSON *root = cJSON_Parse(c.buf);
    TEST_ASSERT_NOT_NULL(root);
    cJSON *n0 = cJSON_GetArrayItem(cJSON_GetObjectItem(root, "nodes"), 0);
    TEST_ASSERT_EQUAL_STRING(nasty, cJSON_GetObjectItem(n0, "name")->valuestring);
    TEST_ASSERT_EQUAL_STRING("0xffffffffffffffff", cJSON_GetObjectItem(n0, "faults")->valuestring);
    TEST_ASSERT_EQUAL_INT(4, cJSON_GetArraySize(cJSON_GetObjectItem(n0, "shelves")));
    /* NULL cfg_sync_failed -> every zone treated as OK */
    TEST_ASSERT_EQUAL_STRING("OK", cJSON_GetObjectItem(n0, "cfg_sync")->valuestring);
    cJSON_Delete(root);
}

/* ---- writer abort: the 3rd w() call (the second node, "Mint" -- master+open
 * is call 1, Basil is call 2) returns -1; state_snap_write must abort
 * immediately and propagate -1, without ever reaching the ring/close call. */
static void test_writer_abort_on_third_call_propagates(void) {
    hg_node_t tab[2];
    set_node(&tab[0], 1, "Basil", MAC1, NODE_H_ONLINE);
    set_node(&tab[1], 2, "Mint",  MAC2, NODE_H_DEGRADED);

    ring_status_t rs;
    memset(&rs, 0, sizeof rs);
    rs.state = RING_ST_OK;
    rs.size = 2;

    snap_master_t m = default_master();

    collector_t c;
    collector_init(&c, 3);
    int rc = state_snap_write(&m, tab, 2, &rs, NULL, 200000, collect, &c);
    TEST_ASSERT_EQUAL_INT(-1, rc);
    TEST_ASSERT_EQUAL_INT(3, c.calls);   /* stopped right at the failing call */
}

/* ---- hb_age_s must not falsely reset to "just heard" across the ~49.7-day
 * millisecond wrap: last_hb_ms/now_ms are plain uint32 ms counters, so the
 * age has to come from unsigned modular subtraction (ring_health.c's own
 * convention: plain `now_ms - nd->last_hb_ms`), never a "now_ms >=
 * last_hb_ms" guard that discards the wrap and reports 0. */
static void test_hb_age_s_survives_ms_wraparound(void) {
    hg_node_t nd;
    set_node(&nd, 1, "Basil", MAC1, NODE_H_OFFLINE);
    nd.last_hb_ms = 0xFFFF0000u;

    ring_status_t rs;
    memset(&rs, 0, sizeof rs);
    rs.state = RING_ST_IDLE;
    snap_master_t m = default_master();

    collector_t c;
    collector_init(&c, 0);
    int rc = state_snap_write(&m, &nd, 1, &rs, NULL, 0x00010000u, collect, &c);
    TEST_ASSERT_EQUAL_INT(0, rc);

    cJSON *root = cJSON_Parse(c.buf);
    TEST_ASSERT_NOT_NULL(root);
    cJSON *n0 = cJSON_GetArrayItem(cJSON_GetObjectItem(root, "nodes"), 0);
    TEST_ASSERT_EQUAL_INT(131, cJSON_GetObjectItem(n0, "hb_age_s")->valueint);   /* 0x20000 ms / 1000 */
    cJSON_Delete(root);
}

/* Never-heard row (last_hb_ms == 0, e.g. loaded from NVS, never HB'd): there
 * is no real "last heard" instant to measure from, so state_snap reports the
 * plain arithmetic result (now_ms/1000) rather than a sentinel -- documented
 * here and at the computation site in state_snap.c. A dashboard showing a
 * very large hb_age_s next to health EMPTY/OFFLINE is self-explanatory. */
static void test_hb_age_s_never_heard_row_is_plain_now_ms(void) {
    hg_node_t nd;
    set_node(&nd, 1, "Basil", MAC1, NODE_H_OFFLINE);
    nd.last_hb_ms = 0;

    ring_status_t rs;
    memset(&rs, 0, sizeof rs);
    rs.state = RING_ST_IDLE;
    snap_master_t m = default_master();

    collector_t c;
    collector_init(&c, 0);
    int rc = state_snap_write(&m, &nd, 1, &rs, NULL, 100000u, collect, &c);
    TEST_ASSERT_EQUAL_INT(0, rc);

    cJSON *root = cJSON_Parse(c.buf);
    TEST_ASSERT_NOT_NULL(root);
    cJSON *n0 = cJSON_GetArrayItem(cJSON_GetObjectItem(root, "nodes"), 0);
    TEST_ASSERT_EQUAL_INT(100, cJSON_GetObjectItem(n0, "hb_age_s")->valueint);
    cJSON_Delete(root);
}

/* ---- master block worst-case size: every snap_master_t `const char*` (the
 * ones unbounded by the struct itself -- version/fleet_line/time_src/fw.*)
 * filled to the longest realistic producer string, plus the fixed char[]
 * fields (ssid/reason/ip) maxed out too, plus a 47-char ring blame (the
 * struct's char[48]). Zero nodes isolates the measurement to the master+open
 * write call. Confirms the master segment fits its 1 KB scratch and that the
 * resulting document still parses whole. */
static void test_master_block_worst_case_size_fits(void) {
    char version_buf[16];
    memset(version_buf, 'V', 15); version_buf[15] = '\0';
    char fleet_buf[16];
    memset(fleet_buf, 'F', 15); fleet_buf[15] = '\0';

    snap_master_t m = default_master();
    m.version = version_buf;
    m.fleet_line = fleet_buf;
    m.time_src = "NTP";
    m.fw.slot = "MASTER-A";        /* 8 */
    m.fw.state = "UPLOADNG";       /* 8 */
    m.fw.other = "ZONE-B12";       /* 8 */
    m.fw.upload_kind = "master";   /* 6, the longest of "" | "master" | "zone" */
    memset(m.sta.ssid, 'S', 32); m.sta.ssid[32] = '\0';
    memset(m.sta.reason, 'R', 23); m.sta.reason[23] = '\0';
    memset(m.ap.ssid, 'A', 32); m.ap.ssid[32] = '\0';
    snprintf(m.sta.ip, sizeof m.sta.ip, "255.255.255.255");
    snprintf(m.ap.ip, sizeof m.ap.ip, "255.255.255.255");

    ring_status_t rs;
    memset(&rs, 0, sizeof rs);
    rs.state = RING_ST_OPEN;
    memset(rs.blame, 'B', 47); rs.blame[47] = '\0';

    collector_t c;
    collector_init(&c, 0);
    int rc = state_snap_write(&m, NULL, 0, &rs, NULL, 100000, collect, &c);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(1024, c.max_call_n);

    cJSON *root = cJSON_Parse(c.buf);
    TEST_ASSERT_NOT_NULL(root);
    cJSON *master = cJSON_GetObjectItem(root, "master");
    TEST_ASSERT_EQUAL_STRING(version_buf, cJSON_GetObjectItem(master, "version")->valuestring);
    cJSON *ring = cJSON_GetObjectItem(root, "ring");
    TEST_ASSERT_EQUAL_size_t(47, strlen(cJSON_GetObjectItem(ring, "blame")->valuestring));
    TEST_ASSERT_EQUAL_INT(0, cJSON_GetArraySize(cJSON_GetObjectItem(root, "nodes")));
    cJSON_Delete(root);
}

int main(void) { UNITY_BEGIN();
    RUN_TEST(test_two_node_snapshot_shapes_and_values);
    RUN_TEST(test_escaping_and_max_size_node_fits);
    RUN_TEST(test_writer_abort_on_third_call_propagates);
    RUN_TEST(test_hb_age_s_survives_ms_wraparound);
    RUN_TEST(test_hb_age_s_never_heard_row_is_plain_now_ms);
    RUN_TEST(test_master_block_worst_case_size_fits);
    return UNITY_END(); }
