#include "cp_ota.h"

/* Pure decision, compiled on every target (host tests included) -- see
 * cp_ota.h for why this can't just call esp_hosted's own
 * eh_host_mcu_transport_verify_fw_compat(). Bit layout matches esp_hosted's
 * EH_VERSION_VAL(major, minor, patch) == (major << 16) | (minor << 8) | patch. */
static uint32_t ver_major(uint32_t v) { return (v >> 16) & 0xFFu; }
static uint32_t ver_minor(uint32_t v) { return (v >> 8) & 0xFFu; }

int cp_ota_needed(uint32_t cp_ver) {
    return (ver_major(cp_ver) != ver_major(CP_OTA_HOST_VERSION) ||
            ver_minor(cp_ver) != ver_minor(CP_OTA_HOST_VERSION)) ? 1 : 0;
}

/* Explicit little-endian byte-offset read (C code rule: wire/persisted data
 * is packed by explicit offset, never via struct-cast) -- mirrors
 * hg_blob.c's and fw_srv.c's own private rd32() helpers. */
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Pure, host-tested (tests/host/test_cp_ota.c) -- see cp_ota.h for the
 * header format and what body_crc means. */
int cp_ota_parse_header(const uint8_t hdr[CP_OTA_HDR_LEN], uint32_t part_size,
                         uint32_t body_crc, uint32_t *len_out) {
    if (rd32(hdr) != CP_OTA_HDR_MAGIC) return -1;
    uint32_t len = rd32(hdr + 4);
    uint32_t want_crc = rd32(hdr + 8);
    if (len == 0 || (uint64_t)CP_OTA_HDR_LEN + len > part_size) return -1;
    if (body_crc != want_crc) return -1;
    *len_out = len;
    return 0;
}

/* Everything below is the RPC push -- not pure, not host-tested. HOST_TEST
 * (tests/host/CMakeLists.txt's hg_test()) compiles this file straight from
 * source for the host suite, which has no sdkconfig.h and no esp_hosted, so
 * this whole section is compiled out there, exactly like hg_model.c's
 * #ifndef HOST_TEST split. */
#ifndef HOST_TEST
#include "sdkconfig.h"

#if CONFIG_IDF_TARGET_ESP32P4
/* Real implementation: only the P4 master has the C6 co-processor esp_hosted
 * talks to over SDIO. components/cp_ota/CMakeLists.txt only pulls in the
 * esp_hosted component under this same CONFIG_IDF_TARGET_ESP32P4 gate --
 * see that file and Ruling 2 of the task-6 brief for why an unconditional
 * esp_hosted requirement here would break the zone/rescue builds (and the
 * ESP32 master build) that discover this component too but never fetch
 * esp_hosted at all. */

#include "esp_partition.h"
#include "esp_task_wdt.h"
#include "esp_log.h"
#include "eh_host_cp_ota.h"
#include "eh_host_mcu_transport_init_event.h"
#include "eh_host_feat_rpc_ext_v2_types.h"   /* EH_RPC_OTA_CHUNK_MAX */
#include "hg_blob.h"                         /* hg_crc32 -- same check-value family
                                                 fw_srv.c and tools/flash_app.py's
                                                 build_hgfw_image() use for this
                                                 header format */

static const char *TAG = "cp_ota";

#define CP_OTA_CHUNK EH_RPC_OTA_CHUNK_MAX   /* esp_hosted's RPC OTA frame limit, 1536 B */

/* esp_task_wdt_reset() logs an ESP_LOGE("task not found") every call for a
 * task that isn't subscribed (fw_srv.c's wdt_kick() found this first) --
 * app_main()'s task may or may not be subscribed by the time cp_ota_sync()
 * runs, so this is gated on esp_task_wdt_status() rather than called blind. */
static void wdt_kick(void) {
    if (esp_task_wdt_status(NULL) == ESP_OK) esp_task_wdt_reset();
}

int cp_ota_sync(void) {
    uint32_t cp_ver = eh_host_mcu_transport_get_fw_version();
    if (!cp_ota_needed(cp_ver)) {
        ESP_LOGI(TAG, "co-processor firmware 0x%08lx already matches the host -- no OTA",
                 (unsigned long)cp_ver);
        return 0;
    }

    /* Explicit subtype, not ESP_PARTITION_SUBTYPE_ANY (fw_srv.c and
     * http_upload_zone.c both use ANY and match zone_fw by name alone) --
     * cp_fw's subtype 0x41 is asserted against partitions_p4.csv by
     * tests/host/test_partitions_p4.c, which is what makes this lookup safe
     * against a hand-edited CSV. */
    const esp_partition_t *part =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x41, "cp_fw");
    if (!part) {
        ESP_LOGW(TAG, "cp_fw partition not found -- nothing staged");
        return -1;
    }

    uint8_t hdr[CP_OTA_HDR_LEN];
    if (esp_partition_read(part, 0, hdr, CP_OTA_HDR_LEN) != ESP_OK) {
        ESP_LOGW(TAG, "cp_fw header read failed -- nothing staged");
        return -1;
    }

    static uint8_t buf[CP_OTA_CHUNK];

    /* Validate BEFORE touching the radio (fix-round finding, Task 6 concern
     * 1): a truncated or corrupted staged image must be caught here, while
     * the C6 is still running working firmware. A half-written radio image
     * means no Wi-Fi at all, recoverable only from the board's C6-UART
     * header -- so this crc check is not optional polish.
     *
     * hdr's own length field isn't trusted yet (that's exactly what's being
     * validated), so the read below is capped at the partition's physical
     * remainder -- never a runaway read even if the header is garbage -- and
     * cp_ota_parse_header() below is the one place that decides the header
     * is actually good, the same way fw_srv.c's validate_image() folds its
     * own magic/length/crc checks into one decision. */
    uint32_t body_max = (uint32_t)(part->size - CP_OTA_HDR_LEN);
    uint32_t peek_len = rd32(hdr + 4);
    uint32_t scan_len = peek_len < body_max ? peek_len : body_max;

    uint32_t crc = 0, off = CP_OTA_HDR_LEN, rem = scan_len;
    while (rem) {
        wdt_kick();
        uint32_t n = rem < CP_OTA_CHUNK ? rem : CP_OTA_CHUNK;
        if (esp_partition_read(part, off, buf, n) != ESP_OK) {
            ESP_LOGW(TAG, "cp_fw read failed while validating -- nothing staged");
            return -1;
        }
        crc = hg_crc32(crc, buf, n);
        off += n;
        rem -= n;
    }

    uint32_t len;
    if (cp_ota_parse_header(hdr, part->size, crc, &len) != 0) {
        ESP_LOGW(TAG, "cp_fw image failed validation (magic/length/crc) -- "
                      "nothing staged (or a corrupt/truncated transfer)");
        return -1;
    }

    esp_err_t rc = eh_host_cp_ota_begin();
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "eh_host_cp_ota_begin failed: %s", esp_err_to_name(rc));
        return -2;
    }

    /* Push exactly the validated length, starting after the header -- not
     * the whole partition (the first cut of this component pushed all of
     * part->size, including trailing erased/stale bytes past the real
     * image). */
    off = CP_OTA_HDR_LEN;
    rem = len;
    int push_failed = 0;
    while (rem) {
        wdt_kick();   /* the whole push takes ~7.3 s; the TWDT is 8 s */
        uint32_t n = rem < CP_OTA_CHUNK ? rem : CP_OTA_CHUNK;
        if (esp_partition_read(part, off, buf, n) != ESP_OK ||
            eh_host_cp_ota_write(buf, n) != ESP_OK) {
            push_failed = 1;
            break;
        }
        off += n;
        rem -= n;
    }
    if (push_failed) {
        ESP_LOGE(TAG, "cp_fw OTA write failed at %lu/%lu B -- C6 keeps its old firmware",
                 (unsigned long)(off - CP_OTA_HDR_LEN), (unsigned long)len);
        return -2;   /* no eh_host_cp_ota_end()/activate(): the transfer never completed */
    }

    rc = eh_host_cp_ota_end();
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "eh_host_cp_ota_end failed: %s -- whole-image check rejected it, "
                      "C6 keeps its old firmware", esp_err_to_name(rc));
        return -2;
    }

    /* eh_host_cp_ota_activate() sets the C6's boot partition then reboots it
     * on its own ~2000 ms timer, inside the host's 5000 ms RPC wait -- so
     * the reply routinely never arrives and this call comes back ESP_FAIL
     * with esp_hosted logging "no response". That is the expected outcome,
     * not a failure of the push: the image was already accepted by
     * eh_host_cp_ota_end() above. Log either outcome at WARN and return 1;
     * per cp_ota.h, this return code is never proof by itself -- confirming
     * the new firmware is running means re-reading the C6's version after
     * the link re-handshakes, which is outside this function. */
    rc = eh_host_cp_ota_activate();
    if (rc != ESP_OK)
        ESP_LOGW(TAG, "eh_host_cp_ota_activate: %s (expected -- the C6 reboots inside "
                      "the RPC wait)", esp_err_to_name(rc));
    else
        ESP_LOGW(TAG, "eh_host_cp_ota_activate ok");
    return 1;
}

#else /* !CONFIG_IDF_TARGET_ESP32P4 */

int cp_ota_sync(void) {
    return 0;   /* the ESP32 master has no co-processor -- its Wi-Fi is on-die */
}

#endif /* CONFIG_IDF_TARGET_ESP32P4 */
#endif /* !HOST_TEST */
