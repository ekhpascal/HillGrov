#pragma once
#include <stdint.h>
#include "app_if.h"

/* Canned/records state for the fake app_if_t used by test_cmd_common.
 * fake_app_if_reset() restores every field to its default before each test. */
typedef struct {
    char last_call[24];        /* name of the most recently invoked app_if function */

    int      log_calls;
    char     log_level[16];    /* level passed on the last log_set() call ("" if NULL) */
    char     log_tag[24];      /* tag passed on the last log_set() call ("" if NULL) */
    char     log_eff[16];      /* current effective level fake reports back */

    int      time_set_calls;
    int      ts_y, ts_mo, ts_d, ts_h, ts_mi, ts_s;

    int      fw_update_calls;
    char     fu_ssid[40];
    char     fu_pass[72];
    char     fu_url[104];

    int      reboot_calls;
    int      fw_rollback_calls;
    int      factory_reset_calls;

    int      fail_save;         /* save_flush() returns -1 when set */
    int      fail_fw_rollback;  /* fw_rollback() returns -1 when set */
    int      fail_fw_update;    /* fw_update() returns -1 when set */
    int      fail_factory_reset;/* factory_reset() returns -1 when set */
} fake_app_if_state_t;

extern fake_app_if_state_t g_fake_app;
extern const app_if_t      FAKE_APP_IF;

void fake_app_if_reset(void);
