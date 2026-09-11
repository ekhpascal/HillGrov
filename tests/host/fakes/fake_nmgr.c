#include <string.h>
#include "fake_nmgr.h"
#include "fake_clock.h"
#include "node_mgr_internal.h"

fnm_t g_nmgr;

void fnm_reset(void) {
    memset(&g_nmgr, 0, sizeof g_nmgr);
    g_nmgr.next_seq = 100;
}

void fnm_node(uint8_t id, uint32_t cfg_gen, uint32_t cfg_crc, uint32_t hw_crc) {
    if (id < 1 || id > HG_MAX_ZONES) return;
    hg_node_t *n = &g_nmgr.tab[id - 1];
    memset(n, 0, sizeof *n);
    n->used = 1; n->id = id; n->health = NODE_H_ONLINE;
    n->hb.cfg_gen = cfg_gen; n->hb.cfg_crc = cfg_crc; n->hb.hw_crc = hw_crc;
}

int fnm_count(const fnm_rec_t *v, int n, uint8_t type) {
    int c = 0;
    for (int i = 0; i < n; i++) if (v[i].type == type) c++;
    return c;
}

static void record(fnm_rec_t *v, int *n, uint8_t dst, uint8_t type,
                   const uint8_t *payload, uint8_t len, uint16_t seq) {
    if (*n >= FNM_MAX) return;
    fnm_rec_t *r = &v[(*n)++];
    memset(r, 0, sizeof *r);
    r->dst = dst; r->type = type; r->len = len; r->seq = seq;
    if (payload && len) memcpy(r->payload, payload, len);
}

/* ---- the seam ---- */

uint32_t nmgr_now_ms(void)  { return fake_clock_now(); }
uint16_t nmgr_next_seq(void) { return ++g_nmgr.next_seq; }

uint8_t nmgr_ring_size(void) {
    uint8_t n = 0;
    for (int i = 0; i < HG_MAX_ZONES; i++) if (g_nmgr.tab[i].used) n++;
    return n;
}

hg_node_t *nmgr_node_by_id(uint8_t id) {
    if (id < 1 || id > HG_MAX_ZONES) return NULL;
    return &g_nmgr.tab[id - 1];
}

hg_node_t *nmgr_table(void) { return g_nmgr.tab; }

void nmgr_lock(void) {
    if (++g_nmgr.lock_depth > g_nmgr.lock_max) g_nmgr.lock_max = g_nmgr.lock_depth;
}
void nmgr_unlock(void) { g_nmgr.lock_depth--; }

int nmgr_submit(uint8_t dst, uint8_t type, const uint8_t *payload, uint8_t len, uint16_t *seq_out) {
    if (g_nmgr.submit_rc != 0) return g_nmgr.submit_rc;
    uint16_t seq = nmgr_next_seq();
    record(g_nmgr.sub, &g_nmgr.n_sub, dst, type, payload, len, seq);
    if (seq_out) *seq_out = seq;
    return 0;
}

int nmgr_cancel(uint16_t seq) {
    if (g_nmgr.n_cancel < FNM_MAX) g_nmgr.cancel[g_nmgr.n_cancel++] = seq;
    return 0;
}

void nmgr_send_raw(uint8_t dst, uint8_t type, const uint8_t *payload, uint8_t len) {
    record(g_nmgr.raw, &g_nmgr.n_raw, dst, type, payload, len, 0);
}
