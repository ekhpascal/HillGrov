#pragma once
#include <stdint.h>
#include "hg_mcfg.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The master-config read-modify-write, owned by a component rather than by
 * master/main, so every caller takes the SAME lock. http_srv used to reach the
 * app's copy through extern declarations; the panel UI would have been a second
 * such reach, and a replicated guard is how two callers drift apart.
 *
 * mcfg_commit() serializes commits against each other but NOT the
 * snapshot-modify-commit sequence, which is why this lock exists on top. */
void mcfg_ops_init(void);                  /* idempotent; call once at boot */

/* 0 acquired / -1 not acquired within ms. "Not created yet" counts as NOT
 * acquired -- a caller that somehow runs before boot wiring answers busy
 * rather than proceeding unsynchronised. NOT recursive: a caller that invokes
 * mcfg_ops_edit() must not already hold the lock. */
int  mcfg_ops_lock(uint32_t ms);
void mcfg_ops_unlock(void);

/* Atomic snapshot -> fn(copy) -> commit. fn returns 0 to commit, non-zero to
 * refuse (returned unchanged, nothing written). -1 lock unavailable, -2 commit
 * failed. fn must NOT itself return -1 or -2 -- both are reserved for this
 * function's own two failure modes, so a validator that reused -1 for "bad
 * value" would be silently misreported as "the lock could not be taken".
 * fn receives a PRIVATE copy, so a refusal cannot leave the live config
 * half-modified. */
int  mcfg_ops_edit(int (*fn)(hg_mcfg_t *m, void *ctx), void *ctx);

#ifdef __cplusplus
}
#endif
