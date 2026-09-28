#pragma once
/* pcfg_edit.h -- the panel's dirty set (pure). A save applies ONLY these entries, on the worker, to a
 * fresh copy taken at save time: the web PUT has no generation check, so both faces are last-writer-wins
 * per field (map-cfg §6). Kept per zone in PSRAM; survives tab and destination changes. */
#include <stdint.h>
#include "hg_cfg.h"
#include "psvc_edit.h"
#include "pcfg_gen.h"
#ifdef __cplusplus
extern "C" {
#endif

#define PCFG_EDIT_MAX 128
typedef struct { pcfg_table_t table; uint8_t zone; int n; psvc_fedit_t d[PCFG_EDIT_MAX]; } pcfg_edits_t;  /* ~10 KB: PSRAM */
void pcfg_edits_reset(pcfg_edits_t *e, pcfg_table_t t, uint8_t zone);
/* replace-or-append keyed (group, idx, f); idx normalised (-1: master rows and scope-0 groups);
 * 0 / -1 full, text too long (>= PSVC_FEDIT_TEXT_MAX), idx outside -1..127 or NULL args / -2 hardware-plane row */
int  pcfg_edits_set(pcfg_edits_t *e, uint8_t group, int idx, const hg_field_t *f, const char *text);
int  pcfg_edits_drop(pcfg_edits_t *e, uint8_t group, int idx, const hg_field_t *f);   /* 0 / -1 absent; order kept */
const psvc_fedit_t *pcfg_edits_get(const pcfg_edits_t *e, uint8_t group, int idx, const hg_field_t *f);  /* NULL = clean */
/* n copied (insertion order); master table: a blank secret is dropped (blank = unchanged); 0 -> "No changes to
 * save"; -1 when cap is smaller than the set (nothing is ever partially exported) */
int  pcfg_edits_export(const pcfg_edits_t *e, psvc_fedit_t *out, int cap);
void pcfg_edits_wipe(pcfg_edits_t *e);   /* memset 0 (secrets included) + reset, keeping table and zone */
/* After a save that succeeded: drop each saved entry whose current text still equals the saved text -- an entry
 * re-edited while the save was in flight stays dirty. Both editors (zones, master) use it. Returns the number dropped
 * (0 on NULL args). The vacated slots are zeroed, as pcfg_edits_drop does. */
int  pcfg_edits_drop_saved(pcfg_edits_t *e, const psvc_fedit_t *saved, int n);

#ifdef __cplusplus
}
#endif
