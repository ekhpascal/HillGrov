#pragma once
#include <stddef.h>
#include <stdint.h>
#include "psvc_rc.h"

#ifdef __cplusplus
extern "C" {
#endif

/* THE RULE (spec "The rule"): the LVGL task never makes a blocking call. Every
 * write and every command is a job: run() executes on the pnl_work task (may
 * block: mutexes, NVS, esp_hosted RPCs, ring round trips; never calls lv_*),
 * then done() executes on the LVGL task from a 20 ms drain timer (may call
 * lv_*; must be short). The worker never takes the display lock -- results
 * come back through a queue, the "mailbox".
 *
 * Thread tags used across panel_ui: [LVGL] only on the LVGL task (or under
 * panel_lock()); [WORKER] only on pnl_work / pnl_poll / pnl_wifi; [ANY] safe
 * everywhere. */

typedef struct pnl_job pnl_job_t;
typedef void (*pnl_job_run_fn)(pnl_job_t *j);    /* runs on pnl_work: may block; never calls lv_* */
typedef void (*pnl_job_done_fn)(pnl_job_t *j);   /* runs on the LVGL task (the drain timer); may call lv_* */

#define PNL_JOB_ARG_MAX 160   /* by value: ssid 33 + pass 65, TZ 48, a password 64, a SET TIME line; bigger payloads
                                 (console line + reply, edit sets, doc buffers) go by pointer to module-owned storage */
#define PNL_JOB_OUT_MAX 256
#define PNL_JOB_POOL    8     /* the pool wipes every job's arg/out/err (pnl_zero) after done() returns -- secrets never linger */

struct pnl_job {
    pnl_job_run_fn  run;
    pnl_job_done_fn done;
    uint32_t        screen_gen;          /* pnl_screen_gen() at submit; done() must not touch widgets if it changed */
    uint32_t        t_submit_ms, t_done_ms;
    psvc_rc_t       rc;                  /* psvc_* results */
    int             irc;                 /* int-returning results (pnl_cmd_run, pnl_sd_mount) */
    char            err[96];             /* path / detail */
    uint8_t         arg[PNL_JOB_ARG_MAX];/* copied at submit */
    uint8_t         out[PNL_JOB_OUT_MAX];/* run() -> done() */
};

/* Task pnl_work (core 0, prio 2, 8192 B internal stack, NOT TWDT-subscribed),
 * the job pool, the request and done queues, and the 20 ms drain lv_timer
 * (created under panel_lock(2000)). Idempotent. Call from
 * panel_services_start(). */
void     pnl_worker_start(void);

/* [LVGL] 0 queued / -1 pool full, arg_len > PNL_JOB_ARG_MAX, run NULL or the
 * worker not started. arg is copied by value. Any buffer referenced BY POINTER
 * inside arg belongs to the worker from submit until done() returns -- the
 * submitter must not touch it meanwhile. */
int      pnl_worker_submit(pnl_job_run_fn run, pnl_job_done_fn done, const void *arg, size_t arg_len);

int      pnl_worker_pending(void);  /* [ANY] jobs queued or running (or run, awaiting done()) */

/* [ANY] Smallest free stack pnl_work has had, in bytes (ESP-IDF's
 * uxTaskGetStackHighWaterMark counts bytes); 0 before the task exists. The
 * stack is chosen, not measured: the Stage 3 and 4 gates record this. */
uint32_t pnl_worker_stack_free(void);

uint32_t pnl_screen_gen(void);      /* [ANY] */
void     pnl_screen_gen_bump(void); /* [LVGL] called by the shell on every teardown */

/* [ANY] 1 when the calling task is the LVGL adapter's (PANEL_LVGL_TASK_NAME,
 * panel_hw.h). Every [WORKER] function in panel_ui checks it first, logs ERROR
 * and refuses (-1) when it is 1: the tripwire for THE RULE. */
int      pnl_on_lvgl_task(void);

#ifdef __cplusplus
}
#endif
