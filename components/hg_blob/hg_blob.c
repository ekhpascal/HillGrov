#include <string.h>
#include "hg_blob.h"

uint32_t hg_crc32(uint32_t seed, const void *buf, size_t n) {
    uint32_t c = ~seed;
    const uint8_t *p = (const uint8_t *)buf;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(c & 1u)));
    }
    return ~c;
}

static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t *p, uint32_t v) { wr16(p, (uint16_t)v); wr16(p + 2, (uint16_t)(v >> 16)); }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) { return rd16(p) | ((uint32_t)rd16(p + 2) << 16); }

size_t hg_blob_wrap(uint32_t magic, uint16_t version, uint32_t generation,
                    const void *payload, uint16_t len, uint8_t *out, size_t cap) {
    if (cap < (size_t)HG_BLOB_HDR_LEN + len) return 0;
    wr32(out, magic);
    wr16(out + 4, version);
    wr16(out + 6, len);
    wr32(out + 8, generation);
    memcpy(out + HG_BLOB_HDR_LEN, payload, len);
    uint32_t c = hg_crc32(0, out, 12);
    c = hg_crc32(c, payload, len);
    wr32(out + 12, c);
    return (size_t)HG_BLOB_HDR_LEN + len;
}

hg_blob_rc_t hg_blob_unwrap(uint32_t magic, uint16_t cur_ver, uint16_t min_compat,
                            const uint8_t *in, size_t in_len,
                            void *payload_out, uint16_t payload_cap, uint32_t *gen_out) {
    if (in_len < HG_BLOB_HDR_LEN) return HG_BLOB_E_SHORT;
    if (rd32(in) != magic) return HG_BLOB_E_MAGIC;
    uint16_t len = rd16(in + 6);
    if (len > payload_cap || (size_t)HG_BLOB_HDR_LEN + len > in_len) return HG_BLOB_E_LENGTH;
    uint32_t c = hg_crc32(0, in, 12);
    c = hg_crc32(c, in + HG_BLOB_HDR_LEN, len);
    if (c != rd32(in + 12)) return HG_BLOB_E_CRC;
    uint16_t ver = rd16(in + 4);
    if (ver > cur_ver) return HG_BLOB_E_VERSION_NEWER;
    if (ver < min_compat) return HG_BLOB_E_VERSION_OLD;
    memset(payload_out, 0, payload_cap);
    memcpy(payload_out, in + HG_BLOB_HDR_LEN, len);
    if (gen_out) *gen_out = rd32(in + 8);
    return (ver < cur_ver || len < payload_cap) ? HG_BLOB_MIGRATED : HG_BLOB_OK;
}
