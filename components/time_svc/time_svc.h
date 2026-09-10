#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Master-only glue between mcfg's tz/ntp fields and (a) time_core's POSIX-TZ
 * offset calculator, exposed to node_mgr for TIME_SYNC's utc_offset_s, and
 * (b) the SNTP client, whose lifecycle tracks Wi-Fi STA up/down -- there is
 * no point polling an NTP server the AP-only board (SP3's wifi_ap, still the
 * only Wi-Fi component until Task 8's STA manager lands) can never reach. */

/* Reads mcfg's tz/ntp, wires tz_check into mcfg_store's commit-time
 * validator, and prepares the SNTP client (operating mode, server name,
 * sync-notification callback) -- does NOT start SNTP itself. Call once at
 * boot, after mcfg_store_init(). */
void time_svc_start(void);

/* Starts SNTP (up=1) or stops it (up=0); idempotent against repeated calls
 * with the same value. Task 8's STA manager calls this on every STA
 * up/down transition -- nothing in this task calls it yet. */
void time_svc_sta_changed(int up);

/* Seconds EAST of UTC in effect right now, from the parsed mcfg tz string;
 * 0 if the stored tz string doesn't parse (should not happen once mcfg_store
 * validates every commit through tz_check, but a corrupt/pre-Task-7 NVS
 * blob loaded before this wiring existed could still hold one). */
int32_t time_svc_utc_offset(void);

/* 1 once the SNTP callback has fired at least once this boot, else 0. */
int time_svc_is_ntp(void);

/* Re-reads mcfg's tz/ntp fields; call after every mcfg_commit() that may have
 * touched TIME.TZ or TIME.NTP (the tz string was already validated by
 * tz_check at commit time, so this re-parse is not expected to fail, but it
 * inherits time_svc_utc_offset()'s 0-on-failure fallback if it somehow does). */
void time_svc_apply_mcfg(void);

#ifdef __cplusplus
}
#endif
