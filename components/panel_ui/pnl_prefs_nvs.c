#include "esp_log.h"
#include "nvs.h"
#include "pnl_worker.h"
#include "pnl_prefs_nvs.h"

static const char *TAG = "pnl_prefs";
static pnl_prefs_t s_prefs;
static uint8_t     s_inflight;                    /* [LVGL] saves submitted whose done() has not run yet */
static pnl_prefs_save_state_t s_last = PNL_PREFS_SAVE_NONE;

void pnl_prefs_load(void) {
    pnl_prefs_defaults(&s_prefs);
    nvs_handle_t h;
    esp_err_t e = nvs_open("panel", NVS_READONLY, &h);
    if (e != ESP_OK) {
        if (e != ESP_ERR_NVS_NOT_FOUND) ESP_LOGW(TAG, "nvs_open(panel): %s -- defaults", esp_err_to_name(e));
        return;
    }
    uint8_t buf[64];
    size_t n = sizeof buf;
    e = nvs_get_blob(h, "prefs", buf, &n);
    nvs_close(h);
    if (e != ESP_OK) {
        if (e != ESP_ERR_NVS_NOT_FOUND) ESP_LOGW(TAG, "panel/prefs unreadable (%s) -- defaults", esp_err_to_name(e));
        return;
    }
    if (pnl_prefs_unpack(buf, n, &s_prefs) != 0) ESP_LOGW(TAG, "panel/prefs corrupt or from a newer image -- defaults");
}

const pnl_prefs_t *pnl_prefs_get(void) { return &s_prefs; }

void pnl_prefs_preview(const pnl_prefs_t *p) {
    pnl_prefs_t c = *p;
    pnl_prefs_clamp(&c);
    s_prefs = c;
}

static void save_run(pnl_job_t *j) { j->irc = pnl_prefs_save((const pnl_prefs_t *)j->arg); }

static void save_done(pnl_job_t *j) {
    if (s_inflight) s_inflight--;
    s_last = j->irc == 0 ? PNL_PREFS_SAVE_OK : PNL_PREFS_SAVE_FAILED;
}

void pnl_prefs_set(const pnl_prefs_t *p) {
    pnl_prefs_t c = *p;
    pnl_prefs_clamp(&c);
    s_prefs = c;
    if (pnl_worker_submit(save_run, save_done, &c, sizeof c) != 0) {
        ESP_LOGW(TAG, "preferences not queued for saving -- kept in RAM until the next change");
        s_last = PNL_PREFS_SAVE_FAILED;
        return;
    }
    s_inflight++;
}

pnl_prefs_save_state_t pnl_prefs_save_state(void) { return s_inflight ? PNL_PREFS_SAVE_PENDING : s_last; }

int pnl_prefs_save(const pnl_prefs_t *p) {
    if (pnl_on_lvgl_task()) { ESP_LOGE(TAG, "pnl_prefs_save called on the LVGL task -- refused"); return -1; }
    uint8_t buf[64];
    size_t n = pnl_prefs_pack(p, buf, sizeof buf);
    if (n == 0) return -1;
    nvs_handle_t h;
    esp_err_t e = nvs_open("panel", NVS_READWRITE, &h);
    if (e == ESP_OK) {
        e = nvs_set_blob(h, "prefs", buf, n);
        if (e == ESP_OK) e = nvs_commit(h);
        nvs_close(h);
    }
    if (e != ESP_OK) {
        /* recovery design 6.5: NVS may be deliberately read-only after a downgrade -- the value stays live in RAM */
        ESP_LOGW(TAG, "preferences not saved (%s) -- the new value stays in RAM", esp_err_to_name(e));
        return -1;
    }
    return 0;
}
