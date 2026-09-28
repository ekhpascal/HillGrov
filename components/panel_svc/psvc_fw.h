#pragma once
#include <stddef.h>
#include <stdint.h>
#include "psvc_rc.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Firmware-install state and the install core shared by every face: the web upload (http_upload.c) and the panel's
 * microSD install run exactly the same sequence, so every refusal means the same thing in both.
 *
 * Progress: lock-free by construction: two word-sized stores by the one installer, two loads by any reader. kind must be
 * a string literal ("" | "master" | "zone"), so the pointer stays valid however the install ends; pct is stored FIRST,
 * so a reader never sees a kind without its pct. */
void psvc_fw_progress_set(const char *kind, uint32_t pct);   /* NULL kind == "" */

/* [ANY] 1 while an install streams, else 0. *kind ("" when idle) and *pct
 * (0..100, clamped) are always written; either pointer may be NULL. */
int  psvc_fw_progress(const char **kind, uint8_t *pct);

/* ---- the install core ---- */

typedef enum { PSVC_FW_MASTER = 0, PSVC_FW_ZONE } psvc_fw_kind_t;
typedef struct { char slot[17]; char version[33]; uint32_t len; } psvc_fw_result_t;
typedef struct { size_t consumed; uint8_t started; uint8_t src_failed; } psvc_fw_stats_t;
#define PSVC_FW_SRC_AGAIN     0     /* no bytes yet: the core kicks the TWDT and calls again (the source decides when to give up) */
#define PSVC_FW_SRC_STALLED (-1)
#define PSVC_FW_SRC_FAILED  (-2)
typedef int (*psvc_fw_read_fn)(void *src, void *buf, size_t cap);   /* >0 bytes / AGAIN / STALLED / FAILED */
typedef struct { int (*ready)(void); size_t (*max)(void); int (*begin)(size_t len);
                 int (*write)(const void *buf, size_t n); int (*finish)(psvc_fw_result_t *res); void (*cancel)(void); } psvc_fw_sink_t;
     /* ready: 0 go / -1 NO_SLOT / -2 TRIAL_PENDING; max 0 = partition missing; others 0 / -1 WRITE_FAILED.
        ready() runs only once the install owns the claim (it may touch the sink's statics); max() is meaningful only
        after a ready() of 0; begin() runs only after the image identity passed, so a refused image erases nothing;
        write() gets every byte exactly once, in order; finish() runs after the last byte and fills *res; cancel() undoes
        a begun-but-failed install (including a failed finish()) and must leave nothing that could be booted or served. */
typedef struct {
    int (*claim)(void); void (*release)(void);            /* the ONE upload claim (test-and-set) */
    int (*fleet_idle)(void); uint32_t (*heap_free)(void);
    int (*wdt_begin)(void); void (*wdt_kick)(void); void (*wdt_end)(int token);   /* begin: subscribe unless already subscribed */
    void (*yield)(void);
    int (*zone_fw_claim)(void); void (*zone_fw_release)(void);
    uint16_t self_chip;                                   /* CONFIG_IDF_FIRMWARE_CHIP_ID in production */
} psvc_fw_env_t;
#define PSVC_FW_LOW_HEAP_B   (40u * 1024u)
#define PSVC_FW_BUF          4096u   /* static, internal RAM */
#define PSVC_FW_YIELD_BLOCKS 8

psvc_rc_t psvc_fw_install(psvc_fw_kind_t kind, size_t len, psvc_fw_read_fn rd, void *src,
                          psvc_fw_result_t *res, psvc_fw_stats_t *st);
     /* [WORKER or httpd] DEFINED IN psvc_fw_env.c (glue) = psvc_fw_install_with(psvc_fw_env_default(),
        kind == PSVC_FW_MASTER ? psvc_fw_sink_master() : psvc_fw_sink_zone(), ...) -- so the pure psvc_fw.c the host
        test links never references a glue symbol. The calling task is TWDT-subscribed for the flash loop only (and
        makes no esp_hosted RPC inside it); the install never reboots. */
psvc_rc_t psvc_fw_install_with(const psvc_fw_env_t *env, const psvc_fw_sink_t *sink, psvc_fw_kind_t kind, size_t len,
                               psvc_fw_read_fn rd, void *src, psvc_fw_result_t *res, psvc_fw_stats_t *st);
     /* psvc_fw.c (pure) -- the core; host-tested (tests/host/test_psvc_fw.c).
        order (== http_upload.c:189-306): len 0 -> INVALID; claim (UPLOAD_ACTIVE); fleet_idle else FLEET_ACTIVE; sink->ready
        (-2 TRIAL_PENDING, other != 0 NO_SLOT); heap < PSVC_FW_LOW_HEAP_B LOW_HEAP; max 0 INTERNAL; len > max TOO_LARGE ->
        progress(kind,0) -> wdt_begin -> read loop (AGAIN: kick+retry; STALLED/FAILED -> STALLED/RECV_FAILED, src_failed=1)
        -> identity BEFORE the first write: hg_image_is(buf, fill, kind==MASTER ? env->self_chip : HG_CHIP_ESP32,
        kind==MASTER ? HG_PROJ_MASTER : HG_PROJ_ZONE) else IMAGE_MISMATCH -> (ZONE: zone_fw_claim else ZONE_FW_BUSY) ->
        begin (fail: WRITE_FAILED, no cancel) -> write per PSVC_FW_BUF, yield every PSVC_FW_YIELD_BLOCKS -> finish ->
        failures: cancel if begun -> wdt_end -> zone_fw_release -> progress("",0) -> release.
        claim() returns 1 = taken / 0 = held. st->started = 1 once the guards passed (the web drains the rest of the body
        only when started && !src_failed). res and st may be NULL. */

int  psvc_fw_busy(void);                           /* [ANY] psvc_fw_env.c: the claim flag -- node_mgr's fleet gate */
const psvc_fw_env_t  *psvc_fw_env_default(void);   /* psvc_fw_env.c */
const psvc_fw_sink_t *psvc_fw_sink_master(void);   /* fw_sink_master.c */
const psvc_fw_sink_t *psvc_fw_sink_zone(void);     /* fw_sink_zone.c */
void psvc_fw_wdt_kick(void);                       /* esp_task_wdt_status(NULL)==ESP_OK ? esp_task_wdt_reset() : no-op */

#ifdef __cplusplus
}
#endif
