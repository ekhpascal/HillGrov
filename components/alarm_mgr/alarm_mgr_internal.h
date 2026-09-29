#pragma once
/* Ring-buffer / active-set storage shared between alarm_mgr.c (owns and mutates
 * it via alarm_mgr_sink) and alarm_mgr_json.c (reads it to build the export
 * document). Not part of the public API -- alarm_mgr.h only. */
#include "alarm_mgr.h"

typedef struct {
    char     key[AM_KEY_MAX];
    char     text[72];
    uint32_t since_s;
    uint8_t  used;
} am_active_t;

extern am_event_t  am_ring[AM_EVENTS];
extern uint32_t     am_total;             /* events ever recorded; am_total % AM_EVENTS is the next write slot */
extern am_active_t am_active[AM_ACTIVE_MAX];

/* One-entry copies for alarm_mgr_json: each takes the lock for ONE ~92 B copy, so the export needs no whole snapshot.
 * am_copy_total(): am_total, read under the lock.
 * am_copy_active(): 1 and *out filled when active slot `slot` is in use, else 0.
 * am_copy_event(): the i-th newest event as of `total` (an am_copy_total() result), newest first. 1 and *out filled;
 *   0 when i is past the events kept at `total`, or when that event has since been overwritten by newer ones. */
uint32_t am_copy_total(void);
int      am_copy_active(int slot, am_active_view_t *out);
int      am_copy_event(uint32_t total, uint32_t i, am_event_t *out);
