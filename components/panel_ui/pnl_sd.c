#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include "psvc_fw.h"          /* PSVC_FW_SRC_FAILED */
#include "pnl_worker.h"
#include "pnl_sd.h"

/* Never bsp_sdcard_mount(): it calls the real sdmmc_host_init() a second time (esp_hosted already created the one SDMMC
 * controller before app_main, so the claim fails) and its unmount never deletes the LDO handle. Same slot settings as
 * the BSP (slot 0 on its IOMUX pins, SDMMC_FREQ_HIGHSPEED, LDO VO4, width 4, no CD/WP), but host.init is ctlr_init()
 * below and deinit_p stays SDMMC_HOST_DEFAULT()'s sdmmc_host_deinit_slot: it removes slot 0, and when slot 1 is still
 * registered it maps the controller delete's "still in use" to ESP_OK (esp_driver_sdmmc/legacy/src/sdmmc_host.c), so
 * the C6 keeps its controller across every mount. */

static const char *TAG = "pnl_sd";

#define SD_LDO_CHAN 4   /* bsp_sdcard_mount(): on-chip LDO VO4 powers the SD IO (the DSI PHY is VO3) */

static sdmmc_card_t         *s_card;       /* non-NULL exactly while mounted */
static sd_pwr_ctrl_handle_t  s_pwr;
static volatile uint8_t      s_in_use;     /* pnl_sd_mounted(): slot 0 is (or may be) on the controller */

/* 1 once this layer created the controller itself (esp_hosted had not, this boot). The legacy driver's
 * sdmmc_host_deinit_slot() then DELETES it at every slot-0 teardown -- slot 0 is its only slot -- but never clears its
 * static s_ctlr, so a later no-op init would add a slot to freed memory. While this is 1, every mount creates the
 * controller afresh. */
static uint8_t s_ctlr_ours;

static esp_err_t ctlr_init(void) {
    if (!s_ctlr_ours) return ESP_OK;              /* esp_hosted created the one controller: nothing to do */
    esp_err_t e = sdmmc_host_init();
    if (e == ESP_ERR_NOT_FOUND) {
        /* the controller is claimed again -- esp_hosted came back (recovery) or ours outlived a failed slot add: it is
         * live, and the legacy s_ctlr points at it, so use it as it is */
        s_ctlr_ours = 0;
        return ESP_OK;
    }
    return e;
}

static pnl_sd_rc_t map_err(esp_err_t e) {
    switch (e) {
    case ESP_OK:                return PNL_SD_OK;
    case ESP_FAIL:              return PNL_SD_NO_FS;     /* no FAT volume: exFAT, unformatted or damaged -- never formatted */
    case ESP_ERR_TIMEOUT:
    case ESP_ERR_NOT_FOUND:
    case ESP_ERR_INVALID_RESPONSE:
    case ESP_ERR_INVALID_CRC:
    case ESP_ERR_NOT_SUPPORTED: return PNL_SD_NO_CARD;
    case ESP_ERR_INVALID_STATE: return PNL_SD_BUSY;
    default:                    return PNL_SD_IO;
    }
}

const char *pnl_sd_rc_text(pnl_sd_rc_t rc) {
    switch (rc) {
    case PNL_SD_OK:      return "OK";
    case PNL_SD_NO_CARD: return "No microSD card found -- insert a FAT32 card";
    case PNL_SD_NO_FS:   return "Card is not FAT32 -- exFAT cards (64 GB and up) must be reformatted to FAT32";
    case PNL_SD_BUSY:    return "microSD busy -- try again";
    default:             return "microSD read failed";
    }
}

pnl_sd_rc_t pnl_sd_mount(void) {
    if (pnl_on_lvgl_task()) { ESP_LOGE(TAG, "pnl_sd_mount called on the LVGL task -- refused"); return PNL_SD_IO; }
    if (s_in_use) return PNL_SD_BUSY;
    s_in_use = 1;                           /* before the slot add: D20's refusal covers the whole attempt */
    sd_pwr_ctrl_ldo_config_t ldo = { .ldo_chan_id = SD_LDO_CHAN };
    esp_err_t e = sd_pwr_ctrl_new_on_chip_ldo(&ldo, &s_pwr);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "SD power (LDO %d): %s", SD_LDO_CHAN, esp_err_to_name(e));
        s_pwr = NULL;
        s_in_use = 0;
        return PNL_SD_IO;
    }
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_0;
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;
    host.pwr_ctrl_handle = s_pwr;
    host.init = &ctlr_init;                /* deinit_p stays sdmmc_host_deinit_slot: removes slot 0 only */
    const sdmmc_slot_config_t slot = { .cd = SDMMC_SLOT_NO_CD, .wp = SDMMC_SLOT_NO_WP, .width = 4, .flags = 0 };
    const esp_vfs_fat_sdmmc_mount_config_t mc = {
        .format_if_mount_failed = false,   /* never: an exFAT card is refused, not wiped */
        .max_files = 2,
        .allocation_unit_size = 16 * 1024,
    };
    e = esp_vfs_fat_sdmmc_mount(PNL_SD_MOUNT, &host, &slot, &mc, &s_card);
    if (e == ESP_ERR_INVALID_ARG && !s_ctlr_ours) {
        /* the slot add found no controller: esp_hosted did not create it this boot (radio down). Create it here (the
         * card is then the only way to update this master); the teardown's sdmmc_host_deinit_slot() deletes it again
         * because slot 0 is then its only slot. If a controller does exist after all, ctlr_init() falls back to it. */
        ESP_LOGW(TAG, "no SDMMC controller (radio down this boot?) -- creating it for slot 0");
        s_ctlr_ours = 1;
        e = esp_vfs_fat_sdmmc_mount(PNL_SD_MOUNT, &host, &slot, &mc, &s_card);
    }
    if (e != ESP_OK) {
        sd_pwr_ctrl_del_on_chip_ldo(s_pwr);
        s_pwr = NULL;
        s_card = NULL;
        s_in_use = 0;
        ESP_LOGW(TAG, "microSD mount failed: %s", esp_err_to_name(e));
        return map_err(e);
    }
    return PNL_SD_OK;
}

void pnl_sd_unmount(void) {
    if (pnl_on_lvgl_task()) { ESP_LOGE(TAG, "pnl_sd_unmount called on the LVGL task -- refused"); return; }
    if (!s_card) return;
    esp_err_t e = esp_vfs_fat_sdcard_unmount(PNL_SD_MOUNT, s_card);
    if (e != ESP_OK) ESP_LOGW(TAG, "microSD unmount: %s", esp_err_to_name(e));
    if (s_pwr) { sd_pwr_ctrl_del_on_chip_ldo(s_pwr); s_pwr = NULL; }   /* the BSP's unmount never does this */
    s_card = NULL;
    s_in_use = 0;
}

int pnl_sd_mounted(void) { return s_in_use; }

static int scan_dir(const char *dir, pnl_sd_file_t *out, int cap, int n) {
    DIR *d = opendir(dir);
    if (!d) return n;
    struct dirent *de;
    while (n < cap && (de = readdir(d)) != NULL) {
        if (!pnl_sd_is_bin_name(de->d_name)) continue;
        pnl_sd_file_t *f = &out[n];
        memset(f, 0, sizeof *f);
        int w = snprintf(f->path, sizeof f->path, "%s/%s", dir, de->d_name);
        if (w < 0 || (size_t)w >= sizeof f->path) continue;          /* a name too long to open: skip it */
        struct stat st;
        if (stat(f->path, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        snprintf(f->name, sizeof f->name, "%.*s", (int)(sizeof f->name - 1), de->d_name);
        f->size = (uint32_t)st.st_size;
        FILE *fp = fopen(f->path, "rb");
        if (fp) {
            f->hdr_len = (uint8_t)fread(f->hdr, 1, HG_IMG_ID_BYTES, fp);
            fclose(fp);
        }
        n++;
    }
    closedir(d);
    return n;
}

int pnl_sd_list_bins(pnl_sd_file_t *out, int cap) {
    if (pnl_on_lvgl_task()) { ESP_LOGE(TAG, "pnl_sd_list_bins called on the LVGL task -- refused"); return -1; }
    if (!s_card || !out || cap <= 0) return s_card ? 0 : -1;
    int n = scan_dir(PNL_SD_MOUNT, out, cap, 0);
    return scan_dir(PNL_SD_DIR, out, cap, n);
}

int pnl_sd_read(void *src, void *buf, size_t cap) {
    pnl_sd_src_t *s = (pnl_sd_src_t *)src;
    if (!s || !s->f || s->left == 0) return PSVC_FW_SRC_FAILED;
    size_t want = cap < s->left ? cap : s->left;
    size_t got = fread(buf, 1, want, s->f);
    if (got != want) return PSVC_FW_SRC_FAILED;   /* short read: the card was pulled or the file is damaged */
    s->left -= (uint32_t)got;
    return (int)got;
}
