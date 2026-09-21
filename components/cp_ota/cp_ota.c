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

static const char *TAG = "cp_ota";

/* cp_fw is 0x180000 B (master/partitions_p4.csv) -- exactly 1024 chunks of
 * EH_RPC_OTA_CHUNK_MAX (1536 B), no partial final chunk. The image itself is
 * shorter (~1.146 MB); the tail is whatever flash_app.py's --app cpfw left
 * behind (erased 0xFF on a clean chip), which is harmless -- the C6's own
 * esp_image loader reads exactly as many bytes as its image header
 * describes, the same way any ESP OTA partition is normally larger than the
 * image it holds. */
#define CP_OTA_CHUNK EH_RPC_OTA_CHUNK_MAX

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

    uint8_t first_byte;
    if (esp_partition_read(part, 0, &first_byte, 1) != ESP_OK || first_byte == 0xFF) {
        ESP_LOGW(TAG, "cp_fw is empty (erased) -- nothing staged, run "
                      "flash_app.py --app cpfw first");
        return -1;
    }

    esp_err_t rc = eh_host_cp_ota_begin();
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "eh_host_cp_ota_begin failed: %s", esp_err_to_name(rc));
        return -2;
    }

    static uint8_t buf[CP_OTA_CHUNK];
    uint32_t off = 0, rem = part->size;
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
                 (unsigned long)off, (unsigned long)part->size);
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
