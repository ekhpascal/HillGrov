#pragma once
#include <stdlib.h>
#include "FreeRTOS.h"

/* A single-threaded fake mutex. Host tests never call mcfg_ops_lock()/
 * mcfg_ops_edit() from two threads at once, so a real timed block would only
 * ever burn wall-clock time waiting for a release that can never come -- the
 * `ticks` argument is accepted (so call sites need no #ifdef) but not
 * actually waited on; "already held" fails immediately instead of after the
 * caller's timeout, which is the same observable answer (pdFALSE) a real
 * mutex gives once nothing else can ever xSemaphoreGive() it. */
typedef struct { int taken; } fake_sem_t;
typedef fake_sem_t *SemaphoreHandle_t;

static inline SemaphoreHandle_t xSemaphoreCreateMutex(void) {
    return (SemaphoreHandle_t)calloc(1, sizeof(fake_sem_t));
}

static inline int xSemaphoreTake(SemaphoreHandle_t sem, TickType_t ticks) {
    (void)ticks;
    if (!sem || sem->taken) return pdFALSE;
    sem->taken = 1;
    return pdTRUE;
}

static inline int xSemaphoreGive(SemaphoreHandle_t sem) {
    if (!sem) return pdFALSE;
    sem->taken = 0;
    return pdTRUE;
}
