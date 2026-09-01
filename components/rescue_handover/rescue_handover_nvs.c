#include "rescue_handover.h"
#include "nvs_flash.h"
#include "esp_err.h"
#include "esp_log.h"

static const char *TAG = "handover";

int hg_handover_write(const hg_handover_t *h) {
    if (!h) return -1;

    uint8_t buf[HG_HANDOVER_LEN];
    if (hg_handover_pack(h, buf) != 0) return -1;

    nvs_handle_t handle;
    esp_err_t err = nvs_open("hg", NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return -1;
    }

    err = nvs_set_blob(handle, "hando", buf, HG_HANDOVER_LEN);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_set_blob failed: %s", esp_err_to_name(err));
        nvs_close(handle);
        return -1;
    }

    err = nvs_commit(handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_commit failed: %s", esp_err_to_name(err));
    }
    nvs_close(handle);

    return (err == ESP_OK) ? 0 : -1;
}

int hg_handover_take(hg_handover_t *out) {
    if (!out) return -1;

    nvs_handle_t handle;
    esp_err_t err = nvs_open("hg", NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return -1;
    }

    uint8_t buf[HG_HANDOVER_LEN];
    size_t len = HG_HANDOVER_LEN;
    err = nvs_get_blob(handle, "hando", buf, &len);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_get_blob failed: %s", esp_err_to_name(err));
        nvs_close(handle);
        return -1;
    }

    if (len != HG_HANDOVER_LEN) {
        ESP_LOGW(TAG, "bad blob length: %u (expected %u)", (unsigned)len, HG_HANDOVER_LEN);
        /* Erase corrupt record before returning error */
        nvs_erase_key(handle, "hando");
        nvs_commit(handle);
        nvs_close(handle);
        return -1;
    }

    if (hg_handover_unpack(buf, out) != 0) {
        ESP_LOGW(TAG, "unpack failed (bad magic/version/crc/nul)");
        /* Erase corrupt record before returning error */
        nvs_erase_key(handle, "hando");
        nvs_commit(handle);
        nvs_close(handle);
        return -1;
    }

    err = nvs_erase_key(handle, "hando");
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_erase_key failed: %s", esp_err_to_name(err));
        nvs_close(handle);
        return -1;
    }

    err = nvs_commit(handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_commit failed: %s", esp_err_to_name(err));
    }
    nvs_close(handle);

    return (err == ESP_OK) ? 0 : -1;
}
