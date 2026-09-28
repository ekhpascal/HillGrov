#pragma once
/* Glue: the live preferences and their NVS home, "panel"/"prefs". Never the master config (spec: Panel holds
 * display-local preferences only). A write that fails -- NVS full, or the recovery design's 6.5 "writes disabled" --
 * leaves the value live in RAM until the next restart and logs one WARN. */
#include "pnl_prefs.h"

#ifdef __cplusplus
extern "C" {
#endif

void               pnl_prefs_load(void);                     /* boot: NVS "panel"/"prefs"; defaults on any failure */
const pnl_prefs_t *pnl_prefs_get(void);                      /* [ANY] live copy (LVGL task writes it) */
void               pnl_prefs_set(const pnl_prefs_t *p);      /* [LVGL] updates the live copy + submits a save job */
void               pnl_prefs_preview(const pnl_prefs_t *p);  /* [LVGL] live copy only, no save (a slider being dragged) */
int                pnl_prefs_save(const pnl_prefs_t *p);     /* [WORKER] 0 / -1 (value stays in RAM; logged) */

/* [LVGL] The outcome of the saves pnl_prefs_set() queued, kept here so a Panel screen rebuilt later still shows it:
 * NONE before the first save since boot; PENDING while any save is queued or running; else how the last one ended.
 * FAILED covers both "the worker refused the job" and "NVS refused the write". */
typedef enum { PNL_PREFS_SAVE_NONE = 0, PNL_PREFS_SAVE_PENDING, PNL_PREFS_SAVE_OK, PNL_PREFS_SAVE_FAILED }
    pnl_prefs_save_state_t;
pnl_prefs_save_state_t pnl_prefs_save_state(void);

#ifdef __cplusplus
}
#endif
