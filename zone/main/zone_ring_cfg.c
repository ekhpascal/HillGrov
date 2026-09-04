#include <stdio.h>
#include <string.h>
#include "ring_link.h"
#include "hg_blob.h"
#include "hg_model.h"
#include "hg_cfg.h"
#include "hg_store.h"
#include "zone_ring_internal.h"

/* CFG_CHUNK/CFG_COMMIT/CFG_GET -- the config transfer half of zone_ring,
 * split out per Task 12's brief to keep zone_ring.c under the line cap. */

static ring_casm_t s_casm;

void zone_ring_cfg_init(void) { ring_casm_init(&s_casm); }

static uint32_t rd32le(const uint8_t *p) {
    return ((uint32_t)p[0]) | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int casm_complete(const ring_casm_t *a) {
    return a->active && a->count > 0 && a->got_mask == (uint8_t)((1u << a->count) - 1u);
}

void zone_ring_cfg_chunk(const ring_frame_t *f, uint32_t now) {
    /* spec 2.9: 2 s idle abort -- ring_casm_feed only restarts on a kind/gen
     * mismatch, so a same-kind/gen resume after an idle gap must be forced
     * fresh here first, or stale partial data would linger in the buffer. */
    if (ring_casm_idle_expired(&s_casm, now)) ring_casm_init(&s_casm);
    ring_casm_feed(&s_casm, f->payload, f->hdr.len, now);   /* unACKed: no response either way */
}

/* Every CFG_COMMIT reply also goes into the shared at-most-once cache, so the
 * master's retransmit after a lost ACK replays this exact answer instead of
 * hitting the (already consumed) assembler and reading back MISSING_CHUNK. */
static void commit_reply(const ring_frame_t *f, uint8_t status, const char *detail, uint8_t dlen) {
    char cached[126];
    if (dlen > 125) dlen = 125;
    memcpy(cached, detail, dlen);
    cached[dlen] = '\0';
    zring_dup_finish(f->hdr.seq, status, cached);
    zring_send_ack(f->hdr.src, f->hdr.seq, status, detail, dlen);
}

static void commit_fail(const ring_frame_t *f, const char *detail, uint8_t dlen) {
    commit_reply(f, 1, detail, dlen);
    ring_casm_init(&s_casm);
}

static void commit_ok(const ring_frame_t *f, int applied) {
    if (applied) hg_store_flush(2000);
    commit_reply(f, 0, "OK", 2);
    ring_casm_init(&s_casm);
}

#define COMMIT_FAIL(f, tok) commit_fail((f), (tok), (uint8_t)(sizeof(tok) - 1))

/* HG_BLOB_E_VERSION_NEWER/_OLD are a real version mismatch (spec 2.9's
 * "CFG_VERSION never retried" token, distinct from wire corruption); every
 * other non-OK/MIGRATED rc (short/magic/length/crc) stays CRC_FAIL. */
static void commit_fail_unwrap(const ring_frame_t *f, hg_blob_rc_t rc) {
    if (rc == HG_BLOB_E_VERSION_NEWER || rc == HG_BLOB_E_VERSION_OLD)
        commit_fail(f, "ERR CFG_VERSION", (uint8_t)(sizeof("ERR CFG_VERSION") - 1));
    else
        commit_fail(f, "ERR CRC_FAIL", (uint8_t)(sizeof("ERR CRC_FAIL") - 1));
}

/* §4.4 revert-push race (final review): a push whose CFG_CHUNKs are already on
 * the wire, with its CFG_COMMIT queued in the master's tracker behind a
 * forwarded CMD, commits AFTER that CMD's own SET has been applied here -- the
 * zone's generation has moved past the pushed payload, which would otherwise be
 * applied silently over the operator's own edit.
 *
 * Every legitimate push carries max(hb_gen, cache_gen)+1 (node_mgr_cfg.c's
 * try_start, the manual-push branch, and a retry replaying the same push_gen --
 * confirmed, no path sends gen <= the zone's), i.e. a gen strictly ABOVE this
 * zone's, so:
 *   gen <  cur  -> stale push, never applied: ERR CFG_VERSION (never retried).
 *   gen == cur  -> exactly one benign origin, the master's COMMIT retry after a
 *                  LOST OK ACK: the payload is byte-identical to what is live,
 *                  so ACK OK WITHOUT re-applying (a re-apply would re-dirty the
 *                  plane and re-stamp NVS for no change). A gen == cur payload
 *                  that DIFFERS is an aliased/stale push: ERR CFG_VERSION.
 *   gen >  cur  -> the existing four-way path below.
 * A plain "gen <= cur -> reject" would have latched CFG_SYNC_FAILED on that
 * lost-ACK retry, which is why the identity test is the gate at gen == cur. */
static int commit_stale(const ring_frame_t *f, uint32_t gen, uint32_t cur,
                        const void *payload, const void *live, size_t n) {
    if (gen > cur) return 0;                                 /* normal push: carry on */
    if (gen == cur && memcmp(payload, live, n) == 0) {
        commit_ok(f, 0);                                     /* lost-ACK retry: replay OK, no re-apply */
        return 1;
    }
    COMMIT_FAIL(f, "ERR CFG_VERSION");
    return 1;
}

void zone_ring_cfg_commit(const ring_frame_t *f, uint32_t now) {
    if (f->hdr.len < 5) return;                              /* malformed: never crash on a short frame */
    if (zring_dup_begin(f, now)) return;                     /* retransmit: absorbed / cached ACK replayed */
    if (ring_casm_idle_expired(&s_casm, now)) ring_casm_init(&s_casm);

    uint8_t  kind = f->payload[0];
    uint32_t gen  = rd32le(f->payload + 1);
    if (!casm_complete(&s_casm) || s_casm.kind != kind || s_casm.gen != gen) {
        commit_fail(f, "ERR MISSING_CHUNK", (uint8_t)(sizeof("ERR MISSING_CHUNK") - 1));
        return;
    }

    char err[48];
    char detail[126];
    if (kind == 1) {                                         /* CFG plane */
        hg_zone_cfg_t cfg;
        uint32_t env_gen;
        hg_blob_rc_t rc = hg_blob_unwrap(HG_MAGIC_CFG, HG_CFG_VER, HG_CFG_VER_MIN,
                                          s_casm.buf, s_casm.total, &cfg, sizeof cfg, &env_gen);
        if (rc != HG_BLOB_OK && rc != HG_BLOB_MIGRATED) { commit_fail_unwrap(f, rc); return; }
        hg_zone_cfg_t live;
        hg_model_snapshot_cfg(&live, NULL);          /* live.generation == hg_model_cfg_info's gen */
        if (commit_stale(f, gen, live.generation, &cfg, &live, sizeof cfg)) return;
        /* Full gen chain must agree before apply (identity contract, verbatim
         * apply): COMMIT.gen == assembler gen (already tied above, since the
         * completeness check above required s_casm.gen == gen), envelope gen
         * == COMMIT/assembler gen, and envelope gen == the payload's own
         * embedded cfg.generation. Without the middle tie, a push whose
         * chunk/COMMIT headers carry one gen over a blob whose envelope+
         * embedded fields agree on a *different* gen would pass every other
         * check and silently re-open the spec 4.4 re-push loop. */
        if (gen != env_gen || env_gen != cfg.generation) {
            commit_fail(f, "ERR INVALID_FIELD generation", (uint8_t)(sizeof("ERR INVALID_FIELD generation") - 1));
            return;
        }
        hg_zone_hw_t hw;
        hg_model_snapshot_hw(&hw);
        if (hg_cfg_validate(&cfg, &hw, err, sizeof err) != 0) {
            int n = snprintf(detail, sizeof detail, "ERR INVALID_FIELD %s", err);
            commit_fail(f, detail, (uint8_t)(n < 0 ? 0 : (n > 125 ? 125 : n)));
            return;
        }
        hg_model_apply_cfg(&cfg);                            /* verbatim: gen + source as pushed */
    } else if (kind == 2) {                                  /* HW plane */
        hg_zone_hw_t hw;
        hg_blob_rc_t rc = hg_blob_unwrap(HG_MAGIC_HW, HG_HW_VER, HG_HW_VER_MIN,
                                          s_casm.buf, s_casm.total, &hw, sizeof hw, NULL);
        if (rc != HG_BLOB_OK && rc != HG_BLOB_MIGRATED) { commit_fail_unwrap(f, rc); return; }
        /* The HW plane carries NO generation anywhere (zone_ring_cfg_get sends
         * gen 0, node_mgr caches gen 0), so the gen ordering above cannot
         * discriminate stale from fresh here -- gen == cur == 0 always. Only
         * the identity half of the ruling applies: an identical payload is the
         * lost-ACK retry and is ACKed without re-applying; a DIFFERING payload
         * is a normal HW push and must not be rejected as CFG_VERSION (which,
         * at a permanently equal gen, would reject every HW push there will
         * ever be). Reported as a deviation from the brief's letter. */
        hg_zone_hw_t live;
        hg_model_snapshot_hw(&live);
        if (memcmp(&hw, &live, sizeof hw) == 0) { commit_ok(f, 0); return; }
        /* hg_model_apply_hw validates (hw alone, then the *current* cfg
         * against this *new* hw -- atomicity) before writing anything. */
        if (hg_model_apply_hw(&hw, err, sizeof err) != 0) {
            int n = snprintf(detail, sizeof detail, "ERR INVALID_FIELD %s", err);
            commit_fail(f, detail, (uint8_t)(n < 0 ? 0 : (n > 125 ? 125 : n)));
            return;
        }
    } else {
        commit_fail(f, "ERR MISSING_CHUNK", (uint8_t)(sizeof("ERR MISSING_CHUNK") - 1));
        return;
    }
    commit_ok(f, 1);
}

void zone_ring_cfg_get(const ring_frame_t *f, uint32_t now) {
    (void)now;
    if (f->hdr.len < 1) return;
    uint8_t kind = f->payload[0];
    if (kind != 1 && kind != 2) return;
    zring_send_ack(f->hdr.src, f->hdr.seq, 0, "OK", 2);      /* OK = transfer starting (spec 2.9) */

    uint8_t  wire[HG_BLOB_HDR_LEN + sizeof(hg_zone_cfg_t)];  /* CFG is the larger of the two planes */
    size_t   wire_len;
    uint32_t gen;
    if (kind == 1) {
        hg_zone_cfg_t cfg;
        hg_model_snapshot_cfg(&cfg, NULL);
        gen = cfg.generation;
        wire_len = hg_blob_wrap(HG_MAGIC_CFG, HG_CFG_VER, gen, &cfg, (uint16_t)sizeof cfg, wire, sizeof wire);
    } else {
        hg_zone_hw_t hw;
        hg_model_snapshot_hw(&hw);
        gen = 0;   /* the HW plane carries no generation concept anywhere on the wire (no HB field either) */
        wire_len = hg_blob_wrap(HG_MAGIC_HW, HG_HW_VER, gen, &hw, (uint16_t)sizeof hw, wire, sizeof wire);
    }
    if (!wire_len) return;

    int count = ring_cfg_chunk_count(wire_len);
    for (int i = 0; i < count; i++) {
        uint8_t payload[9 + RING_CFG_DATA_MAX];
        int n = ring_cfg_chunk_build(kind, gen, wire, wire_len, (uint8_t)i, payload, sizeof payload);
        if (n < 0) break;
        ring_hdr_t h = { .src = hg_store_zid(), .dst = f->hdr.src, .type = RING_T_CFG_CHUNK, .flags = 0,
                          .ttl = RING_TTL_INIT, .len = (uint8_t)n, .seq = zring_next_seq() };
        ring_link_send(&h, payload);
    }
}
