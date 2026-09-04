#include <stdio.h>
#include <string.h>
#include "fake_node_ops.h"

fake_node_ops_state_t g_fake_nodes;

void fake_node_ops_reset(void) { memset(&g_fake_nodes, 0, sizeof g_fake_nodes); }

static int f_node_count(void) {
    strcpy(g_fake_nodes.last_call, "node_count");
    int n = 0;
    for (int i = 0; i < HG_MAX_ZONES; i++) if (g_fake_nodes.nodes[i].used) n++;
    return n;
}

static int f_get(int slot, hg_node_t *out) {
    strcpy(g_fake_nodes.last_call, "get");
    if (slot < 0 || slot >= HG_MAX_ZONES || !g_fake_nodes.nodes[slot].used) return -1;
    *out = g_fake_nodes.nodes[slot];
    return 0;
}

static void f_ring_status(ring_status_t *out) {
    strcpy(g_fake_nodes.last_call, "ring_status");
    *out = g_fake_nodes.ring_status;
}

static int f_set_name(uint8_t zone, const char *name) {
    strcpy(g_fake_nodes.last_call, "set_name");
    g_fake_nodes.set_name_calls++;
    g_fake_nodes.set_name_zone = zone;
    snprintf(g_fake_nodes.set_name_name, sizeof g_fake_nodes.set_name_name, "%s", name);
    return g_fake_nodes.fail_set_name ? -1 : 0;
}

static int f_clear(uint8_t zone) {
    strcpy(g_fake_nodes.last_call, "clear");
    g_fake_nodes.clear_calls++;
    g_fake_nodes.clear_zone = zone;
    return g_fake_nodes.fail_clear ? -1 : 0;
}

static int f_unassigned(uint8_t macs[][6], int cap) {
    strcpy(g_fake_nodes.last_call, "unassigned");
    int n = g_fake_nodes.unassigned_n < cap ? g_fake_nodes.unassigned_n : cap;
    for (int i = 0; i < n; i++) memcpy(macs[i], g_fake_nodes.unassigned_macs[i], 6);
    return n;
}

static void f_trace(int on) {
    strcpy(g_fake_nodes.last_call, "trace");
    g_fake_nodes.trace_calls++;
    g_fake_nodes.trace_on = on;
}

static int f_time_valid(void) {
    strcpy(g_fake_nodes.last_call, "time_valid");
    return g_fake_nodes.time_valid;
}

static int f_cfg_sync_failed(uint8_t zone) {
    strcpy(g_fake_nodes.last_call, "cfg_sync_failed");
    if (zone < 1 || zone > HG_MAX_ZONES) return 0;
    return g_fake_nodes.cfg_sync_failed[zone];
}

static int f_fw_zone(uint8_t zone) {
    strcpy(g_fake_nodes.last_call, "fw_zone");
    g_fake_nodes.fw_zone_calls++;
    g_fake_nodes.fw_zone_arg = zone;
    return g_fake_nodes.fw_zone_rc;
}

static int f_fw_all(void) {
    strcpy(g_fake_nodes.last_call, "fw_all");
    g_fake_nodes.fw_all_calls++;
    return g_fake_nodes.fw_all_rc;
}

static int f_fw_abort(void) {
    strcpy(g_fake_nodes.last_call, "fw_abort");
    g_fake_nodes.fw_abort_calls++;
    return g_fake_nodes.fw_abort_rc;
}

static int f_fw_status(char *buf, size_t n) {
    strcpy(g_fake_nodes.last_call, "fw_status");
    g_fake_nodes.fw_status_calls++;
    snprintf(buf, n, "%s", g_fake_nodes.fw_status_text);
    return 0;
}

const node_ops_t FAKE_NODE_OPS = {
    .node_count      = f_node_count,
    .get             = f_get,
    .ring_status     = f_ring_status,
    .set_name        = f_set_name,
    .clear           = f_clear,
    .unassigned      = f_unassigned,
    .trace           = f_trace,
    .time_valid      = f_time_valid,
    .cfg_sync_failed = f_cfg_sync_failed,
    .fw_zone         = f_fw_zone,
    .fw_all          = f_fw_all,
    .fw_abort        = f_fw_abort,
    .fw_status       = f_fw_status,
};
