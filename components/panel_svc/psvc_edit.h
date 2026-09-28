#pragma once
#include <stdint.h>
#include "hg_cfg.h"   /* hg_field_t */

#ifdef __cplusplus
extern "C" {
#endif

/* The ONE field-edit record, for zone rows (HG_FIELDS) and master rows
 * (HG_MFIELDS) alike, used by both faces. A save applies a SET of these to a
 * fresh copy taken at save time, never a whole edited struct: the web PUT has
 * no generation check, so writes are last-writer-wins per field (panel plan
 * Global Constraints, "Saves"). Pure. */

/* Positive refusals an edit function returns. mcfg_ops.h reserves every
 * NEGATIVE return of an edit fn for itself, so these must stay > 0. */
#define PSVC_EDIT_BAD_JSON      1
#define PSVC_EDIT_INVALID_FIELD 2
#define PSVC_EDIT_VALIDATION    3

#define PSVC_FEDIT_TEXT_MAX     65    /* longest field text + NUL (STA_PASS/AP_PASS capacity 64) */

typedef struct {
    uint8_t           group;          /* zone: hg_group_t; master: hg_mgroup_t */
    int8_t            idx;            /* shelf 0..3 / aux 0..1; -1 for zone scope 0 and every master row */
    const hg_field_t *f;              /* the HG_FIELDS / HG_MFIELDS row */
    char              text[PSVC_FEDIT_TEXT_MAX];   /* hg_field_write() text form; NUL-terminated */
} psvc_fedit_t;

typedef struct { const psvc_fedit_t *e; int n; } psvc_fedits_t;

#ifdef __cplusplus
}
#endif
