#pragma once
/* Counting stand-ins for the two apply hooks psvc_mcfg_edit() runs under the
 * mcfg_ops lock after a successful commit: wifi_mgr_apply() (esp_hosted RPCs
 * on the P4) and time_svc_apply_mcfg() (the SNTP/TZ reload). The real ones are
 * IDF-bound; the tests only need to know whether, and how often, each ran. */
extern int fake_apply_wifi_n;    /* wifi_mgr_apply() calls since fake_apply_reset() */
extern int fake_apply_time_n;    /* time_svc_apply_mcfg() calls since fake_apply_reset() */
extern int fake_apply_wifi_rc;   /* what the next wifi_mgr_apply() returns (default 0) */
void fake_apply_reset(void);
