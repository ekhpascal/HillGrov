#include <string.h>
#include "hg_blob.h"
#include "hg_cfg_types.h"
#include "node_mgr.h"
#include "node_mgr_internal.h"
#include "node_mgr_cfg_internal.h"

/* §4.4 TRANSFER half: the single "current transfer" slot that carries one
 * reconciliation decision out on the wire -- pull (CFG_GET + chunk assembly),
 * push (chunk burst + tracked CFG_COMMIT), the tracker completion hook and the
 * retry ladder. The decision half (node_mgr_cfg.c) owns the caches and the
 * latch; see node_mgr_cfg_internal.h for the seam.
 *
 * One slot is the right shape, not a simplification of a bigger one: the
 * tracker allows only ONE in-flight tracked frame ring-wide and a push's
 * chunks go out synchronously right before its tracked CFG_COMMIT, so at most
 * one zone can ever be mid-transfer. Single-writer: everything here is touched
 * ONLY from the node_mgr task. */

typedef enum { CX_IDLE = 0, CX_PULL_ACK, CX_PULL_STREAM, CX_PUSH_COMMIT, CX_RETRY_PULL, CX_RETRY_PUSH } cx_state_t;
typedef struct {
    cx_state_t state;
    uint8_t  zone, kind;
    uint8_t  attempt;          /* 1..4 (initial try + 3 retries, backoff 1/2/4 s between them) */
    uint32_t retry_at_ms;
    uint16_t trk_seq;
    uint32_t push_gen;                                    /* push only */
    uint32_t hb_gen, hb_crc, cache_gen, cache_crc;         /* push only: frozen latch identity, ruling important #2 */
} cx_t;
static cx_t        s_cx;
static ring_casm_t s_casm;
static const uint32_t BACKOFF_MS[3] = { 1000, 2000, 4000 };

/* A push's wire bytes are built HERE, never in the cache. start_push stamps the
 * pushed generation and source=MASTER into the payload before wrapping, and
 * doing that in the cache blob in place left the cache's embedded gen ahead of
 * c->gen for as long as the push then took to fail (fix round minor). This
 * buffer holds exactly the bytes the chunks carried, so accept_push can adopt
 * them once the zone has ACKed them -- the cache then matches the zone byte for
 * byte, which is what makes the cfg_crc comparison meaningful. */
static uint8_t s_push_wire[HG_BLOB_HDR_LEN + sizeof(hg_zone_cfg_t)];

static size_t cache_payload_len(uint8_t kind) { return kind == 1 ? sizeof(hg_zone_cfg_t) : sizeof(hg_zone_hw_t); }

void nmgr_cx_init(void) {
    memset(&s_cx, 0, sizeof s_cx);
    ring_casm_init(&s_casm);
}

int nmgr_cx_idle(void) { return s_cx.state == CX_IDLE; }

static void finish_failed(int terminal) {
    nmgr_cfg_note_failed(s_cx.zone, s_cx.kind, terminal,
                          s_cx.hb_gen, s_cx.hb_crc, s_cx.cache_gen, s_cx.cache_crc);
    memset(&s_cx, 0, sizeof s_cx);
    ring_casm_init(&s_casm);
}

static void start_pull(uint8_t zone, uint8_t kind, uint8_t attempt);
static void start_push(uint8_t zone, uint32_t gen, uint8_t attempt,
                        uint32_t hb_gen, uint32_t hb_crc, uint32_t cache_gen, uint32_t cache_crc);

/* terminal=1 skips the retry ladder entirely and latches (ruling #5:
 * CFG_VERSION / INVALID_FIELD ACK details are never retried). */
static void fail_or_retry(int is_push, int terminal) {
    if (terminal || s_cx.attempt >= 4) { finish_failed(terminal); return; }
    uint32_t wait = BACKOFF_MS[s_cx.attempt - 1];
    s_cx.attempt = (uint8_t)(s_cx.attempt + 1);
    s_cx.state = is_push ? CX_RETRY_PUSH : CX_RETRY_PULL;
    s_cx.retry_at_ms = nmgr_now_ms() + wait;
}

static void start_pull(uint8_t zone, uint8_t kind, uint8_t attempt) {
    uint8_t payload[1] = { kind };
    uint16_t seq;
    s_cx.zone = zone; s_cx.kind = kind; s_cx.attempt = attempt;
    if (nmgr_submit(zone, RING_T_CFG_GET, payload, 1, &seq) != 0) {
        s_cx.state = CX_RETRY_PULL; s_cx.retry_at_ms = nmgr_now_ms() + 1000;   /* tracker full: no attempt burned */
        return;
    }
    ring_casm_init(&s_casm);
    s_cx.state = CX_PULL_ACK; s_cx.trk_seq = seq;
}

static void accept_pull(void) {
    uint8_t  zone = s_cx.zone, kind = s_cx.kind;
    nmgr_cache_t *c = nmgr_cfg_cache(zone, kind);
    if (!c) { finish_failed(0); return; }                 /* defensive: zone/kind validated upstream */
    size_t   plen  = cache_payload_len(kind);
    uint32_t magic = (kind == 1) ? HG_MAGIC_CFG : HG_MAGIC_HW;
    uint16_t ver   = (kind == 1) ? HG_CFG_VER : HG_HW_VER;
    uint16_t vmin  = (kind == 1) ? HG_CFG_VER_MIN : HG_HW_VER_MIN;

    uint8_t  tmp[sizeof(hg_zone_cfg_t)];             /* CFG is the larger of the two planes */
    uint32_t gen = 0;
    /* payload_cap = plen, not sizeof tmp: cap<actual len reads as MIGRATED
     * regardless of version, so the oversized CFG buffer would misreport
     * every HW unwrap even on an exact current-version match. */
    hg_blob_rc_t rc = hg_blob_unwrap(magic, ver, vmin, s_casm.buf, s_casm.total, tmp, (uint16_t)plen, &gen);
    if (rc != HG_BLOB_OK && rc != HG_BLOB_MIGRATED) { fail_or_retry(0, 0); return; }

    /* Re-wrap canonically so the cache always matches cache_payload_len(kind)
     * even when the zone's own wire envelope was an older MIGRATED layout. */
    hg_blob_wrap(magic, ver, gen, tmp, (uint16_t)plen, c->blob, sizeof c->blob);
    c->gen   = (kind == 1) ? gen : 0;   /* ruling #7: HW carries no gen concept anywhere */
    c->crc   = hg_crc32(0, c->blob + HG_BLOB_HDR_LEN, plen);
    c->valid = 1;
    nmgr_cfg_note_synced(zone);
    memset(&s_cx, 0, sizeof s_cx);
    ring_casm_init(&s_casm);
}

static void start_push(uint8_t zone, uint32_t gen, uint8_t attempt,
                        uint32_t hb_gen, uint32_t hb_crc, uint32_t cache_gen, uint32_t cache_crc) {
    nmgr_cache_t *c = nmgr_cfg_cache(zone, 1);
    if (!c) return;                                    /* defensive: zone validated upstream */
    hg_zone_cfg_t work;
    memcpy(&work, c->blob + HG_BLOB_HDR_LEN, sizeof work);
    work.generation = gen;                  /* identity contract (ruling #4): embedded gen ... */
    work.source = HG_SRC_MASTER;            /* ... and source stamped before wrapping */
    hg_blob_wrap(HG_MAGIC_CFG, HG_CFG_VER, gen, &work, (uint16_t)sizeof work,
                  s_push_wire, sizeof s_push_wire);

    int count = ring_cfg_chunk_count(sizeof(hg_zone_cfg_t) + HG_BLOB_HDR_LEN);
    for (int i = 0; i < count; i++) {
        uint8_t payload[9 + RING_CFG_DATA_MAX];
        int n = ring_cfg_chunk_build(1, gen, s_push_wire, sizeof s_push_wire,
                                      (uint8_t)i, payload, sizeof payload);
        if (n < 0) break;
        nmgr_send_raw(zone, RING_T_CFG_CHUNK, payload, (uint8_t)n);   /* unACKed, back-to-back (spec §2.9) */
    }

    s_cx.zone = zone; s_cx.kind = 1; s_cx.attempt = attempt; s_cx.push_gen = gen;
    s_cx.hb_gen = hb_gen; s_cx.hb_crc = hb_crc; s_cx.cache_gen = cache_gen; s_cx.cache_crc = cache_crc;

    uint8_t commit[5] = { 1, (uint8_t)gen, (uint8_t)(gen >> 8), (uint8_t)(gen >> 16), (uint8_t)(gen >> 24) };
    uint16_t seq;
    if (nmgr_submit(zone, RING_T_CFG_COMMIT, commit, 5, &seq) != 0) {
        s_cx.state = CX_RETRY_PUSH; s_cx.retry_at_ms = nmgr_now_ms() + 1000;
        return;
    }
    s_cx.state = CX_PUSH_COMMIT; s_cx.trk_seq = seq;
}

static void accept_push(void) {
    nmgr_cache_t *c = nmgr_cfg_cache(s_cx.zone, 1);
    if (!c) { memset(&s_cx, 0, sizeof s_cx); return; }
    memcpy(c->blob, s_push_wire, sizeof c->blob);   /* adopt exactly the bytes the zone ACKed */
    c->gen   = s_cx.push_gen;
    c->crc   = hg_crc32(0, c->blob + HG_BLOB_HDR_LEN, sizeof(hg_zone_cfg_t));   /* ruling #4: recompute post-ACK */
    c->valid = 1;
    nmgr_cfg_note_synced(s_cx.zone);
    memset(&s_cx, 0, sizeof s_cx);
}

void nmgr_cx_pull(uint8_t zone, uint8_t kind) { start_pull(zone, kind, 1); }

void nmgr_cx_push(uint8_t zone, uint32_t gen,
                  uint32_t hb_gen, uint32_t hb_crc, uint32_t cache_gen, uint32_t cache_crc) {
    start_push(zone, gen, 1, hb_gen, hb_crc, cache_gen, cache_crc);
}

void nmgr_cx_abort(uint8_t zone) {
    if (s_cx.state == CX_IDLE || s_cx.zone != zone) return;
    /* Withdraw the tracked frame this transfer is waiting on if it is still
     * QUEUED (attempts == 0) -- which is exactly the §4.4 revert race: the
     * CFG_COMMIT of a revert push sits behind the operator's forwarded CMD, and
     * without this it would go out AFTER that SET's OK ACK and overwrite the
     * value the operator just set. An entry already in flight cannot be
     * withdrawn (nmgr_cancel returns -1); its ACK/timeout still routes to
     * nmgr_cfg_on_ev and is dropped by the seq check there, while the zone's own
     * gen guard rejects the now-stale COMMIT with ERR CFG_VERSION. */
    if (s_cx.state == CX_PULL_ACK || s_cx.state == CX_PUSH_COMMIT) nmgr_cancel(s_cx.trk_seq);
    memset(&s_cx, 0, sizeof s_cx);
    ring_casm_init(&s_casm);
}

int nmgr_cx_tick_1s(uint32_t now) {
    switch (s_cx.state) {
    case CX_IDLE:
        return 0;                            /* the decision half may start something */
    case CX_PULL_STREAM:
        if (ring_casm_idle_expired(&s_casm, now)) fail_or_retry(0, 0);
        return 1;
    case CX_RETRY_PULL:
    case CX_RETRY_PUSH:
        if ((int32_t)(now - s_cx.retry_at_ms) < 0) return 1;   /* wrap-safe: minor #5 */
        if (s_cx.state == CX_RETRY_PULL) start_pull(s_cx.zone, s_cx.kind, s_cx.attempt);
        else start_push(s_cx.zone, s_cx.push_gen, s_cx.attempt,
                        s_cx.hb_gen, s_cx.hb_crc, s_cx.cache_gen, s_cx.cache_crc);
        return 1;
    default:
        return 1;                            /* PULL_ACK / PUSH_COMMIT: waiting on the tracker */
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
        /* "ERR INVALID_FIELD" is 17 bytes, not 18 (fix round important #1;
         * trace in task-13-report.md) -- n=18 never matched. */
        int terminal = strncmp(ev->detail, "ERR CFG_VERSION", 15) == 0 ||
                       strncmp(ev->detail, "ERR INVALID_FIELD", 17) == 0;
        fail_or_retry(1, terminal);
    } else {
        fail_or_retry(1, 0);   /* ZONE_TIMEOUT / ZONE_UNKNOWN: always retryable */
    }
}
