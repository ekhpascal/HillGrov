#include <string.h>
#include "mcfg_store.h"
#include "hg_blob.h"
#include "nvs_flash.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "mcfg_store";

#define MCFG_BUF_LEN (HG_BLOB_HDR_LEN + (uint16_t)sizeof(hg_mcfg_t))   /* 16 + 368 = 384 */

static hg_mcfg_t         s_buf[2];   /* double buffer; s_active picks the readable half */
static volatile int      s_active;
static volatile uint32_t s_gen;
static SemaphoreHandle_t s_mux;

static int mcfg_load(void) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open("hg", NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open failed: %s -- defaults in use", esp_err_to_name(err));
        return -1;
    }

    uint8_t buf[MCFG_BUF_LEN];
    size_t len = sizeof buf;
    err = nvs_get_blob(handle, "mcfg", buf, &len);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "no stored config (%s) -- defaults in use", esp_err_to_name(err));
        nvs_close(handle);
        return -1;
    }

    hg_mcfg_t loaded;
    uint32_t gen = 0;
    if (hg_mcfg_unpack(buf, len, &loaded, &gen) != 0) {
        ESP_LOGW(TAG, "corrupt stored config (bad magic/version/crc) -- defaults in use");
        nvs_close(handle);
        return -1;
    }
    nvs_close(handle);

    s_buf[0] = loaded;
    s_buf[1] = loaded;
    s_gen = gen;
    return 0;
}

int mcfg_store_init(void) {
    hg_mcfg_defaults(&s_buf[0]);
    s_buf[1] = s_buf[0];
    s_active = 0;
    s_gen = 0;
    s_mux = xSemaphoreCreateMutex();
    if (!s_mux) ESP_LOGE(TAG, "xSemaphoreCreateMutex failed -- mcfg_commit will refuse writes (-2)");

    int rc = mcfg_load();
    ESP_LOGI(TAG, "gen %u loaded", (unsigned)s_gen);
    return rc;
}

const hg_mcfg_t *mcfg_get(void) {
    return &s_buf[s_active];
}

uint32_t mcfg_gen(void) {
    return s_gen;
}

/* Commits are serialized: the whole validate -> pack -> NVS write -> RAM
 * swap sequence runs under s_mux, held from entry to exit (every return
 * path below releases it). This is what makes generation numbers and the
 * NVS/RAM contents agree even when two writer tasks race a commit (e.g. a
 * browser password change racing a CLI SET WIFI, both landing in Tasks
 * 4/8): without it, both could read the same s_gen, both write NVS at the
 * same generation, and leave RAM pointing at whichever one happened to flip
 * s_active last -- a struct that may not match what's actually durable.
 * mcfg_get() readers still never take s_mux (see mcfg_store.h), so this
 * only serializes writers against each other, never against readers. */
int mcfg_commit(const hg_mcfg_t *m) {
    if (!m) return -1;

    if (!s_mux) {
        ESP_LOGE(TAG, "no mutex (xSemaphoreCreateMutex failed at init) -- refusing commit");
        return -2;
    }
    if (xSemaphoreTake(s_mux, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGW(TAG, "commit mutex busy (5000 ms) -- refusing commit");
        return -2;
    }

    char err[48];
    if (hg_mcfg_validate(m, NULL, err, sizeof err) != 0) {   /* NULL tzck: real check lands in Task 7 */
        ESP_LOGW(TAG, "validate failed: %s", err);
        xSemaphoreGive(s_mux);
        return -1;
    }

    uint32_t new_gen = s_gen + 1;
    uint8_t packed[MCFG_BUF_LEN];
    size_t n = hg_mcfg_pack(m, new_gen, packed, sizeof packed);
    if (n == 0) {
        ESP_LOGW(TAG, "pack failed (buffer too small)");
        xSemaphoreGive(s_mux);
        return -1;
    }

    nvs_handle_t handle;
    esp_err_t nerr = nvs_open("hg", NVS_READWRITE, &handle);
    if (nerr != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open failed: %s", esp_err_to_name(nerr));
        xSemaphoreGive(s_mux);
        return -2;
    }
    nerr = nvs_set_blob(handle, "mcfg", packed, n);
    if (nerr == ESP_OK) nerr = nvs_commit(handle);
    nvs_close(handle);
    if (nerr != ESP_OK) {
        ESP_LOGW(TAG, "nvs write failed: %s", esp_err_to_name(nerr));
        xSemaphoreGive(s_mux);
        return -2;
    }

    /* NVS write is durable now -- publish it to readers: fill the inactive
     * buffer, then flip the index (still under s_mux, so a concurrent
     * commit can't interleave its own swap) so mcfg_get() never returns a
     * torn struct. */
    int next = s_active ^ 1;
    s_buf[next] = *m;
    s_active = next;
    s_gen = new_gen;

    xSemaphoreGive(s_mux);
    return 0;
}
