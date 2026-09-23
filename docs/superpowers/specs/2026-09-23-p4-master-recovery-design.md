# P4 Master recovery — design

**Status:** approved in brainstorming 2026-09-23 (owner, section by section). Not yet planned.
Corrected the same day after a source verification pass; the corrections are listed at the end.
**Companion spec:** `2026-09-23-c6-self-validating-rollback-design.md` — the co-processor side.
**Context:** `docs/superpowers/plans/2026-09-21-master-v2-followups.md`, "Do these first" item 1;
system spec (`2026-08-31-hillgrow-system-design.md`) §3.4, §3.9 and §11.10.

## Why this exists

The Master v2 migration moved the master to an ESP32-P4 whose Wi-Fi is an on-board ESP32-C6
co-processor, reached over SDIO through `esp_hosted`. The P4's `factory` partition is **empty**:
the existing `rescue/` app is ESP32-only. So the migration removed the rescue safety net the ESP32
master had. Two bad OTA slots now mean USB and a PC.

It is a design problem rather than a retarget. The ESP32 rescue recovers over Wi-Fi, and on the P4
Wi-Fi *is* the co-processor, which may be the very thing that broke. The ESP32 had one thing that
could fail; the P4 has two independent ones — the P4 application and the C6 firmware.

## Findings that shaped the design

Established from source and the board schematic before any design choice was made:

- **The P4 cannot reflash a dead C6 on the board as built.** The C6's boot strap (IO9) and UART
  reach only the external header H4. The P4 drives only the C6's EN (GPIO54, a reset) and an
  optional IO2 line that is not a strap. The P4 can reset the radio but cannot put it in download
  mode. No rescue app can fix a C6 that will not talk; that case belongs to the companion spec.
- **Most failures need no rescue.** With rollback on and two OTA slots, a new image that crashes
  during its trial rolls back by itself. Rescue exists for a narrower set: a firmware that *passed*
  its trial and later crash-loops or hangs, and both slots going bad.
- **The most useful recovery needs no radio.** Swapping back to the previous slot fixes the
  likeliest field failure with no network, no media and no dependence on the C6.
- **The system spec's §3 safety layer is specified but not built.** No safety manager, HOLD/SAFE
  mode, boot record or LOCAL mode exists in code (SP2, waiting on hardware). This design builds the
  master's part of the §3.9 boot record (the abnormal-reset count that drives §3.4's crash-loop
  ladder) rather than a parallel counter. The HOLD rung stays SP2's.
- **Per spec §3.4, the zones do not need the master to keep plant care going.** Once SP2 exists, a
  zone enters LOCAL after 30 s of master silence and "all plant care continues indefinitely". While
  the master is in rescue, what stops is the master's own equipment and interlocks: reservoir
  refill, ventilation, the room grow light, leak detection, and the fleet-wide low-reservoir pump
  inhibit. Today there is no plant care yet to lose.

## Owner decisions

1. **The field fix is a phone.** No PC, no microSD card prepared in advance.
2. **Rescue entry is automatic plus a button.** The master enters rescue by itself after a crash
   loop; a button on P3 IO34 is the manual override.
3. **C6 recovery is C6 rollback**, accepting one H4 visit per board. No board modification.
   (Companion spec.)
4. **No panel in rescue.** A status LED (P3 IO30), a fixed rescue SSID, and a captive portal.
5. **Uploads accept both a master image and a C6 radio image.**
6. **A separate, master-only P4 rescue project** (`rescue_p4/`). The bench-verified ESP32
   `rescue/` used by the zones is not modified.
7. **An automatic rescue with nothing left to try returns to the normal slot after 30 minutes**
   with no phone connected, with automatic rescue off until the next power-on.
8. **An automatic rescue's upload page is armed only by a press of IO34** during the session.

Engineering defaults, stated so they can be overridden: rescue never erases NVS (that is the 1-9 s
button hold's job); there is no microSD path (a phone-only fixer has no prepared card).

## Scope

**Built by this design:**
- `rescue_p4/` — a master-only rescue app for the ESP32-P4, in the 2 MB `factory` partition.
- The master's part of the §3.9 boot record, as the crash-loop trigger.
- `components/hg_rtc/` — one shared definition of the RTC area: the bootloader's rescue flag plus a
  versioned record. Used by the master and `rescue_p4`.
- The OTA-trial predicate moved out of `master/main/app_main.c` into the `ota_trial` component as
  `ota_trial_running_on_trial()`, so every caller shares one copy (§2.3).
- `cp_ota` split into `cp_ota_sync()` (normal app) and an ungated `cp_ota_push_staged()` (rescue).
- Image identification, in two parts: (a) follow-ups "Open code and test items" item 5 — the HGFW
  header parsers in `cp_ota.c` and `fw_srv.c` consolidated into `hg_blob`, so rescue does not become
  a third; (b) `identity_ok()` extracted from `components/http_srv/http_upload.c` into a shared
  helper and extended with the chip id and app-descriptor magic of §4.2. The helper also holds the
  three project names as constants (§5.7).
- Master hardening this design depends on (§6): `RESTART_ON_FAILURE=n` with a real handler, explicit
  radio init, NVS changes.
- Flash tooling: `tools/flash_all.py` locates the `rescue_p4` build and refuses a pair of builds that
  disagree on shared settings (§5.6, §5.7).

**Unchanged:** the shared custom bootloader (`bootloader_components/main/bootloader_start.c`), the
ESP32 `rescue/` app, the zone firmware. The zone keeps writing the rescue flag by hand
(`zone/main/app_if_zone.c`, `hg_reboot_to_rescue()`); `hg_rtc`'s header records that it writes the
same four bytes. Moving the zone onto `hg_rtc` is a follow-up.

**Out of scope:** microSD recovery; the 7-inch panel; bootloader, partition-table and otadata
recovery (these stay PC-only via P4 ROM download); board modifications; the rest of the §3 safety
layer (HOLD/SAFE modes, LOCAL) beyond the boot record's crash accounting.

**Deferrable if the scope must shrink:** the NVS downgrade guard (§6.5). It matters only when an
automatic swap lands on an older image.

## 1. Architecture

```
            +---------------------------- shared bootloader (unchanged) --------------------------+
reset --->  | 1. bootloader_utility_get_selected_boot_partition()  <-- PENDING_VERIFY -> ABORTED   |
            | 2. reads + zeroes custom[0..3]; flag was set? -> boot factory once                  |
            | 3. IO34 held >= 10 s?        -> boot factory                                         |
            | 4. selected slot unloadable  -> walks back to factory if populated                   |
            +-------------------------------------------------------------------------------------+
                  |                                                    |
                  v                                                    v
          master app (ota_0/ota_1)                           rescue_p4 (factory)
          - boot record first in app_main                    - reads the RTC record into RAM
          - crash loop -> RTC reason + flag -> restart       - IO34 sampled first
          - CLI/web rescue request (trial-guarded)           - swap (radio-free)  OR  upload mode
          - explicit esp_hosted bring-up                     - upload mode: esp_hosted, SoftAP,
          - cp_ota_sync() (call-site trial gate)               captive portal, cp_ota_push_staged()
```

The RTC area is `rtc_retain_mem_t.custom[]` (`CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC=y`,
`CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC_SIZE=0x100`, `..._IN_CRC` unset). **Bytes 0-3 are the
bootloader's rescue flag** and stay exactly as today. **Bytes 4 onward are one packed, versioned
record** with its own magic and CRC, always accessed by `memcpy`. `docs/what_we_learned.md` (SP1)
records why: `custom[]` is CRC-excluded and word-cast-unsafe.

The system spec places the boot record in `RTC_NOINIT` (§3.9). This design moves it into
`custom[4..]` deliberately: the record must be read by two different applications (master and
`rescue_p4`), and an `RTC_NOINIT` variable's address is fixed only within one build, whereas
`custom[]` sits at an address the bootloader's reservation fixes for every app. The offset of
`custom[]` depends on `CONFIG_BOOTLOADER_RESERVE_RTC_SIZE`, so that key is compared too (§5.6).

The record carries:
- the entry reason, and rescue's retry count;
- the boot record: crash count, running image identity (§2.2), and the latches `fired`,
  `rescue_entered` and `auto_rescue_off`;
- the swap guard: crash slot, target slot, target image identity;
- the transport-restart count (§6.1).

On every reset except a deep-sleep wake, the bootloader reads and zeroes `custom[0..3]`, whether or
not the flag was set, and touches nothing else in `custom[]`. It wipes the whole structure only when
the pre-`custom` CRC is invalid, which is at power-on. So the record survives warm resets into rescue
and is cleared by a power cycle, which bounds any misbehaviour to one power cycle.

## 2. Entry

### 2.1 Four ways in

| Entry | Trigger | Guard |
|---|---|---|
| Crash loop | The boot record's crash count reaches 3 (§2.2) | Never while the running slot is `PENDING_VERIFY` |
| Software request | CLI verb `RESCUE CONFIRM` and the same verb from the web UI (§6.3) | Refused during an OTA trial; refused unless `factory` holds a verified `rescue_p4` image |
| Button | IO34 held ≥ 10 s at reset (bootloader, unchanged) | — |
| Nothing bootable selected | The bootloader lands in `factory` by itself | — |

### 2.2 The boot record (system spec §3.9, master part)

It runs **first in `app_main`**, before anything that could crash, including radio bring-up (§6.2).

**Reset classification** is a pure, host-tested function over all 16 `esp_reset_reason_t` values
(`esp_system.h:25-40`):
- **CRASH** — counts: `ESP_RST_PANIC`, `ESP_RST_INT_WDT`, `ESP_RST_TASK_WDT`, `ESP_RST_WDT`,
  `ESP_RST_CPU_LOCKUP`.
- **CLEAR** — resets the count: `ESP_RST_POWERON`, `ESP_RST_BROWNOUT`, `ESP_RST_PWR_GLITCH`.
  Power faults are not image faults.
- **NEUTRAL** — leaves the count unchanged: `ESP_RST_SW`, `ESP_RST_EXT`, `ESP_RST_SDIO`,
  `ESP_RST_USB`, `ESP_RST_JTAG`, `ESP_RST_UNKNOWN`, `ESP_RST_DEEPSLEEP`, `ESP_RST_EFUSE`, and — as the
  `default` arm — any value a later IDF adds. `ESP_RST_SW` must be neutral: this design itself
  restarts through `esp_restart()` (the transport handler, the rescue swap, `cp_ota`'s post-push
  restart), and a crash loop must not be able to launder itself through those.

**The count is keyed to the running image.** Identity is the running partition's offset plus the
first 8 bytes of `esp_app_desc_t.app_elf_sha256`, read with `esp_ota_get_partition_description()`.
The master, rescue and the swap guard all use exactly this field. When the identity differs from the
stored one, the count resets to zero and **this boot's reset reason is not counted**: after a
rollback the fallback image boots with the reset reason of the crash that belonged to the other
image, and would otherwise be blamed for it.

**Counting and firing.** A CRASH reset adds one; a NEUTRAL reset between crashes neither adds nor
clears. Only a CLEAR reset, the stable window, or an identity change resets the count. The master
fires on the boot where the count reaches **3** (`HG_BOOTREC_CRASH_LIMIT`).

**The count clears after 600 s of stable running** (`HG_BOOTREC_STABLE_S`, the system spec's
window), driven by its own one-shot timer, with a static assertion that it exceeds
`TRIAL_FLEET_WINDOW_MS` (180 s). That margin is what makes "passes its trial, then crashes"
detectable. The same timer clears the swap guard (§3.2) and the transport-restart count (§6.1).

**The gate is "the running slot is not `PENDING_VERIFY`"**, via `ota_trial_running_on_trial()`
(§2.3). While the slot is `PENDING_VERIFY` the boot record neither counts nor fires, and the stable
timer does not clear anything; it still records the identity. The gate is deliberately **not** "the
slot is `VALID`": boards flashed by the tools sit in `ESP_OTA_IMG_UNDEFINED` (`tools/hg_otadata.py`),
and only `ota_trial` ever writes `VALID`. A `VALID` gate would leave automatic rescue disabled across
the tool-flashed fleet. `UNDEFINED`, `ABORTED`, `INVALID` and a failed state query all count as
eligible.

**On firing:** set `fired`, zero the count, record the running slot as the crash slot, write the
reason `CRASH_LOOP`, then the flag, then `bootloader_common_update_rtc_retain_mem(NULL, false)`, then
`esp_restart()`. Firing happens at the top of `app_main`, before any store is open, so there is
nothing to flush.

**Latch lifecycle.** Every *deliberate* exit from rescue (a swap, an upload reboot, the time-box
return) clears `fired` and `rescue_entered`. So at the top of `app_main`:
- `rescue_entered` still set → rescue crashed out instead of exiting. Latch `auto_rescue_off`, clear
  both latches, log.
- `fired` set without `rescue_entered` → the flag never reached rescue (factory did not boot). Latch
  `auto_rescue_off`, clear `fired`, log.

`auto_rescue_off` blocks firing until power-on. It does not block the button or the software entry.

**Before setting the flag**, both this path and the software entry check that `factory` holds a real
`rescue_p4`: magic `0xE9`, chip id `0x12`, app-descriptor project name `hillgrow_rescue_p4`, plus a
full `esp_image_verify` for the software path. On a board with an empty factory the flag would
silently boot `ota_0` once (the bootloader tries factory, then walks forward from slot 0). If the
check fails, the software entry returns a clear error, and the crash-loop path logs, latches
`auto_rescue_off` and keeps running. On the ESP32 master build `factory` holds `hillgrow_rescue`, so
the check never passes there and neither entry fires.

**Known blind spot:** a crash inside IDF startup (PSRAM, cache, constructors) happens before
`app_main` and is recoverable only by the button, because the bootloader is unchanged.

### 2.3 Why the software entry must refuse during a trial

`bootloader_start.c:81` calls `bootloader_utility_get_selected_boot_partition()` on **every**
reset, before it checks the RTC flag (`:93-104`) or the button (`:106-124`). With rollback enabled,
that call rewrites every `PENDING_VERIFY` otadata entry to `ESP_OTA_IMG_ABORTED`
(`bootloader_utility.c:395-403`), and an `ABORTED` slot is not booted again until a new OTA rewrites
otadata.

So `hg_reboot_to_rescue()` refuses while the running slot is on trial, returning an error that
points at `SET OTA CONFIRM` or waiting for the trial. The predicate is today `static` in
`master/main/app_main.c:66-71` (`running_slot_on_ota_trial()`), and `http_upload_master.c:100-105`
carries its own inline copy of the same test. Both move to one shared
`ota_trial_running_on_trial()` in the `ota_trial` component. The boot record, `hg_reboot_to_rescue()`,
the CLI verb, `app_main`'s `cp_ota` gate and `http_upload_master.c` all call it. The refusal is
host-tested on both the CLI and the web path.

**A field-procedure fact:** **until a new master image's trial passes, any reset permanently retires
it** — the IO34 button (either hold length), a power cut, or a software request. The trial lasts at
least 60 s (`TRIAL_BENCH_WINDOW_MS`, no link breadcrumb). With a breadcrumb it ends at the first
master frame or fails at `TRIAL_FLEET_WINDOW_MS` (180 s). If the base criteria are never met it runs
until `SET OTA CONFIRM` (`trial_eval.c`). The software request refuses; the button and a power cut
cannot be stopped. The field procedure must say so.

### 2.4 How rescue learns why

Rescue needs no bootloader change to know its entry reason. It copies the RTC record into RAM at
start, then decides **in this order of precedence**:
1. **IO34 low at start → BUTTON**, overriding any stored reason.
2. **A stored reason** in the record: `CRASH_LOOP` or `SOFTWARE`. A nonzero retry count means rescue
   itself panicked and re-armed (§5.3); the stored reason is still the original one.
3. **The selected slot fails verify and the other slot passes the swap checks →
   SELECTED_UNLOADABLE.**
4. **Neither OTA slot passes `esp_image_verify` → BOTH_SLOTS_DEAD.**
5. **No otadata entry is active → NO_ACTIVE_SLOT.**
6. **Otherwise BUTTON**, released early.

Every reason has exactly one row in §3. The classification is host-tested.

The stored reason is zeroed by every deliberate exit from rescue (with the flag) and by the master on
every normal boot, so a stale reason cannot misroute a later entry.

**Rescue never calls `esp_restart()` while IO34 is held.** The next bootloader would measure the
hold from zero, and a release between 1 and 9 s erases NVS. Rescue waits for release, showing an LED
pattern.

## 3. Rescue behaviour

| Entry reason | First action | Then |
|---|---|---|
| `CRASH_LOOP` | Swap to the other slot — radio-free (§3.1) | If there is nothing to swap to: upload mode, **armed by IO34**, **30-min time-box** |
| `SELECTED_UNLOADABLE` | Swap once (§3.1) | If the swap fails: upload mode, armed, **no time-box** |
| `BOTH_SLOTS_DEAD` | — | Upload mode, armed, no time-box |
| `NO_ACTIVE_SLOT` | — | Upload mode, armed, no time-box |
| `BUTTON`, `SOFTWARE` | — | Upload mode, **unbounded, no arming** |

The time-box applies only where the selected slot still passes `esp_image_verify`. Where it does
not, returning to it would walk the bootloader straight back into `factory`, and rescue would cycle
every 30 minutes forever. That is decision 7's intent, not a departure from it: there is no normal
slot to go back to.

**The swap completes before any radio code runs,** so a dead C6 cannot block the recovery that needs
nothing.

### 3.1 Swap target and checks

The crash slot is the one the master recorded when it fired. For `SELECTED_UNLOADABLE` the master
never ran, so the crash slot is `esp_ota_get_boot_partition()`. The target is the other OTA slot.
It must pass all of: subtype `OTA_0`/`OTA_1`; chip id `0x12`; project name `hillgrow_master`;
`esp_image_verify`; and not already `esp_ota_get_boot_partition()`. Rescue writes the swap guard,
then calls the slot-select wrapper (§5.5), then reads back and restarts only if the boot partition
is the target and its state is `NEW`. On any failure it goes to upload mode.

`esp_ota_set_boot_partition()` accepts an `ABORTED` slot and writes it `NEW`; the bootloader turns
`NEW` into `PENDING_VERIFY`, so the swapped-to image gets a fresh trial and ordinary rollback catches
it if it is also bad. Swapping onto an `ABORTED` slot is therefore allowed. It is also necessary:
there is only one other slot, and the commonest incident is "v1 crash-loops after v2's trial
failed", where the good-looking target is exactly the one marked `ABORTED`. An `ABORTED` mark can
also be spurious, since the button or a power loss during a trial produces one.

The bootloader walks backwards from the selected slot before it reaches `factory`
(`bootloader_utility.c`, the loop after `/* work backwards from start_index */`). An unloadable
`ota_1` therefore falls back to `ota_0` inside the bootloader without reaching rescue. So
`SELECTED_UNLOADABLE` with a swappable other slot occurs only when `ota_0` is selected. The
classification test covers both orders.

### 3.2 Ping-pong guard

If both slots are bad, a naive swap alternates forever. Rescue writes the swap guard {crash slot,
target slot, target identity} **before** it selects the target. **While a swap guard is set, a
`CRASH_LOOP` entry does not swap again: it goes to upload mode.** Both ways a swap can fail lead
here. The target may crash during its trial, in which case the bootloader rolls back to the
crash-looping image, which fires again. Or the target may pass its trial and then crash-loop itself.

**Only the master clears the guard**, after its 600 s stable window and never while
`PENDING_VERIFY`. The reset-reason logic never touches it. A power cycle clears it too, which costs
at most one extra swap cycle per power cycle. The sequence, including a power loss between each
step, is host-tested.

### 3.3 Automatic entries: arming and the time-box

For an automatic entry (`CRASH_LOOP`, `SELECTED_UNLOADABLE`, `BOTH_SLOTS_DEAD`, `NO_ACTIVE_SLOT`),
the rescue AP comes up but **uploads are refused until IO34 is pressed** during the session, with a
"press to arm" LED pattern. With automatic entry, "physical access is the gate" no longer holds by
itself; the press restores it using hardware already fitted.

**The time-box** (`CRASH_LOOP` row only) is a one-shot deadline 30 minutes (`RESCUE_TIMEBOX_S`,
1800) after rescue reaches upload mode. The first station association or an arming press cancels it
for the rest of the session. At expiry rescue waits for IO34 to be released if it is held (§2.4). It
then latches `auto_rescue_off`, clears the reason and the latches, and returns to the selected slot
**without touching otadata**. The master therefore does not re-fire at once. After that the button
is the way back in.

Button and software entries keep **unbounded** upload mode with no arming, since someone came to fix
the board on purpose.

### 3.4 Status LED (P3 IO30)

Distinct patterns for: rescue active, press to arm, armed, uploading, swapping, radio dead, and
waiting for IO34 release. A swap onto an **older** firmware gets its own pattern and a log line, so a
downgrade is never silent. "Older" means the target's `esp_app_desc_t.version` compares lower than
the crash slot's as `MAJOR.MINOR.PATCH` (`version.txt`); an equal version is not older.

## 4. Uploads

### 4.1 Landing page and captive portal

The phone's OS captive-portal sheet receives a **landing page**: what is wrong, which firmware boots
next, and a prominent link to `http://192.168.7.7/` labelled "open in your browser". The upload form
lives at that URL, and also works inside the sheet where the sheet supports it. Captive-portal sheets
may not support file pickers or multi-megabyte POSTs; this keeps decision 4 — the page opens by
itself — without depending on that.

DNS answers every query with 192.168.7.7. IDF 6.0.1 has no `dns_server` component in
`components/`; its captive-portal example carries one
(`examples/protocols/http_server/captive_portal/components/dns_server`), which `rescue_p4` copies into
its own `components/`. lwIP, DHCP and DNS run on the P4 side of `esp_wifi_remote`.

The AP is `HillGrow-Rescue-XXXXXX` (last 3 MAC bytes, the same pattern as
`rescue/main/rescue_wifi.c:169`), WPA2 with password `hillgrow1`, the same default as the ESP32
rescue and the master AP (`rescue/main/rescue.h:15`). "Fixed" in decision 4 is read as this fixed
pattern.

### 4.2 Identification before any erase

Rescue reads the first 112 bytes and checks, in order: image magic `0xE9`; chip id at +12; the
app-descriptor magic `0xABCD5432` at +32; the project name at +80.

| chip id | project name | treated as |
|---|---|---|
| `0x12` (ESP32-P4) | `hillgrow_master` | master firmware |
| `0x0D` (ESP32-C6) | `eh_cp_wifi_softap` (`coproc/CMakeLists.txt`) | radio firmware |
| anything else | — | refused, nothing touched |

The order matters. `esp_ota_end()` verifies chip id and revision, segments, checksum and SHA-256, but
**not** the project name or the secure version, and there is no signature check. Identity must be
established before `esp_ota_begin()`.

The size is checked from `Content-Length` before any erase too: a master image must fit its OTA slot,
and a radio image must fit `cp_fw` minus `CP_OTA_HDR_LEN`. `cp_fw` is `0x180000` including the
header, smaller than a C6 OTA slot (`0x1C0000`), so a valid C6 image can pass identification and
still not fit.

Radio firmware is accepted as the **raw** `.bin`; rescue writes the HGFW header itself.

### 4.3 Master firmware

- Target: the OTA slot that is **not** `esp_ota_get_boot_partition()` — the rule of
  `rescue/main/rescue_pull.c`'s `rescue_target_slot()`. `rescue_p4` carries its own copy of those
  eight lines rather than sharing them, because decision 6 leaves `rescue/` unmodified.
- `OTA_WITH_SEQUENTIAL_WRITES`, streamed to flash as it arrives.
- `esp_ota_abort()` on every failure path. A dropped upload leaves the boot selection and the
  selected slot intact. **It does not leave the target slot intact:** `esp_ota_begin()` invalidates
  that slot's otadata entry (`esp_ota_ops.c`, `esp_ota_invalidate_inactive_ota_data_slot()`), and the
  sequential writes erase the old image as data arrives. After a failed upload that slot is no longer
  a swap or rollback candidate. This is acceptable because an upload happens only in upload mode,
  after any swap has already been tried.
- `esp_ota_end()` completes the write, but **`set_boot` waits for the end of the session** (§4.5).
- After the reboot the image is `PENDING_VERIFY` and runs the master's normal trial, so a bad upload
  still rolls back by itself.

### 4.4 Radio firmware — rescue pushes it itself

Handing a radio image to the normal app does not work. `cp_ota_sync()` returns before opening
`cp_fw` unless the C6's major.minor differs from `CP_OTA_HOST_VERSION`, and `app_main` does not call
it at all while the running slot is `PENDING_VERIFY` (`app_main.c:346-361`; the trial gate lives at
that call site and stays there). So a same-version fix would never be pushed, and uploading a master
image plus a radio image would deadlock and retire the new master image.

So `cp_ota` gains an ungated `cp_ota_push_staged()` (validate, begin/write/end, activate), and
rescue calls it directly. That is safe from `factory`: `esp_ota_get_state_partition()` returns
`ESP_ERR_NOT_SUPPORTED` there, so there is no master trial to spoil. `cp_ota_push_staged()` neither
reads nor writes `cp_ota`'s CRC memory (§6.4), so rescue writes no master NVS. The cost is at most
one extra push by the master, if the C6 rolled the rescue-pushed image back.

If the C6 is still validating a previous image (`PENDING_VERIFY` on the C6), the C6's
`esp_ota_begin()` refuses with `ESP_ERR_OTA_ROLLBACK_INVALID_STATE`. Rescue reports "radio busy,
retry in a minute" rather than a bad image.

`cp_fw` is written in `http_upload_zone.c`'s order: erase the header sector, erase in steps with
watchdog feeds, stream the body while chaining the CRC, **write the header last**, then read it back
through the existing `cp_ota_parse_header()`. A half-finished write therefore never looks valid. The
image stays staged in `cp_fw`, so later master boots can see it.

### 4.5 One session, one reboot

The page can stage a master image, a radio image, or both. **Reboot** then:
1. pushes the radio image, if one is staged;
2. if it pushed one, waits `CP_VALIDATE_WAIT_MS` from the moment activate returns (§6.4) —
   rebooting sooner would reset the C6 before it could mark the new image good, and roll it back;
3. selects the staged master image, if there is one, through the slot-select wrapper (§5.5);
4. disarms rescue's self re-arm and clears the reason and latches (§5.3);
5. restarts once.

If the session ends any other way (a power cut, a panic that re-arms), nothing staged is selected
and the session starts over. A staged master image then sits unselected in the other slot, and the
user uploads it again.

**The page warns, before the user commits, that the phone will drop off Wi-Fi during the radio
push** — rescue's own AP is the C6. The phone cannot watch that step finish; the next master boot
shows the result.

### 4.6 Limits

- Rescue does **not** fix a flapping link: an unstable link cannot reliably carry an upload.
- The radio upload relies on the companion C6 rollback for its safety net. A bad radio image pushed
  from rescue is the same risk as today's normal-app push; until C6 rollback exists, recovering from
  one still needs an H4 visit.

## 5. Robustness

### 5.1 Radio configuration, pinned and guarded

`rescue_p4/sdkconfig.defaults` sets:
- `CONFIG_ESP_HOSTED_AUTO_CALL_INIT_BEFORE_APP_MAIN=n`
- `CONFIG_ESP_HOSTED_HOST_CP_BRINGUP_ON_TIMEOUT_NONE=y`
- `CONFIG_ESP_HOSTED_HOST_TRANSPORT_RESTART_ON_FAILURE=n`

Each has a matching `#if`/`#error` guard in `rescue_p4` source. **An unknown Kconfig name in
`sdkconfig.defaults` is silently ignored**, so without the guards a single typo would re-enable the
exact behaviour the setting exists to prevent. (An earlier draft of this design named a symbol that
does not exist, `CONFIG_ESP_HOSTED_AUTO_INIT`; the real one is
`CONFIG_ESP_HOSTED_AUTO_CALL_INIT_BEFORE_APP_MAIN`, defined in `common/eh_common/Kconfig.ext`.) The
master pins the same three (§6.2).

An existing `sdkconfig` overrides `sdkconfig.defaults`. After changing defaults, delete the build's
`sdkconfig` (for the P4 master, `master/build_p4/sdkconfig`) or the old values persist silently.

### 5.2 Bring-up is explicit and bounded

Rescue calls `esp_hosted_init()` and `esp_hosted_connect_to_slave()` and treats any nonzero return
as failure (`connect_to_slave()` returns `-1`, `-EIO`, `-ETIMEDOUT`, `-EINVAL` or `-ENOMEM`; `-EIO`
means the C6 did not answer). The failure states are sticky in `eh_host_core.c`:
- A failed `esp_hosted_connect_to_slave()` latches `BRINGUP_FAILED` and returns `-1` on every later
  call. So each retry is `esp_hosted_deinit()`, then `esp_hosted_init()`, then
  `esp_hosted_connect_to_slave()`. After deinit, connect returns `-EINVAL` until init runs again.
- A failed `esp_hosted_init()` is sticky for the whole boot, because deinit is a no-op unless init
  succeeded. It means radio dead, with no retry in that boot.

At most 3 attempts, 2 s and then 4 s apart. Measured from source, `connect_to_slave()` is bounded at
about 3 s against a dead C6 and about 8 s against a silent one, and every wait is `vTaskDelay` or
semaphore based.

A `TRANSPORT_FAILURE` handler only latches a flag. A normal task calls `esp_hosted_deinit()` once,
which stops the SDIO tasks, and switches the LED to radio dead.

### 5.3 Rescue re-arms itself before touching the radio

Some abort paths in `esp_hosted` remain even with `RESTART_ON_FAILURE=n`: `EH_CHECK_OK` on the CMD52
and set-blocksize path, the bring-up-timeout REATTEMPT/RESTART choices (which is why
`BRINGUP_ON_TIMEOUT_NONE` is pinned), and asserts in bus init. Since the bootloader has already
consumed the flag, a panic in rescue would otherwise boot the broken master. For a crash-loop entry
that is an endless master → rescue → abort → master cycle.

So **before its first `esp_hosted` call, rescue re-arms the RTC rescue flag**, keeping the
**original** entry reason in the record and incrementing the retry count. A panic then lands back in
rescue (warm resets only; power-on zeroes `custom[]`) with the same §3 row. **After 2 such panics
(`RESCUE_RADIO_MAX_RETRIES`) rescue skips the radio entirely**, shows radio dead, and stays up
radio-free. It disarms the re-arm immediately before any slot selection (the swap, and §4.5 step 3)
and before every deliberate restart.

### 5.4 Watchdog

`CONFIG_ESP_TASK_WDT_TIMEOUT_S=30` with `CONFIG_ESP_TASK_WDT_PANIC=y`, as `rescue/sdkconfig.defaults`
does. Radio and SoftAP bring-up run in a dedicated task that is **not** watchdog-subscribed. The
subscribed main task drives the LED and supervises a deadline, `RESCUE_RADIO_DEADLINE_MS` (60 000)
from the first `esp_hosted` call. If the SoftAP is not up by then, rescue shows radio dead and calls
`esp_restart()` with the re-arm still set, which counts as one §5.3 retry. No watchdog subscription is
ever held across an `esp_hosted` call. Flash erases proceed in 64 KB steps with feeds
(`http_upload_zone.c` is the precedent). Restarts come from a normal task, never an `esp_timer`
callback.

### 5.5 NVS and otadata

- Rescue **never** calls `nvs_flash_erase()`. The swap path touches no NVS and runs before
  `nvs_flash_init()`. Upload mode initialises NVS and skips it on any error.
- All slot selection goes through one wrapper around `esp_ota_set_boot_partition()`. It **returns
  `ESP_ERR_INVALID_ARG` — never asserts or aborts —** when the target is not `OTA_0`/`OTA_1` or equals
  `esp_ota_get_boot_partition()`, and the caller then goes to upload mode. So rescue **can never
  select `factory`** — the blank-otadata trap `docs/what_we_learned.md` records from SP1. An abort
  here would come after §5.3's disarm and boot the crash-looping master, which is why the wrapper
  must not abort.

### 5.6 The two builds cannot disagree

Master (P4 build) and `rescue_p4` must agree on:
- `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`
- `CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC=y`, `CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC_SIZE=0x100`,
  `CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC_IN_CRC` unset
- `CONFIG_BOOTLOADER_RESERVE_RTC_SIZE`, which fixes `custom[]`'s offset
- `CONFIG_BOOTLOADER_WDT_TIME_MS=30000`, on which the 10 s button hold depends
- the chip-revision selection (`CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y`, `CONFIG_ESP32P4_REV_MIN_100=y`)
- `CONFIG_PARTITION_TABLE_OFFSET=0xF000` and the same partition table (`master/partitions_p4.csv`)

Today these keys are split between `master/sdkconfig.defaults` (shared with the ESP32 master build,
which keeps `0xE000`) and `master/sdkconfig.defaults.esp32p4`. `rescue_p4` sets them in its own
defaults. The guarantee does not rest on the defaults files agreeing. It rests on two checks:
- `rescue_p4` source carries `#error` guards for the rollback and RTC keys, and `hg_rtc`'s header
  asserts the struct's size and offsets at compile time in both apps.
- `tools/flash_all.py` reads both builds' final `sdkconfig` and refuses to flash a master/rescue pair
  that disagrees on any key above. `CONFIG_BOOTLOADER_RESERVE_RTC_MEM` is derived (a promptless bool
  selected by `CUSTOM_RESERVE_RTC`), so it appears in guards and comparisons, never in defaults.

### 5.7 `rescue_p4` identity and tooling

- The project is `project(hillgrow_rescue_p4)` in `rescue_p4/CMakeLists.txt`. It includes
  `cmake/hillgrow.cmake`, so `PROJECT_VER` comes from `version.txt` like the other apps.
- `hillgrow_master`, `hillgrow_rescue_p4` and `eh_cp_wifi_softap` are defined once, as constants in
  the shared image-identification helper. They are not hand-copied literals. A host test checks each
  against its `CMakeLists.txt`.
- `tools/flash_all.py --board master --target esp32p4` flashes
  `rescue_p4/build/hillgrow_rescue_p4.bin` into `factory`. Today it looks for `hillgrow_rescue.bin`
  in `rescue/build` and only warns when that is missing. For the P4 a missing rescue build **fails**
  the flash. Only the master build's bootloader is flashed.

## 6. Master app changes

### 6.1 Radio failure handling

With `RESTART_ON_FAILURE=n` a dead link no longer calls `abort()`. The handler latches once, and a
normal task that is not watchdog-subscribed acts.

**A deliberate outage is not a failure.** While `cp_ota_sync()` has a push in flight, and for
`CP_VALIDATE_WAIT_MS` after activate returns, the handler only logs. It issues no GPIO54 pulse, no
deinit or reconnect, and no restart. Every push reboots the C6 on purpose (`CP_OTA_ASSUMED_CP_REBOOT_TICKS`),
and reacting to that would reset the C6 while its new image is `PENDING_VERIFY` and roll back every
healthy push — the failure the companion spec exists to prevent.

Otherwise:
- **During an OTA trial:** an explicit trial **FAIL** — unregister the `esp_wifi_stop` shutdown
  handler, then `esp_ota_mark_app_invalid_rollback_and_reboot()`. This preserves the rollback vote
  `=y` produced through its panic, but deterministically.
- **Outside a trial:** `esp_hosted_deinit()`, then `esp_hosted_init()` and
  `esp_hosted_connect_to_slave()`, at most 3 attempts, 2 s and then 4 s apart. Under
  `CONFIG_ESP_HOSTED_HOST_CP_RESET_STRATEGY_ALWAYS` the connect itself pulses GPIO54
  (`eh_host_bus_sdio.c`), so no separate pulse is issued. Whether this can work at all within one
  P4 boot is a Tier 1 question (§7.2).
- **If reconnect fails: a controlled restart**, bounded by the transport-restart count in the RTC
  record. A software restart is NEUTRAL to the boot record, so it cannot be mistaken for a bad image
  and trigger a pointless slot swap. After **3** controlled restarts (`HG_TRANSPORT_RESTART_LIMIT`)
  with no 600 s stable run between them, the master stops restarting and degrades: Wi-Fi down,
  greenhouse control continuing, and `notify_emit(NTF_ALARM, 0, "RADIO DEGRADED ...")`. The count is
  a boot count, not a timer, so no clock has to survive the reset; the stable window clears it.

The controlled restart is a refinement added after the adversarial review, which proposed degrading
without ever restarting. That would regress today's behaviour, where a transient C6 glitch heals
because the abort reboots everything.

Residual host-side aborts are documented (for example the SW_AGGR assert in
`eh_host_mcu_transport_init_event.c`).

**Watchdog rule for hosted RPCs:** no RPC that can block longer than the 8 s task watchdog runs in a
subscribed task. `eh_host_cp_ota_begin()` alone can block up to `EH_HOST_OTA_BEGIN_TIMEOUT_MS`
(30 s). Long RPCs run in an unsubscribed task supervised by a subscribed one, as in §5.4.
`cp_ota.c` already documents that `cp_ota_sync()`'s caller is deliberately unsubscribed for this
reason.

### 6.2 Radio init after the boot record

The master pins the same three settings as `rescue_p4` (§5.1) in `master/sdkconfig.defaults.esp32p4`,
with matching `#if`/`#error` guards in master source. Today `esp_hosted` starts before `app_main`, so
a deterministic crash in it would loop forever without ever being counted.

`app_main`'s order becomes:
1. the boot record (§2.2), before `notify_init()`;
2. the existing init sequence as today, through `nvs_flash_init()` and `ota_trial_start(1)`;
3. `esp_hosted_init()` and `esp_hosted_connect_to_slave()`, bounded and checked (§5.2's rules);
4. `wifi_mgr_start()`, `time_svc_start()`, `http_srv_start()`, as today;
5. the `cp_ota_sync()` call, behind its trial gate.

If step 3 fails, the master does not start `wifi_mgr`, `http_srv` or `cp_ota_sync()` in that boot,
since all three need the radio. It enters §6.1's path instead: a FAIL during a trial, or reconnect
and then a bounded restart outside one. Greenhouse control runs regardless.

### 6.3 Software rescue entry

A CLI verb **`RESCUE CONFIRM`** in `CMD_AREA_SYSTEM`, with the same shape as `REBOOT CONFIRM`
(`components/cmd_common/cmd_common.c`, the `A_CONF` argument). The web UI sends the same verb through
the authenticated `POST /api/cmd` route (`http_routes.c`), so web authentication applies and no new
route is added. Both refuse:
- during an OTA trial, with `ERR OTA_TRIAL`;
- unless `factory` holds a verified `rescue_p4` (§2.2), with `ERR NO_RESCUE`.

The two tokens join the existing `ERR` token set.

### 6.4 `cp_ota`

- Split into `cp_ota_sync()`, which keeps its version gate and gains the CRC memory below, and the
  ungated `cp_ota_push_staged()` (§4.4). The trial gate stays at `app_main`'s call site.
- **Confirmation** means the C6 reports the staged image's version, in the same boot as the push or
  on the next one. Tier 1 (§7.2) decides which: if `esp_hosted` cannot re-handshake with a C6 that
  rebooted within the same P4 boot, `cp_ota_sync()` can never return 1. Confirmation then moves to
  the next boot, and the post-push restart becomes unconditional after `CP_VALIDATE_WAIT_MS`.
- **CRC memory:** remember, in NVS, the CRC of a staged image whose push was not confirmed, and do
  not push the same CRC automatically again. Today a stale image causes a C6 reflash and a Wi-Fi
  outage at every boot. A push that is followed by the C6 reporting its old version (the C6 rolled
  it back) is recorded the same way, with `notify_emit(NTF_ALARM, ...)`. Repeating it is an operator
  action: a rescue upload, or staging a different image.
- `ESP_ERR_OTA_ROLLBACK_INVALID_STATE` from the C6's `esp_ota_begin()` means the C6 is still
  validating. It is retry-later, never a failed image, and its CRC is not recorded.
- **`CP_VALIDATE_WAIT_MS`**, defined once in `cp_ota.h`: how long anything on the P4 waits, from the
  moment activate returns, before resetting the C6 after a push. It must be at least the C6's
  self-validation time plus its boot time plus margin (companion spec). The initial value is 45 000,
  confirmed on the bench (§7.2). A comment ties it to the companion spec's C6 constant, which must not
  exceed it. It applies only when a radio image was pushed: `rescue_p4` §4.5, and
  `cp_ota_restart_for_new_radio()` in the master.

### 6.5 NVS

- On `ESP_ERR_NVS_NEW_VERSION_FOUND` the master **stops erasing** its NVS. It raises an alarm and runs
  on defaults with writes disabled. Today `app_main` wipes config it cannot read.
- *(Deferrable.)* The config stores (`mcfg_store`, `node_store`, `http_auth`) refuse to overwrite a
  stored value written by a newer image, and raise a NOTIFY alarm, so a downgrade swap cannot replace
  newer settings with an older image's defaults.

## 7. Verification

### 7.1 Host tests

- Reset-reason classification for all 16 `esp_reset_reason_t` values, plus the `default` arm.
- The boot-record state machine: the `PENDING_VERIFY` suppression; the identity reset and the
  uncounted reason on an identity change; firing on the third crash with NEUTRAL resets interleaved;
  every OTA state (`VALID`, `UNDEFINED`, `ABORTED`, `INVALID`, query failure); crashes at
  `TRIAL_FLEET_WINDOW_MS` + 1 s and at the stable window − 1 s; both latch cases.
- The entry-reason classification with its precedence, including IO34 over a stored reason, a
  retry, and both slot orders for `SELECTED_UNLOADABLE`.
- Image identification from the 112-byte header, and the size check: master, C6, garbage, truncated,
  oversized.
- Swap-target selection and the ping-pong guard, both failure routes, with a simulated power loss
  between every step.
- The slot-select wrapper returning an error for a non-OTA subtype and for the current boot partition.
- Upload target selection for boot = `ota_0`, `ota_1`, `factory`, undefined.
- The software entry refusing during a trial and with no `rescue_p4`, on both the CLI and web paths.
- `cp_ota`'s CRC memory, the rolled-back case, `ROLLBACK_INVALID_STATE` as retry-later, and the
  transport-restart bound.
- The project-name constants against each `CMakeLists.txt`.
- `hg_rtc` layout, asserted at compile time in master (both targets) and `rescue_p4`.

### 7.2 Bench, in three tiers

**Tier 1 — settled BEFORE the implementation plan is written**, as a short spike, because the design
depends on each answer:
- Whether P4 v1.3 RTC memory (`custom[4..]`) survives each reset type: `esp_restart`, `abort()`,
  interrupt watchdog, task watchdog, RWDT/MWDT system reset, SUPER_WDT, CPU lockup, brownout. The
  entry protocol and the boot record assume it does.
- Whether `esp_hosted` can reconnect to a C6 that rebooted within the same P4 boot. The source
  suggests not. The answer decides §6.1's reconnect path **and** whether `cp_ota_sync()` can ever
  confirm a push in the same boot (§6.4).
- Whether the Android and iOS captive-portal sheets support a file picker and a 1-4 MB POST, and
  whether Chrome and Safari reach 192.168.7.7 with mobile data on.

**Tier 2 — during the build:**
- `connect_to_slave()` timing against a C6 held in reset versus an erased C6. Use an erased C6 app or
  an EN jumper, not "GPIO54 held low", which contends with the P4's push-pull driver.
- `RESTART_ON_FAILURE=n` behaviour after the C6 dies post-bring-up: whether D1 goes low and the read
  task spins, event floods, and the watchdog. Test idle, under web traffic and mid-RPC.
- Whether the `EH_CHECK_OK` abort in card init can be triggered: pulse the C6 reset during
  enumeration, or run a non-hosted C6 app.
- Erase and write timing with `OTA_WITH_SEQUENTIAL_WRITES` on a 4 MB slot and on the 1.5 MB
  `cp_fw`, against the 30 s watchdog and the phone's TCP timeout.
- Whether supply sags above the brownout threshold cause PANIC-class resets, i.e. false counts.
- The P4-restart-to-GPIO54-pulse timing against the C6's boot and self-validation write. This sets
  the **margin** in `CP_VALIDATE_WAIT_MS`.
- The initial constants — retry backoffs, `HG_TRANSPORT_RESTART_LIMIT`, `RESCUE_RADIO_DEADLINE_MS` —
  against what the bench shows.

**Tier 3 — acceptance, owner present:**
- Every entry path end to end: software request (`rescue flag set -> factory`, record intact),
  crash loop, button, nothing bootable selected.
- IO34 level and timing at `rescue_p4` start after a ≥ 10 s hold with natural releases (10 trials).
- A swap onto a previously `ABORTED` slot, and onto an older master build.
- Uploading master and radio images from a real phone; rescue's radio push while rescue's own SoftAP
  and the phone share that C6, and whether the SoftAP returns afterwards.
- Rescue surviving a dead C6.
- The landing page on real Android and iOS devices.
- What currently sits in `ota_1` on every bench and deployed board
  (`esptool read-flash 0x630000 0x100`).

## 8. Risks and open questions

- **Tier 1 may overturn parts of this design.** If `custom[4..]` does not survive a panic reset on
  P4 v1.3, the entry protocol and boot record need a different store. That is why Tier 1 precedes the
  plan.
- **Long term,** `rescue_p4` freezes an `esp_hosted` version, and rescue is updated only over USB.
  Its compatibility with a C6 one minor version ahead or behind must be checked whenever the host
  `esp_hosted` is bumped. The version pins added during the migration (`~3.0.7` on both sides) make
  that a deliberate act rather than an accident.
- **`CP_VALIDATE_WAIT_MS` and the C6's validation constant live in two projects.** A later C6 image
  that validates more slowly would outrun older master and rescue builds. The comment on each side and
  the Tier 2 measurement are the only ties. Accepted, because the C6 constant changes rarely and a
  mismatch costs one rolled-back push, not a dead radio.
- **`docs/pin-mapping.md` must record IO30 (status LED) and IO34 (rescue button)** as required on
  deployed Master v2 units: without IO34, only software and nothing-bootable entry remain.
- **Off-topic, found during research:** `components/board/board.h` swaps the P4's I²C pins (SDA 8,
  SCL 7) against the Waveshare BSP (`BSP_I2C_SCL = GPIO_NUM_8`, `BSP_I2C_SDA = GPIO_NUM_7`), which is
  proven correct by the working touch controller. `docs/pin-mapping.md:72` carries the same swap.
  Nothing uses these defines on the P4 yet; fix separately before SP2.

## Corrections after source verification (2026-09-23)

The approved design was checked against the IDF 6.0.1, `esp_hosted` and repo sources before owner
review. Changes of substance, beyond wording and line numbers:
- The trial predicate is not yet shared; it moves into `ota_trial` (§2.3).
- The firing sequence no longer calls `hg_store_flush()`, which the master does not link (§2.2).
- `ESP_RST_SDIO` and a `default` arm were added to the classification (§2.2).
- The entry reasons now include `BOTH_SLOTS_DEAD`, with IO34 taking precedence, and each reason has
  exactly one behaviour row (§2.4, §3).
- The latch lifecycle is now defined. Previously, loop prevention made the ping-pong guard
  unreachable (§2.2, §3.2).
- The time-box no longer applies when the selected slot is unloadable, where it would cycle forever
  (§3).
- A panic re-arm keeps the original reason (§5.3).
- The master image's `set_boot` moves to the end of the session, so the radio push keeps its panic
  safety net (§4.5).
- An aborted upload destroys the target slot's previous image (§4.3).
- The `esp_hosted` retry sequence is deinit → init → connect (§5.2, §6.1).
- The transport handler stays quiet during a push, which would otherwise roll back every C6 update
  (§6.1).
- `dns_server` comes from an IDF example, not a component (§4.1).
- The approved "one shared defaults fragment" is replaced by guards plus a `flash_all.py` comparison
  of the two final `sdkconfig`s (§5.6). The keys are split today between two master files, one of
  them shared with the ESP32 build, so a fragment alone could not guarantee agreement.
- `rescue_p4`'s identity and tooling are specified (§5.7).
- The CLI verb and error tokens are named (§6.3).
- The C6 wait constant is named, and push confirmation is defined (§6.4).
