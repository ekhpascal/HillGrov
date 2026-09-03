#pragma once

#include <stdint.h>
#include <stddef.h>
#include "ring_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HG_MAGIC_ZTAB 0x545A4748u  /* "HGZT" LE */

#define ZTAB_F_ASSIGNED     0x01
#define ZTAB_F_UNCONFIGURED 0x02

typedef struct {
    uint8_t mac[6];
    uint8_t id;
    uint8_t flags;
    char name[16];
} ztab_ent_t;  /* 24 B packed */

typedef struct {
    ztab_ent_t e[HG_MAX_ZONES];
} ztab_t;  /* 192 B */

_Static_assert(sizeof(ztab_ent_t) == 24, "ztab entry layout");

/* Pure zone table operations */
int ztab_find_mac(const ztab_t *t, const uint8_t mac[6]);        /* slot index or -1 */
int ztab_find_id(const ztab_t *t, uint8_t id);                   /* slot index or -1 */
int ztab_assign(ztab_t *t, const uint8_t mac[6]);                /* lowest free id 1..8; sets ASSIGNED|UNCONFIGURED; returns id or -1 full */
int ztab_set_name(ztab_t *t, uint8_t id, const char *name);      /* -1 unknown id / name too long (15 max) */
int ztab_clear(ztab_t *t, uint8_t id);                           /* forget the entry (spec §4.5: retire) */

typedef enum {
    ZTAB_EN_KNOWN = 0,      /* MAC already has this id -> re-send ASSIGN_ID */
    ZTAB_EN_ASSIGNED,        /* new MAC assigned (out_id) -> save + ASSIGN_ID + NOTIFY NODE NEW */
    ZTAB_EN_CONFLICT,        /* claimed id owned by another MAC -> reset intruder to 0xFE */
    ZTAB_EN_STALE,           /* known MAC heartbeating with wrong/0xFE id -> re-send ASSIGN_ID */
    ZTAB_EN_FULL
} ztab_en_t;

ztab_en_t ztab_enrol(ztab_t *t, const uint8_t mac[6], uint8_t claimed_id, uint8_t *out_id);
/* the whole spec 2.8 decision table in one pure function; node_mgr only acts on the verdict */

/* Pack/unpack with envelope */
int ztab_pack(const ztab_t *t, uint32_t gen, uint8_t *out, size_t cap);   /* envelope-wrapped; 16+192 */
int ztab_unpack(const uint8_t *in, size_t n, ztab_t *t, uint32_t *gen);   /* full envelope checks */

/* target glue (node_store_nvs.c): namespace "hg", key "ztab" */
int  node_store_load(ztab_t *t);        /* 0 ok, -1 absent/corrupt (caller starts empty) */
int  node_store_save(const ztab_t *t);  /* wrap with incrementing gen + nvs_set_blob + commit */

#ifdef __cplusplus
}
#endif
