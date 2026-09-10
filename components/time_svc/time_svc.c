#include <string.h>
#include <stdio.h>
#include <time.h>
#include <sys/time.h>
#include "esp_sntp.h"
#include "esp_log.h"
#include "time_core.h"
#include "time_svc.h"
#include "mcfg_store.h"
#include "app_if_common.h"
#include "node_mgr.h"

static const char *TAG = "time_svc";

static tz_rule_t s_rule;
static uint8_t   s_rule_valid;
static char      s_ntp_host[48];
static uint8_t   s_sta_up;
static volatile uint8_t s_ntp_synced;   /* set from the SNTP/lwIP task's callback */

void time_svc_apply_mcfg(void) {
    const hg_mcfg_t *m = mcfg_get();
    tz_rule_t r;
    if (tz_parse(m->tz, &r) == 0) {
        s_rule = r;
        s_rule_valid = 1;
    } else {
        /* Should not happen once mcfg_store validates every commit through
         * tz_check (wired below) -- kept defensive for a pre-Task-7 NVS blob
         * loaded before this component existed. */
        s_rule_valid = 0;
        ESP_LOGW(TAG, "stored TZ '%s' does not parse -- utc_offset stays 0", m->tz);
    }
    snprintf(s_ntp_host, sizeof s_ntp_host, "%s", m->ntp);
}

/* Runs on the SNTP/lwIP task, not the caller's -- keep this to flag-setting
 * only (context ruling): no logging, no mutex, nothing that could block the
 * lwIP task. hg_app_time_note_source and node_mgr_time_was_set are both
 * plain static-variable writes, same weight as s_ntp_synced below. */
static void sntp_sync_cb(struct timeval *tv) {
    (void)tv;
    s_ntp_synced = 1;
    hg_app_time_note_source("NTP", hg_app_uptime_s());
    node_mgr_time_was_set();
}

void time_svc_start(void) {
    mcfg_store_set_tz_check(tz_check);
    time_svc_apply_mcfg();

    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_set_time_sync_notification_cb(sntp_sync_cb);
}

void time_svc_sta_changed(int up) {
    if (up) {
        if (!s_sta_up) {
            esp_sntp_setservername(0, s_ntp_host);   /* must precede esp_sntp_init() */
            esp_sntp_init();
            ESP_LOGI(TAG, "STA up -- SNTP started (%s)", s_ntp_host);
        }
        s_sta_up = 1;
    } else {
        if (s_sta_up) {
            esp_sntp_stop();
            ESP_LOGI(TAG, "STA down -- SNTP stopped");
        }
        s_sta_up = 0;
    }
}

int32_t time_svc_utc_offset(void) {
    if (!s_rule_valid) return 0;
    return tz_offset_at(&s_rule, (uint32_t)time(NULL));
}

int time_svc_is_ntp(void) {
    return s_ntp_synced;
}
