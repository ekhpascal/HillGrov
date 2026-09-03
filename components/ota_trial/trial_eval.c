#include "ota_trial.h"

int trial_eval(const trial_probes_t *p, uint8_t expect_link, uint32_t start_ms, uint32_t now_ms) {
    if (p->confirmed) return TRIAL_PASS;

    uint8_t base = p->cfg_loaded && p->drivers_ok && p->ticks >= TRIAL_MIN_TICKS &&
                   p->twdt_ok && p->min_heap_kb > TRIAL_MIN_HEAP_KB;
    uint32_t elapsed = now_ms - start_ms;

    if (base) {
        if (expect_link) {
            if (p->master_frame_seen) return TRIAL_PASS;
        } else if (elapsed >= TRIAL_BENCH_WINDOW_MS) {
            return TRIAL_PASS;
        }
    }

    /* Bench (expect_link == 0) deliberately never times out here: a bench board
     * with base criteria unmet stays PENDING forever rather than rolling back --
     * only a crash/TWDT reboot (which never reaches mark-valid at all) or an
     * operator's SET OTA CONFIRM ends a stuck bench trial. */
    if (expect_link && elapsed >= TRIAL_FLEET_WINDOW_MS) return TRIAL_FAIL;

    return TRIAL_PENDING;
}
