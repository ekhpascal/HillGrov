#pragma once
#include <stdint.h>
#include <stddef.h>
#include "cmd_core.h"
#include "ring_proto.h"     /* hg_node_t, ring_status_t */

#ifdef __cplusplus
extern "C" {
#endif

/* RING/NODES CLI rows (spec §5.4). node_mgr is reached only through this
 * thin ops struct so the rows stay host-testable against a fake -- no IDF
 * header, no direct node_mgr.h dependency here. Two members beyond the
 * brief's own snippet, both needed for a row to actually work and both kept
 * host-testable the same way as every other member here:
 *   - time_valid: GET RING's "TIME <VALID|NONE>" field (node_mgr.h's own
 *     node_mgr_time_valid() comment: "for GET RING display").
 *   - cfg_sync_failed: GET NODE's "CfgSync" field (Task 14 controller
 *     ruling), backed by node_mgr_cfg_sync_failed() (the §4.4 CFG_SYNC
 *     failure latch). */
typedef struct {
    int  (*node_count)(void);
    int  (*get)(int slot, hg_node_t *out);
    void (*ring_status)(ring_status_t *out);
    int  (*set_name)(uint8_t zone, const char *name);
    int  (*clear)(uint8_t zone);
    int  (*unassigned)(uint8_t macs[][6], int cap);
    void (*trace)(int on);
    int  (*time_valid)(void);
    int  (*cfg_sync_failed)(uint8_t zone);
    int  (*fw_zone)(uint8_t zone);    /* Task 15 fills; stub/fake returns -1 */
    int  (*fw_all)(void);
    int  (*fw_abort)(void);
    int  (*fw_status)(char *buf, size_t n);
} node_ops_t;

void master_cmds_init(const node_ops_t *ops);

extern const cmd_entry_t MASTER_CMD_ROWS[];
extern const int         MASTER_CMD_ROWS_N;

#ifdef __cplusplus
}
#endif
