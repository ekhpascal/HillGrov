#include <string.h>
#include "hg_blob.h"
#include "hg_cfg_types.h"
#include "notify.h"
#include "node_mgr.h"
#include "node_mgr_internal.h"

/* §4.4 config reconciliation + push/pull machinery. The ring's pending
 * tracker (ring_trk) allows only ONE in-flight tracked frame ring-wide
 * (stop-and-wait), and push's CFG_CHUNKs are sent synchronously right
 * before its tracked CFG_COMMIT -- so at most one zone is ever mid-transfer
 * at a time. This file mirrors that with a single global "current transfer"
 * slot (s_cx) and a single shared pull assembler, round-robining across
 * zones on the 1 Hz tick rather than running N independent state machines --
 * a deliberate simplification (disclosed in the task report), not a
 * per-zone concurrency limit the ring itself doesn't already impose. */

typedef struct {
    uint8_t  blob[HG_BLOB_HDR_LEN + sizeof(hg_zone_cfg_t)];   /* sized for the larger (CFG) plane */
    uint32_t gen, crc;                                        /* crc = hg_crc32 over the UNWRAPPED payload */
    uint8_t  valid;
} nmgr_cache_t;

static nmgr_cache_t s_cfg[HG_MAX_ZONES];   /* kind 1, slot = id-1 */
static nmgr_cache_t s_hw[HG_MAX_ZONES];    /* kind 2 -- display/export only, never pushed (spec §4.4) */

typedef enum { CX_IDLE = 0, CX_PULL_ACK, CX_PULL_STREAM, CX_PUSH_COMMIT, CX_RETRY_PULL, CX_RETRY_PUSH } cx_state_t;
typedef struct {
    cx_state_t state;
    uint8_t  zone, kind;
    uint8_t  attempt;          /* 1..4 (initial try + 3 retries, backoff 1/2/4 s between them) */
    uint32_t retry_at_ms;
    uint16_t trk_seq;
    uint32_t push_gen;         /* push only */
} cx_t;
static cx_t       s_cx;
static ring_casm_t s_casm;
static const uint32_t BACKOFF_MS[3] = { 1000, 2000, 4000 };

static size_t cache_payload_len(uint8_t kind) { return kind == 1 ? sizeof(hg_zone_cfg_t) : sizeof(hg_zone_hw_t); }

static void finish_failed(uint8_t zone) {
    notify_emit(NTF_NODE, zone, "%u CFG_SYNC_FAILED", zone);
    hg_node_t *nd = nmgr_node_by_id(zone);
    if (nd && nd->cmd_timeouts < 3) nd->cmd_timeouts = 3;   /* DEGRADED via ring_health_eval's own rule */
    memset(&s_cx, 0, sizeof s_cx);
    ring_casm_init(&s_casm);
}

/* terminal=1 skips the retry ladder entirely (ruling #5: CFG_VERSION /
 * INVALID_FIELD ACK details are never retried). */
static void fail_or_retry(int is_push, int terminal) {
    if (terminal || s_cx.attempt >= 4) { finish_failed(s_cx.zone); return; }
    uint32_t wait = BACKOFF_MS[s_cx.attempt - 1];
    s_cx.attempt = (uint8_t)(s_cx.attempt + 1);
    s_cx.state = is_push ? CX_RETRY_PUSH : CX_RETRY_PULL;
    s_cx.retry_at_ms = nmgr_now_ms() + wait;
}

static void start_pull(uint8_t zone, uint8_t kind, uint8_t attempt) {
    uint8_t payload[1] = { kind };
    uint16_t seq;
    if (nmgr_submit(zone, RING_T_CFG_GET, payload, 1, &seq) != 0) {
        s_cx.zone = zone; s_cx.kind = kind; s_cx.attempt = attempt;
        s_cx.state = CX_RETRY_PULL; s_cx.retry_at_ms = nmgr_now_ms() + 1000;   /* tracker full: no attempt burned */
        return;
    }
    ring_casm_init(&s_casm);
    s_cx.state = CX_PULL_ACK; s_cx.zone = zone; s_cx.kind = kind; s_cx.attempt = attempt; s_cx.trk_seq = seq;
}

static void accept_pull(void) {
    uint8_t  zone = s_cx.zone, kind = s_cx.kind;
    nmgr_cache_t *c = (kind == 1) ? &s_cfg[zone - 1] : &s_hw[zone - 1];
    size_t   plen  = cache_payload_len(kind);
    uint32_t magic = (kind == 1) ? HG_MAGIC_CFG : HG_MAGIC_HW;
    uint16_t ver   = (kind == 1) ? HG_CFG_VER : HG_HW_VER;
    uint16_t vmin  = (kind == 1) ? HG_CFG_VER_MIN : HG_HW_VER_MIN;

    uint8_t  tmp[sizeof(hg_zone_cfg_t)];             /* CFG is the larger of the two planes */
    uint32_t gen = 0;
    /* payload_cap = plen (not sizeof tmp): hg_blob_unwrap treats cap<actual
     * wrapped length as HG_BLOB_MIGRATED regardless of version, so passing
     * the oversized CFG scratch size here would misreport every HW unwrap
     * as "migrated" even on an exact current-version match. */
    hg_blob_rc_t rc = hg_blob_unwrap(magic, ver, vmin, s_casm.buf, s_casm.total, tmp, (uint16_t)plen, &gen);
    if (rc != HG_BLOB_OK && rc != HG_BLOB_MIGRATED) { fail_or_retry(0, 0); return; }

    /* Re-wrap into our own canonical (current-version, fixed-size) envelope
     * so the cache always matches cache_payload_len(kind) even when the
     * zone's own wire envelope was an older MIGRATED layout. */
    hg_blob_wrap(magic, ver, gen, tmp, (uint16_t)plen, c->blob, sizeof c->blob);
    c->gen   = (kind == 1) ? gen : 0;   /* ruling #7: HW carries no gen concept anywhere */
    c->crc   = hg_crc32(0, c->blob + HG_BLOB_HDR_LEN, plen);
    c->valid = 1;
    memset(&s_cx, 0, sizeof s_cx);
    ring_casm_init(&s_casm);
}

static void start_push(uint8_t zone, uint32_t gen, uint8_t attempt) {
    nmgr_cache_t *c = &s_cfg[zone - 1];
    hg_zone_cfg_t work;
    memcpy(&work, c->blob + HG_BLOB_HDR_LEN, sizeof work);
    work.generation = gen;                  /* identity contract (ruling #4): embedded gen ... */
    work.source = HG_SRC_MASTER;            /* ... and source stamped before wrapping */
    hg_blob_wrap(HG_MAGIC_CFG, HG_CFG_VER, gen, &work, (uint16_t)sizeof work, c->blob, sizeof c->blob);

    int count = ring_cfg_chunk_count(sizeof(hg_zone_cfg_t) + HG_BLOB_HDR_LEN);
    for (int i = 0; i < count; i++) {
        uint8_t payload[9 + RING_CFG_DATA_MAX];
        int n = ring_cfg_chunk_build(1, gen, c->blob, sizeof c->blob, (uint8_t)i, payload, sizeof payload);
        if (n < 0) break;
        nmgr_send_raw(zone, RING_T_CFG_CHUNK, payload, (uint8_t)n);   /* unACKed, back-to-back (spec §2.9) */
    }

    uint8_t commit[5] = { 1, (uint8_t)gen, (uint8_t)(gen >> 8), (uint8_t)(gen >> 16), (uint8_t)(gen >> 24) };
    uint16_t seq;
    if (nmgr_submit(zone, RING_T_CFG_COMMIT, commit, 5, &seq) != 0) {
        s_cx.zone = zone; s_cx.kind = 1; s_cx.attempt = attempt; s_cx.push_gen = gen;
        s_cx.state = CX_RETRY_PUSH; s_cx.retry_at_ms = nmgr_now_ms() + 1000;
        return;
    }
    s_cx.state = CX_PUSH_COMMIT; s_cx.zone = zone; s_cx.kind = 1;
    s_cx.attempt = attempt; s_cx.trk_seq = seq; s_cx.push_gen = gen;
}

static void accept_push(void) {
    nmgr_cache_t *c = &s_cfg[s_cx.zone - 1];
    c->gen   = s_cx.push_gen;
    c->crc   = hg_crc32(0, c->blob + HG_BLOB_HDR_LEN, sizeof(hg_zone_cfg_t));   /* ruling #4: recompute post-ACK */
    c->valid = 1;
    memset(&s_cx, 0, sizeof s_cx);
}

/* one reconciliation decision per used, reachable zone -- spec §4.4 */
static int try_start(uint8_t zone, const hg_node_t *nd) {
    nmgr_cache_t *cfg = &s_cfg[zone - 1];
    nmgr_cache_t *hw  = &s_hw[zone - 1];
    if (!cfg->valid) { start_pull(zone, 1, 1); return 1; }
    if (!hw->valid || nd->hb.hw_crc != hw->crc) { start_pull(zone, 2, 1); return 1; }

    if (nd->hb.cfg_gen == cfg->gen && nd->hb.cfg_crc == cfg->crc) return 0;   /* in sync */
    uint32_t new_gen = (nd->hb.cfg_gen > cfg->gen ? nd->hb.cfg_gen : cfg->gen) + 1;
    if (nd->hb.cfg_gen == cfg->gen)
        notify_emit(NTF_NODE, zone, "%u CFG_FORK", zone);
    else if (nd->hb.cfg_gen > cfg->gen)
        notify_emit(NTF_NODE, zone, "%u CFG_REVERTED %lu", zone, (unsigned long)new_gen);
    start_push(zone, new_gen, 1);
    return 1;
}

void nmgr_cfg_init(void) {
    memset(s_cfg, 0, sizeof s_cfg);
    memset(s_hw, 0, sizeof s_hw);
    memset(&s_cx, 0, sizeof s_cx);
    ring_casm_init(&s_casm);
}

void nmgr_cfg_clear(uint8_t zone) {
    if (zone < 1 || zone > HG_MAX_ZONES) return;
    memset(&s_cfg[zone - 1], 0, sizeof s_cfg[0]);
    memset(&s_hw[zone - 1], 0, sizeof s_hw[0]);
    if (s_cx.state != CX_IDLE && s_cx.zone == zone) { memset(&s_cx, 0, sizeof s_cx); ring_casm_init(&s_casm); }
}

void nmgr_cfg_tick_1s(uint32_t now) {
    if (s_cx.state == CX_PULL_STREAM) {
        if (ring_casm_idle_expired(&s_casm, now)) fail_or_retry(0, 0);
        return;
    }
    if (s_cx.state == CX_RETRY_PULL || s_cx.state == CX_RETRY_PUSH) {
        if (now < s_cx.retry_at_ms) return;
        if (s_cx.state == CX_RETRY_PULL) start_pull(s_cx.zone, s_cx.kind, s_cx.attempt);
        else                             start_push(s_cx.zone, s_cx.push_gen, s_cx.attempt);
        return;
    }
    if (s_cx.state != CX_IDLE) return;   /* PULL_ACK / PUSH_COMMIT: waiting on the tracker */

    for (uint8_t id = 1; id <= HG_MAX_ZONES; id++) {
        hg_node_t *nd = nmgr_node_by_id(id);
        if (!nd || !nd->used) continue;
        if (nd->health == NODE_H_OFFLINE || nd->health == NODE_H_UPDATING) continue;
        if (try_start(id, nd)) return;
    }
}

void nmgr_cfg_on_chunk(const ring_frame_t *f, uint32_t now) {
    if (s_cx.state != CX_PULL_STREAM || f->hdr.src != s_cx.zone) return;
    if (ring_casm_idle_expired(&s_casm, now)) ring_casm_init(&s_casm);
    int rc = ring_casm_feed(&s_casm, f->payload, f->hdr.len, now);
    if (rc == 1) accept_pull();
    else if (rc < 0) fail_or_retry(0, 0);
}

void nmgr_cfg_on_ev(const ring_trk_ev_t *ev) {
    if (s_cx.state != CX_PULL_ACK && s_cx.state != CX_PUSH_COMMIT) return;
    if (ev->seq != s_cx.trk_seq) return;

    if (s_cx.state == CX_PULL_ACK) {
        if (ev->kind == RING_TRK_EV_DONE && ev->status == 0) s_cx.state = CX_PULL_STREAM;
        else fail_or_retry(0, 0);
        return;
    }
    if (ev->kind == RING_TRK_EV_DONE && ev->status == 0) { accept_push(); return; }
    if (ev->kind == RING_TRK_EV_DONE) {
        int terminal = strncmp(ev->detail, "ERR CFG_VERSION", 15) == 0 ||
                       strncmp(ev->detail, "ERR INVALID_FIELD", 18) == 0;
        fail_or_retry(1, terminal);
    } else {
        fail_or_retry(1, 0);   /* ZONE_TIMEOUT / ZONE_UNKNOWN: always retryable */
    }
}

int node_mgr_push_cfg(uint8_t zone) {
    if (zone < 1 || zone > HG_MAX_ZONES || s_cx.state != CX_IDLE) return -1;
    nmgr_cache_t *cfg = &s_cfg[zone - 1];
    if (!cfg->valid) return -1;
    hg_node_t *nd = nmgr_node_by_id(zone);
    uint32_t hb_gen = (nd && nd->used) ? nd->hb.cfg_gen : 0;
    start_push(zone, (hb_gen > cfg->gen ? hb_gen : cfg->gen) + 1, 1);
    return 0;
}
