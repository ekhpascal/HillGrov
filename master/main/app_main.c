#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"   /* esp_restart() -- cp_ota_restart_for_new_radio() below */
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
#include "cp_ota.h"
#include "http_srv.h"
#include "mcfg_store.h"
#include "mcfg_ops.h"
#include "time_svc.h"
#include "alarm_mgr.h"

static const char *TAG = "hg_main";
extern const app_if_t APP_IF_MASTER;
extern const cmd_entry_t *master_table(int *n);
static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }
static uint8_t  master_id_fn(void) { return 0; }   /* RING_ID_MASTER -- the master's ring id never changes */

/* Final-review F3: the ONE case where the master deliberately reboots itself
 * after a co-processor OTA. cp_ota_sync() returns 1 only when it re-read the
 * C6's reported version after the C6 rebooted into the new image and
 * cp_ota_needed() now accepts it -- i.e. the radio is CONFIRMED running the
 * new firmware. But the C6 *is* this master's radio: wifi_mgr's AP and STA,
 * mDNS, SNTP and the live esp_http_server are all still bound to the
 * pre-reboot radio, wifi_mgr registers no co-processor-reinit handler and
 * nothing calls wifi_mgr_apply() afterwards, so without this the one boot
 * that actually performs a CP update ends with no AP and no web UI until
 * somebody power-cycles the board.
 *
 * This SUPERSEDES the earlier blanket ruling (recorded in cp_ota.h) that the
 * master must never auto-reboot after a CP OTA -- for the CONFIRMED case
 * only, and that narrowing is the entire reason it is safe: a confirmed match
 * means cp_ota_needed() returns 0 on the very next boot, so the update cannot
 * repeat and this cannot loop. Do NOT extend it to cp_ota_sync()'s -2
 * (pushed but not confirmed) return -- that one CAN repeat, so it stays a
 * loud log only, which is exactly what the original ruling was protecting.
 * The reboot lives here rather than inside cp_ota_sync() because it is boot
 * policy, not a property of the push: cp_ota is discovered (and compiled) by
 * the zone, rescue and host-test builds too, and its requirements graph is
 * deliberately down to esp_partition + hg_blob -- see that component's
 * CMakeLists.txt for what this project has already lost to widening it.
 *
 * The one state that must NOT be rebooted through is an OTA trial.
 * CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y (master/sdkconfig.defaults) and
 * ota_trial only calls esp_ota_mark_app_valid_cancel_rollback() from its 1 s
 * timer once the dwell window has elapsed (TRIAL_BENCH_WINDOW_MS is 60 s --
 * longer than a whole cp_ota_sync() push). Restarting a PENDING_VERIFY slot
 * hands the bootloader a still-unconfirmed image and it rolls a demonstrably
 * healthy master BACK, which is strictly worse than the missing AP this
 * reboot exists to fix -- and the master-self-OTA-plus-new-radio boot is
 * precisely the case the review flagged as the natural one. So on a trial
 * boot this logs and returns: the trial confirms on its own timer and the
 * operator reboots. */
static void cp_ota_restart_for_new_radio(void) {
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    if (running && esp_ota_get_state_partition(running, &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGE(TAG, "co-processor updated, but this boot is still an OTA trial "
                      "(PENDING_VERIFY) -- NOT restarting, because that would roll this "
                      "master image back. Wi-Fi/web stay down for this boot; reboot the "
                      "master once TRIAL PASS is reported");
        return;
    }
    ESP_LOGW(TAG, "co-processor firmware was updated and confirmed -- restarting the "
                  "master so the Wi-Fi stack rebinds to the new radio firmware (cannot "
                  "loop: the version gate makes cp_ota_sync() a no-op on the next boot)");
    vTaskDelay(pdMS_TO_TICKS(200));   /* let that line drain to the console first */
    esp_restart();
}

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

    /* Must run before cmd_task_start()/cli_start(): those hand the console
     * to a task that can immediately run a NET/TIME/WEB row, and the mutex
     * has to exist by then. A fix-round regression once placed this call
     * down by mcfg_store_init() instead, after both of the below -- nothing
     * on this bench caught it, because mcfg_ops.c's internal lock_take()
     * used to report SUCCESS on a missing mutex (an unsynchronised write,
     * not a rejected one). lock_take() now fails closed instead (mcfg_ops.c),
     * so that class of bug can no longer hide, but the mutex still belongs
     * here, ahead of the first task that could reach it. */
    mcfg_ops_init();
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

    /* Task 6: co-processor OTA, gated on version so a healthy C6 is never
     * re-flashed on every boot (the bring-up spike's bug). Placement, with
     * every reason that put it here, oldest first -- all three still hold:
     *
     *  - AFTER wifi_mgr_start()/http_srv_start(): a working AP is what proves
     *    the esp_hosted RPC path this pushes over is actually alive.
     *  - AFTER ring_link_start()/node_mgr_start() (fix round 2, Major 4): the
     *    ring is this device's core function and the radio is not, and the
     *    ring must not wait behind an eh_host_cp_ota_begin() that alone can
     *    block up to 30 s. cp_ota_sync()'s own internal link-up gate means an
     *    unresponsive C6 can no longer make the host commit to that begin()
     *    at all, but even a *quick* no-op return shouldn't sit ahead of the
     *    ring on principle.
     *  - LAST of everything (final-review F3): a *successful* CP OTA reboots
     *    the C6, i.e. the whole radio, underneath a live AP, httpd, mDNS and
     *    SNTP. With this call anywhere earlier, two things straddled that
     *    teardown. (1) The PENDING_VERIFY trial's drivers_ok criterion was
     *    latched at http_srv_start() and only *applied* after cp_ota_sync()
     *    had torn the radio down, so an image could self-confirm on an
     *    observation of a stack that no longer existed. (2) NTF_BOOT -- the
     *    first NOTIFY of the boot by design -- sat behind up to ~45 s of
     *    cp_ota_sync() AND behind the radio dropping, so the web alarm view
     *    could miss it entirely. Running last removes the interleaving
     *    instead of reasoning about it: the trial judges a stack nothing has
     *    torn down, and the boot NOTIFY is out before the radio can go.
     *
     * cp_ota_sync() logs its own outcome at the right level for each case
     * (INFO/WARN/ERROR) -- deliberately no summary log here: an earlier
     * version of this line always logged "up to date" at WARN, including on
     * the ESP32 master (whose cp_ota_sync() is a stub that always returns 0),
     * which was false every single boot on hardware that has no co-processor
     * at all. Return 1 (pushed AND confirmed) is the one outcome that needs
     * something from this function -- see cp_ota_restart_for_new_radio(). */
    if (cp_ota_sync() == 1) cp_ota_restart_for_new_radio();

    vTaskDelete(NULL);
}
