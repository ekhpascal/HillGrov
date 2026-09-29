#include <string.h>
#include "nvs.h"

int       fake_nvs_has_prefs;
esp_err_t fake_nvs_open_rc;
int       fake_nvs_commits, fake_nvs_open_n, fake_nvs_close_n;
static int s_open_panel;

void fake_nvs_reset(void) {
    fake_nvs_has_prefs = 0;
    fake_nvs_open_rc = ESP_OK;
    fake_nvs_commits = fake_nvs_open_n = fake_nvs_close_n = 0;
    s_open_panel = 0;
}

esp_err_t nvs_open(const char *ns, nvs_open_mode_t mode, nvs_handle_t *out) {
    (void)mode;
    if (fake_nvs_open_rc != ESP_OK) return fake_nvs_open_rc;
    fake_nvs_open_n++;
    s_open_panel = ns && strcmp(ns, "panel") == 0;
    *out = 1;
    return ESP_OK;
}

esp_err_t nvs_erase_key(nvs_handle_t h, const char *key) {
    (void)h;
    if (!s_open_panel || !key || strcmp(key, "prefs") != 0 || !fake_nvs_has_prefs) return ESP_ERR_NVS_NOT_FOUND;
    fake_nvs_has_prefs = 0;
    return ESP_OK;
}

esp_err_t nvs_commit(nvs_handle_t h) { (void)h; fake_nvs_commits++; return ESP_OK; }
void      nvs_close(nvs_handle_t h) { (void)h; fake_nvs_close_n++; }
