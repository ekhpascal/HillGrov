#include "mcfg_store.h"

/* Host fake of components/hg_mcfg/mcfg_store.c's RAM+NVS layer. The real file
 * is not host-testable (nvs_flash.h/esp_err.h/esp_log.h are ESP-IDF-only) and
 * is excluded from the host build entirely -- same shape as fake_nmgr.c
 * standing in for node_mgr.c's real, hardware-touching implementation of the
 * node_mgr_internal.h seam. Validation is the one piece that must stay REAL
 * (test_mcfg_ops.c's hostnames have to actually pass hg_mcfg_validate()), so
 * this calls straight through to it; only the NVS durability step is left
 * out, since nothing under test reads it back from a fresh process.
 *
 * Lazily defaulted on first touch rather than requiring an explicit
 * mcfg_store_init() call: test_mcfg_ops.c only ever calls mcfg_ops_init(),
 * exactly like production boot (app_main.c calls mcfg_store_init() before
 * mcfg_ops_init(), so by the time any real caller reaches here the store is
 * already initialised) -- this just stands in for that ordering guarantee. */
static hg_mcfg_t s_cfg;
static uint32_t  s_gen;
static int       s_ready;
static hg_tz_check_fn s_tzck;

static void ensure_ready(void) {
    if (s_ready) return;
    hg_mcfg_defaults(&s_cfg);
    s_gen = 0;
    s_ready = 1;
}

int mcfg_store_init(void) {
    hg_mcfg_defaults(&s_cfg);
    s_gen = 0;
    s_ready = 1;
    return 0;
}

const hg_mcfg_t *mcfg_get(void) {
    ensure_ready();
    return &s_cfg;
}

uint32_t mcfg_gen(void) {
    ensure_ready();
    return s_gen;
}

void mcfg_store_set_tz_check(hg_tz_check_fn fn) {
    s_tzck = fn;
}

int mcfg_commit(const hg_mcfg_t *m) {
    ensure_ready();
    if (!m) return -1;
    char err[48];
    if (hg_mcfg_validate(m, s_tzck, err, sizeof err) != 0) return -1;
    s_cfg = *m;
    s_gen++;
    return 0;
}
