#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Browser firmware upload -- POST /api/fw/master (a raw app image into the
 * inactive OTA slot) and POST /api/fw/zone (a raw zone app image into the
 * zone_fw data partition, behind the 16-byte HGFW header fw_srv.c validates).
 *
 * This is the one path in the product that writes flash from the network, so
 * every guard is stated once here and implemented once in http_upload.c:
 *
 *   - the route's auth bit (http_routes.c) has already run: route_entry
 *     rejects an unauthenticated POST before the handler is reached;
 *   - Transfer-Encoding present -> 400 (esp_http_server does NOT de-chunk
 *     request bodies -- the KraftWerk scar http_srv_body carries too);
 *   - Content-Type must be application/octet-stream, content_len must be
 *     non-zero and must fit the target partition;
 *   - exactly one upload at a time, and never while the fleet sequencer is
 *     running (it reads the very partition a zone upload erases);
 *   - the image is identified from its own esp_app_desc before anything is
 *     erased or written, so pointing the wrong file at the wrong endpoint
 *     costs nothing.
 */

/* /api/state's view of an upload in flight (http_api.c -> snap_master_t.fw).
 * *kind is "" when nothing is uploading, else "master"/"zone" -- a pointer to
 * a string literal, so it stays valid however the upload ends. *pct is
 * 0..100. Both out-params are always written. Returns 1 while an upload is in
 * flight, 0 otherwise.
 *
 * Lock-free by construction (two word-sized stores on the httpd task, two
 * loads on the reader) because the reader may be any task. In THIS build the
 * reader is h_state on the single httpd task, i.e. the same task that runs
 * the upload, so /api/state cannot actually be served while a body is
 * streaming -- the browser gets its progress from XHR's own upload events.
 * The published value is what a future multi-worker httpd, or any other
 * caller, would need. */
int http_upload_progress(const char **kind, uint8_t *pct);

/* ---- one upload target ----
 * begin() runs only after the image identity check has passed, so a refused
 * image never erases anything; write() gets every body byte exactly once, in
 * order; finish() is called after the last byte and fills the 200 response
 * body; cancel() undoes a begun-but-failed upload (including a failed
 * finish()) and must leave nothing behind that could later be booted or
 * served. All four run on the httpd task, one upload at a time.
 * 0 = ok, -1 = failed (the handler answers 422 WRITE_FAILED). */
typedef struct {
    int  (*begin)(size_t content_len);
    int  (*write)(const void *buf, size_t n);
    int  (*finish)(char *resp, size_t cap);
    void (*cancel)(void);
} upload_sink_t;

/* http_upload_master.c: the inactive OTA slot (esp_ota_begin/write/end/
 * set_boot_partition). ready() picks that slot and must be called first --
 * 0 = go ahead, -1 = there is no inactive slot (500 NO_SLOT), -2 = the
 * RUNNING image is still on trial, so esp_ota_begin would refuse anyway
 * (409 TRIAL_PENDING). max() is only meaningful after a ready() of 0. */
const upload_sink_t *http_upload_master_sink(void);
size_t               http_upload_master_max(void);
int                  http_upload_master_ready(void);

/* http_upload_zone.c: the zone_fw half (erase, body at offset 16, HGFW header
 * written last, fw_srv_revalidate). max() is the largest body that fits --
 * partition size minus the 16-byte header -- or 0 if the partition is
 * missing, which the handler answers 500 INTERNAL. */
const upload_sink_t *http_upload_zone_sink(void);
size_t               http_upload_zone_max(void);

#ifdef __cplusplus
}
#endif
