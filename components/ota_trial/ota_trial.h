#pragma once
#include <stdint.h>
#include "cmd_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/* spec 3.10: after an OTA-updated app boots into ESP_OTA_IMG_PENDING_VERIFY, it
 * runs a trial period before self-confirming (mark-valid) or self-rolling-back.
 * trial_probes_t is the pure snapshot trial_eval() judges each evaluation; the
 * target side (ota_trial.c) owns the live copy and updates it via the hook
 * functions below. */
typedef struct {
    uint8_t  cfg_loaded;          /* config parsed or defaulted (never-abort rule) */
    uint8_t  drivers_ok;          /* ring driver installed (+ master: AP netif up + httpd up) */
    uint16_t ticks;               /* periodic-task tick count since boot */
    uint8_t  twdt_ok;              /* no TWDT event observed */
    uint32_t min_heap_kb;
    uint8_t  master_frame_seen;   /* any valid master-sourced ring frame */
    uint8_t  confirmed;           /* SET OTA CONFIRM issued */
} trial_probes_t;

enum { TRIAL_PENDING = 0, TRIAL_PASS = 1, TRIAL_FAIL = 2 };

#define TRIAL_FLEET_WINDOW_MS 180000u
#define TRIAL_BENCH_WINDOW_MS  60000u
#define TRIAL_MIN_TICKS 50
#define TRIAL_MIN_HEAP_KB 40

/* Pure: PASS = confirmed || (base criteria met && (expect_link ? master_frame_seen
 *   : now-start >= TRIAL_BENCH_WINDOW_MS));
 * base = cfg_loaded && drivers_ok && ticks >= TRIAL_MIN_TICKS && twdt_ok &&
 *   min_heap_kb > TRIAL_MIN_HEAP_KB;
 * FAIL = expect_link && now-start >= TRIAL_FLEET_WINDOW_MS (window expired);
 *   bench never times out to FAIL -- it stays PENDING until criteria/confirm
 *   (rollback happens only via crash/TWDT reboot before mark-valid, per the
 *   spec's firmware-attributable rule). confirmed short-circuits every other
 *   criterion (operator override). */
int trial_eval(const trial_probes_t *p, uint8_t expect_link, uint32_t start_ms, uint32_t now_ms);

/* ---- target side (ota_trial.c) ---- */

/* No-op (cheaply) unless the running slot's OTA state is ESP_OTA_IMG_PENDING_VERIFY --
 * a board flashed directly by tools never is. Reads+erases the NVS hg/"trial"
 * breadcrumb rescue_pull() leaves behind (absent -> expect_link 0) and starts the
 * 1 s evaluation timer. is_master (1 master / 0 zone) is accepted for parity with
 * each app's boot call site; trial_eval()'s own criteria don't branch on it -- the
 * master-vs-zone difference in drivers_ok's definition is the caller's to apply via
 * when it calls ota_trial_drivers_ok(). */
void ota_trial_start(int is_master);
void ota_trial_master_frame(void);   /* ring glue: any valid master-sourced frame seen */
/* One liveness tick. EVERY app that calls ota_trial_start() MUST call this from
 * a periodic task, or trial_eval's "ticks >= TRIAL_MIN_TICKS" gate can never be
 * met and a PENDING_VERIFY image will never self-confirm -- the bootloader
 * rolls it back instead. Zone: zone_ring's loop. Master: node_mgr's 50 ms loop
 * (50 ticks = 2.5 s). It is a cheap no-op when no trial is running. */
void ota_trial_tick(void);
void ota_trial_drivers_ok(void);     /* ring driver (+ master: AP/httpd) is up */
int  ota_trial_confirm(void);        /* SET OTA CONFIRM hook; -1 if no trial active */

extern const cmd_entry_t OTA_TRIAL_ROWS[];   /* 1 row: SET OTA CONFIRM */
extern const int         OTA_TRIAL_ROWS_N;

#ifdef __cplusplus
}
#endif
