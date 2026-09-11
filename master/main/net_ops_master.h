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

/* The set_web_password member, exposed on its own because it is the one op
 * with a runtime dependency of its own (a wa_state_t for web_auth's sha/rand
 * hooks) that Task 11 will re-point at http_srv's shared state. 0 ok, -1 if
 * the password is outside 8..63 or the commit could not be stored. */
int master_web_set_password(const char *pw);

#ifdef __cplusplus
}
#endif
