#include "esp_log.h"
#include "lvgl.h"
#include "panel_ui.h"

static const char *TAG = "panel";

/* Task 6 stub: proves the pinned LVGL is linked into the P4 master and that
 * app_main's two calls sit where the recovery design expects them. Task 7
 * replaces this with the real bring-up. */
int panel_start(void) {
    ESP_LOGI(TAG, "LVGL %d.%d.%d linked; display not started",
             LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH);
    return -1;
}

int panel_services_start(void) { return 0; }
