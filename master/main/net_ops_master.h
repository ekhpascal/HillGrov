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

/* The same read-modify-write mutex every net_ops_t member above takes
 * internally (net_set_sta/net_set_ap/net_set_tz/master_web_set_password),
 * exported so Task 12's GET /api/wifi/scan can hold it across
 * wifi_mgr_scan()'s ~2 s blocking radio scan: a wifi apply racing that scan
 * would reconfigure the STA/AP mid-scan, and the scan itself parks the radio
 * an apply would need. Callers that only invoke a net_ops_t member (set_sta,
 * set_ap, ...) must NOT also take this lock first -- that member already
 * takes it internally, and the mutex is not recursive. 0 = acquired (call
 * master_net_ops_unlock() when done), -1 = not acquired within ms (including
 * "not created yet", i.e. before the first master_net_ops() call at boot --
 * treated as a failure to take rather than an open lock, so a caller that
 * somehow ran before boot wiring answers busy rather than proceeding
 * unsynchronised). */
int  master_net_ops_try_lock(uint32_t ms);
void master_net_ops_unlock(void);

#ifdef __cplusplus
}
#endif
