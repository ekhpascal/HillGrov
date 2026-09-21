#pragma once
/* Host-only stand-in for ESP-IDF's freertos/FreeRTOS.h. components/mcfg_ops's
 * mcfg_ops.c is moved verbatim out of master/main/net_ops_master.c (Task 5)
 * and touches the raw FreeRTOS semaphore API directly rather than hiding it
 * behind its own seam -- unlike node_mgr, whose decision-logic files
 * (compiled for host in test_node_mgr_cfg.c) reach FreeRTOS only through
 * nmgr_lock()/nmgr_unlock(), which fakes/fake_nmgr.c replaces outright. There
 * is no such seam here to fake instead, so this header (plus semphr.h) is it:
 * just enough for mcfg_ops.c to link and run single-threaded on the host. */
#include <stdint.h>

typedef uint32_t TickType_t;
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#define pdTRUE  1
#define pdFALSE 0
