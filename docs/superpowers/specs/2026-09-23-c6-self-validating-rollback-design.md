# C6 self-validating rollback — design

**Status:** approved in scope 2026-09-23 as the companion to `2026-09-23-p4-master-recovery-design.md`.
The owner approved the approach (C6 rollback, one H4 visit per board, no board modification) and the
split into its own spec. **The mechanism details below were not walked through section by section**;
they were derived by an adversarial review of the recovery design, corrected the same day by a source
verification pass, and should be reviewed as such.

## Why this exists

The master's phone-only recovery runs through the C6: the phone reaches the master by joining an AP,
and on the P4 the AP *is* the co-processor. So a bad C6 image cannot be fixed from a phone. On the
board as built the P4 also cannot reflash a C6 that has stopped talking (see the recovery spec,
"Findings"). The C6 therefore has to recover from a bad image **by itself**.

Today it cannot. The C6 has no rollback and no factory slot, so an activated image that never
completes its handshake leaves the master with no Wi-Fi until someone attaches a USB-UART adapter to
header H4.

## What the source establishes

- `coproc/partitions_eh_cp_ota_4m.csv` has otadata plus **two** OTA app slots of `0x1C0000` each, and
  no factory. Rollback is structurally possible.
- The co-processor side of `esp_hosted`'s CP OTA is **stock IDF** `esp_ota_begin/write/end` plus
  `esp_ota_set_boot_partition`, so standard IDF rollback semantics apply to pushed images. In
  particular, with rollback on, `esp_ota_begin()` refuses with `ESP_ERR_OTA_ROLLBACK_INVALID_STATE`
  while the running image is `PENDING_VERIFY` (`esp_ota_ops.c`). A C6 that is still validating cannot
  accept a second push.
- The P4 pulses GPIO54, the C6's EN, **on every host bring-up**
  (`CONFIG_ESP_HOSTED_HOST_CP_RESET_STRATEGY_ALWAYS`). The C6's software reset-pin ISR is compiled out
  (`CONFIG_EH_TRANSPORT_CP_SDIO_GPIO_RESET=-1`), so GPIO54 acts on EN directly.
- After a successful push the C6 reboots **itself**, on its own timer (`cp_ota.c`,
  `CP_OTA_ASSUMED_CP_REBOOT_TICKS`). That reboot is an `esp_restart()` on the C6, not a GPIO54 pulse.
- The C6 bootloader's `PENDING_VERIFY` → `ABORTED` pass runs whatever the reset reason.
- `coproc` runs `esp_hosted` through `CONFIG_ESP_HOSTED_AUTO_CALL_INIT_BEFORE_APP_MAIN=y`
  (`coproc/sdkconfig`). A constructor starts `eh_cp_init` at priority 5, above `app_main`, and
  `coproc/main/main.c` never calls `esp_hosted_init()` itself.

## Why a handshake-based rule is not enough

The natural rule is "mark the new C6 image valid after its first successful host handshake". Whether
it works depends on something not yet proven: whether the P4 re-handshakes with the new image
**before** anything resets the C6.

Today's master restarts after a push only when `cp_ota_sync()` returns 1, and that happens only after
it has read the new version over a re-established link in the same P4 boot. If that works, a
handshake has already happened and the naive rule would hold. But it fails whenever the P4 resets the
C6 first:
- if `esp_hosted` cannot re-handshake with a C6 that rebooted within the same P4 boot (recovery spec,
  Tier 1 — the source suggests it cannot);
- if the P4 restarts or aborts during the C6's post-push reboot. Today that is
  `RESTART_ON_FAILURE=y`'s `abort()`. In `rescue_p4` it is always the case, because rescue pushes and
  then reboots.

In each case the next P4 bring-up pulses GPIO54 before the first handshake with the new image. The
C6 bootloader then sees a `PENDING_VERIFY` image that was reset, and aborts it. A healthy image is
rolled back exactly like a broken one. `esp_hosted`'s `EH_CP_EVT_PRIVATE_RPC_READY` event cannot be
the sole validity trigger, for the same reason.

The self-validating rule below works whichever way Tier 1 answers, so it is adopted.

## Design: the C6 decides its own validity

In `coproc/main/main.c`:
- `CONFIG_ESP_HOSTED_AUTO_CALL_INIT_BEFORE_APP_MAIN=n`, and `app_main` calls `esp_hosted_init()`
  itself after arming the validity logic. That gives a named completion point, the return of
  `esp_hosted_init()`, and an error to act on.
- **If the running image is `PENDING_VERIFY`:** once `esp_hosted_init()` has returned `ESP_OK` and
  uptime has reached **`CP_SELF_VALIDATE_S`**, call `esp_ota_mark_app_valid_cancel_rollback()`.
- If `esp_hosted_init()` fails, or has not returned by uptime **`CP_INIT_DEADLINE_S`**, call
  `esp_ota_mark_app_invalid_rollback_and_reboot()`, and `esp_restart()` if that returns. It returns
  `ESP_ERR_OTA_ROLLBACK_FAILED` without rebooting when no fallback image exists.
- **If the running image is not `PENDING_VERIFY`, do nothing.**

The deadline runs from an `esp_timer` or its own task, independent of `esp_hosted`, so a blocked
`esp_hosted` cannot hold it off.

`CP_SELF_VALIDATE_S` is chosen from how long a bad image takes to show itself (a crash, a boot loop,
a hang). It is expected to be 20-30 s, and the bench measures it. `CP_INIT_DEADLINE_S` is set on the
bench too; 60 s is the starting value. `CP_SELF_VALIDATE_S` + C6 boot time must stay below the
master's `CP_VALIDATE_WAIT_MS` (recovery spec §6.4). The constant carries a comment saying so.

Supporting changes in `coproc/sdkconfig.defaults`, each with a matching `#if`/`#error` guard in
`coproc` source (an unknown Kconfig name in defaults is silently ignored):
- `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`.
- `CONFIG_ESP_HOSTED_AUTO_CALL_INIT_BEFORE_APP_MAIN=n` (above).
- `CONFIG_ESP_TASK_WDT_PANIC=y`, so a bad image that starves the idle task resets and is rolled
  back. The C6's task watchdog watches only IDLE0 (5 s) and no application task subscribes. So a hang
  that only blocks — a stuck `esp_hosted` task, a deadlock — is caught by the `CP_INIT_DEADLINE_S`
  deadline, not by the watchdog.

After changing the defaults, delete `coproc/sdkconfig` (or reconfigure). An existing `sdkconfig`
overrides the defaults, and today's holds rollback and `PANIC` unset.

Also:
- Log the C6's otadata state and running slot at every boot. This is visible **on the H4 UART
  only**: C6 logs go to its UART0, which reaches only H4, not the master console. On the master,
  `cp_ota`'s version log shows which version the C6 runs.
- `coproc/main/main.c` already erases and retries once on `ESP_ERR_NVS_NO_FREE_PAGES` and
  `ESP_ERR_NVS_NEW_VERSION_FOUND`. It still `ESP_ERROR_CHECK`s the erase, the retried init, and every
  other `nvs_flash_init()` error, so those cases abort in a loop, and under rollback a loop rolls back
  a good image. Replace those `ESP_ERROR_CHECK`s with log-and-continue.

**This catches** the realistic bad radio image: one that crashes, boot-loops, hangs, or never
finishes `esp_hosted` init.

**It does not catch** an image that runs but speaks `esp_hosted` badly: it survives
`CP_SELF_VALIDATE_S` and is marked valid. The master recovery spec's upload path covers the adjacent
case, a wrong application uploaded by mistake, by checking the project name before anything is
written.

### Coupling with the master

Anything on the P4 that resets the C6 after a push must wait `CP_VALIDATE_WAIT_MS` from the moment
activate returns. Otherwise the C6 is reset before it marks the image valid, and a good image is
rolled back. This applies to:
- `rescue_p4`'s single final reboot (recovery spec §4.5);
- the master's restart after a push (`cp_ota_restart_for_new_radio()`);
- the master's transport-failure handler and any reconnect, which stay quiet for that window
  (recovery spec §6.1).

If the P4 resets the C6 within the window for an **unrelated** reason, the new C6 image is rolled
back to the old one. That is safe, because the old image worked. The master then sees the C6 report
its old version, records the push as failed and raises an alarm (recovery spec §6.4). It does not
retry by itself: repeating the push is an operator action.

A push attempted while the C6 is still `PENDING_VERIFY` is refused by the C6's `esp_ota_begin()`
with `ESP_ERR_OTA_ROLLBACK_INVALID_STATE`. The P4 treats that as retry-later, not as a failed image.

### Alternative kept as a follow-up only

`CONFIG_ESP_HOSTED_HOST_CP_RESET_STRATEGY_ONLY_IF_NECESSARY` (SDIO only) would stop the P4 resetting
the C6 on every boot, allowing a handshake-based validity rule. It is **not** adopted. It removes the
documented behaviour that any P4 boot resets the C6 — the self-heal recorded in
`docs/what_we_learned.md` — and it depends on a reconnect path that has not been proven. `esp_hosted`
also recommends pairing it with `RESTART_ON_FAILURE=y` (`eh_host_feat_power_save/Kconfig.ext`), which
conflicts with the recovery spec's `=n`. It stays a benched follow-up.

## The one-time H4 visit per board

Only the C6's bootloader can make pushed images rollback-capable, and **the C6 bootloader cannot be
delivered through `esp_hosted`'s RPC OTA**. `coproc/README.md` gains that statement, and its Recovery
path section (currently "No rollback protection is configured on the co-processor image ...") is
rewritten to describe this design.

The visit flashes the **complete** rollback-enabled coproc set, not only the bootloader: without it,
the rollback-capable application would have to arrive through `cp_ota`, which never pushes a
same-version image, and the running application decides whether pushed images get a trial at all.

Procedure — **derived from the schematic and source, never run on the bench:**
1. Hold the P4 in reset or in ROM download mode, so GPIO54 is high-impedance and cannot fight the
   adapter.
2. Read back and record the C6's existing bootloader and partition table:
   `esptool read_flash 0x0 0x8000` and `read_flash 0x8000 0xC00`.
3. Flash the complete set: bootloader at `0x0`, partition table at `0x8000`, `ota_data_initial` at
   `0xd000`, application at `0x10000`.
4. `verify_flash`.
5. Release the P4 and confirm a handshake on the master console.

Document the H4 header's BOOT (C6 GPIO9) and EN pins alongside the procedure. Step 2 matters: a
botched write must be recoverable to the exact previous state.

## Verification

**Bench, owner present:**
- Which firmware the C6 runs today and what its other OTA slot holds. Rollback is worthless if the
  other slot is empty or holds the factory 0.0.0 image. Read the C6 otadata and app descriptors over
  H4, or take the running version from the master's `cp_ota` version log.
- The H4 header's BOOT and EN pins, whether the P4 must be held in reset, and the factory C6's
  current bootloader, partition table and IDF version — all read back **before** anything is written.
- How long a deliberately bad image takes to crash or hang, which sets `CP_SELF_VALIDATE_S`, and the
  C6's boot-to-init time, which with it bounds the master's `CP_VALIDATE_WAIT_MS`.
- A healthy pushed image **survives** the next master boot. This is the test the handshake-based rule
  fails.
- A deliberately bad image — one that crashes, one that hangs, and one whose `esp_hosted` init never
  completes — is rolled back to the previous image by itself.
- A second push during the validation window is refused as retry-later.
- An NVS error other than `NO_FREE_PAGES`/`NEW_VERSION_FOUND`, or a failed erase, no longer
  boot-loops the C6.

## Out of scope

Board modifications to let the P4 reflash a dead C6; a C6 factory partition; detecting a C6 image that
runs but implements the protocol incorrectly.
