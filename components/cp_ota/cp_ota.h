#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Task 6: update the ESP32-C6 co-processor's esp_hosted firmware from the
 * "cp_fw" data partition (Task 2), gated on version so a healthy C6 is not
 * re-flashed on every boot -- the bring-up spike's bug: ungated, it cost a
 * flash-wear cycle on the radio plus ~7 s on every single start-up.
 *
 * This header must be includable on EVERY target -- the MSVC host build,
 * the ESP32 master/zone/rescue and the ESP32-P4 master -- so it never
 * includes anything from esp_hosted (that component only resolves on the
 * P4 build: master/main/idf_component.yml gates espressif/esp_hosted on
 * `target == esp32p4`). components/cp_ota/CMakeLists.txt does NOT mirror
 * that gate with a CONFIG_IDF_TARGET_ESP32P4 guard around REQUIRES/
 * PRIV_REQUIRES -- fix round 1 found that specific pattern silently
 * resolves an empty requirements list on a real P4 build (see that
 * CMakeLists.txt's own comment for the full story); it attaches
 * espressif__esp_hosted with idf_component_optional_requires() AFTER
 * idf_component_register() instead, which is a no-op wherever esp_hosted
 * isn't part of the build. See components/cp_ota/cp_ota.c for the
 * HOST_TEST / CONFIG_IDF_TARGET_ESP32P4 split.
 */

/* esp_hosted host-side firmware version this master is built against,
 * encoded the same way esp_hosted's own EH_VERSION_VAL(major, minor, patch)
 * macro does: (major << 16) | (minor << 8) | patch. 0x00030007 == 3.0.7.
 *
 * Cannot be compile-time-checked against esp_hosted's own value: esp_hosted
 * defines PROJECT_VERSION_MAJOR_1/MINOR_1/PATCH_1 (and the EH_VERSION_VAL
 * macro that combines them) in
 * common/eh_common/include/eh_common_fw_version.h, but that directory is
 * NOT in the esp_hosted component's public INCLUDE_DIRS (its top-level
 * CMakeLists.txt only exposes host/compat/include and each
 * host/features/eh_host_feat_NAME component's own include dir) -- i.e.
 * esp_hosted keeps its own version private to its internals, not part of
 * the surface a consuming component like this one can see. Reaching into
 * that private header path
 * directly would be a landmine for the next esp_hosted upgrade, so this is
 * a plain constant instead: keep it in sync BY HAND with esp_hosted's
 * version whenever master/main/idf_component.yml's espressif/esp_hosted
 * pin changes. */
#define CP_OTA_HOST_VERSION 0x00030007u

/* Pure -- host-tested (tests/host/test_cp_ota.c), no esp_hosted or ESP-IDF
 * dependency. 1 when cp_ver differs from CP_OTA_HOST_VERSION in major or
 * minor (an update is needed); 0 when they match exactly or differ only in
 * patch. This mirrors esp_hosted's own
 * eh_host_mcu_transport_verify_fw_compat() (0 = major.minor match, treats a
 * patch-only difference as compatible) rather than calling it: that
 * function lives in a component the host build does not have, so this is a
 * copy of its decision, not a wrapper around it -- and it must keep
 * agreeing with the original or the host would re-flash the radio for a
 * change esp_hosted itself considers fine. */
int cp_ota_needed(uint32_t cp_ver);

/* HillGrow's shared "data-partition firmware" header format -- the same one
 * zone_fw uses (components/fw_srv/fw_srv.c's FW_HDR_MAGIC/FW_HDR_LEN,
 * tools/flash_app.py's build_hgfw_image()): 16 bytes, { magic 'HGFW' u32 LE,
 * len u32 LE, crc32 u32 LE, rsvd u32 }, prefixed ahead of the raw image at
 * offset 0. A DATA partition can't recover its payload length by parsing
 * esp_image segments (that's bootloader territory), so the length has to be
 * carried explicitly -- fix-round finding (Task 6 concern 1): the first cut
 * of this component pushed the WHOLE cp_fw partition, including trailing
 * erased/stale bytes past the real image, because nothing else told it
 * where the image ended. */
#define CP_OTA_HDR_LEN   16u
#define CP_OTA_HDR_MAGIC 0x57464748u   /* 'HGFW' LE */

/* Validates a 16-byte header already read from cp_fw offset 0, against a
 * body crc32 the CALLER already computed -- hg_crc32(0, ...) (components/
 * hg_blob/hg_blob.h; seed 0 for one-shot) over `len` bytes starting at
 * cp_fw offset CP_OTA_HDR_LEN. Streaming that body is not this function's
 * job (cp_fw can hold up to ~1.5 MB, too big to pass by value), so it stays
 * pure, allocation-free and host-tested (tests/host/test_cp_ota.c) even
 * though the thing it validates is not.
 *
 * Checks, all of which must hold: hdr's magic == CP_OTA_HDR_MAGIC; hdr's len
 * is non-zero and CP_OTA_HDR_LEN + len fits within part_size; body_crc
 * matches hdr's own stored crc32. Returns 0 and sets *len_out to the
 * validated length on success; -1 otherwise (*len_out untouched). An erased
 * (0xFF-filled) partition is rejected by the magic check like any other
 * garbage header -- no separate erased-byte check is needed. */
int cp_ota_parse_header(const uint8_t hdr[CP_OTA_HDR_LEN], uint32_t part_size,
                         uint32_t body_crc, uint32_t *len_out);

/* Pushes the image staged in the "cp_fw" partition to the C6 over
 * esp_hosted's RPC OTA channel, but only when the CP link is actually up
 * AND cp_ota_needed() says the C6's reported version differs from
 * CP_OTA_HOST_VERSION.
 *
 * Returns:
 *    0  nothing done -- either the CP link isn't up yet this boot (init
 *       event not parsed: get_fw_version() would be untrustworthy, since an
 *       unresponsive C6 and a factory CP genuinely reporting 0.0.0 both
 *       read as 0), or the reported version already matches
 *    1  image pushed, activated, AND CONFIRMED: the C6 rebooted, re-parsed
 *       the init event, and reported a version cp_ota_needed() now accepts,
 *       all within a bounded post-activate wait (fix round 2 -- a plain
 *       "activate() didn't error" used to be enough for this return value;
 *       it no longer is)
 *   -1  no VALID image staged in cp_fw: partition absent, the header at
 *       offset 0 fails validation (bad magic/length/crc -- this catches a
 *       truncated or corrupted staging write before anything is sent to the
 *       radio, not just an untouched/erased partition), OR a flash read
 *       failed while checking (an I/O fault, not necessarily an actually-
 *       empty partition -- the log line distinguishes which)
 *   -2  either the RPC push itself failed partway (the C6 keeps running its
 *       old firmware), OR the push and activate looked fine but the C6 did
 *       NOT come back within the bounded wait reporting a version
 *       cp_ota_needed() accepts (fix round 2, Major 5) -- in both cases
 *       treat this as a failed update, not a completed one
 *
 * This function never reboots the master, on any return path: the update is
 * version-gated, so a correct image needs no repeat next boot, but a wrong
 * image auto-rebooting the master on a failed verify would reboot-loop
 * forever. A -2 from the post-activate verify does NOT mean Wi-Fi is
 * guaranteed down for the rest of this boot -- only that this function
 * cannot confirm the C6 is running the version it just tried to push; the
 * C6 has already rebooted once by that point regardless of the outcome.
 *
 * The CALLER, however, now does reboot on 1 -- and only on 1. Final-review
 * F3: the C6 is the master's radio, so a successful update leaves wifi_mgr,
 * mDNS, SNTP and httpd bound to a radio that has just restarted with fresh
 * unconfigured state, and nothing re-applies them. master/main/app_main.c's
 * cp_ota_restart_for_new_radio() restarts the master for that one return
 * value, which is loop-safe *because* it is gated on the confirmed case
 * (cp_ota_needed() returns 0 next boot, so the update cannot repeat) and is
 * additionally suppressed while the running slot is PENDING_VERIFY. That
 * narrow supersession of the never-auto-reboot rule is argued in full at
 * that function; the rule still holds here and for every other return path.
 *
 * ESP32 master build: stub, always returns 0. The ESP32 master's Wi-Fi is
 * on-die -- it has no co-processor to update. The real implementation is
 * ESP32-P4-only (CONFIG_IDF_TARGET_ESP32P4), where esp_hosted's SDIO link
 * to the on-board ESP32-C6 exists. */
int cp_ota_sync(void);

#ifdef __cplusplus
}
#endif
