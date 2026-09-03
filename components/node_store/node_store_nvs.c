#include "node_store.h"
#include "nvs_flash.h"
#include "esp_err.h"
#include "esp_log.h"

static const char *TAG = "node_store";
static uint32_t node_store_gen = 0;

int node_store_load(ztab_t *t) {
    if (!t) return -1;

    nvs_handle_t handle;
    esp_err_t err = nvs_open("hg", NVS_READONLY, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return -1;
    }

    uint8_t buf[16 + 192];
    size_t len = sizeof(buf);
    err = nvs_get_blob(handle, "ztab", buf, &len);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_get_blob failed: %s", esp_err_to_name(err));
        nvs_close(handle);
        return -1;
    }

    if (len != sizeof(buf)) {
        ESP_LOGW(TAG, "bad blob length: %u (expected %u)", (unsigned)len, (unsigned)sizeof(buf));
        nvs_close(handle);
        return -1;
    }

    uint32_t gen = 0;
    if (ztab_unpack(buf, len, t, &gen) != 0) {
        ESP_LOGW(TAG, "unpack failed (bad magic/version/crc)");
        nvs_close(handle);
        return -1;
    }

    node_store_gen = gen;
    nvs_close(handle);
    return 0;
}

int node_store_save(const ztab_t *t) {
    if (!t) return -1;

    uint32_t gen = node_store_gen + 1;
    node_store_gen = gen;

    uint8_t buf[16 + 192];
    int pack_ret = ztab_pack(t, gen, buf, sizeof(buf));
    if (pack_ret < 0) {
        ESP_LOGW(TAG, "pack failed");
        return -1;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open("hg", NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return -1;
    }

    err = nvs_set_blob(handle, "ztab", buf, sizeof(buf));
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
