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

/* Is the image this master is running still on OTA trial -- i.e. is the
 * running slot ESP_OTA_IMG_PENDING_VERIFY, the state the bootloader hands an
 * OTA-updated app before it self-confirms?
 *
 * ONE implementation, deliberately: it is read both by the co-processor-OTA
 * gate at the end of app_main() (which is what must not run during a trial)
 * and by cp_ota_restart_for_new_radio()'s own defence-in-depth check, and two
 * copies of this test would eventually disagree.
 *
 * This is the same query ota_trial_start() itself uses to decide whether a
 * trial is running at all (components/ota_trial/ota_trial.c), so the two agree
 * by construction rather than by coincidence. ota_trial exposes no read-only
 * predicate to call instead, and ota_trial_confirm() is emphatically NOT one:
 * it *confirms* the trial (operator override) rather than reporting it, so
 * calling it here would short-circuit the very dwell period this protects.
 *
 * Every state the running slot can actually be observed in, and why "not on
 * trial" is the safe answer for all the others:
 *   PENDING_VERIFY  the trial -- the one state that must gate
 *   VALID           the trial already passed; there is nothing left to protect
 *   UNDEFINED       the normal bench state, what tools/hg_otadata.py writes
 *   NEW             unreachable at runtime: the bootloader rewrites NEW ->
 *                   PENDING_VERIFY before it hands over
 *   factory / any non-OTA running partition (the rescue app, or a board booted
 *                   from factory) -- esp_ota_get_state_partition() returns
 *                   ESP_ERR_NOT_SUPPORTED, the "== ESP_OK" conjunct fails and
 *                   this reports 0. Correct: the bootloader's rollback logic
 *                   only ever inspects otadata entries for OTA slots, so a
 *                   factory boot has no unconfirmed image to vote against.
 * A query that fails for any other reason lands in the same place, and that is
 * the safe direction rather than a shrug: reporting "no trial" only re-enables
 * work that is unconditionally fine whenever there really is no trial, which
 * is what every failing case above actually means. `state` is pre-initialised
 * but never read after a failed call (short-circuit &&). */
static int running_slot_on_ota_trial(void) {
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    return running && esp_ota_get_state_partition(running, &state) == ESP_OK &&
           state == ESP_OTA_IMG_PENDING_VERIFY;
}

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
 * does not merely re-run the trial: the bootloader rewrites EVERY
 * PENDING_VERIFY otadata entry to ESP_OTA_IMG_ABORTED before it selects a
 * partition, and an ABORTED slot is never booted again until a fresh OTA
 * rewrites otadata -- so a demonstrably healthy master image is retired, not
 * just bounced.
 *
 * That check now lives at the CALL SITE as well, above cp_ota_sync() itself
 * (R1 -- see the block at the call site for why gating the push and not the
 * restart is the correct placement). The copy below is therefore DEFENCE IN
 * DEPTH and is unreachable through today's only call path: PENDING_VERIFY can
 * only be entered at boot, so it cannot appear between that gate and this
 * function. It is kept rather than deleted because cp_ota_sync() has exactly
 * one caller *today* and the obvious next feature -- an operator-triggered CP
 * update from the console or the web UI -- would add a second one that has no
 * reason to know any of this. Both tests read running_slot_on_ota_trial()
 * above, so a future second caller inherits the guard and the two can never
 * drift. */
static void cp_ota_restart_for_new_radio(void) {
    if (running_slot_on_ota_trial()) {
        ESP_LOGE(TAG, "co-processor updated, but this boot is still an OTA trial "
                      "(PENDING_VERIFY) -- NOT restarting, because that would retire "
                      "this master image (the bootloader marks a PENDING_VERIFY slot "
                      "ABORTED and never boots it again). Wi-Fi/web stay down for this "
                      "boot. To recover WITHOUT losing this image, issue SET OTA CONFIRM "
                      "on this console and then reboot -- that marks the slot valid "
                      "within a second whatever the trial criteria say, which is the one "
                      "action that always works here. Do NOT wait for TRIAL PASS: if the "
                      "radio's old firmware is what kept the AP down, drivers_ok was "
                      "never latched this boot and TRIAL PASS can never be reported");
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
     *    SNTP. With this call anywhere earlier, NTF_BOOT -- the first NOTIFY
     *    of the boot by design -- sat behind up to ~45 s of cp_ota_sync() AND
     *    behind the radio dropping, so the web alarm view could miss it
     *    entirely. Running last puts the boot NOTIFY out before the radio can
     *    go, which is worth having on its own.
     *
     * WHY THE WHOLE CALL IS GATED ON THE OTA TRIAL (R1). This SUPERSEDES the
     * previous round's argument, which gated only the restart inside
     * cp_ota_restart_for_new_radio(); do not "simplify" it back to that.
     *
     * The previous round was right that a master restart must be suppressed
     * while the running slot is PENDING_VERIFY -- restarting then retires the
     * slot (ABORTED, never booted again), not merely bounces the trial. But
     * that guards the one door THIS translation unit owns. esp_hosted owns a
     * second door into the same room and it is open by its own default:
     * CONFIG_ESP_HOSTED_HOST_TRANSPORT_RESTART_ON_FAILURE=y, which nothing in
     * master/sdkconfig.defaults.esp32p4 pins shut. On an unrecoverable SDIO
     * failure the transport calls eh_host_port_restart_host(), and that
     * function is a bare abort() -- a panic reset, deliberately not
     * esp_restart() (esp_hosted 3.0.7,
     * port/os/idf/src/eh_host_port_power.c). A CP push DELIBERATELY reboots
     * the C6 mid-session, i.e. deliberately creates the very SDIO outage that
     * fires it, while the AP, httpd, mDNS and SNTP started above are still
     * driving host->C6 traffic across that link. On a trial boot that panic
     * is a crash before mark-valid: exactly the same rollback vote the
     * suppression exists to prevent, cast through a path no check in app_main
     * can see or veto.
     *
     * So the gate belongs above the PUSH, not above the restart. During a
     * trial the radio is then never torn down at all, so neither door can be
     * reached: not this function's esp_restart(), and not esp_hosted's
     * abort(). (That closes the doors a CP OTA opens, which is what this is
     * about. A genuine crash or TWDT reboot in unrelated code is still a
     * rollback vote, exactly as ota_trial.h documents -- the trial's whole
     * point is that it should be.)
     *
     * Skipping the push is safe, not a problem deferred, and THAT is what
     * makes a skip the right answer rather than something to force through:
     * the push is version-gated and idempotent, and this gate OPENS BY
     * ITSELF. A passed trial marks the slot VALID, so the very next boot
     * performs the update with no operator action at all. The log below says
     * so out loud, because an operator reading only the console must not
     * conclude the radio needs rescuing. The skip also costs nothing the
     * previous round did not already cost: suppressing the restart left the
     * radio's freshly-pushed firmware bound to nothing for that boot anyway,
     * whereas deferring the push leaves the radio UNCHANGED and Wi-Fi/web up
     * for the whole trial. It matches IDF's own convention, too --
     * esp_ota_begin() refuses to start an update while the running app is
     * PENDING_VERIFY (ESP_ERR_OTA_ROLLBACK_INVALID_STATE).
     *
     * And it is what finally makes the drivers_ok claim true rather than
     * merely nearly true (final-review compounding effect (1)). Moving this
     * call last fixed the ORDER OF THE LATCH -- ota_trial_drivers_ok() above
     * now records a stack that existed when it was observed -- but not the
     * verdict: ota_trial renders that from its 1 Hz timer, which on a bench
     * trial cannot return TRIAL_PASS before start + TRIAL_BENCH_WINDOW_MS
     * (60 s), while a push starts at ~T+2-5 s and runs 15-45 s. So with the
     * push allowed the radio went down at ~T+10-50 s and the mark-valid still
     * landed at ~T+60 s, AFTER the teardown. Now that no push can run during
     * a trial there is no teardown for the verdict to land after, so the trial
     * really does judge a stack this boot sequence has not torn down. (Stated
     * that precisely on purpose: an operator can still restart the radio out
     * from under a trial with a console SET WIFI, and the claim this replaces
     * was flagged for asserting more than the code delivered.)
     *
     * cp_ota_sync() logs its own outcome at the right level for each case
     * (INFO/WARN/ERROR) -- deliberately no summary log here: an earlier
     * version of this line always logged "up to date" at WARN, including on
     * the ESP32 master (whose cp_ota_sync() is a stub that always returns 0),
     * which was false every single boot on hardware that has no co-processor
     * at all. The skip log below is guarded on CONFIG_IDF_TARGET_ESP32P4 for
     * exactly that reason: the ESP32 master has no co-processor and no cp_fw
     * partition at all (master/partitions.csv), so there the line would be a
     * new instance of that same false claim, on every OTA-trial boot. The
     * guard is on the LOG and not on the branch deliberately -- an #if around
     * the else-if would leave cp_ota_restart_for_new_radio() unreferenced on
     * the ESP32 build, i.e. an unused-static-function warning, and skipping a
     * stub that returns 0 is behaviourally identical to calling it.
     *
     * Return 1 (pushed AND confirmed) is the one outcome that needs something
     * from this function -- see cp_ota_restart_for_new_radio(). */
    if (running_slot_on_ota_trial()) {
#if CONFIG_IDF_TARGET_ESP32P4
        ESP_LOGW(TAG, "this boot is an OTA trial (PENDING_VERIFY) -- SKIPPING the "
                      "co-processor firmware check for this boot, so a CP push cannot "
                      "tear the radio down mid-trial. The radio was NOT updated; whether "
                      "it even needed updating is part of what was skipped. Wi-Fi/web "
                      "stay up on the firmware it already has. Nothing to do by hand: "
                      "this is retried AUTOMATICALLY on the next boot, because a passed "
                      "trial marks this slot VALID and the gate then opens. If the trial "
                      "cannot pass because the radio's OLD firmware is what keeps the AP "
                      "down, issue SET OTA CONFIRM on this console and reboot -- the slot "
                      "is VALID from then on and that boot performs the update");
#endif
    } else if (cp_ota_sync() == 1) {
        cp_ota_restart_for_new_radio();
    }

    vTaskDelete(NULL);
}
