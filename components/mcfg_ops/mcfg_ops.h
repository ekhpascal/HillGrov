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

/* Atomic snapshot -> fn(copy) -> commit. fn receives a PRIVATE copy, so a
 * refusal cannot leave the live config half-modified.
 *
 * fn returns 0 to commit, or a POSITIVE value to refuse -- returned unchanged,
 * nothing written. Every NEGATIVE return belongs to this component, so fn must
 * never return one:
 *   -1  the lock could not be taken
 *   -2  mcfg_commit() failed on storage (NVS or mutex)
 *   -3  mcfg_commit() rejected the config as invalid
 * The -2/-3 split is not decoration: mcfg_commit() distinguishes those two, and
 * the CLI and web surfaces map them to different owner-visible errors
 * (ERR STORAGE vs ERR INVALID).
 *
 * apply runs after a successful commit and STILL HOLDS THE LOCK; pass NULL when
 * there is nothing to apply. That scope is deliberate and predates this
 * component: every writer held the lock across its wifi_mgr_apply() /
 * time_svc_apply_mcfg() / http_auth_sessions_drop(), and GET /api/wifi/scan
 * takes this same lock so a radio reconfigure cannot land underneath a scan.
 * An apply that ran after the release would quietly delete that exclusion.
 * apply must therefore never call back into mcfg_ops_lock()/mcfg_ops_edit() --
 * the lock is not recursive.
 *
 * what is the label the commit path logs, e.g. "SET TZ". */
int  mcfg_ops_edit(int (*fn)(hg_mcfg_t *m, void *ctx), void *ctx,
                    void (*apply)(void *ctx), const char *what);

/* mcfg_ops_edit() with a caller-chosen lock budget -- identical in every other
 * respect (rc contract, private copy, apply under the lock). mcfg_ops_edit()
 * is exactly mcfg_ops_edit_ms(..., MCFG_OPS_EDIT_LOCK_MS). The web's zone-0
 * PUT keeps its historic 100 ms try-lock through this, while the panel worker,
 * which can show "pending", waits the full 6000 ms (panel plan Task 3). */
#define MCFG_OPS_EDIT_LOCK_MS 6000u   /* > mcfg_commit()'s own 5000 ms mutex timeout, so a
                                         caller that loses a race reports the commit's verdict */
int  mcfg_ops_edit_ms(int (*fn)(hg_mcfg_t *m, void *ctx), void *ctx,
                       void (*apply)(void *ctx), const char *what, uint32_t lock_ms);

#ifdef __cplusplus
}
#endif
