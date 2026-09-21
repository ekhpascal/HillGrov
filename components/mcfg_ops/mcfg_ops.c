#include "mcfg_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "mcfg_store.h"

static const char *TAG = "mcfg_ops";

/* Moved out of master/main/net_ops_master.c verbatim (Task 5): that file was
 * an *app*, so components/http_srv could only reach its lock through extern
 * declarations -- awkward, and something the panel UI (next plan) would have
 * had to repeat. Every caller of the master-config read-modify-write now
 * takes this SAME lock instead of growing its own copy.
 *
 * mcfg_commit() serializes commits against each other, but NOT the
 * read-modify-write around them: two ops running concurrently (httpd workers
 * racing the CLI, or each other) would both snapshot the same mcfg and the
 * second commit would silently drop the first one's field. This mutex makes
 * each snapshot->modify->commit atomic. Created by mcfg_ops_init(), which
 * must run once at boot before any other entry point here -- so no lazy-init
 * race. */
static SemaphoreHandle_t s_lock;

static int lock_take(void) {
    if (!s_lock) return 1;   /* pre-init, i.e. still single-threaded boot */
    /* Longer than mcfg_commit()'s own 5000 ms mutex timeout, so a caller that
     * loses this race reports the commit's verdict rather than ours. */
    return xSemaphoreTake(s_lock, pdMS_TO_TICKS(6000)) == pdTRUE;
}

static void lock_give(void) { if (s_lock) xSemaphoreGive(s_lock); }

void mcfg_ops_init(void) {
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        if (!s_lock) ESP_LOGE(TAG, "mcfg ops mutex unavailable -- config writes are unserialized");
    }
}

/* Not for use around an mcfg_ops_edit() call: that already takes this lock
 * internally, and it is not recursive. This is for a caller (GET
 * /api/wifi/scan's ~2 s blocking radio scan) that needs to hold the lock
 * across something that is not itself a snapshot->modify->commit. */
int mcfg_ops_lock(uint32_t ms) {
    if (!s_lock) return -1;   /* pre-init: mcfg_ops_init() has not run yet */
    return xSemaphoreTake(s_lock, pdMS_TO_TICKS(ms)) == pdTRUE ? 0 : -1;
}

void mcfg_ops_unlock(void) { lock_give(); }

/* mcfg_ops_edit()'s own rc convention (mcfg_ops.h): -1 lock unavailable, -2
 * commit failed. mcfg_commit() itself distinguishes a validation reject (-1)
 * from a storage failure (-2), but mcfg_ops_edit() cannot pass that through
 * unchanged -- -1 is already reserved for "lock unavailable" -- so both
 * collapse to -2 here. */
static int commit_and_log(hg_mcfg_t *m) {
    int rc = mcfg_commit(m);
    if (rc == 0) return 0;
    if (rc == -2) {
        ESP_LOGE(TAG, "mcfg_ops_edit: mcfg_commit failed to store (NVS/mutex)");
    } else {
        ESP_LOGW(TAG, "mcfg_ops_edit: rejected by mcfg validation");
    }
    return -2;
}

int mcfg_ops_edit(int (*fn)(hg_mcfg_t *m, void *ctx), void *ctx) {
    if (!lock_take()) return -1;
    hg_mcfg_t m = *mcfg_get();
    int rc = fn(&m, ctx);
    if (rc == 0) rc = commit_and_log(&m);
    lock_give();
    return rc;
}
