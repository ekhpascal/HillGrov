#pragma once
/* Pure: identify an ESP-IDF app image from its first HG_IMG_ID_BYTES bytes, before anything is erased. The one copy of
 * this rule and of the project names (recovery design 4.2 / 5.7); the web upload, the panel's microSD install and
 * rescue_p4 all use it. Layout: esp_image_header_t (24 B, chip_id u16 at +12) + one esp_image_segment_header_t (8 B),
 * then esp_app_desc_t at +32: magic_word +32, version[32] +48, project_name[32] +80. */
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HG_IMG_ID_BYTES   112u
#define HG_IMG_MAGIC      0xE9u
#define HG_IMG_DESC_MAGIC 0xABCD5432u
#define HG_CHIP_ESP32     0x0000u
#define HG_CHIP_ESP32C6   0x000Du
#define HG_CHIP_ESP32P4   0x0012u
#define HG_PROJ_MASTER    "hillgrow_master"
#define HG_PROJ_ZONE      "hillgrow_zone"
#define HG_PROJ_RESCUE_P4 "hillgrow_rescue_p4"
#define HG_PROJ_CP        "eh_cp_wifi_softap"
typedef struct { uint16_t chip_id; char project[33]; char version[33]; } hg_image_id_t;
int hg_image_parse(const uint8_t *b, size_t n, hg_image_id_t *out);
    /* +0 magic, +12 chip_id u16 LE, +32 desc magic u32 LE, +48 version[32], +80 project_name[32] (NUL-bounded, non-printables
       -> '.'); 0 ok / -1 n < HG_IMG_ID_BYTES / -2 image magic / -3 desc magic */
int hg_image_is(const uint8_t *b, size_t n, uint16_t chip, const char *want);   /* 1 = parse ok && chip && project == want */

#ifdef __cplusplus
}
#endif
