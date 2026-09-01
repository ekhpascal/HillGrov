#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

uint32_t hg_crc32(uint32_t seed, const void *buf, size_t n);   /* CRC-32/ISO-HDLC; seed 0 for one-shot; feed previous result to continue */

typedef enum { HG_BLOB_OK = 0, HG_BLOB_MIGRATED,
               HG_BLOB_E_SHORT, HG_BLOB_E_MAGIC, HG_BLOB_E_LENGTH,
               HG_BLOB_E_CRC, HG_BLOB_E_VERSION_NEWER, HG_BLOB_E_VERSION_OLD } hg_blob_rc_t;

#define HG_BLOB_HDR_LEN 16u

size_t hg_blob_wrap(uint32_t magic, uint16_t version, uint32_t generation,
                    const void *payload, uint16_t len, uint8_t *out, size_t cap); /* bytes written, 0 = cap too small */

hg_blob_rc_t hg_blob_unwrap(uint32_t magic, uint16_t cur_ver, uint16_t min_compat,
                            const uint8_t *in, size_t in_len,
                            void *payload_out, uint16_t payload_cap, uint32_t *gen_out);

#ifdef __cplusplus
}
#endif
