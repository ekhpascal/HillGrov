#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The ONE secret wipe (panel plan Global Constraints, "Secrets"), for every component above hg_blob: panel_svc's
 * psvc_mcfg.c and panel_ui's pnl_zero() both use it. The master builds with -Os (CONFIG_COMPILER_OPTIMIZATION_SIZE=y):
 * GCC drops a plain memset on a buffer that is never read again as a dead store, even one whose address was passed
 * out earlier. Volatile stores cannot be dropped. Pure: <std*.h> only, header-only. */
static inline void hg_wipe(void *p, size_t n) {
    volatile uint8_t *v = (volatile uint8_t *)p;
    while (n--) *v++ = 0;
}

#ifdef __cplusplus
}
#endif
