#include "cmd_core.h"
#include "ota_trial.h"

/* ---- SET OTA CONFIRM -- operator override that ends an active trial early ---- */

static int h_confirm(cmd_req_t *q, char *r, int l) {
    (void)q;
    if (ota_trial_confirm() != 0) return cmd_err(r, l, "NOT_READY");
    return cmd_okf(r, l, "OTA CONFIRM");
}

static const cmd_arg_t A_CONF[] = { { "confirm", ARG_ENUM, 0, 0, "CONFIRM" } };

const cmd_entry_t OTA_TRIAL_ROWS[] = {
  { CMDV_SET, CMD_AREA_FW, "OTA", NULL, A_CONF, 0, 1, 1, 0, h_confirm, NULL },
};
const int OTA_TRIAL_ROWS_N = (int)(sizeof OTA_TRIAL_ROWS / sizeof OTA_TRIAL_ROWS[0]);
