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
 * talks to over SDIO. components/cp_ota/CMakeLists.txt attaches the
 * espressif__esp_hosted requirement with idf_component_optional_requires()
 * AFTER idf_component_register() (NOT a CONFIG_IDF_TARGET_ESP32P4 guard
 * around REQUIRES/PRIV_REQUIRES -- fix round 1 found that pattern silently
 * resolves an EMPTY requirements list on a real P4 build, even though
 * components/soc/CMakeLists.txt's superficially similar use of that same
 * `if(CONFIG_IDF_TARGET_ESP32P4)` guard only ever feeds SRCS/INCLUDE_DIRS,
 * never REQUIRES; see that CMakeLists.txt's own comment for the full story).
 * idf_component_optional_requires() checks the already-finalized
 * BUILD_COMPONENTS list and is a silent no-op when espressif__esp_hosted
 * isn't part of the build -- true for zone, rescue and an esp32 (non-P4)
 * master, all of which discover this component too but never fetch
 * esp_hosted at all. */

#include "esp_partition.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "eh_host_cp_ota.h"
#include "eh_host_mcu_transport_init_event.h"
#include "eh_host_feat_rpc_ext_v2_types.h"   /* EH_RPC_OTA_CHUNK_MAX */
#include "eh_common_caps.h"                  /* EH_PRIV_FIRMWARE_CHIP_UNRECOGNIZED --
                                                 transitively public via eh_common's
                                                 own PUBLIC include dir, the same
                                                 propagation path that already brings
                                                 in eh_host_mcu_transport_init_event.h */
#include "hg_blob.h"                         /* hg_crc32 -- same check-value family
                                                 fw_srv.c and tools/flash_app.py's
                                                 build_hgfw_image() use for this
                                                 header format */

static const char *TAG = "cp_ota";

#define CP_OTA_CHUNK EH_RPC_OTA_CHUNK_MAX   /* esp_hosted's RPC OTA frame limit, 1536 B */

/* Fix round 2 finding: the C6 reboots into the new image on its own timer
 * after activate() (see the comment on that call below); this bounds how long
 * cp_ota_sync() waits for it to come back reporting a version
 * cp_ota_needed() accepts, polled every CP_OTA_REHANDSHAKE_POLL_MS.
 *
 * Deferred-findings #6, now closed as far as it can be: that reboot timer is
 * a TICK COUNT ON THE OTHER CHIP, so what it is worth in wall-clock
 * milliseconds depends on the co-processor's FreeRTOS tick rate -- a value
 * this host cannot read and therefore cannot assert. The window works today
 * only because coproc/sdkconfig.defaults pins CONFIG_FREERTOS_HZ=1000, which
 * is what makes 2000 ticks 2000 ms; at IDF's 100 Hz default the same 2000
 * ticks would be 20 s, this poll would give up first, and every genuinely
 * SUCCESSFUL radio update would report -2 -- a false negative on a state the
 * master does not recover from by itself. The coupling cannot be asserted
 * from here, so it is made legible instead: the timeout is derived from the
 * two assumptions rather than written as one number, and the timeout's own
 * error log names both, so that failure points at the co-processor's tick
 * rate instead of looking like a dead radio. */
#define CP_OTA_ASSUMED_CP_REBOOT_TICKS 2000u   /* esp_hosted's post-activate reboot delay */
#define CP_OTA_ASSUMED_CP_TICK_HZ      1000u   /* coproc/sdkconfig.defaults: CONFIG_FREERTOS_HZ */
#define CP_OTA_ASSUMED_CP_REBOOT_MS \
    (CP_OTA_ASSUMED_CP_REBOOT_TICKS * 1000u / CP_OTA_ASSUMED_CP_TICK_HZ)
/* SDIO re-enumeration plus a fresh init-event exchange on top of the reboot.
 * Keeps the total at the 6000 ms this was carrying as a literal, which is the
 * same order of magnitude as esp_hosted's own 5000 ms RPC wait convention
 * used elsewhere in this file. */
#define CP_OTA_REHANDSHAKE_MARGIN_MS   4000u
#define CP_OTA_REHANDSHAKE_TIMEOUT_MS \
    (CP_OTA_ASSUMED_CP_REBOOT_MS + CP_OTA_REHANDSHAKE_MARGIN_MS)
#define CP_OTA_REHANDSHAKE_POLL_MS      250u

/* esp_task_wdt_reset() logs an ESP_LOGE("task not found") every call for a
 * task that isn't subscribed (fw_srv.c's wdt_kick() found this first). Gated
 * on esp_task_wdt_status() rather than called blind. Fix round 2: verified
 * this is a NO-OP TODAY -- cp_ota_sync()'s only caller is app_main()'s boot
 * task, and app_main.c never calls esp_task_wdt_add(NULL) (unlike
 * fw_srv.c:215, http_upload.c:233 and rescue/main/app_main.c:98, which all
 * subscribe their own task before a long operation). It stays gated rather
 * than removed because a FUTURE caller might be subscribed -- cmd_task.c:32
 * already is -- and eh_host_cp_ota_begin() alone can block up to
 * EH_HOST_OTA_BEGIN_TIMEOUT_MS (30000 ms, eh_host_port_master_config.h: the
 * CP erases its whole OTA slot inside it), which would panic-reboot a
 * subscribed caller under the 8 s PANIC=y default long before begin()
 * itself returns. */
static void wdt_kick(void) {
    if (esp_task_wdt_status(NULL) == ESP_OK) esp_task_wdt_reset();
}

int cp_ota_sync(void) {
    /* Fix round 2, Major 4: gate on the CP link actually being up BEFORE
     * trusting get_fw_version(). Without this, an unresponsive C6 (SDIO
     * bus not yet enumerated, or the init event not parsed yet this boot)
     * makes get_fw_version() return 0 -- indistinguishable from a factory
     * Waveshare CP genuinely reporting 0.0.0 -- and the host would commit
     * to a push whose begin() alone can block 30 s, ahead of
     * ring_link_start()/node_mgr_start() if this were called before them
     * (it no longer is -- see app_main.c).
     *
     * eh_host_mcu_transport_get_chip_id() is the right predicate, not
     * eh_host_mcu_transport_peer_advertised_rpc_version(): chip id is a
     * MANDATORY TLV that process_init_event() parses first and validates
     * before it will accept the event at all (eh_host_mcu_transport_
     * init_event.c: an unrecognised chip id resets it back to
     * EH_PRIV_FIRMWARE_CHIP_UNRECOGNIZED and the whole parse fails), so
     * "not UNRECOGNIZED" genuinely means "the init event was parsed" on
     * every CP firmware version. RPC-version advertisement, by contrast, is
     * an OPTIONAL TLV (tag 0x1A) -- the header comment on that function
     * says outright it is only set "iff" the peer's init event carried that
     * tag, so an up-and-parsed but older-firmware CP that never sends it
     * would read as "down" forever and this OTA path -- which exists
     * specifically to fix an old/factory CP -- would never fire for the
     * CP it is most needed for. */
    if (eh_host_mcu_transport_get_chip_id() == EH_PRIV_FIRMWARE_CHIP_UNRECOGNIZED) {
        ESP_LOGW(TAG, "co-processor link not up yet (init event not parsed) -- "
                      "skipping the OTA check this boot");
        return 0;
    }

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
        /* An I/O failure is not "nothing staged" (cp_ota.h's -1 case) --
         * there may well be a good image behind an unreadable header; the
         * flash itself is the problem. Fix round 2 minor: this file's -1
         * paths used to blur that distinction. Still -1 (the caller can't
         * push what it can't read either way), but the log says what
         * actually happened. */
        ESP_LOGE(TAG, "cp_fw header read failed (flash I/O error) -- cannot verify what's staged");
        return -1;
    }

    /* Check the header's own claims BEFORE streaming anything, the same
     * order fw_srv.c's validate_image() uses (magic, then length, both
     * ahead of the read loop) -- fix round 2 minor: this used to defer the
     * length/magic verdict until AFTER a full read+crc pass over up to
     * part_size-16 bytes, so a garbage or erased header paid for the whole
     * scan before being rejected. cp_ota_parse_header() below re-checks
     * magic and length anyway (it is the single source of truth for the
     * accept/reject decision, and stays testable in isolation from this
     * raw peek), but that recheck is over already-known-good bytes once
     * this gate has passed -- it never causes a second scan. */
    uint32_t len = rd32(hdr + 4);
    if (rd32(hdr) != CP_OTA_HDR_MAGIC || len == 0 ||
        (uint64_t)CP_OTA_HDR_LEN + len > part->size) {
        ESP_LOGW(TAG, "cp_fw header invalid (magic/length) -- nothing staged");
        return -1;
    }

    /* Validate BEFORE touching the radio (fix-round finding, Task 6 concern
     * 1): a truncated or corrupted staged image must be caught here, while
     * the C6 is still running working firmware. A half-written radio image
     * means no Wi-Fi at all, recoverable only from the board's C6-UART
     * header -- so this crc check is not optional polish. */
    static uint8_t buf[CP_OTA_CHUNK];
    uint32_t crc = 0, off = CP_OTA_HDR_LEN, rem = len;
    while (rem) {
        wdt_kick();
        uint32_t n = rem < CP_OTA_CHUNK ? rem : CP_OTA_CHUNK;
        if (esp_partition_read(part, off, buf, n) != ESP_OK) {
            ESP_LOGE(TAG, "cp_fw body read failed while validating (flash I/O error) -- "
                          "cannot verify what's staged");
            return -1;
        }
        crc = hg_crc32(crc, buf, n);
        off += n;
        rem -= n;
    }

    uint32_t validated_len;
    if (cp_ota_parse_header(hdr, part->size, crc, &validated_len) != 0) {
        ESP_LOGW(TAG, "cp_fw image failed crc validation -- corrupt or truncated transfer, "
                      "nothing pushed");
        return -1;
    }
    len = validated_len;   /* == what was already checked above; parse_header is still
                               the one place that decided this, per its own contract */

    wdt_kick();   /* freshest possible margin before a call that can block up to
                     EH_HOST_OTA_BEGIN_TIMEOUT_MS (30000 ms) -- see wdt_kick()'s
                     own comment above for why this matters to nobody today. */
    esp_err_t rc = eh_host_cp_ota_begin();
    wdt_kick();
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
        /* Kicks on progress -- a no-op today (see wdt_kick()'s comment: no
         * caller of cp_ota_sync() is TWDT-subscribed yet), but would matter
         * to a future subscribed caller: the push moves ~1.15 MB in ~7.3 s,
         * comfortably inside the 8 s PANIC=y default IF fed, a panic if not. */
        wdt_kick();
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

    wdt_kick();
    rc = eh_host_cp_ota_end();
    wdt_kick();
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "cp_fw OTA end failed: %s -- whole-image check rejected it, "
                      "C6 keeps its old firmware", esp_err_to_name(rc));
        return -2;
    }

    /* eh_host_cp_ota_activate() sets the C6's boot partition then reboots it
     * on its own timer (CP_OTA_ASSUMED_CP_REBOOT_TICKS above, ~2000 ms at the
     * tick rate coproc/sdkconfig.defaults pins), inside the host's 5000 ms
     * RPC wait -- so the reply routinely never arrives and this call comes
     * back ESP_FAIL with esp_hosted logging "no response". Log either
     * outcome; NEITHER is proof, per cp_ota.h -- eh_host_cp_ota_end() above
     * only confirmed the CP accepted and stored the image, not that it will
     * boot into it. */
    rc = eh_host_cp_ota_activate();
    if (rc != ESP_OK)
        ESP_LOGW(TAG, "eh_host_cp_ota_activate: %s (expected -- the C6 reboots inside "
                      "the RPC wait)", esp_err_to_name(rc));
    else
        ESP_LOGW(TAG, "eh_host_cp_ota_activate ok (still not proof -- verifying)");

    /* Fix round 2, Major 5 -- the brief already required this and it was
     * missing: "Verify by re-reading the version after the link
     * re-handshakes; never trust this return code." Bounded poll, feeding
     * the watchdog throughout (same "no-op today" caveat as elsewhere in
     * this function) -- and THIS FUNCTION still never reboots the master, on
     * any path. A wrong image would reboot-loop forever if a failed verify
     * auto-rebooted, so if the C6 never comes back reporting a version
     * cp_ota_needed() accepts within the window, that is logged loudly and
     * returned as -2, not 1: Wi-Fi may be down for the rest of this boot
     * either way (the C6 has already rebooted once by this point regardless
     * of outcome), but an unconfirmed push is exactly the case that could
     * repeat.
     *
     * The confirmed case is different and, per final-review F3, the caller
     * now acts on it: master/main/app_main.c restarts the master on a return
     * of 1 so the Wi-Fi stack rebinds to the radio's new firmware. That is
     * loop-safe precisely because it is confined to the confirmed case --
     * cp_ota_needed() returns 0 on the next boot -- which is why the
     * narrowing matters and why -2 must keep returning without a reboot. */
    int64_t deadline_us = esp_timer_get_time() +
                           (int64_t)CP_OTA_REHANDSHAKE_TIMEOUT_MS * 1000;
    uint32_t relinked_ver;
    int confirmed = 0;
    do {
        wdt_kick();
        vTaskDelay(pdMS_TO_TICKS(CP_OTA_REHANDSHAKE_POLL_MS));
        relinked_ver = eh_host_mcu_transport_get_fw_version();
        if (!cp_ota_needed(relinked_ver)) {
            confirmed = 1;
            break;
        }
    } while (esp_timer_get_time() < deadline_us);

    if (!confirmed) {
        /* Names the cross-chip assumption the window is built on (deferred
         * #6): if coproc/sdkconfig.defaults has stopped pinning
         * CONFIG_FREERTOS_HZ=CP_OTA_ASSUMED_CP_TICK_HZ, the C6's reboot delay
         * is longer than this whole window and a SUCCESSFUL update reports
         * here as a failure. Check that before concluding the radio is
         * dead. */
        ESP_LOGE(TAG, "co-processor did not come back reporting the new version within "
                      "%u ms of activate() (last seen 0x%08lx) -- that window is the "
                      "assumed %u-tick CP reboot delay at an assumed %u Hz CP tick rate "
                      "(= %u ms; see coproc/sdkconfig.defaults CONFIG_FREERTOS_HZ) plus "
                      "%u ms of re-enumeration margin. Wi-Fi may be down until the "
                      "master reboots; treating this OTA as FAILED, not confirmed",
                 (unsigned)CP_OTA_REHANDSHAKE_TIMEOUT_MS, (unsigned long)relinked_ver,
                 (unsigned)CP_OTA_ASSUMED_CP_REBOOT_TICKS,
                 (unsigned)CP_OTA_ASSUMED_CP_TICK_HZ,
                 (unsigned)CP_OTA_ASSUMED_CP_REBOOT_MS,
                 (unsigned)CP_OTA_REHANDSHAKE_MARGIN_MS);
        return -2;
    }

    ESP_LOGW(TAG, "co-processor confirmed running 0x%08lx after re-handshake -- OTA complete",
             (unsigned long)relinked_ver);
    return 1;
}

#else /* !CONFIG_IDF_TARGET_ESP32P4 */

int cp_ota_sync(void) {
    return 0;   /* the ESP32 master has no co-processor -- its Wi-Fi is on-die */
}

#endif /* CONFIG_IDF_TARGET_ESP32P4 */
#endif /* !HOST_TEST */
