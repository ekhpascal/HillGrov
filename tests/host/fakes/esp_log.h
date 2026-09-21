#pragma once
/* Host-only stand-in for ESP-IDF's esp_log.h. mcfg_ops.c (moved verbatim out
 * of master/main/net_ops_master.c, Task 5) is compiled for real into the host
 * test -- only what is underneath it (the FreeRTOS mutex, the mcfg store) is
 * faked -- so its ESP_LOGx call sites still need to compile. Host tests have
 * no console worth logging to, so these swallow every argument. */
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGD(...) ((void)0)
#define ESP_LOGV(...) ((void)0)
