#include <stdio.h>
#include <string.h>
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_partition.h"
#include "esp_log.h"
#include "psvc_fw.h"
#include "ota_trial.h"   /* ota_trial_running_on_trial() -- the ONE trial predicate (recovery design 2.3) */

static const char *TAG = "fw_sink_master";

/* POST /api/fw/master's target: the inactive OTA slot, through esp_ota_*.
 * The image is streamed in by panel_svc's install core (psvc_fw_install_with), from the web upload or the panel's
 * microSD, which has already checked that it really is a hillgrow_master image for this chip before begin() is called
 * here.
 *
 * esp_ota_begin runs with OTA_WITH_SEQUENTIAL_WRITES so the slot is erased
 * sector by sector as the write advances, rather than in one ~2 s stall
 * before the first byte (a stall long enough for the client to give up on a
 * marginal AP link). esp_ota_end then verifies the written image, and only
 * after that does esp_ota_set_boot_partition point the bootloader at it --
 * the reboot itself is the operator's, through REBOOT CONFIRM, never this
 * handler's. */

static const esp_partition_t *s_ota_part;
static esp_ota_handle_t       s_ota;
static uint32_t               s_len;

static int master_begin(size_t content_len) {
    (void)content_len;   /* OTA_WITH_SEQUENTIAL_WRITES erases as it writes */
    s_ota = 0;
    s_len = 0;
    esp_err_t rc = esp_ota_begin(s_ota_part, OTA_WITH_SEQUENTIAL_WRITES, &s_ota);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin(%s): %s", s_ota_part->label, esp_err_to_name(rc));
        s_ota = 0;
        return -1;
    }
    return 0;
}

static int master_write(const void *buf, size_t n) {
    esp_err_t rc = esp_ota_write(s_ota, buf, n);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_write: %s", esp_err_to_name(rc));
        return -1;
    }
    s_len += (uint32_t)n;
    return 0;
}

/* esp_app_desc_t.version is a char[32] that is not guaranteed to be
 * NUL-terminated and comes from the uploaded file, so it is both length-bound
 * and stripped of anything that could break out of the JSON string it is
 * about to land in (the same defence sanitise_path() applies to echoed URIs). */
static void json_safe(const char *in, size_t n, char *out, size_t cap) {
    size_t o = 0;
    for (size_t i = 0; i < n && in[i] && o + 1 < cap; i++) {
        char c = in[i];
        out[o++] = (c >= 0x20 && c < 0x7f && c != '"' && c != '\\') ? c : '.';
    }
    out[o] = '\0';
}

static int master_finish(psvc_fw_result_t *res) {
    esp_err_t rc = esp_ota_end(s_ota);
    s_ota = 0;   /* esp_ota_end frees the handle on EVERY path -- never abort it now */
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end: %s", esp_err_to_name(rc));
        return -1;
    }
    /* esp_ota_end() just verified the whole image, and esp_ota_set_boot_partition() verifies it again: two full reads
     * of up to 4 MB, so the TWDT is fed between them (silent when the caller is not subscribed). */
    psvc_fw_wdt_kick();
    rc = esp_ota_set_boot_partition(s_ota_part);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition(%s): %s", s_ota_part->label, esp_err_to_name(rc));
        return -1;
    }
    esp_app_desc_t d = { 0 };
    if (esp_ota_get_partition_description(s_ota_part, &d) != ESP_OK)
        ESP_LOGW(TAG, "no app description in %s after a good write", s_ota_part->label);
    snprintf(res->slot, sizeof res->slot, "%s", s_ota_part->label);
    json_safe(d.version, sizeof d.version, res->version, sizeof res->version);   /* the web echoes it into JSON */
    res->len = s_len;
    ESP_LOGW(TAG, "master image written to %s (%s) -- awaiting REBOOT CONFIRM", res->slot, res->version);
    return 0;
}

static void master_cancel(void) {
    if (s_ota) esp_ota_abort(s_ota);
    s_ota = 0;
}

static size_t master_max(void) { return s_ota_part ? s_ota_part->size : 0; }

/* Runs under the upload's exclusivity claim (fix round 1), which is what
 * makes writing s_ota_part here safe. */
static int master_ready(void) {
    s_ota_part = esp_ota_get_next_update_partition(NULL);
    if (!s_ota_part) {
        ESP_LOGE(TAG, "no inactive OTA slot to write");
        return -1;
    }
    /* CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE: esp_ota_begin refuses outright
     * while the RUNNING image is still on trial (it would otherwise erase the
     * only slot that is known to work), and it would refuse only after the
     * client had already streamed the first 112 B. Say so up front instead --
     * the operator's answer is simply to wait out the trial. */
    if (ota_trial_running_on_trial()) {
        const esp_partition_t *run = esp_ota_get_running_partition();
        ESP_LOGW(TAG, "upload refused: %s is still on trial", run ? run->label : "?");
        return -2;
    }
    return 0;
}

static const psvc_fw_sink_t MASTER_SINK = {
    .ready = master_ready, .max = master_max, .begin = master_begin,
    .write = master_write, .finish = master_finish, .cancel = master_cancel
};

const psvc_fw_sink_t *psvc_fw_sink_master(void) { return &MASTER_SINK; }
