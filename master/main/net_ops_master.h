#pragma once
#include "master_cmds.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The production net_ops_t (master_cmds.h) behind the NET/TIME CLI rows:
 * mcfg snapshot -> modify -> mcfg_commit() -> wifi_mgr_apply() /
 * time_svc_apply_mcfg(), via components/mcfg_ops's mcfg_ops_edit() (Task 5).
 * Exported rather than kept file-static because Task 12's HTTP handlers apply
 * the identical validate-commit-apply sequence and should share this one
 * rather than grow a second copy of it.
 *
 * Do not read that as a statement that no second copy exists -- final-review
 * F4: components/http_srv/http_api_cfg.c's cfg_put_zone0() still hand-rolls
 * the whole sequence (mcfg_ops_lock(100) by hand, its own snapshot ->
 * hg_json_merge_mcfg -> hg_mcfg_validate -> mcfg_commit -> wifi_mgr_apply ->
 * time_svc_apply_mcfg, SIX explicit unlocks -- five error paths plus the
 * success path, counted at http_api_cfg.c:215, 220, 227, 234, 239, 246; the
 * 409 BUSY path at :205-208 took no lock, so it has none to release), and the
 * two have already diverged: 100 ms vs mcfg_ops_edit()'s 6000 ms lock budget,
 * no per-op commit log, and its own inline -1/-2 -> HTTP mapping. Both are
 * correct in lock scope and mapping today. Consolidating it was deliberately
 * NOT done in this branch (it touches a working, bench-verified web path, and
 * this is the last commit before the branch lands); mcfg_ops_edit() is the
 * canonical one, and a third caller -- the panel UI -- must copy THAT, not
 * cfg_put_zone0(). */
const net_ops_t *master_net_ops(void);

/* The set_web_password member, exposed on its own because http_srv's
 * POST /api/password calls it directly (it owns the mcfg commit and the
 * mcfg_ops lock, which the handler must not duplicate). Hashes through
 * http_srv's shared wa_state_t and, once the commit has landed, drops every
 * live web session. 0 ok, -1 password outside 8..63, -2 valid but not
 * stored, -3 SHA-256 unavailable (nothing was written). */
int master_web_set_password(const char *pw);

#ifdef __cplusplus
}
#endif
