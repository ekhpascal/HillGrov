#include <stdio.h>
#include <string.h>
#include "esp_partition.h"
#include "esp_task_wdt.h"
#include "esp_log.h"
#include "hg_blob.h"   /* hg_crc32 */
#include "fw_srv.h"
#include "http_upload.h"

static const char *TAG = "http_upload_zone";

/* POST /api/fw/zone writes the SAME storage layout tools/flash_app.py's
 * build_zonefw_image() produces and fw_srv.c validates: a 16-byte header at
 * zone_fw+0 -- { 'HGFW' u32 LE, len u32 LE, crc32 u32 LE, rsvd u32 } --
 * followed by the raw zone app image at zone_fw+16. crc32 is hg_crc32(0,
 * image, len), i.e. plain CRC-32/ISO-HDLC seeded 0 (binascii.crc32), NOT the
 * 0xFFFFFFFF-seeded otadata convention.
 *
 * Order matters, and it is the whole safety argument of this file:
 *   1. the header sector is erased FIRST and fw_srv_revalidate() is called
 *      immediately, so from that instant fw_srv_image_ok() reads 0. A fleet
 *      PRECHECK running on node_mgr's task during the ~2 s bulk erase below
 *      therefore sees "no image" instead of being handed a Content-Length for
 *      an image that is being erased under it;
 *   2. the rest of the partition is erased in 64 KB steps, feeding the TWDT;
 *   3. the body streams in at offset 16, chaining the CRC as it goes;
 *   4. the header is written LAST, then fw_srv_revalidate() re-reads the
 *      whole image from flash and re-checks the CRC. A zone only ever pulls
 *      an image that has been read back and verified after the write.
 * Any failure leaves the header erased, which is exactly "no image" to
 * fw_srv.c (404 FW_NO_IMAGE) and to the fleet sequencer's PRECHECK.
 *
 * All of this runs on the httpd task -- the same task as fw_srv.c's
 * /fw/zone.bin handler, which is why the shared 4 KB buffer and the cached
 * verdict inside fw_srv.c stay single-threaded (see fw_srv_revalidate's
 * contract in fw_srv.h). */

#define FW_HDR_LEN   16u
#define FW_HDR_MAGIC 0x57464748u   /* 'HGFW' LE */
#define SECTOR       4096u
#define ERASE_STEP   (64u * 1024u) /* multiple of SECTOR; ~24 steps over 1.5 MB */

static const esp_partition_t *s_part;
static uint32_t s_crc, s_len;
static size_t   s_off;

static const esp_partition_t *part(void) {
    if (!s_part) s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "zone_fw");
    return s_part;
}

size_t http_upload_zone_max(void) {
    const esp_partition_t *p = part();
    return p ? p->size - FW_HDR_LEN : 0;
}

/* Explicit little-endian byte-offset writes -- the mirror of fw_srv.c's
 * rd32(), and the project's rule for anything persisted or on a wire. */
static void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static int erase_header(void) {
    const esp_partition_t *p = part();
    if (!p) return -1;
    esp_err_t rc = esp_partition_erase_range(p, 0, SECTOR);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "erase header sector: %s", esp_err_to_name(rc));
        return -1;
    }
    /* Drops fw_srv's cached verdict to "no image" (the header is gone, so the
     * re-validation is expected to fail -- its nonzero return is not an
     * error here). */
    (void)fw_srv_revalidate();
    return 0;
}

static int zone_begin(size_t content_len) {
    const esp_partition_t *p = part();
    if (!p) return -1;
    if (erase_header() != 0) return -1;

    ESP_LOGW(TAG, "erasing zone_fw (%u B) for a %u B image", (unsigned)p->size, (unsigned)content_len);
    for (size_t off = SECTOR; off < p->size; off += ERASE_STEP) {
        size_t n = p->size - off;
        if (n > ERASE_STEP) n = ERASE_STEP;
        esp_task_wdt_reset();
        esp_err_t rc = esp_partition_erase_range(p, off, n);
        if (rc != ESP_OK) {
            ESP_LOGE(TAG, "erase zone_fw at %u: %s", (unsigned)off, esp_err_to_name(rc));
            return -1;
        }
    }
    esp_task_wdt_reset();

    s_crc = 0;
    s_len = 0;
    s_off = FW_HDR_LEN;
    return 0;
}

static int zone_write(const void *buf, size_t n) {
    esp_err_t rc = esp_partition_write(s_part, s_off, buf, n);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "write zone_fw at %u: %s", (unsigned)s_off, esp_err_to_name(rc));
        return -1;
    }
    s_crc  = hg_crc32(s_crc, buf, n);
    s_off += n;
    s_len += (uint32_t)n;
    return 0;
}

static int zone_finish(char *resp, size_t cap) {
    uint8_t hdr[FW_HDR_LEN];
    wr32(hdr + 0,  FW_HDR_MAGIC);
    wr32(hdr + 4,  s_len);
    wr32(hdr + 8,  s_crc);
    wr32(hdr + 12, 0);

    esp_err_t rc = esp_partition_write(s_part, 0, hdr, FW_HDR_LEN);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "write zone_fw header: %s", esp_err_to_name(rc));
        return -1;
    }

    /* Read-back verification: fw_srv_revalidate() re-reads len bytes from
     * flash and re-checks the CRC against the header just written, so a 200
     * means the zone image on this board has been verified from flash, not
     * merely streamed at it. ~0.5 s for a ~700 KB image; the TWDT is fed on
     * both sides of it (this task is subscribed for the whole upload). */
    esp_task_wdt_reset();
    int ok = fw_srv_revalidate();
    esp_task_wdt_reset();
    if (ok != 0) {
        ESP_LOGE(TAG, "zone_fw failed read-back validation after %u B", (unsigned)s_len);
        return -1;
    }

    snprintf(resp, cap, "{\"ok\":true,\"len\":%lu}", (unsigned long)s_len);
    ESP_LOGW(TAG, "zone image stored: %u B, crc32 %08lx", (unsigned)s_len, (unsigned long)s_crc);
    return 0;
}

static void zone_cancel(void) {
    /* The header is the whole verdict, so erasing that one sector is enough
     * to leave "no image" behind -- and it is the only thing that must be
     * true after a failed upload. */
    (void)erase_header();
}

static const upload_sink_t ZONE_SINK = {
    .begin = zone_begin, .write = zone_write, .finish = zone_finish, .cancel = zone_cancel
};

const upload_sink_t *http_upload_zone_sink(void) { return &ZONE_SINK; }
