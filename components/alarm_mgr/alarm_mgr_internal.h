#pragma once
/* Ring-buffer / active-set storage shared between alarm_mgr.c (owns and mutates
 * it via alarm_mgr_sink) and alarm_mgr_json.c (reads it to build the export
 * document). Not part of the public API -- alarm_mgr.h only. */
#include "alarm_mgr.h"

#define AM_ACTIVE_MAX 16
#define AM_KEY_MAX    16   /* "<TYPE> <node>": longest type name (ALARM/WATER/LIGHT) is 5 + ' ' + up to 3 digits */

typedef struct {
    char     key[AM_KEY_MAX];
    char     text[72];
    uint32_t since_s;
    uint8_t  used;
} am_active_t;

extern am_event_t  am_ring[AM_EVENTS];
extern uint32_t     am_total;             /* events ever recorded; am_total % AM_EVENTS is the next write slot */
extern am_active_t am_active[AM_ACTIVE_MAX];
