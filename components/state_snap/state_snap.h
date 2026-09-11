#pragma once
#include <stdint.h>
#include <stddef.h>
#include "ring_proto.h"

/* Master block for /api/state (SP4 web UI, spec Task 10). Every string
   member is either a fixed-size char[] the caller snprintf()s into, or a
   const char* the caller owns for the duration of the state_snap_write()
   call (never stored). */
typedef struct {
    const char *version;
    uint32_t    uptime_s, heap_min_kb;
    char        time[20];        /* "YYYY-MM-DD HH:MM:SS" */
    const char *time_src;        /* "NTP" | "SET" | "NONE" */
    struct { uint8_t up; char ip[16]; char ssid[33]; int8_t rssi; char reason[24]; } sta;
    struct { char ssid[33]; uint8_t clients; char ip[16]; } ap;
    struct { const char *slot, *state, *other; const char *upload_kind; uint8_t upload_pct; } fw;
                                  /* upload_kind: "" | "master" | "zone" */
    const char *fleet_line;      /* node_mgr_fw_status() text */
    int         alarms_active, alarms_total;
    /* Controller ruling 2026-09-10 (pre-flight): the dashboard shows a
       "still on the factory password" banner while either is true. Task 12
       fills these from mcfg_get()->flags -- not derivable in this pure
       component. */
    uint8_t     web_default, ap_default;
} snap_master_t;

typedef int (*snap_write_fn)(void *ctx, const char *buf, size_t n);   /* 0 ok / -1 abort */

/* Streaming /api/state JSON writer: {"master":{...},"nodes":[...],"ring":{...}}.
 * Pure: never allocates, no IDF headers. Every w() call carries at most
 * 1024 bytes of buf (a fresh 1 KB scratch is snprintf'd per JSON segment --
 * roughly one per node -- then handed to w() whole).
 *
 * tab/n_slots: the node table (e.g. node_mgr's, or a test fixture); slots
 * with used==0 are skipped, not emitted.
 *
 * cfg_sync_failed: NULL, or an array of HG_MAX_ZONES (8) entries indexed by
 * zone id-1 (id 1..8 -> index 0..7); 0 = last CFG sync OK, nonzero =
 * FAILED. hg_node_t carries no such flag itself -- Task 12 fills the array
 * from node_mgr_cfg_sync_failed(zone). NULL means "treat every zone as OK".
 *
 * Returns 0 on success. Returns -1 the first time w() returns nonzero (no
 * further writes are attempted), or -- defensively, should a caller-supplied
 * string ever be implausibly long -- if a JSON segment would not fit its
 * 1 KB scratch buffer; a node object fits by construction for the worst
 * case (15-char name, all quotes/backslashes, 4 shelves).
 */
int state_snap_write(const snap_master_t *m, const hg_node_t *tab, int n_slots, const ring_status_t *rs,
                     const uint8_t *cfg_sync_failed, uint32_t now_ms, snap_write_fn w, void *ctx);
