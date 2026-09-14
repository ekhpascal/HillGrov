#include <stdio.h>
#include <string.h>
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_partition.h"
#include "esp_log.h"
#include "http_upload.h"

static const char *TAG = "http_upload_master";

/* POST /api/fw/master's target: the inactive OTA slot, through esp_ota_*.
 * The image is streamed in by http_upload.c, which has already checked that
 * it really is a hillgrow_master image before begin() is called here.
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

static int master_begin(size_t content_len) {
    (void)content_len;   /* OTA_WITH_SEQUENTIAL_WRITES erases as it writes */
    s_ota = 0;
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
    if (rc != ESP_OK) ESP_LOGE(TAG, "esp_ota_write: %s", esp_err_to_name(rc));
    return rc == ESP_OK ? 0 : -1;
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

static int master_finish(char *resp, size_t cap) {
    esp_err_t rc = esp_ota_end(s_ota);
    s_ota = 0;   /* esp_ota_end frees the handle on EVERY path -- never abort it now */
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end: %s", esp_err_to_name(rc));
        return -1;
    }
    rc = esp_ota_set_boot_partition(s_ota_part);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition(%s): %s", s_ota_part->label, esp_err_to_name(rc));
        return -1;
    }

    esp_app_desc_t d = { 0 };
    if (esp_ota_get_partition_description(s_ota_part, &d) != ESP_OK)
        ESP_LOGW(TAG, "no app description in %s after a good write", s_ota_part->label);
    char ver[40];
    json_safe(d.version, sizeof d.version, ver, sizeof ver);
    snprintf(resp, cap, "{\"ok\":true,\"slot\":\"%s\",\"version\":\"%s\"}", s_ota_part->label, ver);
    ESP_LOGW(TAG, "master image written to %s (%s) -- awaiting REBOOT CONFIRM", s_ota_part->label, ver);
    return 0;
}

static void master_cancel(void) {
    if (s_ota) esp_ota_abort(s_ota);
    s_ota = 0;
}

static const upload_sink_t MASTER_SINK = {
    .begin = master_begin, .write = master_write, .finish = master_finish, .cancel = master_cancel
};

const upload_sink_t *http_upload_master_sink(void) { return &MASTER_SINK; }

size_t http_upload_master_max(void) { return s_ota_part ? s_ota_part->size : 0; }

int http_upload_master_ready(void) {
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
    esp_ota_img_states_t st;
    const esp_partition_t *run = esp_ota_get_running_partition();
    if (run && esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGW(TAG, "upload refused: %s is still on trial", run->label);
        return -2;
    }
    return 0;
}
