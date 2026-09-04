#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Master's zone-firmware image server (Task 15 controller ruling #2):
 * an IDF httpd (stack 4096) with exactly one URI, GET /fw/zone.bin, that
 * streams the "zone_fw" data partition (master's partitions.csv, offset
 * 0x570000, size 0x180000) to a zone rebooted into rescue for a fleet
 * update.
 *
 * Storage convention (plan-fixed, shared with tools/flash_app.py --app
 * zonefw): a 16-byte prefix at zone_fw+0 -- { magic 'HGFW' u32 LE, len u32
 * LE, crc32 u32 LE, rsvd u32 } -- followed by the raw zone app image at
 * zone_fw+16. The partition is data-type (parsing esp_image segments is
 * bootloader territory), so the image length has to be carried explicitly
 * rather than derived from the image itself. crc32 is hg_crc32(0, image,
 * len) -- the same zlib/CRC-32-ISO-HDLC check value family hg_blob uses,
 * matching Python's plain binascii.crc32(data) (seed 0), NOT the
 * 0xFFFFFFFF-seeded esp_rom_crc32_le convention tools/hg_otadata.py uses
 * for otadata (a different check value family; do not conflate the two).
 *
 * fw_srv_start() validates the header + the crc32 over the len image bytes
 * exactly ONCE (at startup) and caches the verdict; fw_srv_image_ok()
 * exposes it without re-reading flash. GET /fw/zone.bin: verdict bad ->
 * 404 "FW_NO_IMAGE"; good -> Content-Length = len, then esp_partition_read
 * + httpd_resp_send_chunk in 4 KB pieces, esp_task_wdt_add/reset/delete
 * around the loop (the same SP1 rescue-upload TWDT pattern rescue_http.c's
 * upload_post uses). 0/-1, every failure logged. */
int fw_srv_start(void);

/* Cached verdict from fw_srv_start()'s one-time validation; 1 = the
 * zone_fw partition holds a good HGFW-prefixed image, 0 = missing/invalid
 * (fw_srv's own GET handler already 404s on this; the fleet sequencer's
 * PRECHECK step reads it too, per-zone, before starting an update). */
int fw_srv_image_ok(void);

#ifdef __cplusplus
}
#endif
