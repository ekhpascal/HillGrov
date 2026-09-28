#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* The Master v2 7" touch panel (spec 2026-09-18-master-v2-panel-ui-design.md).
 * LVGL-free on purpose: these two are the only panel symbols app_main sees,
 * and the header compiles on every target (the component is include-only
 * everywhere but the P4 master).
 *
 * Placement (recovery-design compatible, anchored by call name, never by line):
 *   panel_start()          immediately after ota_trial_start(1) and before the
 *                          first radio call (today wifi_mgr_start(); later the
 *                          recovery plan's explicit esp_hosted_init());
 *   panel_services_start() immediately after node_mgr_start(), before the
 *                          cp_ota_sync() gate.
 * The panel never calls cp_ota_sync() or any esp_hosted_* function, and it is
 * not part of ota_trial_drivers_ok() (D17). */

/* Display + touch + LVGL task + first screen. Soft on EVERY failure: 0 = lit,
 * -1 = dark, and boot continues either way (system spec 3.3). */
int panel_start(void);

/* The panel worker and the 1 Hz poller. 0 / -1 (-1 when the panel is dark).
 * The panel shows "starting" until the first snapshot lands. */
int panel_services_start(void);

#ifdef __cplusplus
}
#endif
