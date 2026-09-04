#pragma once
#include <stdint.h>
#include "master_cmds.h"
#include "ring_proto.h"

/* Fake node_ops_t: tests poke the canned table/status directly (same style
 * as fake_app_if's exposed g_fake_app struct) rather than through setters. */
typedef struct {
    hg_node_t     nodes[HG_MAX_ZONES];               /* slot = id-1; .used=0 = empty */
    ring_status_t ring_status;
    uint8_t       unassigned_macs[HG_MAX_ZONES][6];
    int           unassigned_n;
    int           cfg_sync_failed[HG_MAX_ZONES + 1]; /* index by zone id 1..8 */
    int           time_valid;

    char    last_call[24];
    int     set_name_calls;  uint8_t set_name_zone;  char set_name_name[16];
    int     clear_calls;     uint8_t clear_zone;
    int     trace_calls;     int     trace_on;
    int     fw_zone_calls;   uint8_t fw_zone_arg;
    int     fw_all_calls;
    int     fw_abort_calls;
    int     fw_status_calls; char    fw_status_text[64];

    int fail_set_name, fail_clear;
    int fw_zone_rc, fw_all_rc, fw_abort_rc;   /* canned return values: 0 ok, -1 invalid, -2 busy */
} fake_node_ops_state_t;

extern fake_node_ops_state_t g_fake_nodes;
void fake_node_ops_reset(void);
extern const node_ops_t FAKE_NODE_OPS;
