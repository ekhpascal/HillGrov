#pragma once
#include <stdint.h>
#include "hg_cfg_types.h"
int  hg_store_init(void);          /* nvs_flash_init (erase-retry once), load zid/hw/cfg -> hg_model_boot_load */
void hg_store_start(void);         /* store task: core 0, prio 2, 3072, TWDT; wakes on flush sem or 1000 ms */
int  hg_store_flush(uint32_t timeout_ms);   /* force a pass; 0 when all dirty bits written */
uint8_t hg_store_zid(void);
int  hg_store_set_zid(uint8_t id);
int  hg_store_factory_reset(void); /* erase namespace "hg", esp_restart(); returns only on failure */
