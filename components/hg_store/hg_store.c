#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_err.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "hg_blob.h"
#include "hg_cfg.h"
#include "hg_model.h"
#include "hg_store.h"

static const char *TAG = "hg_store";
#define WRAP_BUF_LEN 368u   /* 16 (hdr) + 336 (largest plane: cfg), rounded */

typedef struct {
    uint8_t     kind;          /* HG_CH_HW or HG_CH_CFG */
    const char *key;
    uint32_t    magic;
    uint16_t    ver;
    uint32_t    throttle_ms;   /* min spacing between successful writes */
    uint32_t    next_ms;       /* earliest tick allowed to (re)attempt a write */
    uint8_t     fail_count;
    uint8_t     pending;       /* wire[0..wire_len) holds a not-yet-durable write */
    uint16_t    wire_len;
    uint8_t     wire[WRAP_BUF_LEN];
} plane_t;

enum { PLANE_HW = 0, PLANE_CFG, PLANE_N };
static plane_t s_plane[PLANE_N] = {
    { HG_CH_HW,  "hw",  HG_MAGIC_HW,  HG_HW_VER,  0,    0, 0, 0, 0, {0} },
    { HG_CH_CFG, "cfg", HG_MAGIC_CFG, HG_CFG_VER, 5000, 0, 0, 0, 0, {0} },
};

static nvs_handle_t      s_nvs;
static uint8_t            s_zid = HG_NODE_UNASSIGNED;
static SemaphoreHandle_t  s_flush_sem;
static volatile uint8_t   s_force;
static union { hg_zone_hw_t hw; hg_zone_cfg_t cfg; } s_staging;

static uint32_t now_ms(void) { return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS); }

/* Wraps payload into the plane's own wire buffer and marks it pending; the
 * background pass (or an explicit flush) will get it into NVS. */
static void stage_write(int i, uint32_t gen, const void *payload, uint16_t len) {
    plane_t *p = &s_plane[i];
    size_t n = hg_blob_wrap(p->magic, p->ver, gen, payload, len, p->wire, sizeof p->wire);
    if (n == 0) { ESP_LOGE(TAG, "%s blob wrap failed (payload too large)", p->key); return; }
    p->wire_len = (uint16_t)n;
    p->pending = 1;
}

static void attempt_write(int i, int force) {
    plane_t *p = &s_plane[i];
    if (!p->pending) return;
    uint32_t t = now_ms();
    if (!force && (int32_t)(t - p->next_ms) < 0) return;   /* throttle/backoff window still open */

    esp_err_t e = nvs_set_blob(s_nvs, p->key, p->wire, p->wire_len);
    if (e == ESP_OK) e = nvs_commit(s_nvs);
    if (e == ESP_OK) {
        p->pending = 0;
        p->fail_count = 0;
        p->next_ms = t + p->throttle_ms;
        return;
    }
    ESP_LOGE(TAG, "%s nvs write failed: %s", p->key, esp_err_to_name(e));
    p->fail_count++;
    p->next_ms = t + 30000u;                                /* back off 30 s before retrying */
    if (p->fail_count >= 3) {
        ESP_LOGE(TAG, "F_NVS placeholder: %s write persistently failing", p->key);
        p->fail_count = 0;
    }
}

/* Pull any freshly-dirtied plane out of hg_model and stage it for writing. */
static void stage_dirty(int force) {
    uint8_t dirty = hg_model_dirty_mask();
    for (int i = 0; i < PLANE_N; i++) {
        plane_t *p = &s_plane[i];
        if (p->pending) continue;                           /* previous write still outstanding */
        if (!(dirty & p->kind)) continue;
        uint32_t t = now_ms();
        if (!force && (int32_t)(t - p->next_ms) < 0) continue;
        void *dst = (p->kind == HG_CH_HW) ? (void *)&s_staging.hw : (void *)&s_staging.cfg;
        uint32_t gen;
        uint32_t size = hg_model_take_dirty(p->kind, dst, &gen);
        if (size == 0) continue;                            /* race: bit cleared elsewhere */
        stage_write(i, gen, dst, (uint16_t)size);
    }
}

static int settled(void) {
    return hg_model_dirty_mask() == 0 && !s_plane[PLANE_HW].pending && !s_plane[PLANE_CFG].pending;
}

static void store_pass(void) {
    int force = s_force;
    stage_dirty(force);
    for (int i = 0; i < PLANE_N; i++) attempt_write(i, force);
    if (force) {
        /* A plane that was pending AND freshly dirty needs its outstanding
         * write to clear before the new edit can be staged -- retry once
         * more within this same forced pass rather than leaving it stuck
         * behind a throttle window for a whole extra store_task wake. */
        stage_dirty(force);
        for (int i = 0; i < PLANE_N; i++) attempt_write(i, force);
        /* Only release the latch once nothing is left outstanding, so a
         * concurrent hg_store_flush() (which re-arms it every poll tick,
         * see below) never sees it drop out from under a still-pending
         * write purely due to preemption timing. */
        if (settled()) s_force = 0;
    }
}

static void store_task(void *arg) {
    (void)arg;
    esp_task_wdt_add(NULL);
    for (;;) {
        esp_task_wdt_reset();
        xSemaphoreTake(s_flush_sem, pdMS_TO_TICKS(1000));    /* wakes on flush or 1000 ms */
        store_pass();
    }
}

void hg_store_start(void) {
    if (!s_flush_sem) s_flush_sem = xSemaphoreCreateBinary();
    xTaskCreatePinnedToCore(store_task, "hg_store", 3072, NULL, 2, NULL, 0);
}

int hg_store_flush(uint32_t timeout_ms) {
    uint32_t start = now_ms();
    for (;;) {
        if (settled()) return 0;
        /* Re-arm on every poll tick (not just once): a single give racing
         * against store_task's own 1000 ms idle wake, or against a
         * background pass that clears the latch one plane early, must not
         * strand a caller with a plane still outstanding. */
        s_force = 1;
        if (s_flush_sem) xSemaphoreGive(s_flush_sem);
        if (now_ms() - start >= timeout_ms) {
            /* Giving up: release the latch so a genuinely failing NVS falls
             * back to attempt_write's normal throttle/backoff instead of
             * every subsequent background pass running forced forever
             * (bypassing the 30 s backoff and spamming F_NVS every ~3 s). */
            s_force = 0;
            return -1;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

uint8_t hg_store_zid(void) { return s_zid; }

int hg_store_set_zid(uint8_t id) {
    esp_err_t err = nvs_set_u8(s_nvs, "zid", id);
    if (err == ESP_OK) err = nvs_commit(s_nvs);
    if (err != ESP_OK) { ESP_LOGE(TAG, "zid write failed: %s", esp_err_to_name(err)); return -1; }
    s_zid = id;
    return 0;
}

int hg_store_factory_reset(void) {
    esp_err_t err = nvs_erase_all(s_nvs);
    if (err == ESP_OK) err = nvs_commit(s_nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "factory reset erase failed: %s", esp_err_to_name(err));
        return -1;
    }
    esp_restart();
    return -1; /* unreachable: esp_restart() never returns */
}

/* ---- boot load: NVS -> hg_blob_unwrap -> hg_*_validate -> hg_model_boot_load ---- */

static int load_hw(hg_zone_hw_t *hw, int *ok) {
    uint8_t buf[HG_BLOB_HDR_LEN + sizeof *hw];
    size_t len = sizeof buf;
    esp_err_t err = nvs_get_blob(s_nvs, "hw", buf, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) { *ok = 0; return 0; }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "hw nvs_get_blob: %s -- using defaults", esp_err_to_name(err));
        *ok = 0; return -1;
    }
    uint32_t gen;
    hg_blob_rc_t brc = hg_blob_unwrap(HG_MAGIC_HW, HG_HW_VER, HG_HW_VER_MIN, buf, len, hw, sizeof *hw, &gen);
    if (brc != HG_BLOB_OK && brc != HG_BLOB_MIGRATED) {
        ESP_LOGW(TAG, "hw blob invalid (rc=%d) -- using defaults", (int)brc);
        *ok = 0; return -1;
    }
    char verr[48];
    if (hg_hw_validate(hw, verr, sizeof verr) != 0) {
        ESP_LOGW(TAG, "hw validate failed: %s -- using defaults", verr);
        *ok = 0; return -1;
    }
    *ok = 1;
    if (brc == HG_BLOB_MIGRATED) stage_write(PLANE_HW, gen, hw, (uint16_t)sizeof *hw);
    return 0;
}

static int load_cfg(hg_zone_cfg_t *cfg, const hg_zone_hw_t *hw_for_validate, int *ok) {
    uint8_t buf[HG_BLOB_HDR_LEN + sizeof *cfg];
    size_t len = sizeof buf;
    esp_err_t err = nvs_get_blob(s_nvs, "cfg", buf, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) { *ok = 0; return 0; }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "cfg nvs_get_blob: %s -- using defaults", esp_err_to_name(err));
        *ok = 0; return -1;
    }
    uint32_t gen;
    hg_blob_rc_t brc = hg_blob_unwrap(HG_MAGIC_CFG, HG_CFG_VER, HG_CFG_VER_MIN, buf, len, cfg, sizeof *cfg, &gen);
    if (brc != HG_BLOB_OK && brc != HG_BLOB_MIGRATED) {
        ESP_LOGW(TAG, "cfg blob invalid (rc=%d) -- using defaults", (int)brc);
        *ok = 0; return -1;
    }
    char verr[48];
    if (hg_cfg_validate(cfg, hw_for_validate, verr, sizeof verr) != 0) {
        ESP_LOGW(TAG, "cfg validate failed: %s -- using defaults", verr);
        *ok = 0; return -1;
    }
    *ok = 1;
    if (brc == HG_BLOB_MIGRATED) stage_write(PLANE_CFG, gen, cfg, (uint16_t)sizeof *cfg);
    return 0;
}

int hg_store_init(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "nvs partition needs erase (%s), retrying once", esp_err_to_name(err));
        err = nvs_flash_erase();
        if (err == ESP_OK) err = nvs_flash_init();
    }
    if (err != ESP_OK) { ESP_LOGE(TAG, "nvs_flash_init failed: %s", esp_err_to_name(err)); return -1; }

    err = nvs_open("hg", NVS_READWRITE, &s_nvs);
    if (err != ESP_OK) { ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err)); return -1; }

    int rc = 0;
    uint8_t zid;
    err = nvs_get_u8(s_nvs, "zid", &zid);
    if (err == ESP_OK) s_zid = zid;
    else if (err != ESP_ERR_NVS_NOT_FOUND) { ESP_LOGW(TAG, "zid read: %s", esp_err_to_name(err)); rc = -1; }

    hg_model_init();

    static hg_zone_hw_t  hw;
    static hg_zone_cfg_t cfg;
    int hw_ok = 0, cfg_ok = 0;
    if (load_hw(&hw, &hw_ok) != 0) rc = -1;
    hg_zone_hw_t hw_for_cfg;
    if (hw_ok) hw_for_cfg = hw; else hg_defaults_hw(&hw_for_cfg);
    if (load_cfg(&cfg, &hw_for_cfg, &cfg_ok) != 0) rc = -1;

    hg_model_boot_load(hw_ok ? &hw : NULL, cfg_ok ? &cfg : NULL);
    return rc;
}
