#pragma once
#include "master_cmds.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The production net_ops_t (master_cmds.h) behind the NET/TIME CLI rows:
 * mcfg snapshot -> modify -> mcfg_commit() -> wifi_mgr_apply() /
 * time_svc_apply_mcfg(). Exported rather than kept file-static because
 * Task 12's HTTP handlers apply the identical validate-commit-apply
 * sequence and must not grow a second copy of it. */
const net_ops_t *master_net_ops(void);

/* The set_web_password member, exposed on its own because http_srv's
 * POST /api/password calls it directly (it owns the mcfg commit and the ops
 * mutex, which the handler must not duplicate). Hashes through http_srv's
 * shared wa_state_t and, once the commit has landed, drops every live web
 * session. 0 ok, -1 password outside 8..63, -2 valid but not stored,
 * -3 SHA-256 unavailable (nothing was written). */
int master_web_set_password(const char *pw);

#ifdef __cplusplus
}
#endif
