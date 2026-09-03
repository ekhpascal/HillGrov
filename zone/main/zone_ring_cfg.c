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

static void commit_fail(const ring_frame_t *f, const char *detail, uint8_t dlen) {
    zring_send_ack(f->hdr.src, f->hdr.seq, 1, detail, dlen);
    ring_casm_init(&s_casm);
}

static void commit_ok(const ring_frame_t *f) {
    hg_store_flush(2000);
    zring_send_ack(f->hdr.src, f->hdr.seq, 0, "OK", 2);
    ring_casm_init(&s_casm);
}

void zone_ring_cfg_commit(const ring_frame_t *f, uint32_t now) {
    if (f->hdr.len < 5) return;                              /* malformed: never crash on a short frame */
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
        /* gen_out NULL: the unwrapped struct's own .generation field (part of
         * the payload itself, spec §2.9) is what hg_model_apply_cfg adopts --
         * the envelope header's copy of it is redundant for our purposes. */
        hg_blob_rc_t rc = hg_blob_unwrap(HG_MAGIC_CFG, HG_CFG_VER, HG_CFG_VER_MIN,
                                          s_casm.buf, s_casm.total, &cfg, sizeof cfg, NULL);
        if (rc != HG_BLOB_OK && rc != HG_BLOB_MIGRATED) {
            commit_fail(f, "ERR CRC_FAIL", (uint8_t)(sizeof("ERR CRC_FAIL") - 1));
            return;
        }
        hg_zone_hw_t hw;
        hg_model_snapshot_hw(&hw);
        if (hg_cfg_validate(&cfg, &hw, err, sizeof err) != 0) {
            int n = snprintf(detail, sizeof detail, "ERR INVALID_FIELD %s", err);
            commit_fail(f, detail, (uint8_t)(n < 0 ? 0 : (n > 125 ? 125 : n)));
            return;
        }
        hg_model_apply_cfg(&cfg);
    } else if (kind == 2) {                                  /* HW plane */
        hg_zone_hw_t hw;
        hg_blob_rc_t rc = hg_blob_unwrap(HG_MAGIC_HW, HG_HW_VER, HG_HW_VER_MIN,
                                          s_casm.buf, s_casm.total, &hw, sizeof hw, NULL);
        if (rc != HG_BLOB_OK && rc != HG_BLOB_MIGRATED) {
            commit_fail(f, "ERR CRC_FAIL", (uint8_t)(sizeof("ERR CRC_FAIL") - 1));
            return;
        }
        if (hg_hw_validate(&hw, err, sizeof err) != 0) {
            int n = snprintf(detail, sizeof detail, "ERR INVALID_FIELD %s", err);
            commit_fail(f, detail, (uint8_t)(n < 0 ? 0 : (n > 125 ? 125 : n)));
            return;
        }
        hg_model_apply_hw(&hw);
    } else {
        commit_fail(f, "ERR MISSING_CHUNK", (uint8_t)(sizeof("ERR MISSING_CHUNK") - 1));
        return;
    }
    commit_ok(f);
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
