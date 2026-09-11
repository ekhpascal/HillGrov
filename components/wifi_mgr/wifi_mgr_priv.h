#pragma once
#include <stdint.h>
#include "esp_netif.h"
#include "hg_mcfg.h"
#include "wifi_mgr.h"

/* Internals shared between wifi_mgr.c (driver/AP/scan/status) and
 * wifi_mgr_sta.c (the STA join + reconnect-backoff state machine). Split
 * purely to keep each file well under the 300-line guideline; they are one
 * component and there is exactly one instance of everything here. */

/* The live status. Written from the Wi-Fi/IP event handler task and from
 * wifi_mgr_status() (which only refreshes the driver-derived rssi/ssid/
 * ap_clients fields); read from CLI and HTTP tasks. Every field is either a
 * scalar or a NUL-terminated fixed buffer written with snprintf, so a
 * concurrent reader can see a stale value but never an out-of-bounds one --
 * a mutex would buy nothing that matters for a status display. */
extern wifi_status_t g_wm;

/* 1 once esp_wifi_start() has succeeded. esp_wifi_connect()/
 * esp_wifi_disconnect() before that point are errors, not no-ops, so
 * both files gate on this instead of logging spurious warnings. */
extern uint8_t       g_wm_started;

extern esp_netif_t  *g_wm_sta_netif;
extern esp_netif_t  *g_wm_ap_netif;
extern wifi_sta_cb   g_wm_sta_cb;

/* Registers the STA/IP event handlers and creates the one-shot backoff timer.
 * Called once from wifi_mgr_start() before esp_wifi_start(). 0 / -1. */
int  wifi_mgr_sta_init(void);

/* Pushes m's sta_ssid/sta_pass into the driver and asks for a connect (or,
 * when sta_ssid is empty, disconnects and cancels any pending retry). Resets
 * the backoff ladder. Safe to call before esp_wifi_start(): the actual
 * esp_wifi_connect() then happens on WIFI_EVENT_STA_START. 0 / -1. */
int  wifi_mgr_sta_apply(const hg_mcfg_t *m);

/* Both files emit NOTIFY WIFI lines; this keeps the "clear the rate-limit
 * latch first" decision in one place. STA up/down are rare, genuinely
 * important edges (the backoff floor is 5 s), so they bypass notify's 2 s
 * per-type throttle; AP client churn does not, since a flapping station
 * could otherwise spam the console. */
void wifi_mgr_notify_edge(const char *fmt, ...);
void wifi_mgr_notify_throttled(const char *fmt, ...);
