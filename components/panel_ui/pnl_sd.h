#pragma once
/* Glue: transient microSD access on SDMMC slot 0, beside esp_hosted on slot 1 of the same controller. Mount, use,
 * unmount -- always on the panel worker, never held. FAT32 only (IDF 6.0.1 FATFS has no exFAT) and never formatted. */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "pnl_sd_pick.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { PNL_SD_OK = 0, PNL_SD_NO_CARD, PNL_SD_NO_FS, PNL_SD_BUSY, PNL_SD_IO } pnl_sd_rc_t;
#define PNL_SD_MOUNT "/sdcard"
#define PNL_SD_DIR   "/sdcard/hillgrow"
pnl_sd_rc_t pnl_sd_mount(void);     /* [WORKER] no-op host.init (esp_hosted holds the controller); if the slot add fails for want
     of a controller, retry once with the real sdmmc_host_init; format_if_mount_failed false; max_files 2; ESP_FAIL -> NO_FS
     ("Card is not FAT32 -- exFAT cards (64 GB and up) must be reformatted to FAT32"); card timeouts -> NO_CARD; already
     mounted -> BUSY. The LDO-4 power handle is created here and deleted by the unmount (or by a failed mount). */
void        pnl_sd_unmount(void);   /* [WORKER] unmount + delete the LDO handle; slot 0 removed, controller stays for the C6 */
int         pnl_sd_mounted(void);   /* [ANY] the recovery plan's esp_hosted deinit/reconnect must refuse while this is 1 (D20).
     1 from the start of pnl_sd_mount() until pnl_sd_unmount() -- or a failed mount's cleanup -- has returned: slot 0 is on
     the shared controller for that whole span, including a mount attempt still waiting out a card timeout. */
int         pnl_sd_list_bins(pnl_sd_file_t *out, int cap);   /* [WORKER] *.bin in "/" and PNL_SD_DIR, first 112 bytes each;
                                                                 n >= 0, or -1 when not mounted */
typedef struct { FILE *f; uint32_t left; } pnl_sd_src_t;
int         pnl_sd_read(void *src, void *buf, size_t cap);   /* psvc_fw_read_fn: >0, or PSVC_FW_SRC_FAILED on a short read */
const char *pnl_sd_rc_text(pnl_sd_rc_t rc);                  /* [ANY] operator text for a mount result */

#ifdef __cplusplus
}
#endif
