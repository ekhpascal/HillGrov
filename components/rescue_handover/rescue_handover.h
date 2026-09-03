#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HG_HANDOVER_MAGIC 0x48524748u  /* 'HGRH' LE */
#define HG_HANDOVER_LEN   176

typedef struct {
    uint8_t expect_link;               /* 1 = fleet path: trial requires a Master frame (SP2) */
    char ssid[33], pass[65], url[64];
} hg_handover_t;

int hg_handover_pack(const hg_handover_t *h, uint8_t out[HG_HANDOVER_LEN]);            /* 0 / -1 bad strings */
int hg_handover_unpack(const uint8_t in[HG_HANDOVER_LEN], hg_handover_t *out);         /* 0 / -1 invalid */

/* target-only NVS glue (namespace "hg", key "hando") */
int hg_handover_write(const hg_handover_t *h);      /* wraps pack + nvs_set_blob + commit */
int hg_handover_take(hg_handover_t *out);           /* read + erase (one-shot); -1 absent/corrupt */

/* target-only NVS glue (namespace "hg", key "trial") -- SP3 rescue-to-app breadcrumb:
 * rescue_pull() writes the consumed handover's expect_link here right after the new
 * boot partition is set, so the freshly-flashed app's ota_trial can read (and erase)
 * it on its own first boot; that is the only channel between the two apps. Best-effort
 * (ESP_LOGW on failure): a write failure only loses the fleet-vs-bench trial-window
 * hint (the app's read side defaults to expect_link 0 when absent), never the pull. */
int hg_trial_write(uint8_t expect_link);

#ifdef __cplusplus
}
#endif
