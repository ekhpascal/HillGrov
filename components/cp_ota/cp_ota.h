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
 * `target == esp32p4`; components/cp_ota/CMakeLists.txt mirrors that gate
 * for its own PRIV_REQUIRES). See components/cp_ota/cp_ota.c for the
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

/* Pushes the image staged in the "cp_fw" partition to the C6 over
 * esp_hosted's RPC OTA channel, but only when cp_ota_needed() says the C6's
 * reported version actually differs from CP_OTA_HOST_VERSION.
 *
 * Returns:
 *    0  already matching -- nothing done
 *    1  image pushed and activated (the C6 reboots itself)
 *   -1  no image staged in cp_fw (partition absent, or its first byte is
 *       still erased 0xFF)
 *   -2  the push failed partway (the C6 keeps running its old firmware)
 *
 * A 1 does NOT mean the new firmware is confirmed running: the C6 reboots
 * on its own ~2 s timer after activation, inside esp_hosted's 5 s RPC wait,
 * so the activate RPC's own reply routinely never arrives. Never trust this
 * return code as proof -- verify by re-reading the C6's version after the
 * link re-handshakes.
 *
 * ESP32 master build: stub, always returns 0. The ESP32 master's Wi-Fi is
 * on-die -- it has no co-processor to update. The real implementation is
 * ESP32-P4-only (CONFIG_IDF_TARGET_ESP32P4), where esp_hosted's SDIO link
 * to the on-board ESP32-C6 exists. */
int cp_ota_sync(void);

#ifdef __cplusplus
}
#endif
