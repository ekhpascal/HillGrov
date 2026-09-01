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

#ifdef __cplusplus
}
#endif
