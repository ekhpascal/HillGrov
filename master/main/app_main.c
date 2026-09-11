#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "nvs_flash.h"
#include "board.h"
#include "notify.h"
#include "cmd_task.h"
#include "cmd_common.h"
#include "cli.h"
#include "esp_timer.h"
#include "ota_trial.h"
#include "app_if_common.h"
#include "ring_link.h"
#include "node_mgr.h"
#include "wifi_mgr.h"
#include "fw_srv.h"
#include "http_srv.h"
#include "mcfg_store.h"
#include "time_svc.h"
#include "alarm_mgr.h"

static const char *TAG = "hg_main";
extern const app_if_t APP_IF_MASTER;
extern const cmd_entry_t *master_table(int *n);
static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }
static uint8_t  master_id_fn(void) { return 0; }   /* RING_ID_MASTER -- the master's ring id never changes */

void app_main(void) {
    ESP_LOGI(TAG, "HillGrow master %s boot", esp_app_get_description()->version);

    notify_init(now_ms, 0);
    /* The web UI's alarm view is fed by a NOTIFY sink, so it has to be
     * registered before anything can emit -- the very first NOTIFY line of the
     * boot (NTF_BOOT, at the end of this function) belongs in the ring too. */
    alarm_mgr_init(hg_app_uptime_s);
    if (notify_add_sink(alarm_mgr_sink, NULL, NTF_MASK_ALL) < 0)
        ESP_LOGE(TAG, "alarm sink registration failed -- /api/alarms will stay empty");
    cmd_common_init(&APP_IF_MASTER);
    int n;
    static cmd_core_t core;
    core.table = master_table(&n); core.table_len = n;
    core.role = CMD_ROLE_MASTER; core.zone_id = 0;
    core.now_ms = now_ms;
    core.debug_key = CONFIG_HILLGROW_DEBUG_KEY;
    /* node_mgr_start() runs later in this function, but node_mgr_forward has
     * its own boot guard (node_mgr_fwd.c: RING_DOWN until node_mgr_start()
     * has run) -- setting it here alongside the other core fields, before
     * cmd_task_start(), is safe and keeps this block in one place. */
    core.forward = node_mgr_forward;
    cmd_task_start(&core);
    cli_init();
    cli_start();

    /* ruling #1: erase-retry-once (same pattern as hg_store_init/rescue),
     * BEFORE node_store/ring start -- ztab and the OTA-trial breadcrumb both
     * need the "hg" NVS namespace to exist. */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "nvs partition needs erase (%s), retrying once", esp_err_to_name(err));
        err = nvs_flash_erase();
        if (err == ESP_OK) err = nvs_flash_init();
    }
    if (err != ESP_OK) ESP_LOGE(TAG, "nvs_flash_init failed: %s", esp_err_to_name(err));

    if (mcfg_store_init() != 0) ESP_LOGW(TAG, "mcfg defaults in use");

    /* Before http_srv_start(), and deliberately not only from it: on a boot
     * where the radio never comes up the server is never started, and a
     * console SET WEB PASSWORD still has to be able to hash a password (it
     * shares http_srv's wa_state_t). Idempotent -- http_srv_start() calls it
     * again. */
    if (http_auth_init() != 0) ESP_LOGE(TAG, "web auth init failed -- web password changes will fail");

    ota_trial_start(1);

    /* Task 15 ruling #7: AP -> httpd -> ring -> node_mgr -> trial
     * drivers_ok, in that order. wifi_mgr_start()/http_srv_start() failures
     * are logged and never abort boot (never-abort rule: greenhouse control
     * still needs ring_link/node_mgr regardless of Wi-Fi/web availability
     * this boot) -- they only gate whether drivers_ok fires, per ruling #1
     * ("do NOT call ota_trial_drivers_ok on failure"). */
    int ap_ok = wifi_mgr_start() == 0;
    if (!ap_ok) ESP_LOGE(TAG, "wifi_mgr_start failed -- AP/STA/web unavailable this boot");

    /* Flash-only work (header + crc32 over the zone image), so it no longer
     * depends on the radio: fw_srv_image_ok() is what the fleet sequencer's
     * PRECHECK reads, and it should be truthful even on a boot with no Wi-Fi.
     * The matching GET /fw/zone.bin handler is registered by http_srv_start()
     * below, on the one shared httpd instance. */
    if (fw_srv_validate() != 0) ESP_LOGE(TAG, "fw_srv_validate failed -- fleet OTA unavailable this boot");

    /* time_svc_start()'s esp_sntp_* setup calls are lwIP APIs that assert
     * ("Invalid mbox") if called before lwIP's tcpip task exists -- that
     * task is created by esp_netif_init(), the first thing wifi_mgr_start()
     * does above, so this must run after it (found on the bench: an
     * earlier placement right after mcfg_store_init() crash-looped the
     * master on every boot, never reaching the CLI prompt). SNTP itself
     * only runs while the STA is up, which is what the registration below
     * wires: wifi_mgr calls time_svc_sta_changed() on every STA edge, and
     * wifi_mgr_on_sta() replays a join that already happened during the few
     * milliseconds between wifi_mgr_start() and here. */
    time_svc_start();
    wifi_mgr_on_sta(time_svc_sta_changed);

    /* One esp_http_server for the web UI, /api/cmd and the fleet pull. It has
     * to come after time_svc_start(): http_auth's session expiry reads the
     * clock quality time_svc owns. &core outlives the server (it is static
     * above) -- /api/cmd and /api/help run against it. */
    int http_ok = ap_ok && http_srv_start(&core) == 0;
    if (ap_ok && !http_ok) ESP_LOGE(TAG, "http_srv_start failed -- web UI unavailable this boot");

    uint8_t mac[6];
    hg_app_get_mac(mac);
    ring_link_start(1, master_id_fn, mac);
    node_mgr_start();

    /* spec 3.10 drivers criterion (master): AP netif + httpd both up,
     * checked once here -- Task 10's plan-sequenced obligation this task
     * completes. The STA is deliberately NOT part of the criterion: a house
     * Wi-Fi that is merely absent must never roll a good image back. Neither
     * is the zone image: a master with an empty zone_fw partition is a
     * perfectly healthy master. */
    if (http_ok) ota_trial_drivers_ok();

    notify_emit(NTF_BOOT, 0, "%s %s", esp_app_get_description()->version, hg_app_reset_reason());
    vTaskDelete(NULL);
}
