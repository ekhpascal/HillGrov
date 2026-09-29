#pragma once
#include <stddef.h>
#include "hg_wipe.h"   /* components/hg_blob: the one volatile-store wipe */

#ifdef __cplusplus
extern "C" {
#endif

/* The secret wipe for every panel_ui file (Global Constraints, "Secrets"): hg_wipe(), under the panel's name. Never a
 * plain memset -- the master builds with -Os, and GCC drops a memset on a buffer that is never read again.
 * Pure: pnl_input.h (Task 19) re-exports it; nothing in panel_ui defines another wipe. */
static inline void pnl_zero(void *p, size_t n) { hg_wipe(p, n); }

#ifdef __cplusplus
}
#endif
