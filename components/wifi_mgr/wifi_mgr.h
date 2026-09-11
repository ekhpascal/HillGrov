#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Master's Wi-Fi manager: AP **and** STA at the same time (WIFI_MODE_APSTA),
 * replacing SP3's AP-only wifi_ap.
 *
 *   AP  -- always up, from mcfg's ap_ssid/ap_pass, WPA2-PSK, channel 6, max 4
 *          stations, fixed IP 192.168.7.7/24 (unchanged from wifi_ap: the
 *          greenhouse must stay reachable with no house Wi-Fi at all, and any
 *          AP a zone node joins for a fleet OTA has to be 2.4 GHz).
 *   STA -- joins the house Wi-Fi when mcfg's sta_ssid is non-empty, with a
 *          5/10/20/40/60/60... s reconnect backoff ladder (reset on success),
 *          the mcfg hostname pushed into DHCP, and mDNS (_http._tcp on 80)
 *          published once the first address arrives, so "<hostname>.local"
 *          resolves on the house LAN.
 *
 * Caveat worth knowing on the bench: with a single radio, ESP-IDF's softAP
 * follows the STA's channel once the STA associates, so the AP does not stay
 * on channel 6 while joined to a house AP on another channel. That is a
 * hardware property, not a policy choice here.
 *
 * Only wifi_mgr.h's types are visible to host tests (plain C, no IDF
 * headers) -- master_cmds.h embeds wifi_status_t in its net_ops_t. */

typedef struct {
    uint8_t sta_up;             /* 1 once IP_EVENT_STA_GOT_IP has landed and no disconnect since */
    char    sta_ip[16];         /* dotted quad while up, "" while down */
    char    sta_ssid[33];       /* the SSID actually joined while up, else the configured one, else "" */
    int8_t  rssi;               /* live RSSI of the joined AP, 0 while down */
    char    sta_reason[24];     /* last disconnect reason, short text (see wifi_mgr_sta.c), "" if never */
    uint8_t ap_clients;         /* stations currently associated to our own AP */
    char    ap_ip[16];          /* always "192.168.7.7" once the AP is up */
    char    ap_ssid[33];        /* mcfg ap_ssid */
} wifi_status_t;

typedef struct { char ssid[33]; int8_t rssi; uint8_t auth; } wifi_scan_t;   /* auth = wifi_auth_mode_t */

typedef void (*wifi_sta_cb)(int up);

/* Brings up netif/event loop/driver, configures AP + STA from mcfg_get() and
 * starts the radio. 0 ok, -1 on the first failing esp_ call (all logged);
 * idempotent -- a second call once up is a no-op returning 0. Must run before
 * time_svc_start(), whose SNTP setup needs lwIP's tcpip task (created by
 * esp_netif_init() in here). */
int  wifi_mgr_start(void);

/* Fills *out with the current status; refreshes the live RSSI/joined SSID and
 * the AP station count from the driver. Safe from any task. */
void wifi_mgr_status(wifi_status_t *out);

/* Re-reads mcfg and re-applies it: AP config (SSID/pass changes restart the
 * AP, dropping its stations) and STA config (disconnect, reconfigure, reset
 * the backoff ladder, reconnect -- or just disconnect if sta_ssid is now
 * empty). Call after a successful mcfg_commit() that touched WIFI.*.
 * 0 ok / -1 (not started, or an esp_ call failed). */
int  wifi_mgr_apply(void);

/* Blocking all-channel active scan (~2 s, well inside the 4 s budget), newest
 * results sorted by RSSI descending with duplicate SSIDs and hidden (empty)
 * SSIDs dropped. Returns the number written (<= cap) or -1. */
int  wifi_mgr_scan(wifi_scan_t *out, int cap);

/* Registers the single STA up/down observer (time_svc_sta_changed in
 * production, wired from app_main once time_svc_start() has run). If the STA
 * is already up when this is called, cb(1) fires immediately so a late
 * registration cannot miss the edge. */
void wifi_mgr_on_sta(wifi_sta_cb cb);

#ifdef __cplusplus
}
#endif
