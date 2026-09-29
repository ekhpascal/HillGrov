/* CLEAR PANEL CONFIRM -- the recovery that needs no touch. If a panel preference makes the glass unusable (above all a
 * Flipped orientation whose touch mapping turns out wrong on real glass), this erases the panel's one NVS record,
 * "panel"/"prefs" (pnl_prefs_nvs.c), so the next boot comes up on the defaults: Normal orientation, default brightness
 * and dimming. It does not touch the live preferences -- the LVGL task owns them -- so it answers "reboot to apply":
 * send REBOOT CONFIRM next. A Panel-screen change made before that reboot saves the live values again.
 * Runs on the command task (UART CLI or a panel console session): an NVS erase, never an lv_* call. */
#include "nvs.h"
#include "cmd_core.h"
#include "pnl_cli.h"

static int h_clear_panel(cmd_req_t *q, char *r, int l) {
    (void)q;
    nvs_handle_t h;
    esp_err_t e = nvs_open("panel", NVS_READWRITE, &h);
    if (e == ESP_OK) {
        e = nvs_erase_key(h, "prefs");
        if (e == ESP_OK) e = nvs_commit(h);
        else if (e == ESP_ERR_NVS_NOT_FOUND) e = ESP_OK;   /* nothing stored: the defaults are already what boots */
        nvs_close(h);
    }
    if (e != ESP_OK) return cmd_err(r, l, "NVS_WRITE");    /* e.g. the recovery design's 6.5 "writes disabled" */
    return cmd_okf(r, l, "PANEL CLEARED REBOOT TO APPLY");
}

static const cmd_arg_t A_CONF[] = { { "confirm", ARG_ENUM, 0, 0, "CONFIRM" } };

const cmd_entry_t PANEL_CMD_ROWS[] = {
  { CMDV_BARE, CMD_AREA_SYSTEM, "CLEAR", "PANEL", A_CONF, 0, 1, 1, CMDF_MASTER | CMDF_SLOW, h_clear_panel,
    "panel prefs to defaults; reboot to apply" },
};
const int PANEL_CMD_ROWS_N = (int)(sizeof PANEL_CMD_ROWS / sizeof PANEL_CMD_ROWS[0]);
