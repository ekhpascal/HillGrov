#pragma once
/* Pure (host-tested, tests/host/test_pnl_sd_pick.c): classify and order the .bin files found on the microSD card. */
#include <stdint.h>
#include "hg_image.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PNL_SD_NAME_MAX 64
typedef struct { char name[PNL_SD_NAME_MAX]; char path[96]; uint32_t size; uint8_t hdr[HG_IMG_ID_BYTES]; uint8_t hdr_len; } pnl_sd_file_t;
typedef enum { PNL_FW_UNKNOWN = 0, PNL_FW_MASTER, PNL_FW_ZONE, PNL_FW_WRONG_CHIP, PNL_FW_RADIO } pnl_fw_class_t;
int            pnl_sd_is_bin_name(const char *name);   /* case-insensitive ".bin" */
pnl_fw_class_t pnl_sd_classify(const pnl_sd_file_t *f, uint16_t self_chip, char version[33]);
     /* master on self_chip -> MASTER; hillgrow_master on another chip -> WRONG_CHIP; hillgrow_zone on ESP32 -> ZONE
        (on another chip -> WRONG_CHIP); eh_cp_wifi_softap on C6 -> RADIO (listed, not installable here); else UNKNOWN;
        size 0 -> UNKNOWN. version (may be NULL) gets the image's version, "" when UNKNOWN. */
void pnl_sd_sort(pnl_sd_file_t *v, pnl_fw_class_t *cls, int n);   /* MASTER, ZONE, RADIO, WRONG_CHIP, UNKNOWN; then by name */

#ifdef __cplusplus
}
#endif
