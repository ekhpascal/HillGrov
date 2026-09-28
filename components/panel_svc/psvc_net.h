#pragma once
#include <stdint.h>
#include "master_cmds.h"   /* net_ops_t */
#include "wifi_mgr.h"      /* wifi_scan_t */
#include "psvc_rc.h"
#include "psvc_mcfg.h"     /* PSVC_LOCK_* -- the budgets callers pass below */

#ifdef __cplusplus
extern "C" {
#endif

/* The master's network/time/web-password writes, owned by panel_svc so every
 * face reaches the SAME code: the CLI rows (through master_net_ops()), the
 * web's POST /api/wifi, POST /api/password and GET /api/wifi/scan, and the
 * panel's System screens. Moved here from master/main/net_ops_master.{c,h}
 * (panel plan Task 4), which as an app could only be reached from a component
 * through extern declarations.
 *
 * Threading: every function here is [WORKER] -- it takes the mcfg_ops lock for
 * up to lock_ms, then mcfg_commit()'s 5 s mutex, then the apply's esp_hosted
 * RPCs (P4). Never from the LVGL task, never from a TWDT-subscribed task. */

/* The production net_ops_t behind the NET/TIME CLI rows. Unchanged name and
 * behaviour: each member is a legacy wrapper over the psvc_* call below with
 * PSVC_LOCK_LEGACY_MS and psvc_rc_to_net_legacy(), so the rows still answer
 * ERR INVALID / ERR STORAGE / ERR INTERNAL exactly as before -- including
 * lock contention as ERR STORAGE (D10). seed_mac, get_mcfg and wifi_status
 * are as they were. */
const net_ops_t *master_net_ops(void);

/* The set_web_password member on its own, for POST /api/password (which has
 * already verified the old password). 0 ok, -1 password outside 8..63 or
 * rejected, -2 valid but not stored (includes a busy lock), -3 SHA-256
 * unavailable (nothing was written). */
int master_web_set_password(const char *pw);

/* Panel-facing setters: same work, but master-config contention is PSVC_E_BUSY
 * rather than STORAGE (D10), so the panel can say "busy, retry" honestly. */
psvc_rc_t psvc_wifi_set_sta(const char *ssid, const char *pass, uint32_t lock_ms);  /* pass "" = open network */
psvc_rc_t psvc_wifi_set_ap(const char *ssid, const char *pass, uint32_t lock_ms);   /* clears MCFG_F_AP_DEFAULT (as today) */
psvc_rc_t psvc_tz_set(const char *tz, uint32_t lock_ms);                              /* bad POSIX TZ -> PSVC_E_INVALID */

/* No old password: the panel is the deliberate recovery path (spec Decision 3).
 * hash rc -1 -> INVALID, -3 -> INTERNAL; lock -> BUSY; commit -2 -> STORAGE;
 * commit -3 -> INVALID. On PSVC_OK every web session has been dropped
 * (http_auth_sessions_drop() ran inside the lock, after the commit). */
psvc_rc_t psvc_web_password_set(const char *pw, uint32_t lock_ms);

/* mcfg_ops_lock(lock_ms) (-1 -> BUSY) -> wifi_mgr_scan(out, cap) (<0 ->
 * INTERNAL) -> unlock. *n is always written (0 on failure). Holding the lock
 * across the scan is what keeps a STA/AP re-apply from landing mid-scan. On
 * the P4 this can block up to 30 s (CONFIG_ESP_HOSTED_HOST_WIFI_SCAN_BLOCK_
 * TIMEOUT_MS) with the radio parked and the AP silent. */
psvc_rc_t psvc_wifi_scan(wifi_scan_t *out, int cap, int *n, uint32_t lock_ms);

#ifdef __cplusplus
}
#endif
