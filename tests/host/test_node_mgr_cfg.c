#include <string.h>
#include <stdio.h>
#include "unity.h"
#include "hg_blob.h"
#include "hg_cfg.h"
#include "notify.h"
#include "node_mgr.h"
#include "node_mgr_internal.h"
#include "node_mgr_cfg_internal.h"
#include "fake_nmgr.h"
#include "fake_clock.h"

/* Spec section 4.4 config reconciliation, on the host: node_mgr_cfg.c (the
 * decision half), node_mgr_cfgx.c (the transfer half) and node_mgr_cfg_api.c
 * (the web UI's primitives) linked against fake_nmgr's plumbing seam and the
 * REAL notify / hg_blob / ring_cfgx codecs. Every assertion is made on what
 * the two halves put on the wire (fake_nmgr's submit/send_raw logs), on the
 * RAM caches (nmgr_cfg_cache) and on the captured NOTIFY lines -- the same
 * three things the bench reads. */

/* ---- notify capture ---- */
#define CAP_MAX 24
static char s_line[CAP_MAX][NTF_LINE_MAX];
static int  s_nline;
static void cap_sink(void *ctx, const char *line) {
    (void)ctx;
    if (s_nline < CAP_MAX) snprintf(s_line[s_nline++], NTF_LINE_MAX, "%s", line);
}
static int saw(const char *needle) {
    for (int i = 0; i < s_nline; i++) if (strstr(s_line[i], needle)) return 1;
    return 0;
}
static int saw_n(const char *needle) {
    int n = 0;
    for (int i = 0; i < s_nline; i++) if (strstr(s_line[i], needle)) n++;
    return n;
}

void setUp(void) {
    fake_clock_set(10000);
    fnm_reset();
    notify_init(fake_clock_now, 0);
    s_nline = 0;
    notify_add_sink(cap_sink, NULL, NTF_MASK_ALL);
    nmgr_cfg_init();
}
void tearDown(void) {
    /* nmgr_lock() is a plain FreeRTOS mutex on the target, so any nesting the
       new code introduced would deadlock there -- every test checks it. */
    TEST_ASSERT_TRUE_MESSAGE(g_nmgr.lock_max <= 1, "nmgr_lock() was nested");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, g_nmgr.lock_depth, "nmgr_lock() left held");
}

/* ---- helpers ---- */

#define CFG_BLOB_LEN (HG_BLOB_HDR_LEN + sizeof(hg_zone_cfg_t))

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void tick(void) { nmgr_cfg_tick_1s(fake_clock_now()); }
static void hb(uint8_t zone) { nmgr_cfg_note_fresh_hb(zone); }
static void second(uint8_t zone) { fake_clock_add(1000); hb(zone); tick(); }

static uint16_t last_seq(void) { return g_nmgr.sub[g_nmgr.n_sub - 1].seq; }

static void ack_ok(uint16_t seq) {
    ring_trk_ev_t ev; memset(&ev, 0, sizeof ev);
    ev.kind = RING_TRK_EV_DONE; ev.seq = seq; ev.status = 0;
    snprintf(ev.detail, sizeof ev.detail, "OK");
    nmgr_cfg_on_ev(&ev);
}
static void ack_err(uint16_t seq, const char *detail) {
    ring_trk_ev_t ev; memset(&ev, 0, sizeof ev);
    ev.kind = RING_TRK_EV_DONE; ev.seq = seq; ev.status = 1;
    snprintf(ev.detail, sizeof ev.detail, "%s", detail);
    nmgr_cfg_on_ev(&ev);
}
static void ack_timeout(uint16_t seq) {
    ring_trk_ev_t ev; memset(&ev, 0, sizeof ev);
    ev.kind = RING_TRK_EV_FAIL; ev.seq = seq; ev.fail_token = "ZONE_TIMEOUT";
    nmgr_cfg_on_ev(&ev);
}

/* the CFG_CHUNK burst a zone answers a CFG_GET with */
static void feed_chunks(uint8_t zone, uint8_t kind, uint32_t gen, const uint8_t *blob, size_t len) {
    int count = ring_cfg_chunk_count(len);
    for (int i = 0; i < count; i++) {
        uint8_t payload[9 + RING_CFG_DATA_MAX];
        int n = ring_cfg_chunk_build(kind, gen, blob, len, (uint8_t)i, payload, sizeof payload);
        TEST_ASSERT_GREATER_THAN_INT(0, n);
        ring_frame_t f; memset(&f, 0, sizeof f);
        f.hdr.src = zone; f.hdr.dst = RING_ID_MASTER; f.hdr.type = RING_T_CFG_CHUNK;
        f.hdr.len = (uint8_t)n;
        memcpy(f.payload, payload, (size_t)n);
        nmgr_cfg_on_chunk(&f, fake_clock_now());
    }
}

/* reassemble the chunk burst node_mgr_cfgx.c just sent (the raw log) */
static size_t gather_push(uint8_t *out, size_t cap) {
    size_t total = 0;
    for (int i = 0; i < g_nmgr.n_raw; i++) {
        const fnm_rec_t *r = &g_nmgr.raw[i];
        if (r->type != RING_T_CFG_CHUNK) continue;
        uint8_t idx = r->payload[5];
        size_t dlen = (size_t)r->len - 9, off = (size_t)idx * RING_CFG_DATA_MAX;
        TEST_ASSERT_TRUE(off + dlen <= cap);
        memcpy(out + off, r->payload + 9, dlen);
        if (off + dlen > total) total = off + dlen;
    }
    return total;
}

static uint32_t seed_cfg(uint8_t zone, const hg_zone_cfg_t *c, uint32_t gen) {
    nmgr_cache_t *k = nmgr_cfg_cache(zone, 1);
    hg_blob_wrap(HG_MAGIC_CFG, HG_CFG_VER, gen, c, (uint16_t)sizeof *c, k->blob, sizeof k->blob);
    k->gen = gen;
    k->crc = hg_crc32(0, k->blob + HG_BLOB_HDR_LEN, sizeof *c);
    k->valid = 1;
    return k->crc;
}
static uint32_t seed_hw(uint8_t zone, const hg_zone_hw_t *h) {
    nmgr_cache_t *k = nmgr_cfg_cache(zone, 2);
    hg_blob_wrap(HG_MAGIC_HW, HG_HW_VER, 0, h, (uint16_t)sizeof *h, k->blob, sizeof k->blob);
    k->gen = 0;
    k->crc = hg_crc32(0, k->blob + HG_BLOB_HDR_LEN, sizeof *h);
    k->valid = 1;
    return k->crc;
}

/* run both pulls of the enrolment adopt to completion */
static void adopt_zone(uint8_t zone, const hg_zone_cfg_t *c, uint32_t gen, const hg_zone_hw_t *h) {
    uint8_t blob[CFG_BLOB_LEN];
    fnm_node(zone, gen, hg_crc32(0, c, sizeof *c), hg_crc32(0, h, sizeof *h));
    hb(zone); tick();
    ack_ok(last_seq());
    size_t bl = hg_blob_wrap(HG_MAGIC_CFG, HG_CFG_VER, gen, c, (uint16_t)sizeof *c, blob, sizeof blob);
    feed_chunks(zone, 1, gen, blob, bl);
    second(zone);
    ack_ok(last_seq());
    bl = hg_blob_wrap(HG_MAGIC_HW, HG_HW_VER, 0, h, (uint16_t)sizeof *h, blob, sizeof blob);
    feed_chunks(zone, 2, 0, blob, bl);
}

/* ---- tests ---- */

/* Spec 4.4 enrolment branch: no cache -> the master ADOPTS what the zone has
   (pull), and never pushes over a config it has not read. */
static void test_adopt_pulls_when_no_cache(void) {
    hg_zone_cfg_t c; hg_defaults_cfg(&c); snprintf(c.name, sizeof c.name, "Kitchen"); c.generation = 3;
    hg_zone_hw_t h; hg_defaults_hw(&h);
    uint32_t ccrc = hg_crc32(0, &c, sizeof c), hcrc = hg_crc32(0, &h, sizeof h);
    fnm_node(2, 3, ccrc, hcrc);

    hb(2); tick();
    TEST_ASSERT_EQUAL_INT(1, g_nmgr.n_sub);
    TEST_ASSERT_EQUAL_UINT8(RING_T_CFG_GET, g_nmgr.sub[0].type);
    TEST_ASSERT_EQUAL_UINT8(2, g_nmgr.sub[0].dst);
    TEST_ASSERT_EQUAL_UINT8(1, g_nmgr.sub[0].payload[0]);        /* kind 1 = CFG */

    ack_ok(g_nmgr.sub[0].seq);
    uint8_t blob[CFG_BLOB_LEN];
    size_t bl = hg_blob_wrap(HG_MAGIC_CFG, HG_CFG_VER, 3, &c, (uint16_t)sizeof c, blob, sizeof blob);
    feed_chunks(2, 1, 3, blob, bl);

    nmgr_cache_t *k = nmgr_cfg_cache(2, 1);
    TEST_ASSERT_EQUAL_UINT8(1, k->valid);
    TEST_ASSERT_EQUAL_UINT32(3, k->gen);
    TEST_ASSERT_EQUAL_UINT32(ccrc, k->crc);
    TEST_ASSERT_EQUAL_INT(0, fnm_count(g_nmgr.sub, g_nmgr.n_sub, RING_T_CFG_COMMIT));
    TEST_ASSERT_EQUAL_INT(0, g_nmgr.n_raw);

    /* the HW plane is the next decision (pull-only, spec 4.4) ... */
    second(2);
    TEST_ASSERT_EQUAL_INT(2, g_nmgr.n_sub);
    TEST_ASSERT_EQUAL_UINT8(RING_T_CFG_GET, g_nmgr.sub[1].type);
    TEST_ASSERT_EQUAL_UINT8(2, g_nmgr.sub[1].payload[0]);        /* kind 2 = HW */
    ack_ok(g_nmgr.sub[1].seq);
    bl = hg_blob_wrap(HG_MAGIC_HW, HG_HW_VER, 0, &h, (uint16_t)sizeof h, blob, sizeof blob);
    feed_chunks(2, 2, 0, blob, bl);
    TEST_ASSERT_EQUAL_UINT32(hcrc, nmgr_cfg_cache(2, 2)->crc);

    /* ... and then the zone is in sync: no more traffic at all */
    second(2);
    second(2);
    TEST_ASSERT_EQUAL_INT(2, g_nmgr.n_sub);
    TEST_ASSERT_EQUAL_INT(0, g_nmgr.n_raw);
}

/* Spec 4.4: a zone-console edit (a higher gen than the master's cache) is
   REVERTED -- the master re-pushes its own cache at gen+1. */
static void test_zone_console_edit_is_reverted(void) {
    hg_zone_cfg_t c; hg_defaults_cfg(&c); c.generation = 3;
    hg_zone_hw_t h; hg_defaults_hw(&h);
    seed_cfg(2, &c, 3);
    uint32_t hcrc = seed_hw(2, &h);
    fnm_node(2, 4, 0xDEADBEEFu, hcrc);      /* the zone bumped itself to gen 4 */

    hb(2); tick();
    TEST_ASSERT_EQUAL_INT(ring_cfg_chunk_count(CFG_BLOB_LEN), g_nmgr.n_raw);
    TEST_ASSERT_EQUAL_UINT8(RING_T_CFG_CHUNK, g_nmgr.raw[0].type);
    TEST_ASSERT_EQUAL_INT(1, g_nmgr.n_sub);
    TEST_ASSERT_EQUAL_UINT8(RING_T_CFG_COMMIT, g_nmgr.sub[0].type);
    TEST_ASSERT_EQUAL_UINT8(1, g_nmgr.sub[0].payload[0]);
    TEST_ASSERT_EQUAL_UINT32(5, le32(g_nmgr.sub[0].payload + 1));
    TEST_ASSERT_TRUE(saw("NOTIFY NODE 2 CFG_REVERTED 5"));

    ack_ok(g_nmgr.sub[0].seq);
    TEST_ASSERT_EQUAL_UINT32(5, nmgr_cfg_cache(2, 1)->gen);
    TEST_ASSERT_EQUAL_UINT8(1, nmgr_cfg_cache(2, 1)->valid);
}

/* The web UI's write: node_mgr_cfg_set queues a request that the 1 Hz tick
   turns into a gen-bumped, MASTER-sourced push of exactly those bytes. */
static void test_cfg_set_request_pushes_new_config(void) {
    hg_zone_cfg_t c; hg_defaults_cfg(&c); snprintf(c.name, sizeof c.name, "Kitchen"); c.generation = 5;
    hg_zone_hw_t h; hg_defaults_hw(&h);
    uint32_t ccrc = seed_cfg(2, &c, 5);
    uint32_t hcrc = seed_hw(2, &h);
    fnm_node(2, 5, ccrc, hcrc);                       /* in sync at gen 5 */

    hg_zone_cfg_t edited = c;
    edited.shelf[0].water.target_pct = 61;
    snprintf(edited.name, sizeof edited.name, "Basil");

    TEST_ASSERT_EQUAL_INT(0, node_mgr_cfg_set(2, &edited));
    TEST_ASSERT_EQUAL_INT(1, node_mgr_cfg_busy(2));
    TEST_ASSERT_EQUAL_INT(-2, node_mgr_cfg_set(2, &edited));    /* HTTP 409 */

    tick();
    TEST_ASSERT_EQUAL_INT(1, g_nmgr.n_sub);
    TEST_ASSERT_EQUAL_UINT8(RING_T_CFG_COMMIT, g_nmgr.sub[0].type);
    TEST_ASSERT_EQUAL_UINT32(6, le32(g_nmgr.sub[0].payload + 1));
    TEST_ASSERT_EQUAL_INT(1, node_mgr_cfg_busy(2));             /* awaiting the ACK */
    TEST_ASSERT_EQUAL_INT(-2, node_mgr_cfg_set(2, &edited));

    uint8_t wire[CFG_BLOB_LEN];
    size_t n = gather_push(wire, sizeof wire);
    TEST_ASSERT_EQUAL_size_t(CFG_BLOB_LEN, n);
    hg_zone_cfg_t got; uint32_t gen = 0;
    TEST_ASSERT_EQUAL_INT(HG_BLOB_OK, hg_blob_unwrap(HG_MAGIC_CFG, HG_CFG_VER, HG_CFG_VER_MIN,
                                                      wire, n, &got, (uint16_t)sizeof got, &gen));
    TEST_ASSERT_EQUAL_UINT32(6, gen);
    hg_zone_cfg_t want = edited;
    want.generation = 6; want.source = HG_SRC_MASTER;
    TEST_ASSERT_EQUAL_MEMORY(&want, &got, sizeof want);

    ack_ok(g_nmgr.sub[0].seq);
    TEST_ASSERT_EQUAL_INT(0, node_mgr_cfg_busy(2));
    TEST_ASSERT_EQUAL_UINT32(6, nmgr_cfg_cache(2, 1)->gen);

    /* and the cache reads back as what was written */
    hg_zone_cfg_t rb; uint32_t rbgen = 0;
    TEST_ASSERT_EQUAL_INT(0, node_mgr_cfg_get(2, &rb, NULL, &rbgen, NULL));
    TEST_ASSERT_EQUAL_UINT32(6, rbgen);
    TEST_ASSERT_EQUAL_MEMORY(&want, &rb, sizeof want);

    TEST_ASSERT_EQUAL_INT(-1, node_mgr_cfg_set(5, &edited));    /* no such zone -> 404 */
    TEST_ASSERT_EQUAL_INT(-1, node_mgr_cfg_set(9, &edited));
    g_nmgr.tab[1].health = NODE_H_OFFLINE;
    TEST_ASSERT_EQUAL_INT(-3, node_mgr_cfg_set(2, &edited));    /* known, not ONLINE -> 409 */
}

/* A write queued while ANOTHER zone's transfer holds the single slot waits in
   its inbox instead of being dropped -- the difference between this and
   node_mgr_push_cfg, whose request the tick discards in the same situation.
   (A write for the zone that is mid-transfer is refused with -2 up front, so
   the pending case can only arise across zones.) */
static void test_set_request_survives_another_zones_transfer(void) {
    hg_zone_cfg_t c; hg_defaults_cfg(&c); c.generation = 5;
    hg_zone_hw_t h; hg_defaults_hw(&h);
    uint32_t ccrc = seed_cfg(2, &c, 5);
    uint32_t hcrc = seed_hw(2, &h);
    fnm_node(2, 5, ccrc, hcrc);                  /* zone 2: in sync, writable */
    fnm_node(3, 3, hg_crc32(0, &c, sizeof c), hcrc);   /* zone 3: needs an adopt */

    hb(3); tick();                               /* zone 3's CFG_GET holds the slot */
    TEST_ASSERT_EQUAL_INT(1, g_nmgr.n_sub);
    TEST_ASSERT_EQUAL_UINT8(RING_T_CFG_GET, g_nmgr.sub[0].type);
    TEST_ASSERT_EQUAL_UINT8(3, g_nmgr.sub[0].dst);

    TEST_ASSERT_EQUAL_INT(0, node_mgr_cfg_set(2, &c));
    fake_clock_add(1000); hb(2); tick();
    TEST_ASSERT_EQUAL_INT(0, g_nmgr.n_raw);              /* nothing pushed ... */
    TEST_ASSERT_EQUAL_INT(1, g_nmgr.n_sub);
    TEST_ASSERT_EQUAL_INT(1, nmgr_cfg_req_pending(2));   /* ... and still queued */
    TEST_ASSERT_EQUAL_INT(1, node_mgr_cfg_busy(2));

    ack_ok(g_nmgr.sub[0].seq);                   /* let zone 3's pull finish */
    uint8_t blob[CFG_BLOB_LEN];
    size_t bl = hg_blob_wrap(HG_MAGIC_CFG, HG_CFG_VER, 3, &c, (uint16_t)sizeof c, blob, sizeof blob);
    feed_chunks(3, 1, 3, blob, bl);

    fake_clock_add(1000); tick();
    TEST_ASSERT_EQUAL_INT(0, nmgr_cfg_req_pending(2));
    TEST_ASSERT_EQUAL_INT(2, g_nmgr.n_sub);
    TEST_ASSERT_EQUAL_UINT8(RING_T_CFG_COMMIT, g_nmgr.sub[1].type);
    TEST_ASSERT_EQUAL_UINT8(2, g_nmgr.sub[1].dst);
    TEST_ASSERT_EQUAL_UINT32(6, le32(g_nmgr.sub[1].payload + 1));
}

/* A terminal CFG latch parks the CFG plane only: the HW envelope is a separate
   blob with its own version, so the zone's hardware page must still fill in. */
static void test_cfg_latch_does_not_block_the_hw_plane(void) {
    hg_zone_cfg_t c; hg_defaults_cfg(&c); c.generation = 3;
    hg_zone_hw_t h; hg_defaults_hw(&h); h.shelf_count = 3;
    uint32_t hcrc = hg_crc32(0, &h, sizeof h);
    fnm_node(2, 3, hg_crc32(0, &c, sizeof c), hcrc);

    hb(2); tick();                               /* CFG pull ... */
    ack_ok(last_seq());
    uint8_t blob[CFG_BLOB_LEN];
    size_t bl = hg_blob_wrap(HG_MAGIC_CFG, HG_CFG_VER + 1, 3, &c, (uint16_t)sizeof c, blob, sizeof blob);
    feed_chunks(2, 1, 3, blob, bl);              /* ... fails terminally */
    TEST_ASSERT_EQUAL_INT(1, node_mgr_cfg_sync_failed(2));

    second(2);                                   /* next decision: the HW plane */
    TEST_ASSERT_EQUAL_INT(2, g_nmgr.n_sub);
    TEST_ASSERT_EQUAL_UINT8(RING_T_CFG_GET, g_nmgr.sub[1].type);
    TEST_ASSERT_EQUAL_UINT8(2, g_nmgr.sub[1].payload[0]);   /* kind 2 = HW */
    ack_ok(g_nmgr.sub[1].seq);
    bl = hg_blob_wrap(HG_MAGIC_HW, HG_HW_VER, 0, &h, (uint16_t)sizeof h, blob, sizeof blob);
    feed_chunks(2, 2, 0, blob, bl);
    TEST_ASSERT_EQUAL_UINT32(hcrc, nmgr_cfg_cache(2, 2)->crc);

    /* both planes settled as far as they can go: no cfg cache, so no push */
    second(2);
    second(2);
    TEST_ASSERT_EQUAL_INT(2, g_nmgr.n_sub);
    TEST_ASSERT_EQUAL_INT(0, g_nmgr.n_raw);
    TEST_ASSERT_EQUAL_INT(-1, node_mgr_cfg_get(2, &c, &h, NULL, NULL));   /* cfg still unreadable */
}

static void test_cfg_get_returns_unwrapped_copy(void) {
    hg_zone_cfg_t c; hg_defaults_cfg(&c); snprintf(c.name, sizeof c.name, "Kitchen"); c.generation = 3;
    hg_zone_hw_t h; hg_defaults_hw(&h); h.shelf_count = 2;
    adopt_zone(2, &c, 3, &h);

    hg_zone_cfg_t gc; hg_zone_hw_t gh; uint32_t cg = 9, hwg = 9;
    TEST_ASSERT_EQUAL_INT(0, node_mgr_cfg_get(2, &gc, &gh, &cg, &hwg));
    TEST_ASSERT_EQUAL_STRING("Kitchen", gc.name);
    TEST_ASSERT_EQUAL_UINT32(3, cg);
    TEST_ASSERT_EQUAL_MEMORY(&c, &gc, sizeof c);
    TEST_ASSERT_EQUAL_UINT8(2, gh.shelf_count);
    TEST_ASSERT_EQUAL_MEMORY(&h, &gh, sizeof h);

    /* cfg cached, hw not yet: still 0, with hw zeroed and its gen 0 */
    seed_cfg(3, &c, 3);
    memset(&gh, 0xAA, sizeof gh); hwg = 9;
    TEST_ASSERT_EQUAL_INT(0, node_mgr_cfg_get(3, &gc, &gh, &cg, &hwg));
    TEST_ASSERT_EQUAL_UINT32(0, hwg);
    hg_zone_hw_t zero; memset(&zero, 0, sizeof zero);
    TEST_ASSERT_EQUAL_MEMORY(&zero, &gh, sizeof zero);

    TEST_ASSERT_EQUAL_INT(-1, node_mgr_cfg_get(5, &gc, &gh, &cg, &hwg));   /* no cache */
    TEST_ASSERT_EQUAL_INT(-1, node_mgr_cfg_get(0, &gc, &gh, &cg, &hwg));
    TEST_ASSERT_EQUAL_INT(-1, node_mgr_cfg_get(9, &gc, &gh, &cg, &hwg));
}

/* A zone running a NEWER config layout than this master can parse is not a
   transient failure: retrying cannot help, so it latches the way an ACK's
   CFG_VERSION does and the reconciler leaves the zone alone until one of the
   two identities changes. */
static void test_version_newer_pull_is_terminal(void) {
    hg_zone_cfg_t c; hg_defaults_cfg(&c); c.generation = 3;
    hg_zone_hw_t h; hg_defaults_hw(&h);
    /* HW already adopted, so the CFG latch is the only thing under test here
       (test_cfg_latch_does_not_block_the_hw_plane covers the other plane). */
    fnm_node(2, 3, hg_crc32(0, &c, sizeof c), seed_hw(2, &h));

    hb(2); tick();
    ack_ok(last_seq());
    uint8_t blob[CFG_BLOB_LEN];
    size_t bl = hg_blob_wrap(HG_MAGIC_CFG, HG_CFG_VER + 1, 3, &c, (uint16_t)sizeof c, blob, sizeof blob);
    feed_chunks(2, 1, 3, blob, bl);

    TEST_ASSERT_EQUAL_INT(1, node_mgr_cfg_sync_failed(2));
    TEST_ASSERT_EQUAL_UINT8(0, nmgr_cfg_cache(2, 1)->valid);
    TEST_ASSERT_TRUE(saw("NOTIFY NODE 2 CFG_SYNC_FAILED"));

    int n = g_nmgr.n_sub;
    for (int i = 0; i < 5; i++) second(2);
    TEST_ASSERT_EQUAL_INT(n, g_nmgr.n_sub);          /* latched: nothing retried */

    g_nmgr.tab[1].hb.cfg_crc = 0x2222u;              /* a different heartbeat identity */
    second(2);
    TEST_ASSERT_EQUAL_INT(n + 1, g_nmgr.n_sub);
    TEST_ASSERT_EQUAL_UINT8(RING_T_CFG_GET, g_nmgr.sub[n].type);
}

/* A zone whose BOTH envelopes are newer than this master: each plane latches
   on its own, and the zone then goes quiet. With one latch slot per zone the
   two planes overwrote each other's latch and the reconciler flapped -- one
   pull and one CFG_SYNC_FAILED per heartbeat, for ever, with cmd_timeouts
   pinned at 3 (DEGRADED). */
static void test_both_planes_terminal_latch_independently(void) {
    hg_zone_cfg_t c; hg_defaults_cfg(&c); c.generation = 3;
    hg_zone_hw_t h; hg_defaults_hw(&h);
    fnm_node(2, 3, hg_crc32(0, &c, sizeof c), hg_crc32(0, &h, sizeof h));
    uint8_t blob[CFG_BLOB_LEN];

    hb(2); tick();                                  /* CFG pull -> version newer */
    TEST_ASSERT_EQUAL_UINT8(1, g_nmgr.sub[0].payload[0]);
    ack_ok(last_seq());
    size_t bl = hg_blob_wrap(HG_MAGIC_CFG, HG_CFG_VER + 1, 3, &c, (uint16_t)sizeof c, blob, sizeof blob);
    feed_chunks(2, 1, 3, blob, bl);

    second(2);                                      /* HW pull -> version newer too */
    TEST_ASSERT_EQUAL_INT(2, g_nmgr.n_sub);
    TEST_ASSERT_EQUAL_UINT8(2, g_nmgr.sub[1].payload[0]);
    ack_ok(last_seq());
    bl = hg_blob_wrap(HG_MAGIC_HW, HG_HW_VER + 1, 0, &h, (uint16_t)sizeof h, blob, sizeof blob);
    feed_chunks(2, 2, 0, blob, bl);

    TEST_ASSERT_EQUAL_INT(1, node_mgr_cfg_sync_failed(2));
    for (int i = 0; i < 5; i++) second(2);
    TEST_ASSERT_EQUAL_INT(2, g_nmgr.n_sub);         /* both planes parked: no traffic at all */
    TEST_ASSERT_EQUAL_INT(0, g_nmgr.n_raw);
    TEST_ASSERT_EQUAL_INT(2, saw_n("CFG_SYNC_FAILED"));   /* one per plane, not one per tick */

    /* and a heartbeat with a new CFG identity re-opens the CFG plane only */
    g_nmgr.tab[1].hb.cfg_crc = 0x2222u;
    second(2);
    TEST_ASSERT_EQUAL_INT(3, g_nmgr.n_sub);
    TEST_ASSERT_EQUAL_UINT8(1, g_nmgr.sub[2].payload[0]);
}

/* The push's terminal ACK tokens keep their SP3 behaviour. */
static void test_push_cfg_version_ack_is_terminal(void) {
    hg_zone_cfg_t c; hg_defaults_cfg(&c); c.generation = 3;
    hg_zone_hw_t h; hg_defaults_hw(&h);
    seed_cfg(2, &c, 3);
    uint32_t hcrc = seed_hw(2, &h);
    fnm_node(2, 4, 0xDEADBEEFu, hcrc);

    hb(2); tick();
    TEST_ASSERT_EQUAL_UINT8(RING_T_CFG_COMMIT, g_nmgr.sub[0].type);
    ack_err(g_nmgr.sub[0].seq, "ERR CFG_VERSION");
    TEST_ASSERT_EQUAL_INT(1, node_mgr_cfg_sync_failed(2));

    int n = g_nmgr.n_sub;
    for (int i = 0; i < 5; i++) second(2);
    TEST_ASSERT_EQUAL_INT(n, g_nmgr.n_sub);
}

/* Rule (a): the scan resumes after the zone that got the last decision, so a
   zone that always has work cannot starve a higher id. */
static void test_round_robin_alternates(void) {
    hg_zone_cfg_t c; hg_defaults_cfg(&c); c.generation = 3;
    hg_zone_hw_t h; hg_defaults_hw(&h);
    uint32_t ccrc = hg_crc32(0, &c, sizeof c), hcrc = hg_crc32(0, &h, sizeof h);
    fnm_node(2, 3, ccrc, hcrc);
    fnm_node(3, 3, ccrc, hcrc);

    hb(2); hb(3); tick();
    TEST_ASSERT_EQUAL_INT(1, g_nmgr.n_sub);
    TEST_ASSERT_EQUAL_UINT8(2, g_nmgr.sub[0].dst);

    ack_ok(g_nmgr.sub[0].seq);
    uint8_t blob[CFG_BLOB_LEN];
    size_t bl = hg_blob_wrap(HG_MAGIC_CFG, HG_CFG_VER, 3, &c, (uint16_t)sizeof c, blob, sizeof blob);
    feed_chunks(2, 1, 3, blob, bl);

    /* zone 2 still needs its HW plane, but zone 3 goes next all the same */
    fake_clock_add(1000); hb(2); hb(3); tick();
    TEST_ASSERT_EQUAL_INT(2, g_nmgr.n_sub);
    TEST_ASSERT_EQUAL_UINT8(3, g_nmgr.sub[1].dst);
}

/* Rule (b): a non-terminal failure (the retry ladder ran out) parks the zone
   for CFG_COOLDOWN_MS instead of flapping CFG_SYNC_FAILED every tick. */
static void test_cooldown_after_nonterminal_failure(void) {
    hg_zone_cfg_t c; hg_defaults_cfg(&c);
    hg_zone_hw_t h; hg_defaults_hw(&h);
    fnm_node(3, 3, hg_crc32(0, &c, sizeof c), hg_crc32(0, &h, sizeof h));

    hb(3); tick();
    TEST_ASSERT_EQUAL_INT(1, g_nmgr.n_sub);
    static const uint32_t backoff[3] = { 1000, 2000, 4000 };
    for (int i = 0; i < 3; i++) {                 /* initial try + 3 retries */
        ack_timeout(last_seq());
        fake_clock_add(backoff[i]);
        tick();
        TEST_ASSERT_EQUAL_INT(i + 2, g_nmgr.n_sub);
    }
    ack_timeout(last_seq());                      /* the 4th gives up, non-terminally */
    TEST_ASSERT_EQUAL_INT(0, node_mgr_cfg_sync_failed(3));   /* no latch, a cooldown */
    TEST_ASSERT_TRUE(saw("NOTIFY NODE 3 CFG_SYNC_FAILED"));

    int n = g_nmgr.n_sub;
    for (int i = 0; i < 29; i++) second(3);
    TEST_ASSERT_EQUAL_INT(n, g_nmgr.n_sub);
    fake_clock_add(2000); hb(3); tick();
    TEST_ASSERT_EQUAL_INT(n + 1, g_nmgr.n_sub);
    TEST_ASSERT_EQUAL_UINT8(3, g_nmgr.sub[n].dst);
}

/* SP3 Task 16's revert race: a forwarded SET the zone ACKed OK makes the
   master's cache stale, so it is dropped and the revert push's COMMIT --
   still queued behind that SET -- is withdrawn rather than landing on top of
   the value the operator just set. */
static void test_invalidate_aborts_push_and_drops_cache(void) {
    hg_zone_cfg_t c; hg_defaults_cfg(&c); c.generation = 3;
    hg_zone_hw_t h; hg_defaults_hw(&h);
    seed_cfg(2, &c, 3);
    uint32_t hcrc = seed_hw(2, &h);
    fnm_node(2, 4, 0xDEADBEEFu, hcrc);

    hb(2); tick();
    TEST_ASSERT_EQUAL_UINT8(RING_T_CFG_COMMIT, g_nmgr.sub[0].type);
    uint16_t commit = g_nmgr.sub[0].seq;

    nmgr_cfg_invalidate(2);
    TEST_ASSERT_EQUAL_INT(1, g_nmgr.n_cancel);
    TEST_ASSERT_EQUAL_UINT16(commit, g_nmgr.cancel[0]);
    TEST_ASSERT_EQUAL_UINT8(0, nmgr_cfg_cache(2, 1)->valid);
    TEST_ASSERT_EQUAL_INT(0, node_mgr_cfg_busy(2));

    ack_ok(commit);                               /* a late ACK must not resurrect it */
    TEST_ASSERT_EQUAL_UINT8(0, nmgr_cfg_cache(2, 1)->valid);

    second(2);                                    /* next decision: adopt, not re-push */
    TEST_ASSERT_EQUAL_INT(2, g_nmgr.n_sub);
    TEST_ASSERT_EQUAL_UINT8(RING_T_CFG_GET, g_nmgr.sub[1].type);
}

int main(void) { UNITY_BEGIN();
    RUN_TEST(test_adopt_pulls_when_no_cache);
    RUN_TEST(test_zone_console_edit_is_reverted);
    RUN_TEST(test_cfg_set_request_pushes_new_config);
    RUN_TEST(test_set_request_survives_another_zones_transfer);
    RUN_TEST(test_cfg_get_returns_unwrapped_copy);
    RUN_TEST(test_version_newer_pull_is_terminal);
    RUN_TEST(test_cfg_latch_does_not_block_the_hw_plane);
    RUN_TEST(test_both_planes_terminal_latch_independently);
    RUN_TEST(test_push_cfg_version_ack_is_terminal);
    RUN_TEST(test_round_robin_alternates);
    RUN_TEST(test_cooldown_after_nonterminal_failure);
    RUN_TEST(test_invalidate_aborts_push_and_drops_cache);
    return UNITY_END(); }
