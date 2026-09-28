#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The secret wipe for every panel_ui file (Global Constraints, "Secrets").
 * The master builds with -Os (CONFIG_COMPILER_OPTIMIZATION_SIZE=y): GCC drops
 * a plain memset on a buffer that is never read again as a dead store, even
 * one whose address was passed out earlier. Volatile stores cannot be dropped.
 * Pure: pnl_input.h (Task 19) re-exports it; nothing else defines a wipe. */
static inline void pnl_zero(void *p, size_t n) {
    volatile uint8_t *v = (volatile uint8_t *)p;
    while (n--) *v++ = 0;
}

#ifdef __cplusplus
}
#endif
