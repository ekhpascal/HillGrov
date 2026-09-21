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
 * each snapshot->modify->commit atomic. mcfg_ops_init() must run once at
 * boot before any other entry point here (app_main.c calls it before
 * cmd_task_start()/cli_start(), so no CLI/HTTP task can reach a writer
 * first) -- but lock_take() below fails CLOSED rather than trusting that,
 * because a fix-round regression put mcfg_ops_init() after cmd_task_start()
 * for one commit and nothing caught it: a "not created yet" that reports
 * success is a silent unsynchronised write, not a safe single-threaded
 * window. */
static SemaphoreHandle_t s_lock;

static int lock_take(void) {
    if (!s_lock) return 0;   /* fail CLOSED: not created yet is NOT an open lock */
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

/* mcfg_ops_edit()'s own rc convention (mcfg_ops.h): fn returns 0 to commit,
 * a POSITIVE value to refuse (returned unchanged). Every NEGATIVE return
 * belongs to this component: -1 lock unavailable, -2 mcfg_commit() failed on
 * storage (its own -2), -3 mcfg_commit() rejected the config as invalid
 * (its own -1). This split has to survive past this function: the CLI and
 * web surfaces map -2/-3 to different owner-visible errors (ERR STORAGE /
 * ERR INVALID) -- fix round 1 collapsed both into -2, which quietly told an
 * owner who typed a bad POSIX TZ that their storage had failed. what is the
 * caller's own label (e.g. "SET TZ"), logged instead of a generic one so
 * fix round 1's lost per-op log context is restored too. */
static int commit_and_log(hg_mcfg_t *m, const char *what) {
    int rc = mcfg_commit(m);
    if (rc == 0) return 0;
    if (rc == -2) {
        ESP_LOGE(TAG, "%s: mcfg_commit failed to store (NVS/mutex)", what);
        return -2;
    }
    ESP_LOGW(TAG, "%s: rejected by mcfg validation", what);
    return -3;
}

int mcfg_ops_edit(int (*fn)(hg_mcfg_t *m, void *ctx), void *ctx,
                   void (*apply)(void *ctx), const char *what) {
    if (!lock_take()) return -1;
    hg_mcfg_t m = *mcfg_get();
    int rc = fn(&m, ctx);
    if (rc == 0) {
        rc = commit_and_log(&m, what);
        /* STILL holding the lock here, on purpose: apply must run in the
         * same exclusion a caller like GET /api/wifi/scan holds mcfg_ops_lock()
         * across, or the radio reconfigure it triggers could land mid-scan. */
        if (rc == 0 && apply) apply(ctx);
    }
    lock_give();
    return rc;
}
