# Master v2 platform migration — follow-ups

The migration plan (`2026-09-21-master-v2-platform-migration.md`) is **complete and
hardware-verified**: the real master runs on the ESP32-P4, the SP4 web suite passes 37/37
including a master self-OTA, fleet OTA works on both zones, blame is correct, and zone
configs survived the platform transplant. See spec §11.10 for the evidence.

This file is what the migration deliberately left open. It was built from the plan's
verified deferred-findings list at close-out (2026-09-23), with every entry re-checked
against HEAD rather than copied forward, so that the backlog survives the plan's
execution workspace being deleted. Items fixed during the migration are not listed.

## Do these first

**1. Give the P4 a rescue app. The migration removed the rescue safety net.**
The P4 `factory` partition is empty. The ESP32 master kept a rescue app there, so two bad
OTA slots still left a recovery path; on the P4, recovery currently means USB and a PC.
Bench-observed: a ≥10 s rescue hold logs `button held 10000 ms -> factory`, finds no
image, and falls through to an OTA slot — it does not brick or loop, but it recovers
nothing. **This is a design question, not a retarget.** The rescue app recovers over
Wi-Fi (`rescue_wifi.c`, `rescue_http.c`), and on the P4 Wi-Fi is the ESP32-C6
co-processor — so rescue would depend on the very component that may be what is broken.
On the ESP32 the radio shared the die, so that coupling did not exist. Candidate
non-Wi-Fi routes: the board's C6-UART header, or loading from the microSD slot the pin
map already reserves (SDMMC slot 0, IO39-44). Also note `rescue/sdkconfig.defaults` pins
`CONFIG_IDF_TARGET="esp32"`, so a P4 build needs its own per-target defaults (the rev<3
pair, 32 MB, table at `0xF000`) and the `esp_hosted` dependency.

**2. Exercise the two co-processor-OTA paths that have never run on hardware.**
The bench only ever hit the version-match no-op branch, because host and co-processor
already matched at 3.0.7. Two paths exist only as reasoning plus a compile:
- *Confirmed-success restart* (`master/main/app_main.c`, `cp_ota_restart_for_new_radio()`):
  after a successful CP push the master restarts so the Wi-Fi stack rebinds to the new
  radio firmware. Loop-safe because the version gate makes the next boot a no-op.
- *Trial skip* (`running_slot_on_ota_trial()` gating the `cp_ota_sync()` call): while the
  running slot is `PENDING_VERIFY` the push is skipped entirely, because tearing the radio
  down mid-trial could let `esp_hosted`'s `abort()` retire a healthy image. Retried
  automatically on the next boot. **This is the cheaper of the two to bench** — stage a
  master OTA and a CP image together and watch for the WARN skip line.
Reverting the restart is one line at the `cp_ota_sync()` call site; the comment block
there records the argument both ways.

**3. A co-processor push can still panic the master outside a trial.**
`CONFIG_ESP_HOSTED_HOST_TRANSPORT_RESTART_ON_FAILURE=y` (esp_hosted's default) makes an
unrecoverable SDIO failure call `abort()`, and a CP push deliberately creates that SDIO
outage. Inside a trial this is now prevented. Outside one it is an unscheduled reboot of a
`VALID` image — not a rollback, and the next boot comes up on the new radio — so it is
undesirable rather than dangerous. Options: disable that Kconfig on the P4 and handle the
failure in `cp_ota`, or accept and document it.

## Open decision for the owner

**Subscribe `httpd` to the task watchdog?** On the P4, a *crash* self-heals in seconds
(panic reboots immediately, and the reboot resets the C6 via GPIO54). But a *hang* in a
task the watchdog does not watch leaves the AP associating while nothing answers — a phone
joins "HillGrow" and silently times out, indefinitely. `app_main` is deliberately
unsubscribed. Subscribing the long-lived service tasks, `httpd` first, would route a wedge
through the panic-and-reboot path that already works. Detail in `docs/what_we_learned.md`
("P4 co-processor keeps serving a dead AP").

## Open code and test items

Most are minor. **Four share one shape** — a constant or expectation hand-copied from a
file nothing ties it back to (items 1-4) — so consider one small "parse the shipped
artifact" helper that retires several at once, rather than four separate patches.

1. `tests/host/test_partitions_p4.c` — the `0x10000` first-partition floor is hand-copied
   from `master/sdkconfig.defaults.esp32p4`; a future table move leaves the test stale and
   silently passing.
2. `tools/flash_app.py` / `tools/flash_all.py` — the P4 offsets duplicate
   `master/partitions_p4.csv`. Accepted during the migration because a flashing tool must
   know an offset before it has a parsed table; the better answer is to parse
   `build*/partition_table/partition-table.bin`.
3. `tools/flash_app.py` — `ZONE_FW_PART_SIZE` / `CP_FW_PART_SIZE` are hand-copied
   partition sizes. Both `0x180000` today, each commented with its CSV source.
4. `bootloader_components/main/bootloader_start.c` — nothing enforces that
   `HG_RESCUE_GPIO` agrees with `HG_GPIO_RESCUE_BTN` in `components/board/board.h`
   (duplicated because the bootloader subproject cannot link `board`). **Asymmetric
   consequence:** on the ESP32 a drift is a dead rescue button; on the P4 the old value
   (15) is the C6 SDIO D1 line, so the same drift presents as an *intermittent Wi-Fi
   fault* plus phantom button presses — exactly the trap `what_we_learned.md` records as
   having cost time. ~15 lines of host test, same shape as `test_partitions_p4.c`.
5. `components/cp_ota/cp_ota.c` and `components/fw_srv/fw_srv.c` each parse the same
   on-flash HGFW format with their own parser, magic constant and `rd32()`. They agree
   today; `hg_blob` is the natural single home. Not consolidated during the migration
   because it touches the working zone OTA path.
6. `components/http_srv/http_api_cfg.c` — `cfg_put_zone0()` still hand-rolls the
   master-config read-modify-write that `components/mcfg_ops` centralised, and the two
   have diverged (100 ms vs 6000 ms lock budget, no per-op commit log, duplicated -1/-2
   mapping, six unlock sites). The false comment claiming otherwise is fixed; the
   refactor is not. Left alone because it touches a working web path.
7. `components/cp_ota/cp_ota.c` — "came back on old firmware" and "never came back" print
   the same stale `last seen` value. A `get_chip_id()` read would separate them.
8. `components/cp_ota/cp_ota.h` — the stated reason for having no `CP_OTA_HOST_VERSION`
   compile-time assertion (that `esp_hosted` does not expose its version through its
   public `INCLUDE_DIRS`) is **inaccurate**: `eh_common_caps.h` is reachable, proven by
   `cp_ota.c` compiling with 32 `esp_hosted` include dirs on a real P4 build. Add the
   assertion or correct the comment.
9. `components/cp_ota/cp_ota.c` — the link-up predicate comment overstates itself: four
   `return -1` paths after the chip-id TLV is stored leave `s_chip_id` set, so it means
   "a chip-id TLV arrived", not "the init event was accepted".
10. `tools/flash_app.py` — staged HGFW images are written into `build_dir("master")`
    without the `--build-dir` override, so with an out-of-tree P4 build the scratch
    `hg_cpfw.bin` / `hg_zonefw.bin` still land in `master/build`. Harmless (each is flashed
    from where it was written), but it is the confusion `--build-dir` exists to prevent.
11. `tools/flash_all.py` — a zero-byte **rescue** image reports "too short to be an ESP app
    image" rather than a dedicated zero-bytes message, because the rescue slot bypasses
    `require_file()`, which now has one.
12. `tests/host/test_mcfg_ops.c` shares static state across `RUN_TEST` cases, so case order
    matters. A `setUp()` reset would remove the coupling.
13. `mcfg_ops` is compiled but not linked for `zone` and `rescue`, because IDF builds every
    `components/` entry for every app. Harmless in itself — **but it is the same behaviour
    that broke the rescue app** (`alarm_mgr` was processed for `rescue` and pulled an
    unresolvable `cjson`). A per-app component allowlist would retire both.
14. `bootloader_components/main/bootloader_start.c:13` — the header comment still says
    "(esp32 target)" a few lines above the both-targets paragraph.
15. `docs/superpowers/specs/2026-08-31-hillgrow-system-design.md` (~line 66) still documents
    only the ESP32 `0xE000` partition-table offset. The P4 uses `0xF000`.
16. `tools/flash_all.py` — the `Usage:` line in the module docstring omits `--target`,
    `--build-dir` and `--dry-run`. `flash_app.py`'s was fixed. Cosmetic, but it is the
    owner-facing help for the only sanctioned flash path.

## Owner housekeeping

- **Delete `C:\Projects\hillgrow-p4-spike`.** Safe: `coproc/` is promoted and rebuilds to
  the identical 1,145,984-byte binary, the panel findings are promoted, a sweep found
  nothing else of value, and no repo file references the path. Left to the owner because it
  is an irreversible delete outside git.
- **Stamp the drawio.** The plan's Task 9 asked for a `bench-verified` stamp on the
  "Master v2 (P4) Topology" page. It was removed from the task because
  `docs/hillgrow-features.drawio` had uncommitted owner edits throughout.
