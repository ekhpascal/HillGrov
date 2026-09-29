#pragma once
/* Host fake of the few IDF nvs.h names pnl_cli.c uses (test_master_cmds only: its own include dir, so no other host
 * build sees an nvs.h). One namespace-less key store: fake_nvs_has_prefs says whether "prefs" exists. */
#include <stdint.h>
#include <stddef.h>

typedef int esp_err_t;
typedef uint32_t nvs_handle_t;
typedef enum { NVS_READONLY = 0, NVS_READWRITE } nvs_open_mode_t;
#define ESP_OK                0
#define ESP_FAIL              (-1)
#define ESP_ERR_NVS_NOT_FOUND 0x1102

extern int       fake_nvs_has_prefs;   /* 1: "panel"/"prefs" is stored */
extern esp_err_t fake_nvs_open_rc;     /* what nvs_open returns (ESP_OK, or e.g. ESP_FAIL for writes disabled) */
extern int       fake_nvs_commits, fake_nvs_open_n, fake_nvs_close_n;
void fake_nvs_reset(void);

esp_err_t nvs_open(const char *ns, nvs_open_mode_t mode, nvs_handle_t *out);
esp_err_t nvs_erase_key(nvs_handle_t h, const char *key);
esp_err_t nvs_commit(nvs_handle_t h);
void      nvs_close(nvs_handle_t h);
