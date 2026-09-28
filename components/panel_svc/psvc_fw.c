#include <string.h>
#include "hg_image.h"
#include "psvc_fw.h"

static const char *volatile s_kind = "";
static volatile uint32_t    s_pct;

void psvc_fw_progress_set(const char *kind, uint32_t pct) {
    s_pct  = pct;
    s_kind = kind ? kind : "";   /* published last: a reader never sees a kind without a pct */
}

int psvc_fw_progress(const char **kind, uint8_t *pct) {
    const char *k = s_kind;
    uint32_t    p = s_pct;
    if (kind) *kind = k ? k : "";
    if (pct)  *pct  = (uint8_t)(p > 100 ? 100 : p);
    return (k && *k) ? 1 : 0;
}

/* ---- the install core (pure; the environment and the sink are injected) ---- */

/* One buffer for every install: the claim makes installs mutually exclusive. Static and in internal RAM (never PSRAM): it
 * is handed to flash writes. */
static uint8_t s_buf[PSVC_FW_BUF];

psvc_rc_t psvc_fw_install_with(const psvc_fw_env_t *env, const psvc_fw_sink_t *sink, psvc_fw_kind_t kind, size_t len,
                               psvc_fw_read_fn rd, void *src, psvc_fw_result_t *res, psvc_fw_stats_t *st) {
    psvc_fw_stats_t st_local;
    psvc_fw_result_t res_local;
    if (!st) st = &st_local;
    if (!res) res = &res_local;
    memset(st, 0, sizeof *st);
    memset(res, 0, sizeof *res);
    if (!env || !sink || !rd || len == 0) return PSVC_E_INVALID;

    /* Claim BEFORE reading the fleet status (http_upload.c:189-193): node_mgr reads the claim inside the lock it starts a
     * sequence under, so a simultaneous pair always has exactly one loser. */
    if (!env->claim()) return PSVC_E_UPLOAD_ACTIVE;

    psvc_rc_t rc = PSVC_OK;
    int rdy = 1;
    size_t max = 0;
    if (!env->fleet_idle())                          rc = PSVC_E_FLEET_ACTIVE;
    else if ((rdy = sink->ready()) == -2)            rc = PSVC_E_TRIAL_PENDING;
    else if (rdy != 0)                               rc = PSVC_E_NO_SLOT;
    else if (env->heap_free() < PSVC_FW_LOW_HEAP_B)  rc = PSVC_E_LOW_HEAP;
    else if ((max = sink->max()) == 0)               rc = PSVC_E_INTERNAL;
    else if (len > max)                              rc = PSVC_E_TOO_LARGE;
    if (rc != PSVC_OK) {
        env->release();
        return rc;
    }

    const char *kname = kind == PSVC_FW_MASTER ? "master" : "zone";
    const uint16_t want_chip = kind == PSVC_FW_MASTER ? env->self_chip : (uint16_t)HG_CHIP_ESP32;
    const char *want_proj = kind == PSVC_FW_MASTER ? HG_PROJ_MASTER : HG_PROJ_ZONE;
    st->started = 1;
    psvc_fw_progress_set(kname, 0);
    int wdt = env->wdt_begin();

    size_t got = 0, fill = 0;
    int blocks = 0, begun = 0, zclaim = 0;
    while (got < len) {
        env->wdt_kick();
        size_t want = len - got;
        if (want > PSVC_FW_BUF - fill) want = PSVC_FW_BUF - fill;
        int n = rd(src, s_buf + fill, want);
        if (n == PSVC_FW_SRC_AGAIN) continue;                          /* the source bounds its own silence */
        if (n == PSVC_FW_SRC_STALLED) { rc = PSVC_E_STALLED; st->src_failed = 1; break; }
        if (n < 0 || (size_t)n > want) { rc = PSVC_E_RECV_FAILED; st->src_failed = 1; break; }
        fill += (size_t)n;
        got += (size_t)n;
        st->consumed = got;
        psvc_fw_progress_set(kname, (uint32_t)((uint64_t)got * 100u / len));

        /* Identify BEFORE the first write, so nothing is erased for a file that was never going to be accepted. */
        if (!begun && (fill >= HG_IMG_ID_BYTES || got == len)) {
            if (!hg_image_is(s_buf, fill, want_chip, want_proj)) { rc = PSVC_E_IMAGE_MISMATCH; break; }
            if (kind == PSVC_FW_ZONE) {
                if (env->zone_fw_claim() != 0) { rc = PSVC_E_ZONE_FW_BUSY; break; }
                zclaim = 1;
            }
            if (sink->begin(len) != 0) { rc = PSVC_E_WRITE_FAILED; break; }
            begun = 1;
        }
        if (begun && (fill == PSVC_FW_BUF || got == len)) {
            if (sink->write(s_buf, fill) != 0) { rc = PSVC_E_WRITE_FAILED; break; }
            fill = 0;
            if (++blocks % PSVC_FW_YIELD_BLOCKS == 0) env->yield();
        }
    }

    if (rc == PSVC_OK && sink->finish(res) != 0) rc = PSVC_E_WRITE_FAILED;
    if (rc != PSVC_OK && begun) sink->cancel();
    env->wdt_end(wdt);
    if (zclaim) env->zone_fw_release();
    psvc_fw_progress_set("", 0);
    env->release();
    return rc;
}
