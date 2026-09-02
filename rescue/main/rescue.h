#pragma once

#include <stdint.h>
#include "esp_partition.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Connects to an existing AP (pull-mode fetch); blocks up to timeout_ms
 * waiting for an IP. 0 on success, -1 on timeout/failure. */
int rescue_wifi_sta(const char *ssid, const char *pass, uint32_t timeout_ms);

/* Brings up the manual-upload fallback AP: fixed IP 192.168.7.7/24,
 * SSID "HillGrow-Rescue-xxxxxx" (last 3 MAC bytes), WPA2 pass "hillgrow1".
 * 0 on success, -1 if any step failed (logged); the caller should treat
 * this as the terminal recovery path failing and signal it visibly. */
int rescue_wifi_ap(void);

/* Fetches url over HTTP into rescue_target_slot() and, on success, sets it
 * as the boot partition. 0 on success, -1 otherwise. */
int rescue_pull(const char *url);

/* Starts the GET/POST httpd (upload page, /upload, /reboot) on port 80. */
void rescue_http_start(void);

/* The OTA slot to write next: whichever of ota_0/ota_1 isn't the one that
 * booted into rescue. NULL if the partition table has no OTA slots. */
const esp_partition_t *rescue_target_slot(void);

/* Ring-UART byte repeater: installs UART2 and starts its forwarding task,
 * so a chain of zones stays passable while one of them is in rescue. */
void ring_fwd_start(void);

#ifdef __cplusplus
}
#endif
