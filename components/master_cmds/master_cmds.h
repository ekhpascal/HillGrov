#pragma once
#include <stdint.h>
#include <stddef.h>
#include "cmd_core.h"
#include "ring_proto.h"     /* hg_node_t, ring_status_t */
#include "hg_mcfg.h"        /* hg_mcfg_t */
#include "wifi_mgr.h"       /* wifi_status_t -- plain C types only, no IDF headers */

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

/* NET/TIME CLI rows (GET WIFI, SET WIFI STA/AP, SET WEB PASSWORD, GET/SET TZ,
 * SET NODE <z> MAC) reach wifi_mgr / mcfg_store / web_auth / node_mgr only
 * through this second ops struct, for the same reason node_ops_t exists: the
 * rows stay host-testable against a fake with no IDF header anywhere near
 * them. master/main/net_ops_master.c holds the production implementation
 * (mcfg copy -> modify -> mcfg_commit -> wifi_mgr_apply/time_svc_apply_mcfg).
 *
 * Every int-returning member is 0 on success and -1 on "the caller asked for
 * something invalid" (a value mcfg_commit's validator rejected, a password
 * outside 8..63, an unknown zone); the rows turn -1 into ERR INVALID, except
 * seed_mac's, which is ERR ZONE_UNKNOWN.
 *
 * The four members that persist something may also return -2, "valid, but it
 * could not be stored" -- an NVS write failure or a commit/ops mutex timeout.
 * That is a different thing to tell an operator than "your value is wrong", so
 * the rows answer ERR STORAGE for it: retry or check the flash, don't retype
 * the value.
 *
 * set_web_password may additionally return -3, "this board cannot hash a
 * password at all" (no SHA-256 provider), which the rows report as the
 * existing ERR INTERNAL -- neither the value nor the flash is at fault and no
 * amount of retrying helps. Nothing is committed in that case. */
typedef struct {
    void (*get_mcfg)(hg_mcfg_t *out);                      /* snapshot copy; never a live pointer */
    int  (*set_sta)(const char *ssid, const char *pass);   /* pass "" = open network */
    int  (*set_ap)(const char *ssid, const char *pass);
    int  (*set_web_password)(const char *pw);
    int  (*set_tz)(const char *tz);
    void (*wifi_status)(wifi_status_t *out);
    int  (*seed_mac)(uint8_t zone, const uint8_t mac[6]);  /* node_mgr_seed_mac (Task 9) */
} net_ops_t;

/* SP3 entry point, kept so nothing that only wants the RING/FW rows has to
 * care about net ops: equivalent to master_cmds_init2(ops, NULL), and with a
 * NULL net the six NET/TIME rows answer ERR INTERNAL instead of crashing. */
void master_cmds_init(const node_ops_t *ops);
void master_cmds_init2(const node_ops_t *ops, const net_ops_t *net);

extern const cmd_entry_t MASTER_CMD_ROWS[];
extern const int         MASTER_CMD_ROWS_N;

#ifdef __cplusplus
}
#endif
