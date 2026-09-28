#include <string.h>
#include "hg_image.h"

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void copy_name(char out[33], const uint8_t *src) {
    size_t i;
    for (i = 0; i < 32 && src[i]; i++) out[i] = (src[i] >= 0x20 && src[i] < 0x7F) ? (char)src[i] : '.';
    out[i] = '\0';
}

int hg_image_parse(const uint8_t *b, size_t n, hg_image_id_t *out) {
    if (!b || n < HG_IMG_ID_BYTES) return -1;
    if (b[0] != HG_IMG_MAGIC) return -2;
    if (rd32(b + 32) != HG_IMG_DESC_MAGIC) return -3;
    if (out) {
        out->chip_id = rd16(b + 12);
        copy_name(out->version, b + 48);
        copy_name(out->project, b + 80);
    }
    return 0;
}

int hg_image_is(const uint8_t *b, size_t n, uint16_t chip, const char *want) {
    hg_image_id_t id;
    if (!want || hg_image_parse(b, n, &id) != 0) return 0;
    return id.chip_id == chip && strcmp(id.project, want) == 0;
}
