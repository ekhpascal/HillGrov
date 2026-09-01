#include "rescue_handover.h"
#include "hg_blob.h"
#include <string.h>

/* Byte layout:
 * 0      magic u32 (4 bytes)
 * 4      ver u8 (1 byte) = 1
 * 5      expect_link u8 (1 byte)
 * 6..7   rsvd (2 bytes)
 * 8..40  ssid[33]
 * 41..105 pass[65]
 * 106..169 url[64]
 * 170..171 rsvd (2 bytes)
 * 172..175 crc32 (4 bytes, little-endian)
 */

_Static_assert(HG_HANDOVER_LEN == 176, "HG_HANDOVER_LEN must be 176");

int hg_handover_pack(const hg_handover_t *h, uint8_t out[HG_HANDOVER_LEN]) {
    if (!h || !out) return -1;

    /* Check string lengths - must fit NUL-terminated in their fields */
    /* ssid[33] can hold max 32 chars + NUL */
    if (strlen(h->ssid) > 32) return -1;
    /* pass[65] can hold max 64 chars + NUL */
    if (strlen(h->pass) > 64) return -1;
    /* url[64] can hold max 63 chars + NUL */
    if (strlen(h->url) > 63) return -1;

    /* Clear the buffer */
    memset(out, 0, HG_HANDOVER_LEN);

    /* Pack fields at specific offsets */
    uint32_t magic = HG_HANDOVER_MAGIC;
    memcpy(&out[0], &magic, 4);
    out[4] = 1;  /* version */
    out[5] = h->expect_link;
    /* out[6..7] reserved, already 0 from memset */

    /* Pack strings */
    strcpy((char *)&out[8], h->ssid);
    strcpy((char *)&out[41], h->pass);
    strcpy((char *)&out[106], h->url);

    /* Calculate and store CRC (bytes 0..171) little-endian at offset 172 */
    uint32_t crc = hg_crc32(0, out, 172);
    memcpy(&out[172], &crc, 4);

    return 0;
}

int hg_handover_unpack(const uint8_t in[HG_HANDOVER_LEN], hg_handover_t *out) {
    if (!in || !out) return -1;

    /* Verify magic */
    uint32_t magic;
    memcpy(&magic, &in[0], 4);
    if (magic != HG_HANDOVER_MAGIC) return -1;

    /* Verify version */
    if (in[4] != 1) return -1;

    /* Verify CRC */
    uint32_t crc_stored;
    memcpy(&crc_stored, &in[172], 4);
    uint32_t crc_calc = hg_crc32(0, in, 172);
    if (crc_calc != crc_stored) return -1;

    /* Extract fields */
    out->expect_link = in[5];

    /* Extract strings - verify NUL termination within bounds */
    const char *ssid_ptr = (const char *)&in[8];
    const char *pass_ptr = (const char *)&in[41];
    const char *url_ptr = (const char *)&in[106];

    /* Check for NUL within field bounds */
    if (memchr(ssid_ptr, '\0', 33) == NULL) return -1;
    if (memchr(pass_ptr, '\0', 65) == NULL) return -1;
    if (memchr(url_ptr, '\0', 64) == NULL) return -1;

    strcpy(out->ssid, ssid_ptr);
    strcpy(out->pass, pass_ptr);
    strcpy(out->url, url_ptr);

    return 0;
}
