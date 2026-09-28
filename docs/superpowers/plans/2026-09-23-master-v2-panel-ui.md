# Master v2 Panel UI Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give the Master v2 (ESP32-P4) a native LVGL UI on its 7" touch panel with full functional parity with the SP4 web UI, plus a home screen with the clock as its hero and panel-local settings. Both faces go through one shared service layer beneath HTTP.

**Architecture:** There are three layers.
- **`panel_svc`** is a new shared component that both faces call. It holds the master-config read-modify-write, the net ops moved out of `master/main`, the shared zone-config edit, the state gather extracted from `h_state`, and the firmware-install core. The web handlers become thin HTTP mappers over it.
- **`panel_ui`** is the LVGL face, built only for the P4 master. It has its own BSP bring-up and one worker task that runs every write and command. A 1 Hz poller publishes snapshot copies, so the LVGL task never blocks. It also holds the screens and a set of pure helpers with no LVGL or IDF dependency, which the MSVC host suite tests.
- **`http_auth`** is the web-session store, split out of `http_srv` so that `panel_svc` can sit beneath `http_srv` without a dependency cycle.

The work is delivered in six benchable stages:
1. Foundations.
2. Shell and read-only views.
3. The generated config editor.
4. System functions and panel settings.
5. Firmware from microSD.
6. Owner acceptance.

**Tech Stack:**
- ESP-IDF 6.0.1 on ESP32-P4 rev v1.3 (RISC-V), with esp_hosted 3.0.7 and esp_wifi_remote reaching the on-board ESP32-C6 over SDIO slot 1.
- The BSP `waveshare/esp32_p4_wifi6_touch_lcd_7b` ==3.0.1.
- `lvgl/lvgl` ==9.5.0 through `espressif/esp_lvgl_adapter` ==0.6.4, with `esp_lcd_touch_gt911`.
- FATFS over SDMMC slot 0, FAT32 only.
- C11. Unity 2.6.0 host tests under MSVC `cl`, driven by CMake and CTest.
- Python 3 bench tools: `web_test.py`, `uart_test.py`, `flash_app.py` and `flash_all.py`.
- `lv_font_conv` 1.5.2, used once to generate the clock font.

**Spec:** C:\Projects\HillGrov\docs\superpowers\specs\2026-09-18-master-v2-panel-ui-design.md (binding). Above it, `docs/superpowers/specs/2026-08-31-hillgrow-system-design.md` is also binding: §11.10 absorbs the display node into the P4 kit, §3.3 forbids aborting, §3.9 covers the watchdog and §6.4 the heap budget. The following approved but unplanned designs must not be contradicted:
- `docs/superpowers/specs/2026-09-23-p4-master-recovery-design.md`
- `docs/superpowers/specs/2026-09-23-c6-self-validating-rollback-design.md`

**Where the tree stands (read at `aceb311`, 2026-09-23; rechecked 2026-09-28):**
- The spec's *Architecture* paragraph describes the tree before the Master v2 migration. Migration Task 5 has since moved the master-config lock and `mcfg_ops_edit()` into `components/mcfg_ops`. Tasks 3 to 5 do only what is still left, so Task 3 is not "already done":
  - remove the two `extern` reach-throughs (`http_api.c:27`, `http_login.c:130`);
  - move `master/main/net_ops_master.c` into a component;
  - replace the hand-rolled `cfg_put_zone0` (`http_api_cfg.c:204-249`, follow-ups item 6);
  - give both faces one zone-config edit.
- The spec says "32 existing tests". There are **35** CTest entries today (`tests/host/CMakeLists.txt:36-101`, one per test file). Every count in this plan starts from 35.
- The panel spike (`C:\Projects\hillgrow-p4-spike`) is gone; its findings live in `docs/what_we_learned.md` ("2026-09-18 — Master v2 P4 panel bring-up"). Nothing in this plan reads it. Where a task needs a BSP or adapter identifier, it re-reads the header in `master/managed_components/` after a GATE-P4 configure.

**Owner answers relayed on 2026-09-28** (they change no default):
- *Spike removed.* Confirmed: the path no longer exists. Task 34 marks the housekeeping item done.
- *"Stamp the drawio?"* It means adding a bench-verified stamp to the title of the "Master v2 (P4) Topology" page of `docs/hillgrow-features.drawio`, in the style the SP3 page already carries ("— SP3 (bench-verified 2026-09-04, 3-board ring)"). A suggested text is "— Master v2 (bench-verified 2026-09-22, P4 master + 2 zones)"; system spec §11.10 records 2026-09-22 as the hardware verification date. It is an owner-only edit, because that file holds the owner's uncommitted work. No task in this plan touches it; Task 34 writes the explanation into the follow-ups.
- *"Should httpd be watched by the task watchdog?"* The owner answered "no idea". Decision D5's default therefore stands: `httpd` and the LVGL task stay unsubscribed, the LVGL task beats a 1 s heartbeat, and the poller raises the active alarm `NOTIFY ALARM 0 W_PANEL_FROZEN ...` after 10 s without one. Task 34 records the decision as still open, together with the acceptance evidence.

## Global Constraints

Every task's requirements implicitly include this section.

### The spec's binding rules (verbatim)

- **Parity:** "full functional parity with the SP4 web UI: dashboard, per-zone view with console, schema-driven config editing for every zone and the master, alarms, and system functions (Wi-Fi, time, web password, firmware, fleet, reboot). Plus the home screen and panel-local settings."
- **Audio:** "The Audio icon is a placeholder in this sub-project." "The home screen reserves its place and the icon opens a screen saying so."
- **No panel auth (Decision 3):** "No authentication on the panel. Physical access is the gate." "Anyone standing at it can change the web password without knowing the old one."
- **Secrets:** "secret fields show masked with a deliberate reveal action. Absence of a login is not a reason to publish secrets to the room."
- **Shared service layer (Decision 4):** "Shared service layer, not loopback HTTP and not unguarded direct calls. Both faces call the layer beneath HTTP."
- **THE RULE:** "The LVGL task must never make a blocking call." "every write and every command is handed to a panel worker task; the UI shows a pending state and updates on completion."
- **Config generated:** "The same field tables that feed `/api/schema` (`hg_cfg_fields`, `hg_mcfg`) drive an LVGL editor, so every zone, shelf and master field — including ones added later — comes from one generator. Fields render by type, not as text boxes: numbers get steppers bounded by the table's own min/max, booleans get switches, enums get segmented controls or rollers, and only genuinely free-text fields (name, SSID, timezone, MAC) summon the keyboard. Same fields and same validation as the web".
- **Firmware:** "Firmware comes from the microSD slot." "The board's microSD sits on SDIO slot 0 while the C6 sits on slot 1".
- **Status band:** "One tile per enrolled zone, built from `state_snap`'s live node list — never a fixed count." "1 to `HG_MAX_ZONES` (8) all look deliberate". "Tapping a tile opens that zone."
- **Alarms:** "Alarms turn the band red and pulse; tapping opens Alarms. No full-screen takeover except SAFE mode". SAFE mode does not exist yet (the system spec's §3 safety layer is unbuilt), so the panel has no takeover at all.
- **Night dimming:** "The panel knows the light schedule and dims when the lights go off, waking on touch."
- **Naming:** "The third icon is Panel, not "Settings": system settings (Wi-Fi, time, password, firmware, fleet) live inside the HillGrow app exactly as they do on the web, and Panel holds display-local preferences — brightness, dim schedule, clock face (including an analogue option), orientation, about."
- **Navigation:** "Five destinations, matching the web UI: Dashboard, Zone (with console), Config, Alarms, System." "Navigation is a persistent left rail".
- **Data flow:** "Poll `state_snap` about once a second, diff against what is displayed, update only what changed." "UI thread validates locally against the field table, hands the change to the worker, shows pending. Worker calls `panel_svc`, which applies the same guards and the same `hg_cfg_validate` the web path uses."
- **Errors:** "Failures reuse the web UI's semantics so a refusal means the same thing in both faces: busy (a save already in flight for that zone), validation (with the offending field named and highlighted, as the web does), zone offline, and master-config contention." "The clock reports an unset time honestly rather than showing a plausible wrong one".
- **Testing:** host tests for the generator and `panel_svc`. The existing host suite stays green. Bench acceptance with the owner, in the shape of SP4 Task 17, which "must include tapping every destination once on real glass".
- **Non-goals:** no rendering of the web UI. "History and plots remain SP4b". "Remote access stays the web UI's job; the panel is local-only." "The HTTP server keeps running."

### Rules this plan adds so those can hold (each is load-bearing)

- **Reads leave the LVGL task too** (map-svc §0.3). On the P4, `wifi_mgr_status()` makes two esp_hosted RPCs of up to 5 s each. `nmgr_lock` waits forever and is held across NVS writes. So the LVGL task reads only copies:
  - `pnl_poll_latest()`, `pnl_poll_alarms()` and `psvc_mcfg_get()`;
  - pure helpers, `hg_field_read()`, `hg_field_write()` and `hg_cfg_validate()`;
  - `time(NULL)`.
  Every call tagged `[WORKER]` runs only on `pnl_work`, `pnl_poll` or `pnl_wifi`.
- **Display lock.** A `lv_*` call is made only in one of two places:
  - inside an `lv_timer` or event callback on the LVGL task (the adapter's own recursive lock is held there);
  - after `panel_lock(ms)` returned `true`.

  `panel_lock()` always passes a real timeout: 0 is promoted to 1, because `bsp_display_lock(0)` means "try once". Ignoring the result panics the board (`esp_lv_adapter_lock(751)`). Nothing blocking is ever called while the lock is held, and the worker and poller never take it.
- **Tasks:**

  | Task | Core | Priority | Stack | TWDT |
  |---|---|---|---|---|
  | LVGL (adapter) | 1 | 3 | 8 KB internal | not subscribed |
  | `pnl_work` | 0 | 2 | 8 KB internal | not subscribed |
  | `pnl_poll` | 0 | 2 | 6 KB internal | not subscribed |
  | `pnl_wifi` | 0 | 1 | 4 KB internal | not subscribed |

  These stacks are chosen, not measured (`pnl_work` runs FATFS, cJSON merges, the install core and esp_hosted RPCs; `pnl_wifi` runs `wifi_mgr_status()`, which httpd runs on 8 KB). Panel → About shows each one's high-water mark (`pnl_worker_stack_free()`, `pnl_poll_stack_free()`, Tasks 8, 11 and 26); the Stage 3 and Stage 4 gates record them and fail below 1024 bytes free on any of the four, because a FreeRTOS canary overflow panics the controller.

  No TWDT subscription is held across an esp_hosted RPC (recovery design §6.1). The only exception is `psvc_fw_install()`, which subscribes the calling task itself for the bounded flash loop and makes no RPC inside it. For comparison, the adapter's default is prio 6 with no affinity, which ties with `ring_rx` (core 0, prio 6). `node_mgr` is core 1, prio 4.
- **Screen callbacks are short.** Any single build or update callback must finish in ≤ 200 ms: measure it with `esp_timer_get_time()` and log a WARN when it overruns. Both idle tasks are watched by the TWDT. Screens are built lazily: only the open destination's widget tree exists, and it is deleted on teardown.
- **Memory.**
  - Internal RAM is the scarce pool. Measure it with `heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)`: `esp_get_minimum_free_heap_size()` includes about 32 MB of PSRAM and cannot see internal exhaustion. The floor at every gate is internal min ≥ 64 KB.
  - Large panel-side stores go in PSRAM via `heap_caps_calloc(..., MALLOC_CAP_SPIRAM)`: console logs, edit sets, snapshot double buffers and the alarm snapshot.
  - Stacks of tasks that do flash operations, and buffers handed to flash writes, stay internal.
  - LVGL uses its builtin allocator: the internal `CONFIG_LV_MEM_SIZE_KILOBYTES` pool (64 KB) plus a 256 KB PSRAM overflow pool that `panel_hw_start()` adds (Task 7). LVGL's default assert handler is `while(1);` and `LV_USE_ASSERT_MALLOC` is on, so without the overflow pool an exhausted pool would spin the LVGL task, starve IDLE1 and let the TWDT reboot the master; with it, exhaustion only costs speed. The budget stays the internal pool: if `lv_mem_monitor()` `max_used` goes above 75 % of `CONFIG_LV_MEM_SIZE_KILOBYTES` (48 KB at 64) at any gate, raise `CONFIG_LV_MEM_SIZE_KILOBYTES` to 128 in `master/sdkconfig.defaults.esp32p4`, but only if internal min stays ≥ 64 KB. The panel's pool lines print `max used N of M KB internal`, M being `CONFIG_LV_MEM_SIZE_KILOBYTES`.
- **UI text** uses printable ASCII plus the only extra glyphs the built-in Montserrat carries: U+00B0 °, U+2022 • and the `LV_SYMBOL_*` icons. The web's "—" (U+2014) and "·" (U+00B7) render as nothing, so the panel uses "--" and " | ". A host test pins this for every string that `pnl_fmt` and `pnl_msg` build.
- **Saves.**
  - A save applies only the **dirty set**, and applies it on the worker to a **fresh copy** taken at save time. It is never a whole edited struct: the web PUT has no generation check, so writes are last-writer-wins per field.
  - Validation uses `hg_cfg_validate(cfg, hw_present ? &hw : NULL, ...)`. That hw_present rule (`http_api_cfg.c:132-152`) is load-bearing both ways.
  - The panel never writes the hardware plane (HW, HWSHELF, CAL).
- **Secrets.**
  - A blank secret means "unchanged". A password therefore cannot be cleared from the panel; an open STA is set by clearing the SSID.
  - Secrets are never logged.
  - A revealed secret re-masks after 10 s, when its screen is torn down, and on the idle wipe.
  - Secret text buffers are wiped after use with `pnl_zero()` (`pnl_input.h`, Task 19: volatile stores), never a plain `memset`: the P4 master builds with `-Os`, and GCC drops a `memset` on a local that is never read again as a dead store.
- **Command session.** Panel command lines run on their own sessions with web semantics: `cmd_session_t { .source = CMD_SRC_HTTP, .echo = 0, .notify_mask = 0, .unlock_until_ms = 0 }`. That means `NOT_LOCAL` for `CMDF_SESSION` rows and no `DEBUG ENABLE` unlock from the glass.
  - A line is 1..191 bytes (`CMD_LINE_MAX - 1`). A longer line is refused before it is sent.
  - A `cmd_task_execute()` return of -2 quarantines that slot for good.
  - SSIDs and passwords never travel as CLI lines, because a line cannot carry spaces. The panel calls the `psvc_*` functions for those.
- **Time.** The master's clock and `SET TIME` are UTC: nothing calls `setenv("TZ")` or `tzset`. The panel's local time is `time(NULL) + time_svc_utc_offset()`. While `hg_app_time_is_set()` is 0, the clock shows `--:--` with "Clock not set", and the schedule context and dimming by schedule are disabled.
- **Reboot during an OTA trial.** Any reset during a trial retires the new image (recovery design §2.3). Reboot shows a warning and asks for a second confirm while `psvc_state_t.fw_state` is `"PENDING"`. Without a pending trial the rule is the web's: one confirm, and "Rebooting..." is shown optimistically before the reply.
- **The web face keeps its exact observable behaviour** unless a task names a change. That covers status codes, `{"error","path"}` bodies, the tokens the CLI rows answer and the web's lock budgets (100 ms for the zone-0 PUT and the scan). The one change this plan makes: the chip-id check in the shared identity helper (Task 30, decision D22). `tools/web_test.py` and `tools/uart_test.py` are the regression net on the bench, and the host suite is the net off it.

### Proven panel traps (docs/what_we_learned.md "2026-09-18 — Master v2 P4 panel bring-up", plus platform findings verified in source)

- **Use the registry BSP**, `waveshare/esp32_p4_wifi6_touch_lcd_7b` 3.0.1. Never hand-roll an EK79007 init sequence.
- **Touch arrives mirrored 180°.** Display rotation `ESP_LV_ADAPTER_ROTATE_180` with touch flags `{swap_xy 0, mirror_x 0, mirror_y 0}` is the correct mapping. Rotation and touch mirroring are two separate knobs. Never enable the `esp_lcd_touch` mirrors: they do `x_max - x` on a `uint16_t` with no clamp, and a point outside the panel wraps to about 65000. The flipped orientation does its own clamped flip in `pnl_touch_map()`.
- **A failed touch read is invisible.** Install a custom touch read (Task 7 sets it with `lv_indev_set_read_cb()` on the indev `esp_lv_adapter_register_touch()` returns; see §3, "Refinements"). Count and log both `esp_lcd_touch_read_data()` and `esp_lcd_touch_get_data()` return codes plus the point count, rate-limited so UART0 is not flooded.
- **`bsp_display_lock(0)` is "try once", not "forever".**
- **LVGL fonts.** Montserrat ships only at 14, and each size is a separate compiled bitmap. The plan enables 20, 28 and 48, with a default of 20. The clock uses a generated TrueType subset.
- **Benign boot noise.** `W ledc: GPIO 32 is not usable, maybe conflict with others` appears on every boot. Do not chase it.
- **`CONFIG_BSP_ERROR_CHECK` defaults to `y`,** and then any BSP failure aborts the greenhouse controller. Set `CONFIG_BSP_ERROR_CHECK=n` in `master/sdkconfig.defaults.esp32p4` and put `#if CONFIG_BSP_ERROR_CHECK` / `#error` in `panel_hw.c`: `sdkconfig.defaults` silently ignores unknown names.
- **Do not use `bsp_display_start()` or `bsp_display_start_with_config()`.** They call `ESP_ERROR_CHECK(esp_lv_adapter_start())` unconditionally, return NULL for the whole display when touch fails, and initialise LEDC twice. `panel_hw_start()` makes the same public calls with soft errors.
- **Also never use** `bsp_sdcard_mount()`, `bsp_spiffs_mount()`, `bsp_audio_*` or `bsp_usb_host_start()`. esp_hosted owns the single SDMMC controller; the master's partition is `data`, not `storage`; audio is SP7.
- **`bsp_display_brightness_set()` logs at INFO on every call** on UART0, which is the machine-parsed CLI. Brightness is set with `ledc_set_duty()` and `ledc_update_duty()` directly, on `LEDC_LOW_SPEED_MODE` channel `CONFIG_BSP_DISPLAY_BRIGHTNESS_LEDC_CH`, with duty `1023*pct/100`.
- **The adapter's `auto_sleep` stays disabled.** It pauses the LVGL worker, which would freeze the clock. Dimming is not sleeping.
- **microSD:**
  - FAT32 only: IDF 6.0.1 FATFS has no exFAT. `format_if_mount_failed = false`, always.
  - Mount transiently on the worker: mount, read, unmount.
  - Mount only after esp_hosted has created the controller. Use a no-op `host.init` and keep `deinit_p = sdmmc_host_deinit_slot`. Create and delete the LDO-4 power handle for each mount.
- **A human finger on glass** is part of every bench gate that touches input.

### Project rules

- **Toolchain.** Run `& C:\esp\v6.0.1\esp-idf\export.ps1` in PowerShell before any `idf.py`. Read the build output; do not trust the exit code. Build with **0 warnings** (`-Werror` on both targets).
- **GATE-P4** (the P4 master build; the migration's canonical form, with `-C` in place of `Set-Location`):
  - `idf.py -C C:\Projects\HillGrov\master -B C:\Projects\HillGrov\master\build_p4 -DIDF_TARGET=esp32p4 -DSDKCONFIG=C:\Projects\HillGrov\master\build_p4\sdkconfig -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.esp32p4" build`
  - Then `Select-String -Path C:\Projects\HillGrov\master\build_p4\sdkconfig -Pattern 'CONFIG_IDF_TARGET="esp32p4"'`, which must match.
  - Then `git -C C:\Projects\HillGrov checkout -- master/dependencies.lock`.
  - `-DIDF_TARGET=esp32p4` and both absolute paths are mandatory. Never run `idf.py set-target` inside `master/`.
  - After editing `master/sdkconfig.defaults.esp32p4`, delete `master\build_p4\sdkconfig` first: an existing sdkconfig overrides the defaults. Then grep the regenerated file for every key the task set.
- **dependencies.lock (why the checkout).**
  - The component manager rewrites the **project's** `master/dependencies.lock` for whichever target was configured last, whatever `-B` says (commit `ee83c0b`). The committed lock is the ESP32 resolution.
  - So no committed file ever records a P4 dependency version. The manifest pin in `master/main/idf_component.yml` (`rules: - if: "target == esp32p4"`) is the only guard, which is why the BSP, lvgl and the adapter are pinned **exactly**.
  - Any manifest edit changes the ESP32 lock's `manifest_hash`. Regenerate it with an ESP32 `idf.py -C C:\Projects\HillGrov\master reconfigure` and commit it **in the same commit** as the manifest. Then prove the pair consistent: a second reconfigure leaves `git diff` empty (the `eeb3c33` lesson).
  - An ESP32 build purges P4-only packages from the shared `master/managed_components/`. The next P4 configure re-extracts about 18 of them.
- **GATE-ESP32** (the ESP32 master is still maintained: it is the DevKitC fallback, and the migration made it a standing gate):
  - `idf.py -C C:\Projects\HillGrov\master build`, then `idf.py -C C:\Projects\HillGrov\zone build`, then `idf.py -C C:\Projects\HillGrov\rescue build`. Each must print "Project build complete".
  - Then `git -C C:\Projects\HillGrov diff --stat -- master/dependencies.lock`, which must print nothing.
  - Run it after every task that touches `components/`, `master/` or `cmake/`.
- **GATE-HOST:**
  - `$env:PATH = "C:\Espressif\tools\cmake\4.0.3\bin;" + $env:PATH`
  - then `cmd /c 'call "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul && cmake -S C:\Projects\HillGrov\tests\host -B C:\Projects\HillGrov\build\host -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=cl && cmake --build C:\Projects\HillGrov\build\host --config Release --parallel && ctest --test-dir C:\Projects\HillGrov\build\host -C Release --output-on-failure'`
  - Add `-R <test>` to run one test.
  - The same suite runs automatically as a dependency of every master and zone ELF (`cmake/hillgrow.cmake:10-26`). Never use `-DHILLGROW_SKIP_HOST_TESTS=ON` in a gate.
  - CTest reports **one entry per test file**. A new file adds a `hg_test(test_<name> <srcs...>)` row, plus the component's include dir at `tests/host/CMakeLists.txt:21-27`.
- **Pure files.** Pure helpers include only `<std*.h>` and HillGrow pure headers. They never include `lvgl.h`, `bsp/*.h`, `esp_*.h` or `freertos/*.h`: that is the `state_snap.h:36-38` precedent. LVGL-typed headers are glue: `panel_hw.h`, `pnl_theme.h`, `scr_*.h` and `wdg_*.h`. Files stay ≤ about 300 lines. Wire and persisted data is packed by explicit byte offset inside a `hg_blob` envelope.
- **Flash.** Flash only with the tools, dry run first:
  - `python C:\Projects\HillGrov\tools\flash_app.py --app master --target esp32p4 --build-dir C:\Projects\HillGrov\master\build_p4 --port COM28 --dry-run`. Every printed path must say `build_p4`.
  - Then the same command without `--dry-run`.
  - Or `flash_all.py --board master --target esp32p4 --build-dir ...` for a full layout.
  - **Never `idf.py flash`.**
- **Bench.**
  - The P4 master's USB-UART is COM28. Its AP is `HillGrow` / `hillgrow1` at 192.168.7.7. The web password default is `hillgrow1`; use the bench's current one if it differs.
  - Run web tests with `python C:\Projects\HillGrov\tools\web_test.py 192.168.7.7 --password hillgrow1 [--only SUITE,...]` and CLI tests with `C:\Python311\python C:\Projects\HillGrov\tools\uart_test.py COM28 --role MASTER`.
  - One foreground process holds a COM port and does the triggering: background captures cannot open ports here.
  - Before reading a timeout as a firmware fault, check `netsh wlan show interfaces`.
- **Commits.**
  - Commit directly to `main` with explicit paths only: `git -C C:\Projects\HillGrov add <path> <path> ...`.
  - **Never** stage `docs/hillgrow-features.drawio` (the owner's uncommitted edits). **Never** `git add docs/`: stage `docs/<file>` by explicit path.
  - A file moved with `git mv` is staged by its **new** path only. `git mv` already staged the rename; the old path is in neither the index nor the working tree, so naming it makes the whole `git add` fail (`fatal: pathspec ... did not match any files`) and stage nothing (Tasks 2, 4 and 30).
  - Use conventional messages, `type(scope): summary`. End each with the executing session's attribution trailer (today: `Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>`).
  - Push after each task's gates are green.
- **Recovery-design compatibility** (approved, not yet planned):
  - `panel_start()` sits **immediately after `ota_trial_start(1)` and before the first radio call**. Today that is `wifi_mgr_start()`; later it will be the explicit `esp_hosted_init()` of recovery §6.2 step 3.
  - `panel_services_start()` sits **after `node_mgr_start()` and before the `cp_ota_sync()` gate**.
  - Anchor both by call name, never by line number.
  - The panel never calls `cp_ota_sync()` or any `esp_hosted_*` function.
  - The trial predicate is `ota_trial_running_on_trial()` (recovery §2.3), never a third copy. Image identity is the shared helper (Task 28), never a fork.
  - `RESCUE CONFIRM` (§6.3) reaches the panel only through its console. There is no panel button for it.
  - Panel NVS writes tolerate "writes disabled" (§6.5): the default then stays in RAM.
  - The panel is not part of `ota_trial_drivers_ok()`.
- **No panel in rescue.** `panel_ui` compiles sources only when `CMAKE_PROJECT_NAME STREQUAL "hillgrow_master"` **and** the target is `esp32p4`. `rescue_p4` (project `hillgrow_rescue_p4`) and `rescue/` get header-only stubs. P4-only managed components attach only through `idf_component_optional_requires(PRIVATE ...)` after `idf_component_register()` (the `cp_ota` and `wifi_mgr` idiom). A plain `REQUIRES` on them breaks the zone, rescue and ESP32 builds. A `REQUIRES` gated on `CONFIG_*` or on the project name silently comes out empty.

---

## 1. File structure

Legend:

| Mark | Meaning |
|---|---|
| (new) | new file |
| (mv) | `git mv` with edits |
| (mod) | modified |
| (del) | deleted |
| P | pure: host-compiled, no LVGL/IDF headers |
| G | target glue |

### `components/http_auth/` — the web-session store, master-only sources (Task 2)
- `CMakeLists.txt` (new): sources only for `hillgrow_master`. `REQUIRES web_auth hg_mcfg`; `PRIV_REQUIRES freertos esp_hw_support nvs_flash mbedtls app_common time_svc`.
- `http_auth.h` (new): the seven `http_auth_*` entry points, moved out of `http_srv.h` and `http_srv_internal.h`.
- `http_auth.c` (mv from `components/http_srv/http_auth.c`): unchanged logic. It includes `http_auth.h` and `web_auth.h` in place of `http_srv_internal.h`.

### `components/panel_svc/` — the shared service layer both faces call; sources only for `hillgrow_master` (both targets)
- `CMakeLists.txt` (new, Task 3; extended by Tasks 4, 5, 9, 25 and 30).
- `psvc_rc.h/.c` (new, P, Task 3; extended by Task 25): the refusal vocabulary, its tokens, and the mapping to the net_ops legacy convention and the fleet codes.
- `psvc_edit.h` (new, P, Task 3): `psvc_fedit_t`, the one field-edit record, used for zone and master edits by both faces.
- `psvc_mcfg.h/.c` (new, P-over-mcfg_ops, Task 3; extended by Task 33): the one canonical master-config read-modify-write (`psvc_mcfg_edit`), the web JSON function and the panel field-edit function.
- `psvc_net.h/.c` (mv from `master/main/net_ops_master.{h,c}`, G, Task 4): the production `net_ops_t`, `master_web_set_password()`, the panel-facing setters with BUSY split out, and the scan under the lock.
- `psvc_zcfg.h/.c` (new, P-over-node_mgr, Task 5): the shared zone-config edit (`psvc_zone_cfg_edit`), plus the JSON and field-edit functions.
- `psvc_state.h` (new, Task 9) with `psvc_state.c` (G) and `psvc_state_conv.c` (P): the owning snapshot struct, the gather extracted from `h_state`, the conversion to `snap_master_t`, and the text parsers.
- `psvc_fw.h/.c` (new, P, Task 9; the core is added in Task 30): upload-progress publication, and the firmware-install core behind an injected environment.
- `psvc_fw_env.c` (new, G, Task 30): the production environment (claim, fleet check, heap, TWDT, yield, `zone_fw` writer claim), `psvc_fw_busy()` (the fleet gate), and `psvc_fw_install()` (default environment plus sink, a wrapper over the pure `psvc_fw_install_with()`).
- `fw_sink_master.c` (mv from `components/http_srv/http_upload_master.c`, G, Task 30): the inactive OTA slot sink.
- `fw_sink_zone.c` (mv from `components/http_srv/http_upload_zone.c`, G, Task 30): the `zone_fw` sink, whose watchdog resets are subscription-aware.
- `psvc_fleet.h/.c` (new, G, Task 25): fleet start and abort with the shared code mapping.

### `components/hg_image/` — pure image identity, shared with the recovery design (Task 28; skipped if already landed)
- `CMakeLists.txt`, `hg_image.h`, `hg_image.c` (new, P): magic, chip id, app-descriptor magic, version and project name, with the four project-name constants.

### `components/panel_ui/` — the panel face; sources only for `hillgrow_master` on `esp32p4`
Infrastructure (G):
- `CMakeLists.txt` (new, Task 6): the gating idiom, `optional_requires` on the BSP, `lvgl__lvgl`, `espressif__esp_lvgl_adapter` and `espressif__esp_lcd_touch`, and `LV_LVGL_H_INCLUDE_SIMPLE`.
- `panel_ui.h/.c` (new, Task 6): `panel_start()` and `panel_services_start()`, the only two symbols `app_main` sees.
- `panel_hw.h/.c` (new, Task 7): adapter and BSP bring-up with soft errors, touch diagnostics, brightness through LEDC, and the I²C pin assertion.
- `panel_lock.h/.c` (new, Task 7): the display-lock wrapper that never ignores the result.
- `pnl_palette.h` (new, P, Task 7): the web's dark palette as hex constants.
- `pnl_theme.h/.c` (new, Task 7): the LVGL theme from the palette, and colours per health state.
- `pnl_worker.h/.c` (new, Task 8): the worker task, job pool, result mailbox and the `lv_timer` that drains it.
- `pnl_poll.h/.c` (new, Task 11; alarms added in Task 16, command quarantine in Task 22): the 1 Hz poller, the 5 s Wi-Fi status task, the double-buffered snapshot and the LVGL heartbeat check.
- `pnl_cmd.h/.c` (new, Task 22): the panel's own command sessions, with web semantics and quarantine.
- `pnl_prefs_nvs.h/.c` (new, Task 26): panel preferences in NVS `"panel"/"prefs"`.
- `pnl_idle.h/.c` (new, Task 27): the dimming timer, the wake-on-touch guard and the idle wipe with return to Home.
- `pnl_sd.h/.c` (new, Task 31): transient microSD mount, the `.bin` listing and the reader.
- `font_clock_180.c` (new, generated, Task 13) and `pnl_fonts.h` (new, Task 13). Only if route D2(b) is taken instead: `font_clock.ttf` (a fontTools subset) and `pnl_fonts.c` (`lv_tiny_ttf`) replace `font_clock_180.c`.
- `pnl_ui_kit.h/.c` (new, Task 22): the card, row, button, field, message-label and confirm-dialog helpers every System, Panel and card section is built from.

Screens and widgets (G):
- `scr_shell.h/.c` (new, Task 12): the destination registry, left rail, navigation, the 250 ms update timer and `pnl_label_set_if_changed()`.
- `scr_diag.h/.c` (new, Task 7; extended by Tasks 8 and 12): the touch test and the worker proof.
- `scr_home.c` (new, Task 13; analogue face added in Task 26): clock hero, date, context line, status band, alarm band and the three icons.
- `scr_audio.c` (new, Task 13): the SP7 placeholder.
- `scr_dashboard.c` (new, Task 12; panel quarantine line added in Task 22).
- `scr_zone.h/.c` (new, Task 14; console and replace sections added in Task 22). The header exports `scr_zone_extra_area()` and `scr_zone_current()`.
- `zone_sections.h`, `zone_console.c`, `zone_replace.c` (new, Task 22). The header declares the two sections' build, teardown and wipe functions.
- `scr_alarms.c` (new, Task 16).
- `scr_config.h/.c` (new, Task 20; zone-0 routing added in Task 21, card actions in Task 33): the editor host (picker, the shared editor frame `cfg_frame_*` with tabs, index selector and save bar, error locate).
- `cfg_zone.c` (new, Task 20), `cfg_master.c` (new, Task 21), `cfg_card.c` (new, Task 33).
- `wdg_field.h/.c` (new, Task 19): one widget per `pcfg_kind_t`.
- `wdg_keyboard.h/.c` (new, Task 19): modal keyboard and keypad with filters, length bounds and masking.
- `scr_system.h/.c` (new, Task 23): the System section registry.
- `sys_wifi.c`, `sys_time.c` (new, Task 23), `sys_password.c` (new, Task 24), `sys_fleet.c` (new, Task 25; fleet and reboot), `sys_firmware.c` (new, Task 32).
- `scr_panel.c` (new, Task 26): brightness, dimming, clock face, orientation, About and the touch test link.

Pure helpers (P, host-tested):
- `pnl_touch.h/.c` (Task 7): the orientation mapping with clamping.
- `pnl_time.h/.c` (Task 10): local time, clock and date text, and the `SET TIME` line from local time.
- `pnl_fmt.h/.c` (Task 10): zone names, health words, Wi-Fi lines, ages, readings and master time text.
- `pnl_home.h/.c` (Task 10): band layout, schedule extraction, lights-on test and the context line.
- `pcfg_gen.h/.c` and `pcfg_pres.h/.c` (Task 17): widget specs from field rows, the presentation table, stepping, formatting and path locating.
- `pcfg_edit.h/.c` (Task 18): the dirty set per zone and its export (blank secrets dropped).
- `pnl_msg.h/.c` (Task 18): refusal and outcome text for each context, with the web's wording where the web has it.
- `pnl_input.h/.c` (Task 19): keyboard character filters, text bounds and MAC parsing.
- `pnl_console.h/.c` (Task 22): forward rewrite, 20-entry log, history and wipe.
- `pnl_prefs.h/.c` (Task 26): preference defaults and pack/unpack in a `hg_blob` envelope.
- `pnl_dim.h/.c` (Task 27): the night and brightness decision.
- `pnl_sd_pick.h/.c` (Task 31): firmware file classification and ordering.

### Modified outside the new components
- `components/http_srv/`:
  - `CMakeLists.txt` (Tasks 2, 4, 28, 30).
  - `http_srv.h` (Task 2): auth declarations replaced by `#include "http_auth.h"`.
  - `http_srv_internal.h` (Task 2): the facade block removed.
  - `http_api.c`:
    - Task 4: extern removed; scan goes through `psvc_wifi_scan`.
    - Task 9: `h_state` goes through `psvc_state`.
  - `http_login.c` (Task 4): extern removed.
  - `http_api_cfg.c` (Tasks 4 and 5): both PUTs go through `panel_svc`; the GET for zone ≥ 1 goes through `psvc_zone_cfg_get`.
  - `http_srv.c`:
    - Task 9: registers the web quarantine hook.
    - Task 30: the fleet gate moves to `app_main`.
  - `http_upload.c`:
    - Task 9: progress goes through `psvc_fw`.
    - Task 30: becomes the HTTP adapter over `psvc_fw_install`.
  - `http_upload.h` (Tasks 9 and 30).
  - `http_fleet.c` (Task 25).
  - `http_upload_master.c`, `http_upload_zone.c`: mv to `panel_svc` in Task 30. Before that, Task 28 edits the trial check in `http_upload_master.c`.
- `components/mcfg_ops/mcfg_ops.h/.c` (Task 3): `mcfg_ops_edit_ms()`.
- `components/alarm_mgr/alarm_mgr.h/.c/_json.c/_internal.h` (Task 15): lock hooks, public view types, `alarm_mgr_copy()`.
- `components/hg_cfg/hg_cfg.h`, `hg_cfg_fields.c` (Task 17): `hg_group_is_hw()`.
- `components/hg_json/hg_json.h`, `hg_json_mcfg.c` (Task 33): `hg_json_merge_mcfg_opts()`.
- `components/state_snap/state_snap.h/.c` (Task 10): `state_snap_health_name()` and `state_snap_ring_state_name()` exported, replacing the private statics.
- `components/ota_trial/ota_trial.h/.c` (Task 28): `ota_trial_running_on_trial()`.
- `components/fw_srv/fw_srv.h/.c` (Task 29): the writer claim and a separate validation buffer.
- `components/board/board.h` (Task 7): P4 `HG_GPIO_I2C_SDA 7`, `HG_GPIO_I2C_SCL 8`.
- `master/main/`:
  - `CMakeLists.txt` (Tasks 2, 4, 6).
  - `cmd_table_master.c` (Task 4).
  - `app_main.c`:
    - Task 6: panel calls.
    - Task 15: alarm lock hooks.
    - Task 28: trial predicate.
    - Task 30: fleet gate.
  - `idf_component.yml` (Task 6).
  - `net_ops_master.{c,h}` (del by mv, Task 4).
- `master/dependencies.lock` (Task 6: ESP32 `manifest_hash` only). `master/sdkconfig.defaults.esp32p4` (Task 6; Task 13 adds `CONFIG_LV_USE_TINY_TTF=y` only on route D2(b)).
- `tests/host/`:
  - `CMakeLists.txt` (every task that adds a test).
  - `test_mcfg_ops.c` (Task 3), `test_state_snap.c` (Task 10), `test_alarm_mgr.c` (Task 15), `test_hg_cfg_fields.c` (Task 17), `test_hg_json.c` and `test_psvc_mcfg.c` (Task 33).
  - New `fakes/fake_apply.{c,h}` (Task 3) and `fakes/fake_nmgr_cfg_api.{c,h}` (Task 5).
  - New test files, one CTest entry each: `test_psvc_rc`, `test_psvc_mcfg` (Task 3), `test_psvc_zcfg` (Task 5), `test_pnl_touch` (Task 7), `test_psvc_state` (Task 9), `test_pnl_time`, `test_pnl_fmt`, `test_pnl_home` (Task 10), `test_pcfg_gen` (Task 17), `test_pcfg_edit`, `test_pnl_msg` (Task 18), `test_pnl_input` (Task 19), `test_pnl_console` (Task 22), `test_pnl_prefs` (Task 26), `test_pnl_dim` (Task 27), `test_hg_image` (Task 28), `test_psvc_fw` (Task 30), `test_pnl_sd_pick` (Task 31): 35 today, 53 after Task 31.
- `tools/web_test.py` (Task 1): the MCFG suite.
- `docs/pin-mapping.md` (Task 7: line 72). `docs/what_we_learned.md`, the spec's status line, and `docs/superpowers/plans/2026-09-21-master-v2-followups.md` (Task 34). Each is staged by explicit path.

---

## 2. Stages

The gate lists below are summaries. The full bench gate for each stage, with its setup, pass and fail criteria, closes that stage in §4 and governs where the two differ.

**Moves against the brief, and why:**
1. **The Audio placeholder moves to Stage 1.** It is one of the home screen's three icons, and a home screen with a dead icon cannot pass its gate.
2. **Firmware from microSD becomes its own Stage 4, and config export/import joins it.** There are two reasons. First, it must consume the recovery design's shared trial predicate and image-identity helper rather than fork them (Task 28 lands them only if the recovery plan has not). Second, it rewrites the bench-verified web upload path and `fw_srv`'s single-task concurrency contract, which deserves its own gate. Export and import need the same SD layer. Fleet stays in Stage 3: it pushes whatever `zone_fw` already holds, and has no SD dependency.
3. **Reads leave the LVGL task, not only writes.** So the poller (Task 11) comes before any screen with data.
4. **The state-gather extraction (Task 9) and the `alarm_mgr` lock (Task 15) sit in Stage 1** next to their first consumers, not in Stage 0. Each touches a web path and is gated by its own web_test suite.
5. **Owner acceptance is its own Stage 5.**

### Stage 0 — Foundations: the shared layer exists and the panel lights on the real master
Tasks 1-8.

**Bench gate (owner, about 40 min; P4 master on COM28, both zones on the ring):**
1. **Before flashing**, with the master still on its current image: `web_test.py ... --only mcfg` is all PASS. This proves the new suite describes today's firmware.
2. FLASH-P4, dry run first.
3. Boot:
   - the panel lights and shows the diagnostics screen (version, internal heap free/min);
   - the boot log shows `panel: up` with LVGL on core 1 at prio 3;
   - there is no flood of `ESP32_P4_EV` INFO lines.
4. With a finger, tap the four corner targets and the centre target. The target under the finger lights up, never the opposite one. The counters show reads > 0 and errors 0.
5. THE RULE: tap "Blocking job (3 s)". The spinner keeps turning, then the screen reports "done after ~3000 ms" with at least 150 UI ticks counted during the job.
6. Full `web_test.py` (all standard suites, including MCFG and LOGIN) passes. The MCFG suite changes the web password briefly and restores it, so phones get logged out. `uart_test.py COM28 --role MASTER` passes. A console `SET TZ <current tz>` answers `OK TZ ...`.
7. Record internal heap free/min before and after `panel_start`, and the image size against the 4 MB slot.

### Stage 1 — Shell and read-only views
Tasks 9-16.

**Bench gate:**
1. Home shows the local time; compare it with a phone.
   - When NTP is absent and no `SET TIME` has been sent, it shows `--:--` and "Clock not set".
   - After a console `SET TIME <UTC>`, it shows the correct local time.
2. The context line matches zone 2's LIGHT ON/OFF.
3. The status band shows exactly the enrolled zones.
4. Hold one zone in reset:
   - its tile goes DEGRADED at about 5 s, then OFFLINE at about 10 s;
   - the band turns red and pulses;
   - tapping the band opens Alarms, showing the blame line with ages that match the web's alarms page;
   - on release the zone returns to ONLINE.
5. Tap each destination: HillGrow → rail → Dashboard, Zone (both zones), Alarms, Config and System (both placeholders at this stage), Home. Dashboard, Zone and Alarms match the web field for field. Audio opens the SP7 placeholder. Panel opens the touch test.
6. `web_test.py --only state` passes while the panel runs. Record internal min and `lv_mem` max.

### Stage 2 — The type-driven config editor
Tasks 17-21.

**Bench gate:**
1. Config for zone 2:
   - tabs appear in the web's order;
   - HW, HWSHELF and CAL are read-only, with hex addresses and "none" for unused pins.
2. WATER TARGET: set 55 with the stepper → Save → "Queued, pushing to zone" → master console `GET ZONE 2 WATER 1` shows `Target : 55` → restore 61.
3. Set shelf 1 LIGHT OFF equal to LIGHT ON → Save. The result is VALIDATION, and the OFF field is highlighted on the LIGHT tab, shelf 1.
4. Input bounds: the NAME keyboard offers no space, and no stepper passes its min or max.
5. Hold zone 2 until it is OFFLINE, then Save. The panel says "Zone is offline -- nothing was saved", not "busy".
6. Save on the panel, then immediately PUT the same zone from the web. The web gets 409 BUSY.
7. Master config:
   - change NTP → "Saved.", and a web GET shows the new value;
   - change HOSTNAME and save; STA stays joined, so the blank STA_PASS kept the stored password;
   - Reveal shows the password and re-masks it after 10 s.
8. A zone that never synced shows "Zone config not adopted yet ..." with Retry.

### Stage 3 — System functions, panel settings, night dimming
Tasks 22-27.

**Bench gate:**
1. Zone 2 console:
   - `GET ID`, then forward on → `GET WATER 1` is answered by zone 2;
   - history ▲▼ works;
   - a line of 192 or more characters is refused locally.
2. Replace board: a bad MAC → "Enter a MAC like aa:bb:cc:dd:ee:ff". Zone 2's own MAC → the reply line verbatim.
3. Wi-Fi:
   - Scan → pick → Join → "Saved -- joining..." → the STA line shows an IP;
   - Set AP shows the drop warning. Changing the AP and back is optional.
4. Time:
   - a TZ change shifts the clock;
   - with NTP absent, "Set clock" in local time gives the correct local time, and the master's `GET TIME` shows the UTC equivalent.
5. Web password set from the panel without the old one:
   - the phone's web session is logged out;
   - log in with the new password;
   - restore the old one.
6. Fleet:
   - Update zone 2 → the buttons are disabled while the status is not IDLE → the NOTIFY sequence ends in `FW 2 TRIAL PASS`;
   - run it again and Abort → "Fleet update aborted.".
7. Reboot: confirm → "Rebooting..." → the panel comes back.
8. Panel settings:
   - the brightness slider changes the backlight;
   - the clock face switches between analogue and digital;
   - orientation flipped → restart → the five-target touch test passes again → restore normal.
9. Dimming and idle:
   - set a fixed night window that covers now, with 30 s idle → the panel dims;
   - the first tap only wakes it and does not press the button underneath;
   - after 300 s idle it returns to Home and the console transcript has been wiped.

### Stage 4 — Firmware and config from microSD
Tasks 28-33.

**Bench gate:**
1. A FAT32 card holds a new `hillgrow_master.bin`, a `hillgrow_zone.bin` and an unrelated `.bin`. Firmware lists each with its kind and version. The unrelated file shows as "unknown" and cannot be installed.
2. Install the zone image:
   - progress runs to "Uploaded (N bytes).";
   - during the install, a web upload gets 409 UPLOAD_ACTIVE and a console `SET FW ZONE 2` gets `ERR FW_BUSY`.
3. Install the master image → Reboot now → `NOTIFY FW 0 TRIAL PASS` → About shows the new slot VALID.
4. Fleet-update zone 2 from the panel, using the image staged from SD. It ends in `FW 2 TRIAL PASS`.
5. `web_test.py --only uploads --master-bin C:\Projects\HillGrov\master\build_p4\hillgrow_master.bin --zone-bin C:\Projects\HillGrov\zone\build\hillgrow_zone.bin --fleet 2` passes. This includes a zone image sent to `/api/fw/master`, which answers 422 IMAGE_MISMATCH with nothing erased.
6. Export and import:
   - export zone 2 → `/hillgrow/zone2.json` is on the card;
   - import it back → "Queued, pushing to zone";
   - the master export contains no passwords.
7. Card failures:
   - an exFAT or unformatted card gives "Card is not FAT32 ..." and is never formatted;
   - a card pulled mid-install gives "microSD read failed", and `GET FW ZONE` reports no image; re-stage it afterwards.

### Stage 5 — Owner acceptance
Task 34. The gate is the Task 34 checklist.

### Parity coverage (the map-parity items and the tasks that cover them)

| Map-parity items | Tasks |
|---|---|
| B1 five destinations | 12, 20, 21 |
| B2, C33-C42 | N/A (record as such) |
| B3-B5 banners and master card | 12 |
| B6 degraded console slots | 12 (web count), 22 (panel count) |
| B7 node cards and band | 12, 13 |
| B8-B10 zone view | 14 |
| B11 replace board | 22 |
| B12 console | 22 |
| B13-B18 config zones | 17-20 |
| B19 export/import | 33 |
| B20 master config | 21 |
| B21 alarms | 15, 16 |
| B22-B26 Wi-Fi, AP, time | 23 |
| B27 web password | 24 |
| B28-B29 firmware | 30-32 |
| B30 fleet | 25, 32 |
| B31 reboot | 25 |
| B32 factory reset | 22 (through the console) |
| D1 slot and trial state | 12, 25, 26 |
| D2 alarm counts | 13 |
| D4 upload progress | 9, 30 |
| A5 worker | 8 |
| A6-A7 command source and quarantine | 22 |
| A8-A9 secrets | 21, 23, 24 |
| A10 operator-state wipe | 27 |
| A11-A12 naming and vocabulary | 10 |

---

## 3. Open decisions (owner unavailable — the plan takes each default so execution never blocks)

Each decision names its default, and every task is written against that default. The owner may overrule any of them before the task that depends on it runs. Where a task refined a default, the refinement is noted under the decision.

- **D1 Command source.** Default: panel commands use `CMD_SRC_HTTP` semantics (exact web parity; no `DEBUG ENABLE` unlock from the glass, `NOT_LOCAL` for session rows). The alternative is a new `CMD_SRC_PANEL`, which changes `cmd_core.h` and would allow a debug unlock.
- **D2 Clock font route.** Default (a): the spec's TrueType subset rendered by `lv_font_conv` 1.5.2 through `npx`, at 180 px, 4 bpp, glyphs space, '-', '0'-'9' and ':', with the generated `.c` committed. This needs registry access once. Fallback (b), fully offline: `C:\Python311\python -m fontTools.subset Montserrat-Medium.ttf --text="0123456789:- "` plus `CONFIG_LV_USE_TINY_TTF=y` and `lv_tiny_ttf_create_data()`. FreeType is not used.
  - *Refinement (Task 13):* route (a) is assumed to compile with `-Werror` against LVGL 9.5.0, which is unverified. If the generated file alone raises a warning, Task 13 regenerates it with `--lv-include lvgl.h`; if that does not clear it, it takes route (b), written out in full as Task 13 Step 2b. `-Werror` is never silenced. The home screen reaches the font only through `pnl_font_clock()`, so route (b) changes only `pnl_fonts.h`/`.c`.
- **D3 Analogue face.** Default: included as the Panel > Clock face option (`lv_scale` with needles); digital is the default face.
- **D4 Dim behaviour.** Default: FOLLOW_LIGHTS. It is night when the clock is set, at least one enabled shelf has a light schedule, and none is on now. The panel then dims to 10 % after 60 s idle; day is 80 %; the first touch only wakes. FIXED hours and OFF are available. The adapter's `auto_sleep` stays off. The minimum duty is clamped to ≥ 5 %; bench-tune it for flicker.
- **D5 TWDT policy (the same question as the open httpd decision).** Default: `httpd` and the LVGL task stay unsubscribed. The LVGL task gets a 1 s heartbeat, and the poller emits `NOTIFY ALARM 0 W_PANEL_FROZEN LVGL task silent <N>s` after 10 s without one: an active alarm (the `W_` prefix), so the band and the web banner show it, cleared by `CLEARED PANEL_FROZEN` on the next beat. The worker, poller and Wi-Fi tasks are never subscribed (the RPC rule). The alternative, subscribing LVGL, lets a UI wedge panic the master; three wedges would count as CRASH_LOOP under the recovery design. What staying unsubscribed buys is limited to a **blocked** wedge (the LVGL task waiting forever): a **spinning** LVGL task on core 1 starves IDLE1, which the TWDT watches (`CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1=y`, 8 s, panic), so it panics the master either way. That is why nothing on the LVGL task may loop unbounded, and why Task 7 gives LVGL a PSRAM overflow pool (its out-of-memory assert is a `while(1);`).
  - *Owner, 2026-09-28:* "no idea". The default stands. Task 34 records the decision in the follow-ups as still open, with the acceptance evidence (whether `W_PANEL_FROZEN` or a web-UI hang was ever seen).
- **D6 board.h I²C swap.** Default: fixed in Task 7 (P4 SDA 7 / SCL 8, `pin-mapping.md:72`, with a `_Static_assert` against `BSP_I2C_SDA`/`BSP_I2C_SCL`). Nothing uses `HG_GPIO_I2C_*` today. Rule for later: any future P4 I²C user shares `bsp_i2c_get_handle()`. Note that the ES7210's default 0x40 collides with the PCA9685; check before SP2.
- **D7 Dependency lock.** Default: Option A, the current policy (exact manifest pins; the ESP32 lock regenerated in the same commit; `git checkout master/dependencies.lock` after every P4 build). Option B, which needs an OK: a per-target lock via `idf_build_set_property(DEPENDENCIES_LOCK "${CMAKE_CURRENT_LIST_DIR}/dependencies.lock.esp32p4")` in `master/CMakeLists.txt`, before `project()`. That would retire the checkout reflex.
- **D8 Transitive P4 pins.** Default: not pinned. `esp_lcd_touch_gt911`, `esp_lcd_touch`, `esp_lcd_ek79007` and the others float within the BSP's own ranges. Task 6 compares the resolved set with the bench-proven table on every P4 configure and stops on a difference.
- **D9 `MCFG_F_AP_DEFAULT` on zone-0 saves.** Default: unchanged (web parity). An AP_SSID or AP_PASS change made through Config, web or panel, leaves the factory-password banner up; System > Set AP clears it as today. The fix (clear it when AP_PASS changes) is one line plus a test once approved.
- **D10 BUSY versus STORAGE.** Default: split only in the panel-facing `psvc_*` functions. The CLI rows and `POST /api/wifi` keep reporting lock contention as `ERR STORAGE` / 503. Unifying the two is a separate, approved change.
- **D11 SHELF.PROFILE.** Default: an editable stepper 0..16 (literal parity), labelled "Profile id (profiles not implemented yet)". The alternative is read-only until SP4b profiles exist (system spec §4.7).
- **D12 STA/AP_PASS maximum.** Default: the panel's presentation clamps `max_len` to 63, the validator's limit. The `hg_mcfg` table rows (64) and `/api/schema` are unchanged.
- **D13 Config export/import on the panel.** Default: included through microSD (Task 33), since Decision 2 read literally requires it. The alternative marks it N/A and drops Task 33.
- **D14 Manual "Set clock" dialog.** Default: included (Task 23), even though it goes beyond parity. The clock hero has no other honest source when NTP is absent. It converts local to UTC with `time_svc_utc_offset()` at the current time; it is off by the DST delta only when set across a DST edge.
- **D15 Factory reset button.** Default: none. `FACTORY RESET CONFIRM` stays reachable through the console, as on the web.
- **D16 Reboot during an OTA trial.** Default: a warning and a second confirm on the panel's Reboot while `fw_state` is PENDING. There is no gate; console lines are unchanged.
- **D17 Panel in `ota_trial_drivers_ok()`.** Default: no. A panel that fails to start must not roll back a master that controls the greenhouse.
- **D18 Idle operator-state wipe (the web's logout wipe).** Default: after 300 s idle, provided no job is pending, wipe the console logs, history and drafts, the unsaved config edit sets, the form text and any revealed secret, and return to Home.
- **D19 Panel start placement.** Default: `panel_start()` right after `ota_trial_start(1)`, which puts about 283 ms of panel bring-up ahead of the AP and the ring. The alternative is a one-shot init task, if the owner wants the AP up first.
- **D20 Recovery-plan interplay.** Task 28 lands `ota_trial_running_on_trial()` and `hg_image` only if the recovery plan has not. The recovery plan must consume both. Its §6.1 `esp_hosted_deinit/init` reconnect must refuse or defer while `pnl_sd_mounted()` is 1, because a mounted slot 0 keeps the SDMMC controller alive. Its references to `http_upload.c`/`http_upload_master.c` become `panel_svc/psvc_fw.c` and `fw_sink_master.c`.
- **D21 New refusal token `ZONE_FW_BUSY`.** Panel only. It is raised when a zone is pulling `/fw/zone.bin` while the panel wants to rewrite `zone_fw`.
- **D22 The chip-id check applies to the web's upload endpoints too** (recovery design §4.2's extension of `identity_ok()`). An ESP32 master image posted to the P4 now answers 422 IMAGE_MISMATCH before any erase, instead of 422 WRITE_FAILED after erasing. Default: yes.
  - *Checked:* `tools/web_test.py`'s uploads suite asserts only the status (422) for its mismatch case (`suite_uploads`, `do_mismatch`), so it needs no change.
  - *Known and kept (Task 30):* the install core's `LOW_HEAP` guard reads `esp_get_free_heap_size()` (`psvc_fw_env.c`, as `http_upload.c:210` does today). On the P4 that figure includes about 30 MB of PSRAM (`CONFIG_SPIRAM_USE_MALLOC=y`), so the 40 KB guard never trips there, for the web or the panel. It is kept for web parity and because the consequence is soft: the install buffer is static, and an internal-RAM shortage during an install surfaces as `WRITE_FAILED`, never as a reset. Switching it to `heap_caps_get_free_size(MALLOC_CAP_INTERNAL)` would be a second deliberate web change and is left to the follow-ups.
- **D23 Wi-Fi status cadence.** Default: every 5 s on its own `pnl_wifi` task, so that a silent C6 (5 s per RPC) never freezes ring and zone data.
- **D24 Orientation option.** Default: normal. "Flipped" uses display `ROTATE_0` plus a clamped 180° touch flip in `pnl_touch_map`, never the `esp_lcd_touch` mirrors, and applies after a restart.
- **D25 Internal RAM floor.** Default: internal min ≥ 64 KB at every gate, which applies the system's heap bar to the pool that can actually run out. LVGL uses its builtin 64 KB internal pool (plus the PSRAM overflow pool of Task 7, which costs no internal RAM); raising the internal pool to 128 is allowed only above that floor.
- **D26 Console HELP button.** Default: none, because the web never calls `/api/help`.
- **D27 The web's "Zone busy, retry" for ZONE_NOT_ONLINE.** Default: the panel splits the two, and the web is unchanged. The owner may approve a matching web fix.
- **D28 The web dashboard's time.** It is UTC, formatted with `localtime_r` and no TZ set, and unlabelled. Default: the panel shows local time with a "(UTC+hh:mm)" suffix; the web fix is out of scope.

### Refinements the tasks make to the outline (defaults; the owner may overrule)

These are not new decisions, but each changes something a reader of §1-§2 might assume. The task named is where the detail lives.
- **Touch read path (Task 7).** The custom touch read is installed with `lv_indev_set_read_cb()` on the indev that `esp_lv_adapter_register_touch()` returns, rather than through `esp_lv_adapter_set_touch_callbacks()`, whose signature could not be checked with the spike gone. The `esp_lcd_touch` mirrors are switched off on the handle (`esp_lcd_touch_set_mirror_x/y`, `set_swap_xy`), because `bsp_touch_new(NULL)` enables them. Every BSP and adapter identifier in `panel_hw.c` is re-checked against the headers after a GATE-P4 configure (Task 7 Step 6): header spelling wins, the behaviour is binding. If the five-target test fails, Task 7 Step 11 stops and reports rather than reaching for the mirrors.
- **Age and AP text (Task 10).** `pnl_fmt_age()` reproduces the web's `fmtAge` ("4m 3s ago", "3h 5m ago", a newer stamp clamps to "0s ago") instead of the outline's single-unit examples and "--"; `pnl_fmt_ap()` says "2 client(s)". The signatures are unchanged. Later text builders must not assume the outline's example strings.
- **Stale marker (Task 12).** The dashboard writes "stale" as a word in the node card's sub-line, not as a separate pill, under exactly the web's condition (health OFFLINE).
- **Shell mechanics (Task 12).** Rail membership comes from the registry row's `rail` flag; `pnl_nav_go()` is deferred with `lv_async_call()`; each destination builds into a fresh page object; screens read data through `pnl_shell_snap()`. Tasks 13, 14, 16, 20, 23 and 26 each swap their own registry line in the `[PNL_DEST_X] = { "Title", &PNL_SCR_X, rail },` form.
- **LVGL pool figure (Task 11).** `lv_mem_monitor()` is shown on the diagnostics screen from Task 11, so the Stage 1 gate can record `lv_mem` max. Task 26's About shows it too.
- **Band pulse (Task 13).** The alarm pulse uses `lv_anim` with `lv_anim_set_duration()` and a triangle-wave exec callback, not the playback/reverse API that was renamed around LVGL 9.3.
- **Zone-0 PUT logging (Task 3).** `psvc_mcfg_edit()`'s apply logs a WARN when `wifi_mgr_apply()` fails; `cfg_put_zone0` ignored that rc silently. Status codes and bodies are unchanged.
- **Redundant zone validation (Task 5).** `psvc_zone_cfg_edit()` runs `hg_cfg_validate()` after `psvc_zone_json_fn()`, whose merge already validated with the same hw argument. For the web this is redundant with the same verdict; it is kept so the panel's field path gets the identical check.
- **Alarm snapshot RAM (Task 15).** `alarm_mgr_json()`'s static `am_snapshot_t` costs about 6.6 KB of internal `.bss` on both the ESP32 and the P4 master. Task 15 records the ESP32 master's heap minimum before and after, against the 64 KB bar.
- **MCFG suite side effect (Task 1).** The password change-and-restore permanently clears `MCFG_F_WEB_DEFAULT` on a bench still on the factory `hillgrow1` (`defaults.web` turns false) and logs every web session out twice. The restore is transport-safe and fails loudly, naming the console fix.
- **Optional re-enrol (Stage 1 gate).** The `CLEAR NODE` check is optional and restores with `SET NODE 2 MAC <mac>`.
- **Hardware-plane note (Task 20).** Shown once per group above its rows, as the web does (`app.js:1141-1144`), not on every read-only row.
- **Index labels (Task 20).** The shelf and aux selector is labelled 0..3 like the web; the CLI is 1-based (`GET ZONE 2 WATER 1` is index 0). The gate text says so.
- **Stage 2 gate split (Tasks 20-21).** Step 6 (the panel/web save collision) is a zone-editor behaviour and runs at Task 20; Task 21 runs step 7 and then the whole gate. Step 8 (a never-synced zone) may be recorded as "not exercised" when no fresh zone board is available: its text and the refusal are host-tested.
- **Refusal texts (Task 18).** `pnl_msg` adds OK texts for ZONE_LOAD, SCAN and TZ, the web's LOW_HEAP wording for ZONE_LOAD, and the master-config BUSY sentence for WIFI_JOIN, WIFI_AP, SCAN and TZ, where the outline said only "the token".
- **Web upload drain (Task 30).** The adapter drains the rest of the body only when `st.started && !src_failed && consumed < len`. The outline's `consumed < len && !src_failed` would drain after guard refusals, which today close the socket without reading (`http_upload.c:213-225`): a web behaviour change.
- **microSD bring-up (Task 31).** The mount is inferred from the IDF 6.0.1 and BSP 3.0.1 sources and has never been run: a no-op `host.init` on esp_hosted's controller, a retry with the real `sdmmc_host_init` only when the first mount returns `ESP_ERR_INVALID_ARG`, the LDO-4 handle created and deleted per mount, and slot-0 IOMUX pins. Only the Stage 4 gate proves it, including that the C6 and the AP survive every mount and unmount.
- **Night window (Task 27).** A FIXED window with `start == end` is empty (never night).
- **RAM added in Stage 4 (Tasks 29-30).** A second 4 KB static buffer in `fw_srv` (`s_vbuf`) and a 1 KB static drain buffer in `http_upload.c` (the 4 KB install buffer moves, net zero): about +5 KB internal. The gates' internal-minimum floor covers it. Each zone console allocates about 88 KB of PSRAM lazily (8 zones: about 700 KB of about 30 MB).

---

## 4. Tasks

Each task gives its Files, its Interfaces (Consumes / Produces, with exact names), what each check proves, and its steps. A name defined under **Produces** is defined only there; later tasks cite it, and no task uses a name that only a later task defines. For CTest counts, "+N" is over the count before the task. The absolute figure assumes that no recovery-plan task lands in between; today's count is 35.

### How to read the tasks

- **GATE-HOST, GATE-P4 and GATE-ESP32** are the exact command sequences in *Global Constraints → Project rules*. Every task that names one runs it exactly as written there, and reads the output rather than the exit code. Every command is PowerShell, run after `& C:\esp\v6.0.1\esp-idf\export.ps1` in that shell.
- **Reading GATE-P4.** Capture the build output and search it, so a warning cannot scroll past:
  ```powershell
  & C:\esp\v6.0.1\esp-idf\export.ps1
  idf.py -C C:\Projects\HillGrov\master -B C:\Projects\HillGrov\master\build_p4 -DIDF_TARGET=esp32p4 -DSDKCONFIG=C:\Projects\HillGrov\master\build_p4\sdkconfig -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.esp32p4" build | Tee-Object -FilePath $env:TEMP\hg_p4_build.log
  Select-String -Path $env:TEMP\hg_p4_build.log -Pattern 'warning:'
  Select-String -Path C:\Projects\HillGrov\master\build_p4\sdkconfig -Pattern 'CONFIG_IDF_TARGET="esp32p4"'
  git -C C:\Projects\HillGrov checkout -- master/dependencies.lock
  ```
  Pass: the log ends with `Project build complete`, the `warning:` search prints nothing, and the target search prints exactly one match. The checkout is never optional: the component manager has just rewritten the committed ESP32 lock for the P4 (commit `ee83c0b`). Only Task 6 edits a manifest, and it regenerates the ESP32 lock with an ESP32 reconfigure in the same commit; nothing in a P4 rewrite of the lock is ever committed.
- **Reading GATE-ESP32.** Each of the three builds prints `Project build complete` with no `warning:` line, and the final `git diff --stat -- master/dependencies.lock` prints nothing. The ESP32 master build purges the P4-only packages from `master/managed_components/`, so the next GATE-P4 re-extracts them (expect a slower configure). A task that needs both usually runs GATE-ESP32 and then re-runs the GATE-P4 build line and the lock checkout, so the P4 image it flashes is the current one.
- **GATE-HOST** passes when the last line reads `100% tests passed, 0 tests failed out of N`, with the N the task states.
- **One host test** (the TDD red/green loop; `<name>` is the CTest name, for example `test_pcfg_gen`). Either add `-R <name>` directly after `--output-on-failure` inside GATE-HOST's quotes, or build only that target:
  ```powershell
  $env:PATH = "C:\Espressif\tools\cmake\4.0.3\bin;" + $env:PATH
  cmd /c 'call "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul && cmake -S C:\Projects\HillGrov\tests\host -B C:\Projects\HillGrov\build\host -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=cl && cmake --build C:\Projects\HillGrov\build\host --config Release --target <name> && ctest --test-dir C:\Projects\HillGrov\build\host -C Release --output-on-failure -R <name>'
  ```
  "Run the single host test with `<name>` = ..." in a step means this command. The whole suite is one CMake project, so with the full build a test file that does not compile stops the build for every test. That is the expected "fail" in each TDD step.
- **FLASH-P4** always means both commands below, in this order. The dry run must print only `build_p4` paths. Never `idf.py flash`.
  ```powershell
  python C:\Projects\HillGrov\tools\flash_app.py --app master --target esp32p4 --build-dir C:\Projects\HillGrov\master\build_p4 --port COM28 --dry-run
  python C:\Projects\HillGrov\tools\flash_app.py --app master --target esp32p4 --build-dir C:\Projects\HillGrov\master\build_p4 --port COM28
  ```
  Before any FLASH-P4, check that the master is not in an OTA trial: in a console (`C:\Python311\python -m serial.tools.miniterm COM28 115200`, quit with Ctrl+]) type `GET VERSION`. The running slot must show `VALID`. If it shows `PENDING`, wait for `NOTIFY FW 0 TRIAL PASS` first: any reset during a trial retires the image (recovery design §2.3).
- **Commit and push** (every task). Stage explicit paths only; never `docs/hillgrow-features.drawio`, never `git add docs/`:
  ```powershell
  git -C C:\Projects\HillGrov status --short
  git -C C:\Projects\HillGrov add <the exact paths the task lists>
  git -C C:\Projects\HillGrov commit -m "<the task's subject line>" -m "Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>"
  git -C C:\Projects\HillGrov push
  ```
  Before `add`, check that `status --short` shows ` M docs/hillgrow-features.drawio` untouched and unstaged. The trailer is the executing session's attribution line; today it is the one above. Push only after the task's gates are green; where a task's commit block omits the `push` line, add it.
- **`panel_ui` sources.** Task 6 creates the gating `if("${CMAKE_PROJECT_NAME}" STREQUAL "hillgrow_master" AND "${panel_target}" STREQUAL "esp32p4")` block with `set(PANEL_SRCS "panel_ui.c")` inside it. Tasks 7-16 grow that one `set(PANEL_SRCS ...)` line. From Task 17 on, each task adds one `list(APPEND PANEL_SRCS ...)` line inside the same block, after the lines earlier tasks added. Pure helpers are compiled there too, so GCC's `-Werror` sees them on the P4 while MSVC compiles the same `.c` files for the host suite.
- **On-target-only code** (LVGL widget trees, BSP and driver glue) cannot reach the MSVC host suite. For those files, each task says what GATE-P4 proves (it compiles with `-Werror` against the pinned LVGL, BSP and adapter) and what the stage's bench gate proves (it behaves on glass).
- **Owner steps.** Lines marked as owner steps, and the stage bench gates, are for the owner at the bench; everything else is for the implementing agent.
- **Sources.** Source facts and line numbers cited in the tasks were read at `aceb311` (2026-09-23 to 2026-09-28). The BSP source lines cited for Task 31 were read in the component-manager cache copy of `waveshare/esp32_p4_wifi6_touch_lcd_7b` 3.0.1; after any GATE-P4 configure the same file is at `master/managed_components/waveshare__esp32_p4_wifi6_touch_lcd_7b/esp32_p4_wifi6_touch_lcd_7b.c`. If a cited line has moved, find the code by name and say so in the task report.

### Interface additions (names the tasks add beyond the outline in §1)

Each is defined in exactly one task, in that task's Interfaces block (marked "addition" or "ADDITION"), and means the same thing everywhere it is used.

| Name | Defined in | Meaning |
|---|---|---|
| `MCFG_TEMP_SUFFIX`, `mcfg_temp_password()`, `secret_digest()`, `mcfg_restore_password()` | Task 1 (`tools/web_test.py`) | the MCFG suite's temporary-password rule, the never-printed secret digest, and the transport-safe restore |
| `fake_apply_wifi_n`, `fake_apply_time_n`, `fake_apply_wifi_rc`, `fake_apply_reset()` | Task 3 (`tests/host/fakes/fake_apply.h`) | counting stand-ins for `wifi_mgr_apply()` and `time_svc_apply_mcfg()` |
| `${COMP}/time_svc` on the host include path | Task 3 (`tests/host/CMakeLists.txt`) | `psvc_mcfg.c` and `fake_apply.c` include `time_svc.h` |
| `psvc_net.h` includes `psvc_mcfg.h` | Task 4 | callers see the `PSVC_LOCK_*` budgets |
| `fake_nmgr_cfg_t`, `g_fnc`, `fake_nmgr_cfg_reset()` | Task 5 (`tests/host/fakes/fake_nmgr_cfg_api.h`) | the scripted `node_mgr_cfg_busy/get/set` |
| `PANEL_HW_W` (1024), `PANEL_HW_H` (600) | Task 7 (`panel_hw.h`) | the panel's resolution |
| `pnl_theme_card()`, `pnl_label()` | Task 7 (`pnl_theme.h`) | card styling; a label with font and colour in one call |
| `pnl_utc_from_civil()`, `pnl_days_in_month()` | Task 10 (`pnl_time.h`) | the civil-time maths `pnl_fmt_master_time()` and `pnl_set_time_line()` share |
| `pnl_nav_title()`, `pnl_shell_snap()`, `pnl_obj_show()` | Task 12 (`scr_shell.h`) | the current registry title; the shell's one LVGL-side snapshot copy; hide/show only on change |
| `pnl_font_clock()` | Task 13 (`pnl_fonts.h`) | the one clock-font accessor (D2 route (b) changes only `pnl_fonts.h`/`.c`) |
| `scr_zone.h` declaring `scr_zone_extra_area()`, `scr_zone_current()` | Task 14 | the zone view's hooks for Task 22 |
| `pcfg_pres_t.zero_text` | Task 17 (`pcfg_pres.h`) | display text for raw 0 (LIGHT.DLI "off"), so the generator does not hard-code the key |
| `wdg_reveal_fn`, `wdg_field_set_reveal()` | Task 19 (`wdg_field.h`) | a SECRET row never holds the secret, so [Reveal] asks this value source |
| `cfg_view_t.reveal` | Task 20 (`scr_config.h`) | SECRET rows only; NULL for zones |
| `cfg_save_fn`, `cfg_frame_build()`, `cfg_frame_rerender()`, `cfg_frame_set_saving()`, `cfg_set_title()` | Task 20 (`scr_config.h`) | the tabs/index/Save/list frame both editors share, and the title |
| `cfg_master_update()` | declared Task 20, defined Task 21 | reconciles the Save button when a master save finishes after a rebuild |
| `time_core` in `panel_ui`'s `PRIV_REQUIRES` | Task 21 | `tz_check` for the master's local pre-validation |
| `pnl_con_hist_add()`, `pnl_con_log_at()` | Task 22 (`pnl_console.h`) | history keeps the original line; oldest-to-newest log access (`pnl_con_push` returns NULL rather than overwrite a pending entry) |
| `zone_sections.h`, `zone_replace_teardown()` | Task 22 | the two zone sections' LVGL-typed header |
| `pnl_ui_kit.h/.c`: `pnl_kit_tone_t`, `pnl_kit_card/row/button/enable/field/msg/msg_set()`, `pnl_confirm_fn`, `pnl_confirm()`, `pnl_confirm_close()` | Task 22 | the widgets every System, Panel and card section is built from |
| `PNL_SYS_SEC_WIFI` .. `PNL_SYS_SEC_FIRMWARE`, `PNL_SYS_SEC_COUNT` | Task 23 (`scr_system.h`) | the section index passed as `pnl_nav_go(PNL_DEST_SYSTEM, arg)`; distinct from the `PNL_SYS_*` section descriptors |
| `sys_password_wipe()`, `sys_reboot_confirm()` declared in `scr_system.h` | Tasks 24, 25 | as in the outline, now with one header |
| `pnl_prefs_clamp()` | Task 26 (`pnl_prefs.h`) | mode > 2 → 1, day/night 5..100, fixed minutes > 1439 → default, idle ≥ 10 s, wipe ≥ 60 s, face/orient > 1 → 0 |
| `pnl_prefs_preview()` | Task 26 (`pnl_prefs_nvs.h`) | live copy only, no save job (slider drag) |
| `fw_srv_writer_claim()` also returns -1 while another writer holds the claim | Task 29 | |
| `psvc_fw` semantics: `claim()` 1 taken / 0 held; `psvc_fw_stats_t.started`; a `begin` failure is WRITE_FAILED without `cancel`; zone identity always against `HG_CHIP_ESP32` | Task 30 | |
| `pnl_sd_rc_text()`; `pnl_sd_list_bins()` returns -1 when not mounted; `pnl_sd_classify()`: `hillgrow_zone` on a non-ESP32 chip → `PNL_FW_WRONG_CHIP` | Task 31 | |
| `cfg_card_bar()`, `cfg_card_set_zone()`, `cfg_card_teardown()` (and the outline's `cfg_card_export/import`) declared in `scr_config.h` | Task 33 | |

Semantics fixed without a new name: `pcfg_edits_export()` returns -1 when `cap` is smaller than the set (never a partial export); `pcfg_edits_set()` normalises `idx` (-1 for master rows and scope-0 groups) and refuses `idx` outside -1..127; `pcfg_locate("aux.pulse_s")` returns 0 with `idx` -1, and `"hw.aux_pin"` returns -1; `wdg_keyboard_close()` never calls `done()`, wipes the text buffers in place and deletes the overlay asynchronously; `pnl_worker_submit(run, done, NULL, 0)` is legal and `done` may be NULL (Task 8); `pnl_label_set_if_changed()` is NULL-safe (Task 12); `cfg_zone_wipe_all()` and `cfg_master_wipe()` are safe when the Config screen is not built (Tasks 20-21); `pnl_poll_latest()` copies into a caller-owned buffer that is static or PSRAM, never on the LVGL stack (Task 11); `psvc_mcfg_fields_fn()` and `psvc_zone_fields_fn()` are pure (`hg_field_write`/`hg_field_base` only, no lock, no `node_mgr` or `mcfg_ops` call), so Tasks 20-21 also call them on the LVGL task for local pre-validation.

---

## Stage 0 — Foundations: the shared service layer exists and the panel lights on the real master

### Task 1: web_test.py MCFG suite, the regression net for the master-config paths (Stage 0)

The web's zone-0 PUT, `POST /api/wifi` and `POST /api/password` are about to move onto a shared layer (Tasks 3-4). Nothing on the bench exercises them today: `suite_config` only touches zone 2. This task writes the net first, so the Stage 0 gate can run it **before** flashing (proving it describes today's firmware) and again after.

**Files:**
- Modify: `tools/web_test.py`
  - module docstring (`:2-6`): name the MCFG suite;
  - add `import hashlib` to the imports (`:27-37`);
  - new pure helpers after `config_problems()` (`:213-228`): `MCFG_TEMP_SUFFIX`, `mcfg_doc_problems(doc, secrets)`, `mcfg_temp_password(password)`, `secret_digest(doc, key)`;
  - new `mcfg_restore_password(api, ip, temp, password, timeout)` and `suite_mcfg(api, results, ip, password, timeout)` after `suite_wifi()` (`:549-563`);
  - `selftest()` (`:764-848`) gains assertion group (i), and its final print says 9 groups;
  - `run_standard()` (`:852-884`) gains the MCFG row **before** LOGIN;
  - the `--only` help text (`:894-896`) lists `mcfg`.

**Interfaces:**
- Consumes: the existing `ApiClient`, `get_json`, `post_json`, `put_json`, `check`, `skip` and `poll_until` helpers, and `FIXTURE_CONFIG0` (`:718-722`).
- Produces: suite name `MCFG` for `--only`. Its checks, all derived from today's code:
  1. `GET /api/config?zone=0&secrets=0` → 200. `WIFI` has `STA_SSID` and `AP_SSID` but no `STA_PASS` or `AP_PASS`; `TIME` has `TZ` and `NTP`; `SYS` has `HOSTNAME`.
  2. PUT zone 0 `{"TIME":{"NTP":"time.google.com"}}` → 200, and a GET shows it. Restore the original → 200. Expect a transient STA re-join, since `wifi_mgr_apply` runs on every zone-0 save; use the retries.
  3. PUT zone 0 `{"SYS":{"HOSTNAME":"Bad Name"}}` → 400. The error is `INVALID_FIELD` or `VALIDATION`, with path `SYS.HOSTNAME`.
  4. PUT zone 0 with a malformed body → 400 `BAD_JSON`.
  5. The SHA-256 of `STA_PASS` from `?secrets=1` is equal before and after a PUT that omits `STA_PASS`. Never print the secret.
  6. `POST /api/wifi {"sta":{"ssid":"x"}}` → 400 `INVALID`, and `{"sta":{...},"ap":{...}}` → 400 `INVALID`.
  7. `POST /api/password {"old":"<wrong>","new":"whatever12"}` → 403 `BAD_PASSWORD`.
  8. Change to a temporary password (`password[:55] + "_mcfgT"`) → 204. A second client logged in beforehand now gets 401: the sessions were dropped. Restore in a `try/finally` → 204.
- Interface additions (Python, this file only): `MCFG_TEMP_SUFFIX = "_mcfgT"`, `mcfg_temp_password(password) -> str`, `secret_digest(doc, key) -> str | None`, `mcfg_restore_password(api, ip, temp, password, timeout) -> bool`.

Why each check reads as it does (all verified in source):
- Check 3 accepts two codes because `hg_json_merge_mcfg()` runs `hg_mcfg_validate()` itself and reports a failure as -2, which is `INVALID_FIELD` (`hg_json_mcfg.c:64-70`, `http_api_cfg.c:219-223`).
- Check 6: `h_wifi_set()` requires exactly one of `sta`/`ap`, each with string `ssid` **and** `pass` (`http_api.c:232-245`).
- Check 7: `h_password()` answers 403 `BAD_PASSWORD` when `http_auth_verify_password(old)` fails, which creates no session and touches no lockout counter (`http_login.c:146-154`).
- Check 8: `master_web_set_password()` drops every session, and `h_password()` then mints a fresh cookie for the caller only (`http_login.c:164-175`).
- **Side effect the owner must know about:** any successful password change clears `MCFG_F_WEB_DEFAULT` (`http_srv.h:78-79`). If the bench is still on the factory `hillgrow1`, the restore stores `hillgrow1` as a real hash, and `/api/state`'s `defaults.web` turns `false` for good. Every web session (phones included) is logged out twice.

- [ ] **Step 1: Write the failing self-test group (i)**

In `selftest()`, insert this block immediately before `if fails:` (`tools/web_test.py:844`):

```python
    # (i) MCFG pure helpers: the zone-0 document shape with and without
    # secrets (hg_json_export_mcfg OMITS the two secret keys, it never blanks
    # them), the temporary-password rule, and the never-printed secret digest.
    stripped = json.loads(json.dumps(FIXTURE_CONFIG0))
    del stripped["WIFI"]["STA_PASS"]
    del stripped["WIFI"]["AP_PASS"]
    expect("i: zone0 without secrets passes secrets=0", mcfg_doc_problems(stripped, False) == [])
    expect("i: zone0 with secrets passes secrets=1", mcfg_doc_problems(FIXTURE_CONFIG0, True) == [])
    expect("i: a secret present under secrets=0 is flagged", mcfg_doc_problems(FIXTURE_CONFIG0, False) != [])
    expect("i: a secret missing under secrets=1 is flagged", mcfg_doc_problems(stripped, True) != [])
    bad8 = json.loads(json.dumps(stripped)); del bad8["SYS"]
    expect("i: zone0 missing SYS flagged", mcfg_doc_problems(bad8, False) != [])
    bad9 = json.loads(json.dumps(stripped)); del bad9["TIME"]["NTP"]
    expect("i: zone0 missing TIME.NTP flagged", mcfg_doc_problems(bad9, False) != [])
    expect("i: not-an-object flagged", mcfg_doc_problems([], False) != [])
    expect("i: temp password for hillgrow1", mcfg_temp_password("hillgrow1") == "hillgrow1_mcfgT")
    long_pw = "p" * 63
    t = mcfg_temp_password(long_pw)
    expect("i: temp password of a 63-char password stays 8..63 and differs", 8 <= len(t) <= 63 and t != long_pw)
    tricky = "q" * 55 + MCFG_TEMP_SUFFIX
    expect("i: temp password never equals the real one", mcfg_temp_password(tricky) != tricky)
    d1 = secret_digest({"WIFI": {"STA_PASS": "housepass1"}}, "STA_PASS")
    expect("i: secret digest is stable",
           d1 == secret_digest({"WIFI": {"STA_PASS": "housepass1"}}, "STA_PASS"))
    expect("i: secret digest never contains the secret", d1 is not None and "housepass1" not in d1)
    expect("i: secret digest differs for a different secret",
           d1 != secret_digest({"WIFI": {"STA_PASS": "housepass2"}}, "STA_PASS"))
    expect("i: digest of a missing secret is None", secret_digest({"WIFI": {}}, "STA_PASS") is None)
    expect("i: digest of a non-document is None", secret_digest(None, "STA_PASS") is None)
```

And change the success print at `:847` from `print("SELFTEST OK (8 assertion groups)")` to:

```python
    print("SELFTEST OK (9 assertion groups)")
```

- [ ] **Step 2: Run the self-test to verify it fails**

Run: `python C:\Projects\HillGrov\tools\web_test.py --selftest`
Expected: a traceback ending in `NameError: name 'mcfg_doc_problems' is not defined`.

- [ ] **Step 3: Add the pure helpers**

Add `import hashlib` to the import block, between `import argparse` and `import http.client`.

Insert after `config_problems()` (after `tools/web_test.py:228`):

```python
# The MCFG suite changes the web password to this and back. Appended, never
# substituted, so it is always a different password of legal length 8..63
# (web_auth.h WA_PW_MIN/WA_PW_MAX): a real password is at least 8 chars.
MCFG_TEMP_SUFFIX = "_mcfgT"

def mcfg_doc_problems(doc, secrets):
    """GET /api/config?zone=0&secrets=0|1: hg_json_export_mcfg's shape
    (hg_json.h:46-50). With secrets=False both secret keys must be ABSENT --
    the exporter omits them, it never blanks them -- and with secrets=True
    both must be present."""
    if not isinstance(doc, dict):
        return ["not a JSON object"]
    p = []
    want = {"WIFI": ("STA_SSID", "AP_SSID"), "TIME": ("TZ", "NTP"), "SYS": ("HOSTNAME",)}
    for group, keys in want.items():
        g = doc.get(group)
        if not isinstance(g, dict):
            p.append(f"missing group {group}")
            continue
        for k in keys:
            if k not in g:
                p.append(f"{group}.{k} missing")
    wifi = doc.get("WIFI") if isinstance(doc.get("WIFI"), dict) else {}
    for k in ("STA_PASS", "AP_PASS"):
        if secrets and k not in wifi:
            p.append(f"WIFI.{k} missing with secrets=1")
        if not secrets and k in wifi:
            p.append(f"WIFI.{k} present with secrets=0 (secret leaked)")
    return p

def mcfg_temp_password(password):
    """The MCFG suite's temporary web password: 8..63 chars, never equal to
    `password` (a 61-char password that already ends in the suffix gets a
    different one)."""
    t = password[:55] + MCFG_TEMP_SUFFIX
    if t == password:
        t = password[:55] + "_mcfgU"
    return t

def secret_digest(doc, key):
    """SHA-256 hex of WIFI.<key> in a zone-0 document, or None when absent.
    Lets a check compare a secret before/after without ever printing it."""
    if not isinstance(doc, dict):
        return None
    v = (doc.get("WIFI") or {}).get(key) if isinstance(doc.get("WIFI"), dict) else None
    if not isinstance(v, str):
        return None
    return hashlib.sha256(v.encode("utf-8")).hexdigest()
```

- [ ] **Step 4: Run the self-test to verify it passes**

Run: `python C:\Projects\HillGrov\tools\web_test.py --selftest`
Expected: `SELFTEST OK (9 assertion groups)` and exit code 0.

- [ ] **Step 5: Add the live suite**

Insert after `suite_wifi()` (after `tools/web_test.py:563`):

```python
def mcfg_restore_password(api, ip, temp, password, timeout):
    """Puts the web password back from `temp` to `password`. First through
    `api` (which holds the fresh cookie the change minted), then through fresh
    clients, because the change's own response may have been lost on this
    lossy AP link. A login with `temp` failing while one with `password`
    succeeds means the change never landed (or an earlier attempt already
    restored it) -- that is also a restored state. True once restored."""
    try:
        s, _, _ = post_json(api, "/api/password", {"old": temp, "new": password}, retries=1)
        if s == 204:
            return True
    except Exception:
        pass
    for _ in range(4):
        try:
            c = ApiClient(ip, timeout=timeout)
            if c.login(temp, retries=3, retry_delay=2.0):
                s, _, _ = post_json(c, "/api/password", {"old": temp, "new": password}, retries=1)
                if s == 204:
                    return True
            elif ApiClient(ip, timeout=timeout).login(password, retries=3, retry_delay=2.0):
                return True
        except Exception:
            pass
        time.sleep(3.0)
    return False

def suite_mcfg(api, results, ip, password, timeout=10.0):
    """The master-config write paths the panel plan moves onto panel_svc:
    PUT /api/config?zone=0 (cfg_put_zone0), POST /api/wifi and POST
    /api/password. Every check describes TODAY's firmware, so this suite must
    pass both before and after the move. Runs just before LOGIN: it changes
    the web password and restores it, which logs every other web session out
    (phones included) and clears MCFG_F_WEB_DEFAULT for good."""
    # 1. shape, secrets omitted
    s, doc0, body = get_json(api, "/api/config?zone=0&secrets=0", retries=3, retry_delay=1.5)
    check(results, "MCFG: GET zone 0 secrets=0 -> 200", s == 200, s)
    if s != 200 or doc0 is None:
        return
    probs = mcfg_doc_problems(doc0, False)
    check(results, "MCFG: zone 0 document shape, secrets omitted", not probs, probs)
    orig_ntp = (doc0.get("TIME") or {}).get("NTP")

    s1, docs1, _ = get_json(api, "/api/config?zone=0&secrets=1", retries=3, retry_delay=1.5)
    check(results, "MCFG: GET zone 0 secrets=1 -> 200 with both secrets",
          s1 == 200 and not mcfg_doc_problems(docs1, True), s1)
    digest_before = secret_digest(docs1, "STA_PASS") if s1 == 200 else None

    # 2. NTP round trip (also the "PUT that omits STA_PASS" for check 5)
    def ntp_is(v):
        s_, d_, _ = get_json(api, "/api/config?zone=0&secrets=0", timeout=5.0, retries=3, retry_delay=2.0)
        return s_ == 200 and ((d_ or {}).get("TIME") or {}).get("NTP") == v

    new_ntp = "time.google.com" if orig_ntp != "time.google.com" else "pool.ntp.org"
    s, d, b = put_json(api, "/api/config?zone=0", {"TIME": {"NTP": new_ntp}}, retries=4, retry_delay=3.0)
    check(results, f"MCFG: PUT TIME.NTP={new_ntp} -> 200 ok", s == 200 and bool(d and d.get("ok")), (s, b[:200]))
    check(results, "MCFG: GET shows the new NTP (STA may re-join meanwhile)",
          poll_until(lambda: ntp_is(new_ntp), 30.0, 2.0), "")

    # 5. the omitted secret survived
    s2, docs2, _ = get_json(api, "/api/config?zone=0&secrets=1", timeout=5.0, retries=4, retry_delay=2.0)
    digest_after = secret_digest(docs2, "STA_PASS") if s2 == 200 else None
    same = digest_before is not None and digest_before == digest_after
    check(results, "MCFG: STA_PASS unchanged by a PUT that omits it (sha256 compared, never printed)",
          same, "digests equal" if same else "digests differ or unreadable")

    if orig_ntp:
        s, d, b = put_json(api, "/api/config?zone=0", {"TIME": {"NTP": orig_ntp}}, retries=4, retry_delay=3.0)
        check(results, "MCFG: restore the original NTP -> 200", s == 200, (s, b[:200]))
        check(results, "MCFG: GET shows the original NTP again", poll_until(lambda: ntp_is(orig_ntp), 30.0, 2.0), "")

    # 3. a validation refusal names the field
    s, d, b = put_json(api, "/api/config?zone=0", {"SYS": {"HOSTNAME": "Bad Name"}}, retries=3, retry_delay=1.5)
    code = (d or {}).get("error") if isinstance(d, dict) else None
    path = (d or {}).get("path") if isinstance(d, dict) else None
    check(results, "MCFG: PUT SYS.HOSTNAME='Bad Name' -> 400 INVALID_FIELD|VALIDATION path SYS.HOSTNAME",
          s == 400 and code in ("INVALID_FIELD", "VALIDATION") and path == "SYS.HOSTNAME", (s, b[:200]))

    # 4. malformed body
    s, b = api.request("PUT", "/api/config?zone=0", data=b"{not json",
                       headers={"Content-Type": "application/json"}, retries=3, retry_delay=1.5)
    try:
        d = json.loads(b) if b else None
    except ValueError:
        d = None
    check(results, "MCFG: PUT zone 0 malformed body -> 400 BAD_JSON",
          s == 400 and isinstance(d, dict) and d.get("error") == "BAD_JSON", (s, b[:200]))

    # 6. /api/wifi shape refusals (nothing is applied on either)
    s, d, b = post_json(api, "/api/wifi", {"sta": {"ssid": "x"}}, retries=3, retry_delay=1.5)
    check(results, "MCFG: POST /api/wifi sta without pass -> 400 INVALID",
          s == 400 and isinstance(d, dict) and d.get("error") == "INVALID", (s, b[:200]))
    s, d, b = post_json(api, "/api/wifi", {"sta": {"ssid": "x", "pass": ""}, "ap": {"ssid": "y", "pass": "yyyyyyyy"}},
                        retries=3, retry_delay=1.5)
    check(results, "MCFG: POST /api/wifi with both sta and ap -> 400 INVALID",
          s == 400 and isinstance(d, dict) and d.get("error") == "INVALID", (s, b[:200]))

    # 7. wrong old password
    s, d, b = post_json(api, "/api/password", {"old": password + "-wrong", "new": "whatever12"},
                        retries=3, retry_delay=1.5)
    check(results, "MCFG: POST /api/password with a wrong old password -> 403 BAD_PASSWORD",
          s == 403 and isinstance(d, dict) and d.get("error") == "BAD_PASSWORD", (s, b[:200]))

    # 8. a real change drops every other session; always restored
    other = ApiClient(ip, timeout=timeout)
    other_ok = other.login(password, retries=4, retry_delay=2.0)
    check(results, "MCFG: a second client logs in before the change", other_ok, "")
    temp = mcfg_temp_password(password)
    attempted = False
    try:
        attempted = True
        # retries=1: a transport retry after a lost 204 would resend old=password,
        # which is by then wrong -- the finally below restores either way.
        s, d, b = post_json(api, "/api/password", {"old": password, "new": temp}, retries=1)
        check(results, "MCFG: POST /api/password to a temporary password -> 204", s == 204, (s, b[:200]))
        if s == 204 and other_ok:
            s_o, _ = other.request("GET", "/api/state", retries=3, retry_delay=1.5)
            check(results, "MCFG: the other client's session was dropped -> 401", s_o == 401, s_o)
            s_me, _ = api.request("GET", "/api/state", retries=3, retry_delay=1.5)
            check(results, "MCFG: the changing client holds a fresh cookie -> 200", s_me == 200, s_me)
    finally:
        if attempted:
            restored = mcfg_restore_password(api, ip, temp, password, timeout)
            check(results, "MCFG: web password restored", restored,
                  "" if restored else "RESTORE FAILED: the web password is --password + '_mcfgT'; "
                                      "restore it with the console: SET WEB PASSWORD <password>")
            api.login(password, retries=4, retry_delay=2.0)
```

In `run_standard()`, change the suite tuple (`tools/web_test.py:871-878`) to:

```python
    for name, fn, fnargs in (
        ("SCHEMA", suite_schema, (api, results)),
        ("STATE", suite_state, (api, results)),
        ("CONFIG", suite_config, (api, results, args.config_zone)),
        ("UPLOADS", suite_uploads, (api, results, args.master_bin, args.zone_bin, args.fleet)),
        ("WIFI", suite_wifi, (api, results)),
        ("MCFG", suite_mcfg, (api, results, args.ip, args.password, args.timeout)),
        ("LOGIN", suite_login, (args.ip, args.password, results, args.timeout)),
    ):
```

Change the `--only` help (`:895`) to `"run only these standard suites (schema,state,config,uploads,wifi,mcfg,login); "`.

Change the docstring's first sentence (`:2-6`) to end: `...a Wi-Fi scan, the master-config write paths (MCFG: zone-0 PUT, /api/wifi, /api/password), and an optional long soak.`

- [ ] **Step 6: Re-run the self-test and a syntax check**

Run: `python C:\Projects\HillGrov\tools\web_test.py --selftest`
Expected: `SELFTEST OK (9 assertion groups)`.
Run: `python -m py_compile C:\Projects\HillGrov\tools\web_test.py`
Expected: no output, exit code 0.

The live run of this suite is Stage 0 gate steps 1 and 6: once on today's image, once on the Stage 0 image. It is the proof that Tasks 3-4 kept the web's observable behaviour.

- [ ] **Step 7: Commit**

```powershell
git -C C:\Projects\HillGrov add tools/web_test.py
git -C C:\Projects\HillGrov commit -m "test(tools): web_test MCFG suite for zone-0 PUT, /api/wifi and /api/password" -m "Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>"
```

### Task 2: Split the web-session store into `components/http_auth` (Stage 0)

`panel_svc` (Task 4) needs `http_auth_hash_password()` and `http_auth_sessions_drop()`, and `http_srv` will require `panel_svc`. If those two stayed inside `http_srv`, the two components would require each other. So the store moves to its own component, unchanged in logic.

**Files:**
- Create: `components/http_auth/CMakeLists.txt`, `components/http_auth/http_auth.h`.
- Move: `components/http_srv/http_auth.c` → `components/http_auth/http_auth.c` (includes and one comment edited).
- Modify:
  - `components/http_srv/http_srv.h`: delete `http_auth_init()` and its comment (`:33-41`), and the whole *shared web-password state* block (`:73-90`); add `#include "http_auth.h"`. (These are the file's real lines at `aceb311`.)
  - `components/http_srv/http_srv_internal.h`: delete the facade block (`:43-50`).
  - `components/http_srv/CMakeLists.txt`: drop `"http_auth.c"` (`:13`); add `http_auth` to `REQUIRES` (`:21`).
  - `master/main/CMakeLists.txt`: add `http_auth` to `REQUIRES` (`:2`).
  - `master/main/net_ops_master.c`: `#include "http_srv.h"` (`:6`) becomes `#include "http_auth.h"`. It uses only the two password functions.

**Interfaces:**
- Consumes: nothing new.
- Produces: `components/http_auth/http_auth.h`. The signatures are identical to today's, and it includes `web_auth.h` and `hg_mcfg.h`:
  ```c
  int  http_auth_init(void);
  int  http_auth_check(const char *cookie_hdr);                                   /* 0 valid / -1 not */
  int  http_auth_login(const char *pw, char cookie_out[2 * WA_TOKEN_LEN + 1]);    /* 0 / -1 bad / -2 locked */
  int  http_auth_verify_password(const char *pw);                                 /* 0 correct / -1 wrong */
  void http_auth_logout_cookie(const char *cookie_hdr);
  int  http_auth_hash_password(hg_mcfg_t *m, const char *pw);                     /* 0 / -1 length / -3 no SHA-256 */
  void http_auth_sessions_drop(void);
  ```
  The CMake uses the http_srv pattern: `set(HTTP_AUTH_SRCS "")`, then `if("${CMAKE_PROJECT_NAME}" STREQUAL "hillgrow_master") set(HTTP_AUTH_SRCS "http_auth.c") endif()`.

This is a pure move, with no host-testable logic change. The builds prove it links. The web LOGIN and MCFG suites at the Stage 0 gate prove it behaves.

- [ ] **Step 1: Create the component's CMakeLists**

`components/http_auth/CMakeLists.txt`:

```cmake
# The web-session store (the ONE wa_state_t: live login sessions + the sha/rand
# hooks web_auth needs), split out of http_srv so components/panel_svc can call
# http_auth_hash_password()/http_auth_sessions_drop() while http_srv itself
# requires panel_svc -- no dependency cycle. Master-only sources, include-only
# on zone/rescue, exactly the http_srv pattern (components/http_srv/CMakeLists.txt).
# REQUIRES is unconditional on purpose: IDF evaluates it in an early pass where
# CMAKE_PROJECT_NAME is not yet set (components/wifi_mgr/CMakeLists.txt:14-19).
set(HTTP_AUTH_SRCS "")
if("${CMAKE_PROJECT_NAME}" STREQUAL "hillgrow_master")
    set(HTTP_AUTH_SRCS "http_auth.c")
endif()

idf_component_register(
    SRCS ${HTTP_AUTH_SRCS}
    INCLUDE_DIRS "."
    REQUIRES web_auth hg_mcfg
    PRIV_REQUIRES freertos esp_hw_support nvs_flash mbedtls app_common time_svc
)
```

- [ ] **Step 2: Create the header**

`components/http_auth/http_auth.h`:

```c
#pragma once
#include <stdint.h>
#include "web_auth.h"   /* WA_TOKEN_LEN */
#include "hg_mcfg.h"    /* hg_mcfg_t */

#ifdef __cplusplus
extern "C" {
#endif

/* The master's web-session store: the ONE wa_state_t in the firmware, holding
 * the live login sessions plus the sha/rand hooks web_auth needs. The httpd
 * task (login/logout/cookie checks, POST /api/password), the CLI task (SET WEB
 * PASSWORD) and the panel (System > Web password, spec Decision 3) all reach
 * it, so every entry point takes this component's mutex internally (3000 ms)
 * and nothing outside http_auth.c ever sees the struct. Sharing it is the
 * point: a password changed from any face drops every web session.
 *
 * Moved out of components/http_srv unchanged in logic (panel plan Task 2), so
 * components/panel_svc can sit beneath http_srv without a dependency cycle. */

/* One-time init: PSA crypto + web_auth_init() over the session array, then the
 * persisted sessions from NVS ("hg"/"sess"). Idempotent. Call it at boot once
 * nvs_flash_init() has run -- app_main does, and so does http_srv_start(),
 * because a boot whose radio never came up still has to be able to hash a
 * console SET WEB PASSWORD. Until it has succeeded every entry point answers
 * as if the board had no crypto (a login fails, a password change reports
 * ERR INTERNAL) rather than touching uninitialised state. 0 ok, -1 if crypto
 * or the mutex is unavailable. */
int  http_auth_init(void);

int  http_auth_check(const char *cookie_hdr);                                   /* 0 valid / -1 not */
int  http_auth_login(const char *pw, char cookie_out[2 * WA_TOKEN_LEN + 1]);    /* 0 / -1 bad / -2 locked */
int  http_auth_verify_password(const char *pw);   /* 0 correct / -1 wrong; no session, no fail count */
void http_auth_logout_cookie(const char *cookie_hdr);

/* Puts a fresh salt + sha256(salt||pw) into *m and clears MCFG_F_WEB_DEFAULT,
 * without touching NVS -- the caller still owns the mcfg_commit(). Does NOT
 * drop sessions (see http_auth_sessions_drop, to be called only after the
 * commit succeeded). 0 ok, -1 pw outside 8..63 (*m untouched), -3 SHA-256
 * unavailable -- the board's crypto is broken, and in that case *m HAS been
 * modified and now holds a fresh salt with an all-zero digest that nothing
 * could ever match, so the caller must discard its copy and commit nothing. */
int  http_auth_hash_password(hg_mcfg_t *m, const char *pw);

/* Invalidates every live web session and rewrites NVS. Call right after a
 * successful password commit: the old cookies must not outlive the password
 * they were issued against. */
void http_auth_sessions_drop(void);

#ifdef __cplusplus
}
#endif
```

- [ ] **Step 3: Move the implementation and fix its includes**

```powershell
New-Item -ItemType Directory -Force C:\Projects\HillGrov\components\http_auth | Out-Null
git -C C:\Projects\HillGrov mv components/http_srv/http_auth.c components/http_auth/http_auth.c
```

In `components/http_auth/http_auth.c`, replace line 12, `#include "http_srv_internal.h"`, with:

```c
#include "web_auth.h"
#include "http_auth.h"
```

And in the comment at `:16-24`, replace `net_ops_master.c) reach it, so every access goes through s_lock and nothing outside this file ever sees the struct -- the rest of http_srv uses the locked facade in http_srv_internal.h.` with:

```c
 * net_ops_master.c) reach it, so every access goes through s_lock and nothing
 * outside this file ever sees the struct -- every caller uses the locked
 * facade in http_auth.h.
```

- [ ] **Step 4: Point http_srv at the new header**

In `components/http_srv/http_srv.h`:
- delete lines 33-41 (the `http_auth_init` comment and prototype);
- delete lines 73-90 (from `/* ---- shared web-password state (master/main/net_ops_master.c) ----` through `void http_auth_sessions_drop(void);`);
- after `#include "hg_mcfg.h"` (`:5`), add:

```c
#include "http_auth.h"   /* the web-session store (components/http_auth) -- every http_auth_* entry point */
```

In `components/http_srv/http_srv_internal.h`, delete lines 43-50: the `/* ---- http_auth.c: the locked facade over the shared wa_state_t ----` comment and the four prototypes under it. They are now in `http_auth.h`, which this header reaches through `http_srv.h`.

In `components/http_srv/CMakeLists.txt`, replace lines 13-15 and 21 so the file reads:

```cmake
    list(APPEND HTTP_SRV_SRCS "http_routes.c" "http_srv.c" "http_login.c" "http_cmd.c" "http_static.c"
         "http_api.c" "http_api_cfg.c" "http_upload.c" "http_upload_master.c" "http_upload_zone.c"
         "http_fleet.c")
```
```cmake
    REQUIRES esp_http_server cmd_core cmd_task web_auth hg_mcfg web_assets fw_srv mcfg_ops http_auth
```

- [ ] **Step 5: Point master/main at the new component**

`master/main/CMakeLists.txt` line 2 becomes:

```cmake
    REQUIRES board cli cmd_task cmd_common app_common notify app_update esp_timer nvs_flash ota_trial ring_link node_mgr master_cmds wifi_mgr fw_srv http_srv http_auth hg_mcfg time_svc alarm_mgr mcfg_ops cp_ota)
```

`master/main/net_ops_master.c` line 6, `#include "http_srv.h"`, becomes `#include "http_auth.h"`.

- [ ] **Step 6: Run the gates**

Run GATE-HOST. Expected: 35/35 tests pass (+0).
Run GATE-ESP32. Expected: master, zone and rescue each print `Project build complete` with 0 warnings, and the lock diff is empty.
Run GATE-P4. Expected: `Project build complete`, 0 warnings, the target grep matches, then the lock checkout.

- [ ] **Step 7: Commit**

```powershell
git -C C:\Projects\HillGrov add components/http_auth/CMakeLists.txt components/http_auth/http_auth.h components/http_auth/http_auth.c components/http_srv/http_srv.h components/http_srv/http_srv_internal.h components/http_srv/CMakeLists.txt master/main/CMakeLists.txt master/main/net_ops_master.c
git -C C:\Projects\HillGrov commit -m "refactor(http_auth): split the web-session store out of http_srv" -m "Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>"
```

### Task 3: `panel_svc` core: refusal vocabulary, edit record, the one master-config edit, `mcfg_ops_edit_ms` (Stage 0)

This creates the component both faces will call, and its first three pieces:
- one refusal vocabulary, so a refusal means the same thing on the web and on the glass;
- one field-edit record;
- the one canonical master-config read-modify-write.

It builds on `mcfg_ops_edit()` and gives it a caller-chosen lock budget. The web keeps today's 100 ms, and the panel worker can wait 6000 ms. Nothing calls the new code yet: Task 4 moves the web onto it.

**Files:**
- Create:
  - `components/panel_svc/CMakeLists.txt`: `PSVC_SRCS` `psvc_rc.c` and `psvc_mcfg.c` for `hillgrow_master`; `REQUIRES hg_cfg hg_mcfg`; `PRIV_REQUIRES mcfg_ops time_core hg_json wifi_mgr time_svc`.
  - `components/panel_svc/psvc_rc.h`, `.c`; `components/panel_svc/psvc_edit.h`; `components/panel_svc/psvc_mcfg.h`, `.c`.
  - `tests/host/test_psvc_rc.c`, `tests/host/test_psvc_mcfg.c`.
  - `tests/host/fakes/fake_apply.h`, `.c`: counting `int wifi_mgr_apply(void)` and `void time_svc_apply_mcfg(void)`, plus `fake_apply_reset()`.
- Modify:
  - `components/mcfg_ops/mcfg_ops.h`, `.c`: `lock_take(ms)` and `mcfg_ops_edit_ms()`.
  - `tests/host/test_mcfg_ops.c`: a short budget refuses while the lock is held; `mcfg_ops_edit` behaves exactly as before.
  - `tests/host/CMakeLists.txt`: include `${COMP}/panel_svc` and `${COMP}/time_svc` (`psvc_mcfg.c` includes `time_svc.h`, a pure header); rows for `test_psvc_rc` and `test_psvc_mcfg`.

**Interfaces:**
- Consumes: `mcfg_ops_edit` (`mcfg_ops.h:48`), `mcfg_get` and `mcfg_commit` (`mcfg_store.h:35-36`), `hg_mcfg_validate` and `hg_mcfg_is_secret` (`hg_mcfg.h:42,45`), `tz_check` (`time_core.h:43`), `hg_json_merge_mcfg` (`hg_json.h:59`), `hg_field_write` (`hg_cfg.h:28`), `wifi_mgr_apply` (`wifi_mgr.h:63`) and `time_svc_apply_mcfg` (`time_svc.h:38`).
- Produces:
  ```c
  /* mcfg_ops.h */
  #define MCFG_OPS_EDIT_LOCK_MS 6000u
  int mcfg_ops_edit_ms(int (*fn)(hg_mcfg_t *m, void *ctx), void *ctx,
                       void (*apply)(void *ctx), const char *what, uint32_t lock_ms);  /* same rc contract as mcfg_ops_edit */
  /* mcfg_ops_edit(fn,ctx,apply,what) == mcfg_ops_edit_ms(fn,ctx,apply,what,MCFG_OPS_EDIT_LOCK_MS) -- unchanged for its callers */

  /* psvc_rc.h (pure) */
  typedef enum {
      PSVC_OK = 0,
      PSVC_E_BUSY, PSVC_E_INVALID, PSVC_E_BAD_JSON, PSVC_E_INVALID_FIELD, PSVC_E_VALIDATION,
      PSVC_E_NO_CACHE, PSVC_E_ZONE_UNKNOWN, PSVC_E_ZONE_NOT_ONLINE, PSVC_E_STORAGE, PSVC_E_INTERNAL,
      PSVC_E_FLEET_BUSY, PSVC_E_FLEET_REJECTED, PSVC_E_NOT_ACTIVE,
      PSVC_E_UPLOAD_ACTIVE, PSVC_E_FLEET_ACTIVE, PSVC_E_TRIAL_PENDING, PSVC_E_NO_SLOT, PSVC_E_LOW_HEAP,
      PSVC_E_TOO_LARGE, PSVC_E_IMAGE_MISMATCH, PSVC_E_WRITE_FAILED, PSVC_E_STALLED, PSVC_E_RECV_FAILED,
      PSVC_E_ZONE_FW_BUSY,
      PSVC_RC_COUNT
  } psvc_rc_t;
  const char *psvc_rc_token(psvc_rc_t rc);  /* "OK","BUSY","INVALID","BAD_JSON",...,"ZONE_FW_BUSY" -- the enum name minus
                                               "PSVC_E_" ("OK" for PSVC_OK); "INTERNAL" for rc >= PSVC_RC_COUNT */
  int psvc_rc_to_net_legacy(psvc_rc_t rc);  /* net_ops_t convention (master_cmds.h:46-60): OK 0;
                                               INVALID/VALIDATION/INVALID_FIELD/BAD_JSON/ZONE_UNKNOWN -1;
                                               BUSY/STORAGE -2; anything else -3 */

  /* psvc_edit.h (pure) */
  #define PSVC_EDIT_BAD_JSON      1     /* positive refusals an edit fn returns (mcfg_ops.h: fn never returns negative) */
  #define PSVC_EDIT_INVALID_FIELD 2
  #define PSVC_EDIT_VALIDATION    3
  #define PSVC_FEDIT_TEXT_MAX     65    /* longest field text + NUL (STA_PASS/AP_PASS capacity 64) */
  typedef struct {
      uint8_t           group;          /* zone: hg_group_t; master: hg_mgroup_t */
      int8_t            idx;            /* shelf 0..3 / aux 0..1; -1 for zone scope 0 and every master row */
      const hg_field_t *f;              /* the HG_FIELDS / HG_MFIELDS row */
      char              text[PSVC_FEDIT_TEXT_MAX];   /* hg_field_write() text form */
  } psvc_fedit_t;
  typedef struct { const psvc_fedit_t *e; int n; } psvc_fedits_t;

  /* psvc_mcfg.h */
  #define PSVC_LOCK_LEGACY_MS 6000u   /* CLI rows, POST /api/wifi, POST /api/password -- today's budget */
  #define PSVC_LOCK_WEB_MS     100u   /* PUT /api/config?zone=0 and GET /api/wifi/scan -- today's budget */
  #define PSVC_LOCK_PANEL_MS  6000u   /* the panel worker can wait; the UI shows pending */
  typedef int (*psvc_mcfg_fn)(hg_mcfg_t *m, void *ctx, char *err, size_t errcap);   /* 0 proceed / PSVC_EDIT_* */
  /* [WORKER] mcfg_ops_edit_ms(lock_ms): snapshot -> fn -> hg_mcfg_validate(m, tz_check, err) (path out) -> commit ->
   * apply = wifi_mgr_apply(); time_svc_apply_mcfg(); under the lock (exactly http_api_cfg.c:244-245).
   * lock -1 -> PSVC_E_BUSY; fn 1/2/3 -> BAD_JSON/INVALID_FIELD/VALIDATION (err = path); commit -2 -> PSVC_E_STORAGE;
   * commit -3 -> PSVC_E_VALIDATION with err[0] = '\0'. apply runs only on PSVC_OK. MCFG_F_AP_DEFAULT is NOT touched (D9). */
  psvc_rc_t psvc_mcfg_edit(psvc_mcfg_fn fn, void *ctx, uint32_t lock_ms, const char *what, char *err, size_t errcap);
  int  psvc_mcfg_json_fn(hg_mcfg_t *m, void *ctx /* const char *json */, char *err, size_t errcap);
       /* hg_json_merge_mcfg: -1 -> PSVC_EDIT_BAD_JSON, -2 -> PSVC_EDIT_INVALID_FIELD (err "GROUP.KEY"), 0 -> 0 */
  int  psvc_mcfg_fields_fn(hg_mcfg_t *m, void *ctx /* const psvc_fedits_t * */, char *err, size_t errcap);
       /* per edit: hg_mcfg_is_secret(f) && text[0]=='\0' -> skipped (blank = unchanged);
          hg_field_write(f, m, text) != 0 -> PSVC_EDIT_INVALID_FIELD, err "<HG_MGROUP_NAMES[group]>.<key>" */
  void psvc_mcfg_get(hg_mcfg_t *out);   /* [ANY] *out = *mcfg_get() */
  ```
- Interface additions (host fakes only): `extern int fake_apply_wifi_n, fake_apply_time_n, fake_apply_wifi_rc;` in `fakes/fake_apply.h`.

- [ ] **Step 1: Write the failing `mcfg_ops_edit_ms` tests**

In `tests/host/test_mcfg_ops.c`, add before `int main(void)` (`:147`):

```c
/* Panel plan Task 3: mcfg_ops_edit_ms() is mcfg_ops_edit() with a caller-chosen
   lock budget, so the web can keep its 100 ms try-lock and the panel worker can
   wait 6000 ms on the SAME lock. The fake mutex fails a held take immediately,
   which is the same answer a real one gives once its budget runs out. */
void test_edit_ms_refuses_while_the_lock_is_held(void) {
    mcfg_ops_init();
    TEST_ASSERT_EQUAL_INT(0, mcfg_ops_lock(10));
    TEST_ASSERT_EQUAL_INT(-1, mcfg_ops_edit_ms(set_hostname, "short-budget", NULL, "TEST", 100));
    mcfg_ops_unlock();
    TEST_ASSERT_EQUAL_STRING("hillgrow", mcfg_get()->hostname);   /* nothing committed */
}

void test_edit_ms_commits_exactly_like_edit(void) {
    mcfg_ops_init();
    TEST_ASSERT_EQUAL_INT(0, mcfg_ops_edit_ms(set_hostname, "via-ms", NULL, "TEST", 100));
    TEST_ASSERT_EQUAL_STRING("via-ms", mcfg_get()->hostname);
    TEST_ASSERT_EQUAL_INT(3, mcfg_ops_edit_ms(reject, NULL, NULL, "TEST", 100));
    TEST_ASSERT_EQUAL_STRING("via-ms", mcfg_get()->hostname);
}

void test_default_budget_is_still_6000_ms(void) {
    TEST_ASSERT_EQUAL_UINT32(6000u, MCFG_OPS_EDIT_LOCK_MS);
}
```

And add to `main()`, after the last `RUN_TEST`:

```c
    RUN_TEST(test_edit_ms_refuses_while_the_lock_is_held);
    RUN_TEST(test_edit_ms_commits_exactly_like_edit);
    RUN_TEST(test_default_budget_is_still_6000_ms);
```

- [ ] **Step 2: Run it to verify it fails**

Run the one-test command with `-R test_mcfg_ops`.
Expected: the host build stops with `error C2065: 'MCFG_OPS_EDIT_LOCK_MS': undeclared identifier` (and a C4013 warning for `mcfg_ops_edit_ms`).

- [ ] **Step 3: Implement `mcfg_ops_edit_ms`**

In `components/mcfg_ops/mcfg_ops.h`, replace the `mcfg_ops_edit` prototype (`:48-49`) with:

```c
int  mcfg_ops_edit(int (*fn)(hg_mcfg_t *m, void *ctx), void *ctx,
                    void (*apply)(void *ctx), const char *what);

/* mcfg_ops_edit() with a caller-chosen lock budget -- identical in every other
 * respect (rc contract, private copy, apply under the lock). mcfg_ops_edit()
 * is exactly mcfg_ops_edit_ms(..., MCFG_OPS_EDIT_LOCK_MS). The web's zone-0
 * PUT keeps its historic 100 ms try-lock through this, while the panel worker,
 * which can show "pending", waits the full 6000 ms (panel plan Task 3). */
#define MCFG_OPS_EDIT_LOCK_MS 6000u   /* > mcfg_commit()'s own 5000 ms mutex timeout, so a
                                         caller that loses a race reports the commit's verdict */
int  mcfg_ops_edit_ms(int (*fn)(hg_mcfg_t *m, void *ctx), void *ctx,
                       void (*apply)(void *ctx), const char *what, uint32_t lock_ms);
```

In `components/mcfg_ops/mcfg_ops.c`, replace `lock_take()` (`:29-34`) with:

```c
static int lock_take(uint32_t ms) {
    if (!s_lock) return 0;   /* fail CLOSED: not created yet is NOT an open lock */
    return xSemaphoreTake(s_lock, pdMS_TO_TICKS(ms)) == pdTRUE;
}
```

and replace `mcfg_ops_edit()` (`:77-91`) with:

```c
int mcfg_ops_edit_ms(int (*fn)(hg_mcfg_t *m, void *ctx), void *ctx,
                      void (*apply)(void *ctx), const char *what, uint32_t lock_ms) {
    if (!lock_take(lock_ms)) return -1;
    hg_mcfg_t m = *mcfg_get();
    int rc = fn(&m, ctx);
    if (rc == 0) {
        rc = commit_and_log(&m, what);
        /* STILL holding the lock here, on purpose: apply must run in the
         * same exclusion a caller like GET /api/wifi/scan holds mcfg_ops_lock()
         * across, or the radio reconfigure it triggers could land mid-scan. */
        if (rc == 0 && apply) apply(ctx);
    }
    lock_give();
    return rc;
}

int mcfg_ops_edit(int (*fn)(hg_mcfg_t *m, void *ctx), void *ctx,
                   void (*apply)(void *ctx), const char *what) {
    return mcfg_ops_edit_ms(fn, ctx, apply, what, MCFG_OPS_EDIT_LOCK_MS);
}
```

- [ ] **Step 4: Run it to verify it passes**

Run the one-test command with `-R test_mcfg_ops`.
Expected: `100% tests passed, 0 tests failed out of 1`. The 10 existing cases and the 3 new ones all pass.

- [ ] **Step 5: Write the failing `psvc_rc` test**

`tests/host/test_psvc_rc.c`:

```c
/* The refusal vocabulary both faces speak. Two properties matter: every value
   has exactly the token the web already sends (so a refusal reads the same on
   the phone and on the glass), and the legacy mapping gives the CLI rows and
   POST /api/wifi the SAME -1/-2/-3 they answered before panel_svc existed
   (master_cmds.h:46-60) -- including master-config contention reported as
   -2 "could not be stored" (D10). */
#include <string.h>
#include "unity.h"
#include "psvc_rc.h"

void setUp(void) {}
void tearDown(void) {}

static const char *const WANT[] = {
    "OK", "BUSY", "INVALID", "BAD_JSON", "INVALID_FIELD", "VALIDATION",
    "NO_CACHE", "ZONE_UNKNOWN", "ZONE_NOT_ONLINE", "STORAGE", "INTERNAL",
    "FLEET_BUSY", "FLEET_REJECTED", "NOT_ACTIVE",
    "UPLOAD_ACTIVE", "FLEET_ACTIVE", "TRIAL_PENDING", "NO_SLOT", "LOW_HEAP",
    "TOO_LARGE", "IMAGE_MISMATCH", "WRITE_FAILED", "STALLED", "RECV_FAILED",
    "ZONE_FW_BUSY",
};

static void test_count_matches_the_token_list(void) {
    TEST_ASSERT_EQUAL_INT((int)(sizeof WANT / sizeof WANT[0]), (int)PSVC_RC_COUNT);
    TEST_ASSERT_EQUAL_INT(25, (int)PSVC_RC_COUNT);
}

static void test_every_value_has_its_token(void) {
    for (int i = 0; i < (int)PSVC_RC_COUNT; i++)
        TEST_ASSERT_EQUAL_STRING(WANT[i], psvc_rc_token((psvc_rc_t)i));
}

static void test_out_of_range_is_internal(void) {
    TEST_ASSERT_EQUAL_STRING("INTERNAL", psvc_rc_token(PSVC_RC_COUNT));
    TEST_ASSERT_EQUAL_STRING("INTERNAL", psvc_rc_token((psvc_rc_t)200));
    TEST_ASSERT_EQUAL_STRING("INTERNAL", psvc_rc_token((psvc_rc_t)-1));
    TEST_ASSERT_EQUAL_INT(-3, psvc_rc_to_net_legacy(PSVC_RC_COUNT));
    TEST_ASSERT_EQUAL_INT(-3, psvc_rc_to_net_legacy((psvc_rc_t)-1));
}

static void test_legacy_mapping_for_every_value(void) {
    for (int i = 0; i < (int)PSVC_RC_COUNT; i++) {
        psvc_rc_t rc = (psvc_rc_t)i;
        int want;
        switch (rc) {
        case PSVC_OK:                                    want = 0;  break;
        case PSVC_E_INVALID: case PSVC_E_VALIDATION:
        case PSVC_E_INVALID_FIELD: case PSVC_E_BAD_JSON:
        case PSVC_E_ZONE_UNKNOWN:                        want = -1; break;
        case PSVC_E_BUSY: case PSVC_E_STORAGE:           want = -2; break;
        default:                                         want = -3; break;
        }
        TEST_ASSERT_EQUAL_INT_MESSAGE(want, psvc_rc_to_net_legacy(rc), psvc_rc_token(rc));
    }
}

int main(void) { UNITY_BEGIN();
    RUN_TEST(test_count_matches_the_token_list);
    RUN_TEST(test_every_value_has_its_token);
    RUN_TEST(test_out_of_range_is_internal);
    RUN_TEST(test_legacy_mapping_for_every_value);
    return UNITY_END(); }
```

In `tests/host/CMakeLists.txt`, extend the `include_directories(...)` list (`:21-27`): after `${COMP}/cp_ota` add `${COMP}/panel_svc ${COMP}/time_svc`. Then append at the end of the file:

```cmake
# panel_svc (panel plan Task 3): the shared service layer both faces call.
# psvc_rc.c is pure; psvc_mcfg.c runs over the REAL mcfg_ops.c, hg_mcfg
# validation, hg_json merge and time_core tz_check -- only the NVS store
# (fake_mcfg_store.c) and the two radio/time apply hooks (fake_apply.c) are faked.
hg_test(test_psvc_rc ${COMP}/panel_svc/psvc_rc.c)
hg_test(test_psvc_mcfg ${COMP}/panel_svc/psvc_mcfg.c ${COMP}/panel_svc/psvc_rc.c ${COMP}/mcfg_ops/mcfg_ops.c
        ${COMP}/hg_mcfg/hg_mcfg.c ${HG_CFG_SRC} ${COMP}/hg_blob/hg_blob.c ${COMP}/hg_json/hg_json_mcfg.c
        ${COMP}/time_core/time_core.c fakes/fake_mcfg_store.c fakes/fake_apply.c)
target_link_libraries(test_psvc_mcfg cjson_host)
```

- [ ] **Step 6: Run it to verify it fails**

Run the one-test command with `-R test_psvc_rc`.
Expected: configure fails with `Cannot find source file: .../components/panel_svc/psvc_rc.c` (and `.../psvc_mcfg.c`). None of the component's files exist yet.

- [ ] **Step 7: Implement the vocabulary and the edit record**

`components/panel_svc/psvc_rc.h`:

```c
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The ONE refusal vocabulary both faces speak (spec "Error handling": a
 * refusal must mean the same thing on the web and on the panel). Each value's
 * token is exactly the error code the web already sends -- http_srv_error()
 * codes and CLI ERR tokens -- so psvc_rc_token() is also what the web maps
 * back onto HTTP. Pure: no IDF headers. Append new values before
 * PSVC_RC_COUNT and add the token in psvc_rc.c (a static assert ties the two). */
typedef enum {
    PSVC_OK = 0,
    PSVC_E_BUSY, PSVC_E_INVALID, PSVC_E_BAD_JSON, PSVC_E_INVALID_FIELD, PSVC_E_VALIDATION,
    PSVC_E_NO_CACHE, PSVC_E_ZONE_UNKNOWN, PSVC_E_ZONE_NOT_ONLINE, PSVC_E_STORAGE, PSVC_E_INTERNAL,
    PSVC_E_FLEET_BUSY, PSVC_E_FLEET_REJECTED, PSVC_E_NOT_ACTIVE,
    PSVC_E_UPLOAD_ACTIVE, PSVC_E_FLEET_ACTIVE, PSVC_E_TRIAL_PENDING, PSVC_E_NO_SLOT, PSVC_E_LOW_HEAP,
    PSVC_E_TOO_LARGE, PSVC_E_IMAGE_MISMATCH, PSVC_E_WRITE_FAILED, PSVC_E_STALLED, PSVC_E_RECV_FAILED,
    PSVC_E_ZONE_FW_BUSY,
    PSVC_RC_COUNT
} psvc_rc_t;

/* "OK", "BUSY", ... "ZONE_FW_BUSY": the enum name minus "PSVC_E_" ("OK" for
 * PSVC_OK). "INTERNAL" for anything outside 0..PSVC_RC_COUNT-1. */
const char *psvc_rc_token(psvc_rc_t rc);

/* The net_ops_t convention the CLI rows and POST /api/wifi answer with
 * (master_cmds.h:46-60): OK 0; INVALID/VALIDATION/INVALID_FIELD/BAD_JSON/
 * ZONE_UNKNOWN -1 ("the value is wrong"); BUSY/STORAGE -2 ("could not be
 * stored, retry" -- lock contention stays -2 there, D10); anything else -3. */
int psvc_rc_to_net_legacy(psvc_rc_t rc);

#ifdef __cplusplus
}
#endif
```

`components/panel_svc/psvc_rc.c`:

```c
#include "psvc_rc.h"

/* Enum order, one per value. */
static const char *const TOK[] = {
    "OK", "BUSY", "INVALID", "BAD_JSON", "INVALID_FIELD", "VALIDATION",
    "NO_CACHE", "ZONE_UNKNOWN", "ZONE_NOT_ONLINE", "STORAGE", "INTERNAL",
    "FLEET_BUSY", "FLEET_REJECTED", "NOT_ACTIVE",
    "UPLOAD_ACTIVE", "FLEET_ACTIVE", "TRIAL_PENDING", "NO_SLOT", "LOW_HEAP",
    "TOO_LARGE", "IMAGE_MISMATCH", "WRITE_FAILED", "STALLED", "RECV_FAILED",
    "ZONE_FW_BUSY",
};
_Static_assert(sizeof TOK / sizeof TOK[0] == PSVC_RC_COUNT, "one token per psvc_rc_t, in enum order");

const char *psvc_rc_token(psvc_rc_t rc) {
    unsigned i = (unsigned)rc;   /* a negative value wraps high and lands in the fallback */
    return i < (unsigned)PSVC_RC_COUNT ? TOK[i] : "INTERNAL";
}

int psvc_rc_to_net_legacy(psvc_rc_t rc) {
    switch (rc) {
    case PSVC_OK:
        return 0;
    case PSVC_E_INVALID:
    case PSVC_E_VALIDATION:
    case PSVC_E_INVALID_FIELD:
    case PSVC_E_BAD_JSON:
    case PSVC_E_ZONE_UNKNOWN:
        return -1;
    case PSVC_E_BUSY:
    case PSVC_E_STORAGE:
        return -2;
    default:
        return -3;
    }
}
```

`components/panel_svc/psvc_edit.h`:

```c
#pragma once
#include <stdint.h>
#include "hg_cfg.h"   /* hg_field_t */

#ifdef __cplusplus
extern "C" {
#endif

/* The ONE field-edit record, for zone rows (HG_FIELDS) and master rows
 * (HG_MFIELDS) alike, used by both faces. A save applies a SET of these to a
 * fresh copy taken at save time, never a whole edited struct: the web PUT has
 * no generation check, so writes are last-writer-wins per field (panel plan
 * Global Constraints, "Saves"). Pure. */

/* Positive refusals an edit function returns. mcfg_ops.h reserves every
 * NEGATIVE return of an edit fn for itself, so these must stay > 0. */
#define PSVC_EDIT_BAD_JSON      1
#define PSVC_EDIT_INVALID_FIELD 2
#define PSVC_EDIT_VALIDATION    3

#define PSVC_FEDIT_TEXT_MAX     65    /* longest field text + NUL (STA_PASS/AP_PASS capacity 64) */

typedef struct {
    uint8_t           group;          /* zone: hg_group_t; master: hg_mgroup_t */
    int8_t            idx;            /* shelf 0..3 / aux 0..1; -1 for zone scope 0 and every master row */
    const hg_field_t *f;              /* the HG_FIELDS / HG_MFIELDS row */
    char              text[PSVC_FEDIT_TEXT_MAX];   /* hg_field_write() text form; NUL-terminated */
} psvc_fedit_t;

typedef struct { const psvc_fedit_t *e; int n; } psvc_fedits_t;

#ifdef __cplusplus
}
#endif
```

- [ ] **Step 8: Run the vocabulary test to verify it passes**

`test_psvc_mcfg`'s sources do not exist yet, so the host project still cannot configure. Temporarily comment out the four `test_psvc_mcfg` lines added in Step 5 (prefix each line of the `hg_test(test_psvc_mcfg ...)` call and the `target_link_libraries(test_psvc_mcfg ...)` line with `#`). Run the one-test command with `-R test_psvc_rc`.
Expected: `100% tests passed, 0 tests failed out of 1`. Then remove the `#`s again.

- [ ] **Step 9: Write the failing `psvc_mcfg` test and its fake**

`tests/host/fakes/fake_apply.h`:

```c
#pragma once
/* Counting stand-ins for the two apply hooks psvc_mcfg_edit() runs under the
 * mcfg_ops lock after a successful commit: wifi_mgr_apply() (esp_hosted RPCs
 * on the P4) and time_svc_apply_mcfg() (the SNTP/TZ reload). The real ones are
 * IDF-bound; the tests only need to know whether, and how often, each ran. */
extern int fake_apply_wifi_n;    /* wifi_mgr_apply() calls since fake_apply_reset() */
extern int fake_apply_time_n;    /* time_svc_apply_mcfg() calls since fake_apply_reset() */
extern int fake_apply_wifi_rc;   /* what the next wifi_mgr_apply() returns (default 0) */
void fake_apply_reset(void);
```

`tests/host/fakes/fake_apply.c`:

```c
#include "wifi_mgr.h"    /* the real prototypes, so a signature drift fails the host build */
#include "time_svc.h"
#include "fake_apply.h"

int fake_apply_wifi_n;
int fake_apply_time_n;
int fake_apply_wifi_rc;

void fake_apply_reset(void) {
    fake_apply_wifi_n = 0;
    fake_apply_time_n = 0;
    fake_apply_wifi_rc = 0;
}

int wifi_mgr_apply(void) { fake_apply_wifi_n++; return fake_apply_wifi_rc; }
void time_svc_apply_mcfg(void) { fake_apply_time_n++; }
```

`tests/host/test_psvc_mcfg.c`:

```c
/* psvc_mcfg_edit() is the ONE master-config read-modify-write, for the web's
   zone-0 PUT (JSON) and the panel's Config editor (field edits) alike. What is
   pinned here is what a second copy got wrong before: every refusal names the
   field and commits nothing, a blank secret is "unchanged" (never a wipe of the
   house Wi-Fi password), contention is BUSY rather than STORAGE for the panel
   (D10), apply runs exactly once and only after a commit, and a zone-0 edit
   leaves MCFG_F_AP_DEFAULT alone (D9). */
#include <stdio.h>
#include <string.h>
#include "unity.h"
#include "psvc_mcfg.h"
#include "mcfg_ops.h"
#include "mcfg_store.h"
#include "fake_mcfg_store.h"
#include "fake_apply.h"

void setUp(void) {
    mcfg_store_init();                    /* defaults: hostname "hillgrow", AP HillGrow/hillgrow1, both DEFAULT flags */
    fake_mcfg_store_force_storage_fail(0);
    fake_apply_reset();
    mcfg_ops_init();
}
void tearDown(void) { fake_mcfg_store_force_storage_fail(0); }

static const hg_field_t *mrow(const char *key) {
    for (int i = 0; i < HG_MFIELD_COUNT; i++)
        if (strcmp(HG_MFIELDS[i].key, key) == 0) return &HG_MFIELDS[i];
    return NULL;
}

static psvc_fedit_t medit(const char *key, const char *text) {
    psvc_fedit_t e;
    memset(&e, 0, sizeof e);
    e.f = mrow(key);
    e.group = e.f ? e.f->group : 0;
    e.idx = -1;
    snprintf(e.text, sizeof e.text, "%s", text);
    return e;
}

static psvc_rc_t edit_fields(const psvc_fedit_t *e, int n, char *err, size_t cap) {
    psvc_fedits_t set = { e, n };
    return psvc_mcfg_edit(psvc_mcfg_fields_fn, &set, PSVC_LOCK_PANEL_MS, "TEST FIELDS", err, cap);
}

static psvc_rc_t edit_json(const char *json, uint32_t lock_ms, char *err, size_t cap) {
    return psvc_mcfg_edit(psvc_mcfg_json_fn, (void *)json, lock_ms, "TEST JSON", err, cap);
}

static void test_fields_edit_commits_and_applies_once(void) {
    psvc_fedit_t e = medit("HOSTNAME", "greenhouse-1");
    char err[64] = "x";
    TEST_ASSERT_EQUAL_INT(PSVC_OK, edit_fields(&e, 1, err, sizeof err));
    TEST_ASSERT_EQUAL_STRING("", err);
    TEST_ASSERT_EQUAL_STRING("greenhouse-1", mcfg_get()->hostname);
    TEST_ASSERT_EQUAL_UINT32(1, mcfg_gen());
    TEST_ASSERT_EQUAL_INT(1, fake_apply_wifi_n);
    TEST_ASSERT_EQUAL_INT(1, fake_apply_time_n);
}

static void test_blank_secret_means_unchanged(void) {
    psvc_fedit_t first[2] = { medit("STA_SSID", "house"), medit("STA_PASS", "housepass1") };
    char err[64];
    TEST_ASSERT_EQUAL_INT(PSVC_OK, edit_fields(first, 2, err, sizeof err));
    psvc_fedit_t second[2] = { medit("STA_SSID", "house2"), medit("STA_PASS", "") };
    TEST_ASSERT_EQUAL_INT(PSVC_OK, edit_fields(second, 2, err, sizeof err));
    TEST_ASSERT_EQUAL_STRING("house2", mcfg_get()->sta_ssid);
    TEST_ASSERT_EQUAL_STRING("housepass1", mcfg_get()->sta_pass);   /* never wiped by a blank */
}

static void test_overlong_hostname_is_invalid_field_and_commits_nothing(void) {
    psvc_fedit_t e = medit("HOSTNAME", "abcdefghijklmnopqrstuvwx");   /* 24 chars > row max 23 */
    char err[64] = "";
    TEST_ASSERT_EQUAL_INT(PSVC_E_INVALID_FIELD, edit_fields(&e, 1, err, sizeof err));
    TEST_ASSERT_EQUAL_STRING("SYS.HOSTNAME", err);
    TEST_ASSERT_EQUAL_UINT32(0, mcfg_gen());
    TEST_ASSERT_EQUAL_STRING("hillgrow", mcfg_get()->hostname);
    TEST_ASSERT_EQUAL_INT(0, fake_apply_wifi_n);
    TEST_ASSERT_EQUAL_INT(0, fake_apply_time_n);
}

static void test_bad_hostname_is_validation_with_path(void) {
    psvc_fedit_t e = medit("HOSTNAME", "Bad Name");   /* writes fine, fails [a-z0-9-] */
    char err[64] = "";
    TEST_ASSERT_EQUAL_INT(PSVC_E_VALIDATION, edit_fields(&e, 1, err, sizeof err));
    TEST_ASSERT_EQUAL_STRING("SYS.HOSTNAME", err);
    TEST_ASSERT_EQUAL_UINT32(0, mcfg_gen());
}

static void test_bad_tz_is_validation_through_tz_check(void) {
    psvc_fedit_t e = medit("TZ", "garbage!!");
    char err[64] = "";
    TEST_ASSERT_EQUAL_INT(PSVC_E_VALIDATION, edit_fields(&e, 1, err, sizeof err));
    TEST_ASSERT_EQUAL_STRING("TIME.TZ", err);
}

static void test_json_malformed_is_bad_json(void) {
    char err[64] = "";
    TEST_ASSERT_EQUAL_INT(PSVC_E_BAD_JSON, edit_json("{", PSVC_LOCK_WEB_MS, err, sizeof err));
    TEST_ASSERT_EQUAL_UINT32(0, mcfg_gen());
    TEST_ASSERT_EQUAL_INT(0, fake_apply_wifi_n);
}

static void test_json_short_ap_pass_is_invalid_field(void) {
    char err[64] = "";
    TEST_ASSERT_EQUAL_INT(PSVC_E_INVALID_FIELD,
                          edit_json("{\"WIFI\":{\"AP_PASS\":\"short\"}}", PSVC_LOCK_WEB_MS, err, sizeof err));
    TEST_ASSERT_EQUAL_STRING("WIFI.AP_PASS", err);
}

static void test_json_good_body_commits(void) {
    char err[64] = "";
    TEST_ASSERT_EQUAL_INT(PSVC_OK,
                          edit_json("{\"TIME\":{\"NTP\":\"time.google.com\"}}", PSVC_LOCK_WEB_MS, err, sizeof err));
    TEST_ASSERT_EQUAL_STRING("time.google.com", mcfg_get()->ntp);
    TEST_ASSERT_EQUAL_INT(1, fake_apply_wifi_n);   /* the web's zone-0 PUT always applied both (http_api_cfg.c:244-245) */
    TEST_ASSERT_EQUAL_INT(1, fake_apply_time_n);
}

static void test_held_lock_is_busy(void) {
    TEST_ASSERT_EQUAL_INT(0, mcfg_ops_lock(10));
    char err[64] = "";
    TEST_ASSERT_EQUAL_INT(PSVC_E_BUSY,
                          edit_json("{\"TIME\":{\"NTP\":\"time.google.com\"}}", 100, err, sizeof err));
    mcfg_ops_unlock();
    TEST_ASSERT_EQUAL_STRING("pool.ntp.org", mcfg_get()->ntp);
    TEST_ASSERT_EQUAL_INT(0, fake_apply_wifi_n);
}

static void test_storage_failure_is_storage_and_does_not_apply(void) {
    fake_mcfg_store_force_storage_fail(1);
    psvc_fedit_t e = medit("HOSTNAME", "greenhouse-2");
    char err[64] = "";
    TEST_ASSERT_EQUAL_INT(PSVC_E_STORAGE, edit_fields(&e, 1, err, sizeof err));
    TEST_ASSERT_EQUAL_STRING("", err);
    TEST_ASSERT_EQUAL_INT(0, fake_apply_wifi_n);
    TEST_ASSERT_EQUAL_INT(0, fake_apply_time_n);
}

static void test_ap_pass_edit_leaves_ap_default(void) {
    TEST_ASSERT_TRUE(mcfg_get()->flags & MCFG_F_AP_DEFAULT);
    psvc_fedit_t e = medit("AP_PASS", "newpass123");
    char err[64] = "";
    TEST_ASSERT_EQUAL_INT(PSVC_OK, edit_fields(&e, 1, err, sizeof err));
    TEST_ASSERT_EQUAL_STRING("newpass123", mcfg_get()->ap_pass);
    TEST_ASSERT_TRUE(mcfg_get()->flags & MCFG_F_AP_DEFAULT);   /* D9: web parity, unchanged */
}

static void test_get_is_a_copy_of_the_live_config(void) {
    hg_mcfg_t m;
    memset(&m, 0xAA, sizeof m);
    psvc_mcfg_get(&m);
    TEST_ASSERT_EQUAL_MEMORY(mcfg_get(), &m, sizeof m);
}

int main(void) { UNITY_BEGIN();
    RUN_TEST(test_fields_edit_commits_and_applies_once);
    RUN_TEST(test_blank_secret_means_unchanged);
    RUN_TEST(test_overlong_hostname_is_invalid_field_and_commits_nothing);
    RUN_TEST(test_bad_hostname_is_validation_with_path);
    RUN_TEST(test_bad_tz_is_validation_through_tz_check);
    RUN_TEST(test_json_malformed_is_bad_json);
    RUN_TEST(test_json_short_ap_pass_is_invalid_field);
    RUN_TEST(test_json_good_body_commits);
    RUN_TEST(test_held_lock_is_busy);
    RUN_TEST(test_storage_failure_is_storage_and_does_not_apply);
    RUN_TEST(test_ap_pass_edit_leaves_ap_default);
    RUN_TEST(test_get_is_a_copy_of_the_live_config);
    return UNITY_END(); }
```

- [ ] **Step 10: Run it to verify it fails**

Run the one-test command with `-R test_psvc_mcfg`.
Expected: configure fails with `Cannot find source file: .../components/panel_svc/psvc_mcfg.c`.

- [ ] **Step 11: Implement `psvc_mcfg` and the component CMake**

`components/panel_svc/psvc_mcfg.h`:

```c
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "hg_mcfg.h"
#include "psvc_rc.h"
#include "psvc_edit.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The ONE master-config read-modify-write (spec Decision 4), over
 * components/mcfg_ops. The web's PUT /api/config?zone=0 calls it with
 * psvc_mcfg_json_fn and a 100 ms budget; the panel's Config editor calls it
 * with psvc_mcfg_fields_fn and 6000 ms. Threading: psvc_mcfg_edit() is
 * [WORKER] -- it can block for lock_ms + mcfg_commit()'s 5 s + the apply's
 * esp_hosted RPCs (P4), so never from the LVGL task nor from a TWDT-subscribed
 * task. psvc_mcfg_get() is [ANY]. */

#define PSVC_LOCK_LEGACY_MS 6000u   /* CLI rows, POST /api/wifi, POST /api/password -- today's budget */
#define PSVC_LOCK_WEB_MS     100u   /* PUT /api/config?zone=0 and GET /api/wifi/scan -- today's budget */
#define PSVC_LOCK_PANEL_MS  6000u   /* the panel worker can wait; the UI shows pending */

typedef int (*psvc_mcfg_fn)(hg_mcfg_t *m, void *ctx, char *err, size_t errcap);   /* 0 proceed / PSVC_EDIT_* */

/* mcfg_ops_edit_ms(lock_ms): snapshot -> fn -> hg_mcfg_validate(m, tz_check,
 * err) -> commit -> apply (wifi_mgr_apply(); time_svc_apply_mcfg(); under the
 * lock, exactly what http_api_cfg.c:244-245 did). err is always written ("" on
 * success). lock -1 -> BUSY; fn 1/2/3 -> BAD_JSON / INVALID_FIELD /
 * VALIDATION with err = the field path; commit -2 -> STORAGE; commit -3 ->
 * VALIDATION with err = "". apply runs only on PSVC_OK. MCFG_F_AP_DEFAULT is
 * NOT touched (D9). what = the per-op log label, e.g. "PUT CONFIG ZONE0". */
psvc_rc_t psvc_mcfg_edit(psvc_mcfg_fn fn, void *ctx, uint32_t lock_ms, const char *what, char *err, size_t errcap);

/* ctx = const char *json. hg_json_merge_mcfg: -1 -> PSVC_EDIT_BAD_JSON,
 * -2 -> PSVC_EDIT_INVALID_FIELD (err "GROUP.KEY"), 0 -> 0. */
int  psvc_mcfg_json_fn(hg_mcfg_t *m, void *ctx, char *err, size_t errcap);

/* ctx = const psvc_fedits_t *. Per edit: a secret row (hg_mcfg_is_secret) with
 * empty text is skipped -- blank means unchanged, so a password cannot be
 * cleared here and an open STA is set by clearing the SSID; hg_field_write()
 * != 0 -> PSVC_EDIT_INVALID_FIELD with err "<HG_MGROUP_NAMES[group]>.<key>". */
int  psvc_mcfg_fields_fn(hg_mcfg_t *m, void *ctx, char *err, size_t errcap);

void psvc_mcfg_get(hg_mcfg_t *out);   /* [ANY] *out = *mcfg_get() */

#ifdef __cplusplus
}
#endif
```

`components/panel_svc/psvc_mcfg.c`:

```c
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "mcfg_ops.h"
#include "mcfg_store.h"
#include "hg_json.h"
#include "time_core.h"
#include "wifi_mgr.h"
#include "time_svc.h"
#include "psvc_mcfg.h"

static const char *TAG = "psvc_mcfg";

typedef struct {
    psvc_mcfg_fn fn;
    void        *ctx;
    char        *err;
    size_t       errcap;
} edit_ctx_t;

/* Runs inside mcfg_ops_edit_ms() on its private copy. The explicit validate is
 * what gives a refusal its field path: mcfg_commit()'s own check has no path
 * out (the old cfg_put_zone0 comment, http_api_cfg.c:185-191). */
static int edit_tramp(hg_mcfg_t *m, void *c_) {
    edit_ctx_t *c = (edit_ctx_t *)c_;
    int rc = c->fn(m, c->ctx, c->err, c->errcap);
    if (rc != 0) return rc > 0 ? rc : PSVC_EDIT_VALIDATION;   /* an fn must never return negative (mcfg_ops.h) */
    if (hg_mcfg_validate(m, tz_check, c->err, c->errcap) != 0) return PSVC_EDIT_VALIDATION;
    return 0;
}

/* Still under the mcfg_ops lock (mcfg_ops.h: apply's scope is deliberate). The
 * credentials are persisted by now, so a failed re-apply is a warning. */
static void edit_apply(void *c_) {
    (void)c_;
    if (wifi_mgr_apply() != 0) ESP_LOGW(TAG, "wifi_mgr_apply failed; the change is stored and takes effect on reboot");
    time_svc_apply_mcfg();
}

psvc_rc_t psvc_mcfg_edit(psvc_mcfg_fn fn, void *ctx, uint32_t lock_ms, const char *what, char *err, size_t errcap) {
    char scratch[2];
    if (!err || errcap == 0) { err = scratch; errcap = sizeof scratch; }
    err[0] = '\0';
    if (!fn) return PSVC_E_INTERNAL;
    edit_ctx_t c = { fn, ctx, err, errcap };
    int rc = mcfg_ops_edit_ms(edit_tramp, &c, edit_apply, what ? what : "MCFG EDIT", lock_ms);
    switch (rc) {
    case 0:                       return PSVC_OK;
    case -1:                      err[0] = '\0'; return PSVC_E_BUSY;
    case -2:                      err[0] = '\0'; return PSVC_E_STORAGE;
    case -3:                      err[0] = '\0'; return PSVC_E_VALIDATION;
    case PSVC_EDIT_BAD_JSON:      return PSVC_E_BAD_JSON;
    case PSVC_EDIT_INVALID_FIELD: return PSVC_E_INVALID_FIELD;
    case PSVC_EDIT_VALIDATION:    return PSVC_E_VALIDATION;
    default:                      return PSVC_E_INTERNAL;
    }
}

int psvc_mcfg_json_fn(hg_mcfg_t *m, void *ctx, char *err, size_t errcap) {
    int rc = hg_json_merge_mcfg(m, (const char *)ctx, err, errcap);
    if (rc == 0) return 0;
    return rc == -1 ? PSVC_EDIT_BAD_JSON : PSVC_EDIT_INVALID_FIELD;
}

int psvc_mcfg_fields_fn(hg_mcfg_t *m, void *ctx, char *err, size_t errcap) {
    const psvc_fedits_t *set = (const psvc_fedits_t *)ctx;
    if (!set || (set->n > 0 && !set->e)) return PSVC_EDIT_INVALID_FIELD;
    for (int i = 0; i < set->n; i++) {
        const psvc_fedit_t *e = &set->e[i];
        const hg_field_t *f = e->f;
        if (!f || f->group >= HG_MG_COUNT) {
            if (err && errcap) snprintf(err, errcap, "?");
            return PSVC_EDIT_INVALID_FIELD;
        }
        char text[PSVC_FEDIT_TEXT_MAX];
        memcpy(text, e->text, sizeof text);
        text[sizeof text - 1] = '\0';                            /* never trust the caller's terminator */
        if (hg_mcfg_is_secret(f) && text[0] == '\0') continue;   /* blank secret = unchanged */
        if (hg_field_write(f, m, text) != 0) {
            if (err && errcap) snprintf(err, errcap, "%s.%s", HG_MGROUP_NAMES[f->group], f->key);
            return PSVC_EDIT_INVALID_FIELD;
        }
    }
    return 0;
}

void psvc_mcfg_get(hg_mcfg_t *out) { *out = *mcfg_get(); }
```

`components/panel_svc/CMakeLists.txt`:

```cmake
# components/panel_svc: the shared service layer BOTH faces call -- the web's
# HTTP handlers and the P4 panel (spec Decision 4: "Both faces call the layer
# beneath HTTP"). Master-only sources, include-only on zone/rescue: the
# http_srv pattern. REQUIRES stays unconditional (evaluated in IDF's early
# requirements pass, where CMAKE_PROJECT_NAME is not yet set) and names only
# components present in every app build.
set(PSVC_SRCS "")
if("${CMAKE_PROJECT_NAME}" STREQUAL "hillgrow_master")
    list(APPEND PSVC_SRCS "psvc_rc.c" "psvc_mcfg.c")
endif()

idf_component_register(
    SRCS ${PSVC_SRCS}
    INCLUDE_DIRS "."
    REQUIRES hg_cfg hg_mcfg
    PRIV_REQUIRES mcfg_ops time_core hg_json wifi_mgr time_svc
)
```

- [ ] **Step 12: Run both new tests to verify they pass**

Run the one-test command with `-R test_psvc`.
Expected: `100% tests passed, 0 tests failed out of 2`.

- [ ] **Step 13: Run the gates**

Run GATE-HOST. Expected: 37/37 tests pass (+2).
Run GATE-ESP32. Expected: master, zone and rescue each print `Project build complete` with 0 warnings, and the lock diff is empty. `panel_svc` compiles on the ESP32 master and is include-only on zone and rescue.
Run GATE-P4. Expected: `Project build complete`, 0 warnings, the target grep matches, then the lock checkout.

- [ ] **Step 14: Commit**

```powershell
git -C C:\Projects\HillGrov add components/mcfg_ops/mcfg_ops.h components/mcfg_ops/mcfg_ops.c components/panel_svc/CMakeLists.txt components/panel_svc/psvc_rc.h components/panel_svc/psvc_rc.c components/panel_svc/psvc_edit.h components/panel_svc/psvc_mcfg.h components/panel_svc/psvc_mcfg.c tests/host/test_mcfg_ops.c tests/host/test_psvc_rc.c tests/host/test_psvc_mcfg.c tests/host/fakes/fake_apply.h tests/host/fakes/fake_apply.c tests/host/CMakeLists.txt
git -C C:\Projects\HillGrov commit -m "feat(panel_svc): shared master-config edit and refusal vocabulary" -m "Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>"
```

### Task 4: Net ops into `panel_svc`; the web's zone-0 PUT, `/api/wifi`, scan and password on the shared layer (Stage 0)

This moves `net_ops_master.{c,h}` out of `master/main` (an app, which no component can include) into `panel_svc`. Both `extern` reach-throughs go, and the web's zone-0 PUT and Wi-Fi scan run on the shared layer from Task 3. The panel gets setters that split master-config contention (`BUSY`) from a storage failure (`STORAGE`). The CLI rows and `POST /api/wifi` keep reporting contention as `ERR STORAGE` / 503, unchanged (D10).

**Files:**
- Move: `master/main/net_ops_master.c` → `components/panel_svc/psvc_net.c`, and `master/main/net_ops_master.h` → `components/panel_svc/psvc_net.h`. Both are rewritten below.
- Modify:
  - `components/panel_svc/CMakeLists.txt`: add `psvc_net.c`; `REQUIRES` gains `master_cmds wifi_mgr`; `PRIV_REQUIRES` gains `http_auth node_mgr`.
  - `master/main/CMakeLists.txt`: drop `"net_ops_master.c"` from `SRCS`; `REQUIRES` gains `panel_svc`.
  - `master/main/cmd_table_master.c`: `#include "net_ops_master.h"` (`:10`) becomes `#include "psvc_net.h"`, and the comment at `:43` names `psvc_net.c`.
  - `components/http_srv/CMakeLists.txt`: `REQUIRES` gains `panel_svc`.
  - `components/http_srv/http_api.c`:
    - delete the extern block `:21-27`;
    - add `#include "psvc_net.h"`;
    - drop `#include "mcfg_ops.h"` (`:15`);
    - `h_wifi_scan` calls `psvc_wifi_scan(out, 20, &n, PSVC_LOCK_WEB_MS)`, with BUSY → 409 `BUSY` and INTERNAL → 500 `INTERNAL`.
  - `components/http_srv/http_login.c`: delete `:125-130` and add `#include "psvc_net.h"`.
  - `components/http_srv/http_api_cfg.c`: `cfg_put_zone0` becomes `psvc_mcfg_edit(psvc_mcfg_json_fn, (void *)body, PSVC_LOCK_WEB_MS, "PUT CONFIG ZONE0", err, sizeof err)` plus one switch (see below). Drop the now-unused includes `wifi_mgr.h`, `time_svc.h` and `time_core.h` (`:10-12`) and the `mcfg_ops.h` block (`:17-26`).
  - Comments that name `master/main/net_ops_master.c`: `http_srv.h:22`, `master_cmds.h:43`, `mcfg_ops.c:9-16`, `http_auth.c:19`, `tests/host/fakes/esp_log.h:3` and `tests/host/fakes/freertos/FreeRTOS.h:3`. Find them with `git -C C:\Projects\HillGrov grep -n net_ops_master`.

**Interfaces:**
- Consumes:
  - from Task 2: `http_auth_hash_password` and `http_auth_sessions_drop`;
  - from Task 3: `psvc_mcfg_edit`, `psvc_mcfg_json_fn`, `psvc_rc_to_net_legacy`, `mcfg_ops_edit_ms` and the `PSVC_LOCK_*` constants.
- Produces (`psvc_net.h`):
  ```c
  #include "master_cmds.h"   /* net_ops_t */
  #include "wifi_mgr.h"      /* wifi_scan_t */
  #include "psvc_rc.h"
  const net_ops_t *master_net_ops(void);      /* unchanged name and behaviour; members = legacy wrappers over the psvc_* below
                                                  with PSVC_LOCK_LEGACY_MS and psvc_rc_to_net_legacy(); seed_mac/get_mcfg/wifi_status as today */
  int master_web_set_password(const char *pw); /* unchanged: 0 / -1 length or invalid / -2 not stored or busy / -3 no crypto */
  /* [WORKER] panel-facing: master-config contention is PSVC_E_BUSY (not STORAGE) -- D10 */
  psvc_rc_t psvc_wifi_set_sta(const char *ssid, const char *pass, uint32_t lock_ms);  /* pass "" = open */
  psvc_rc_t psvc_wifi_set_ap(const char *ssid, const char *pass, uint32_t lock_ms);   /* clears MCFG_F_AP_DEFAULT (as today) */
  psvc_rc_t psvc_tz_set(const char *tz, uint32_t lock_ms);                              /* bad POSIX TZ -> PSVC_E_INVALID */
  psvc_rc_t psvc_web_password_set(const char *pw, uint32_t lock_ms);
       /* no old password (spec Decision 3); hash rc -1 -> INVALID, -3 -> INTERNAL; lock -> BUSY; commit -2 -> STORAGE;
          commit -3 -> INVALID; on OK every web session has been dropped (http_auth_sessions_drop inside the lock) */
  psvc_rc_t psvc_wifi_scan(wifi_scan_t *out, int cap, int *n, uint32_t lock_ms);
       /* mcfg_ops_lock(lock_ms) (-1 -> BUSY) -> wifi_mgr_scan (<0 -> INTERNAL) -> unlock; P4: up to 30 s, radio parked */
  ```
  `psvc_net.h` also includes `psvc_mcfg.h`, so its callers see the `PSVC_LOCK_*` constants.

  The `cfg_put_zone0` status mapping, identical to today's:

  | `psvc_rc_t` | Response |
  |---|---|
  | `PSVC_OK` | 200 `{"ok":true}` |
  | `BUSY` | 409 `BUSY` |
  | `BAD_JSON` | 400 |
  | `INVALID_FIELD` | 400 with path |
  | `VALIDATION` | 400, with path if `err[0]`, else no path |
  | `STORAGE` | 503 |
  | anything else | 500 `INTERNAL` |

The legacy mapping is proven equal to today's, value by value:

| Case | `net_ops_master.c` today | psvc | Legacy |
|---|---|---|---|
| lock not taken | `-1` → -2 | `BUSY` | -2 |
| commit storage | -2 | `STORAGE` | -2 |
| commit invalid | `-3` → -1 | `INVALID` | -1 |
| hash length | `hash_rc` -1 | `INVALID` | -1 |
| no SHA-256 | `hash_rc` -3 | `INTERNAL` | -3 |

`test_psvc_rc` pins the right-hand column; `test_master_cmds` still pins the rows through `fake_net_ops`. The G file itself is proven by the builds and the Stage 0 web and UART runs.

- [ ] **Step 1: Move the files**

```powershell
git -C C:\Projects\HillGrov mv master/main/net_ops_master.c components/panel_svc/psvc_net.c
git -C C:\Projects\HillGrov mv master/main/net_ops_master.h components/panel_svc/psvc_net.h
```

- [ ] **Step 2: Rewrite the header**

Replace the whole of `components/panel_svc/psvc_net.h` with:

```c
#pragma once
#include <stdint.h>
#include "master_cmds.h"   /* net_ops_t */
#include "wifi_mgr.h"      /* wifi_scan_t */
#include "psvc_rc.h"
#include "psvc_mcfg.h"     /* PSVC_LOCK_* -- the budgets callers pass below */

#ifdef __cplusplus
extern "C" {
#endif

/* The master's network/time/web-password writes, owned by panel_svc so every
 * face reaches the SAME code: the CLI rows (through master_net_ops()), the
 * web's POST /api/wifi, POST /api/password and GET /api/wifi/scan, and the
 * panel's System screens. Moved here from master/main/net_ops_master.{c,h}
 * (panel plan Task 4), which as an app could only be reached from a component
 * through extern declarations.
 *
 * Threading: every function here is [WORKER] -- it takes the mcfg_ops lock for
 * up to lock_ms, then mcfg_commit()'s 5 s mutex, then the apply's esp_hosted
 * RPCs (P4). Never from the LVGL task, never from a TWDT-subscribed task. */

/* The production net_ops_t behind the NET/TIME CLI rows. Unchanged name and
 * behaviour: each member is a legacy wrapper over the psvc_* call below with
 * PSVC_LOCK_LEGACY_MS and psvc_rc_to_net_legacy(), so the rows still answer
 * ERR INVALID / ERR STORAGE / ERR INTERNAL exactly as before -- including
 * lock contention as ERR STORAGE (D10). seed_mac, get_mcfg and wifi_status
 * are as they were. */
const net_ops_t *master_net_ops(void);

/* The set_web_password member on its own, for POST /api/password (which has
 * already verified the old password). 0 ok, -1 password outside 8..63 or
 * rejected, -2 valid but not stored (includes a busy lock), -3 SHA-256
 * unavailable (nothing was written). */
int master_web_set_password(const char *pw);

/* Panel-facing setters: same work, but master-config contention is PSVC_E_BUSY
 * rather than STORAGE (D10), so the panel can say "busy, retry" honestly. */
psvc_rc_t psvc_wifi_set_sta(const char *ssid, const char *pass, uint32_t lock_ms);  /* pass "" = open network */
psvc_rc_t psvc_wifi_set_ap(const char *ssid, const char *pass, uint32_t lock_ms);   /* clears MCFG_F_AP_DEFAULT (as today) */
psvc_rc_t psvc_tz_set(const char *tz, uint32_t lock_ms);                              /* bad POSIX TZ -> PSVC_E_INVALID */

/* No old password: the panel is the deliberate recovery path (spec Decision 3).
 * hash rc -1 -> INVALID, -3 -> INTERNAL; lock -> BUSY; commit -2 -> STORAGE;
 * commit -3 -> INVALID. On PSVC_OK every web session has been dropped
 * (http_auth_sessions_drop() ran inside the lock, after the commit). */
psvc_rc_t psvc_web_password_set(const char *pw, uint32_t lock_ms);

/* mcfg_ops_lock(lock_ms) (-1 -> BUSY) -> wifi_mgr_scan(out, cap) (<0 ->
 * INTERNAL) -> unlock. *n is always written (0 on failure). Holding the lock
 * across the scan is what keeps a STA/AP re-apply from landing mid-scan. On
 * the P4 this can block up to 30 s (CONFIG_ESP_HOSTED_HOST_WIFI_SCAN_BLOCK_
 * TIMEOUT_MS) with the radio parked and the AP silent. */
psvc_rc_t psvc_wifi_scan(wifi_scan_t *out, int cap, int *n, uint32_t lock_ms);

#ifdef __cplusplus
}
#endif
```

- [ ] **Step 3: Rewrite the implementation**

Replace the whole of `components/panel_svc/psvc_net.c` with:

```c
#include <stdint.h>
#include <stdio.h>
#include "esp_log.h"
#include "mcfg_ops.h"
#include "mcfg_store.h"
#include "http_auth.h"
#include "wifi_mgr.h"
#include "time_svc.h"
#include "node_mgr.h"
#include "psvc_net.h"

static const char *TAG = "psvc_net";

/* The one place that owns WIFI.*, WEB.* and TIME.TZ writes for every face, on
 * top of components/mcfg_ops (snapshot -> modify -> mcfg_commit() -> apply,
 * atomic under one lock). mcfg_commit() does the validating (including TZ,
 * through the tz_check time_svc installs) and the NVS write.
 *
 * mcfg_ops_edit_ms() reserves three negative codes (mcfg_ops.h): -1 the lock
 * was not taken within lock_ms, -2 mcfg_commit() failed on storage, -3
 * mcfg_commit() rejected the config. from_ops() turns them into the shared
 * vocabulary; the legacy net_ops_t members then go through
 * psvc_rc_to_net_legacy(), which reproduces this file's pre-panel_svc
 * convention exactly: BUSY and STORAGE -> -2, INVALID -> -1, INTERNAL -> -3.
 *
 * Every apply below runs as mcfg_ops_edit_ms()'s `apply` callback, i.e. still
 * holding the lock: GET /api/wifi/scan holds the same lock across its blocking
 * radio scan specifically so a STA/AP reconfigure cannot land mid-scan. */

static psvc_rc_t from_ops(int rc) {
    switch (rc) {
    case 0:  return PSVC_OK;
    case -1: return PSVC_E_BUSY;      /* the mcfg_ops lock was not acquired within lock_ms */
    case -2: return PSVC_E_STORAGE;   /* mcfg_commit() failed on NVS or its own mutex */
    case -3: return PSVC_E_INVALID;   /* mcfg_commit() rejected the config (bad TZ, SSID or pass) */
    default: return PSVC_E_INTERNAL;
    }
}

/* mcfg_get() hands back a pointer into the live RAM buffer; copy it at once
 * and never touch that pointer again (mcfg_store.h's RAM contract). */
static void net_get_mcfg(hg_mcfg_t *out) { *out = *mcfg_get(); }

/* ---- STA ---- */

static int set_sta_fn(hg_mcfg_t *m, void *ctx) {
    const char **a = (const char **)ctx;   /* [0]=ssid [1]=pass */
    snprintf(m->sta_ssid, sizeof m->sta_ssid, "%s", a[0]);
    snprintf(m->sta_pass, sizeof m->sta_pass, "%s", a[1]);
    return 0;
}

/* The credentials are already persisted here, so a failed re-apply is a
 * warning, not a rejection: the next boot joins anyway. */
static void set_sta_apply(void *ctx) {
    (void)ctx;
    if (wifi_mgr_apply() != 0) ESP_LOGW(TAG, "wifi_mgr_apply failed; STA change takes effect on reboot");
}

psvc_rc_t psvc_wifi_set_sta(const char *ssid, const char *pass, uint32_t lock_ms) {
    if (!ssid || !pass) return PSVC_E_INVALID;
    const char *a[2] = { ssid, pass };
    return from_ops(mcfg_ops_edit_ms(set_sta_fn, a, set_sta_apply, "SET WIFI STA", lock_ms));
}

static int net_set_sta(const char *ssid, const char *pass) {
    return psvc_rc_to_net_legacy(psvc_wifi_set_sta(ssid, pass, PSVC_LOCK_LEGACY_MS));
}

/* ---- AP ---- */

static int set_ap_fn(hg_mcfg_t *m, void *ctx) {
    const char **a = (const char **)ctx;   /* [0]=ssid [1]=pass */
    snprintf(m->ap_ssid, sizeof m->ap_ssid, "%s", a[0]);
    snprintf(m->ap_pass, sizeof m->ap_pass, "%s", a[1]);
    m->flags &= (uint8_t)~MCFG_F_AP_DEFAULT;   /* no longer the shipped HillGrow/hillgrow1 pair */
    return 0;
}

static void set_ap_apply(void *ctx) {
    (void)ctx;
    if (wifi_mgr_apply() != 0) ESP_LOGW(TAG, "wifi_mgr_apply failed; AP change takes effect on reboot");
}

psvc_rc_t psvc_wifi_set_ap(const char *ssid, const char *pass, uint32_t lock_ms) {
    if (!ssid || !pass) return PSVC_E_INVALID;
    const char *a[2] = { ssid, pass };
    return from_ops(mcfg_ops_edit_ms(set_ap_fn, a, set_ap_apply, "SET WIFI AP", lock_ms));
}

static int net_set_ap(const char *ssid, const char *pass) {
    return psvc_rc_to_net_legacy(psvc_wifi_set_ap(ssid, pass, PSVC_LOCK_LEGACY_MS));
}

/* ---- TZ ---- */

static int set_tz_fn(hg_mcfg_t *m, void *ctx) {
    snprintf(m->tz, sizeof m->tz, "%s", (const char *)ctx);
    return 0;
}

static void set_tz_apply(void *ctx) {
    (void)ctx;
    time_svc_apply_mcfg();
}

psvc_rc_t psvc_tz_set(const char *tz, uint32_t lock_ms) {
    if (!tz) return PSVC_E_INVALID;
    /* a bad POSIX TZ fails tz_check inside mcfg_commit -> -3 -> INVALID */
    return from_ops(mcfg_ops_edit_ms(set_tz_fn, (void *)tz, set_tz_apply, "SET TZ", lock_ms));
}

static int net_set_tz(const char *tz) {
    return psvc_rc_to_net_legacy(psvc_tz_set(tz, PSVC_LOCK_LEGACY_MS));
}

/* ---- web password ----
 * The wa_state_t that hashes this password lives in components/http_auth: it
 * is the same state that holds the live web login sessions, so a password
 * changed from the console or the panel invalidates the cookies issued against
 * the old one. Order matters: hash into the PRIVATE copy, commit it, and only
 * then drop the sessions (the apply below) -- dropping first would log every
 * operator out even when the commit went on to fail. */

typedef struct { const char *pw; int hash_rc; } pw_edit_ctx_t;

static int set_password_fn(hg_mcfg_t *m, void *ctx_) {
    pw_edit_ctx_t *ctx = (pw_edit_ctx_t *)ctx_;
    /* http_auth_hash_password: 0 ok, -1 outside 8..63, -3 SHA-256 unavailable.
     * Both failures are negative and mcfg_ops.h reserves every negative for
     * itself, so a failure declines with 1 and the verdict travels in ctx. */
    ctx->hash_rc = http_auth_hash_password(m, ctx->pw);
    return ctx->hash_rc == 0 ? 0 : 1;
}

static void set_password_apply(void *ctx) {
    (void)ctx;
    http_auth_sessions_drop();
}

psvc_rc_t psvc_web_password_set(const char *pw, uint32_t lock_ms) {
    if (!pw) return PSVC_E_INVALID;
    pw_edit_ctx_t ctx = { .pw = pw, .hash_rc = 0 };
    int rc = mcfg_ops_edit_ms(set_password_fn, &ctx, set_password_apply, "SET WEB PASSWORD", lock_ms);
    if (rc == 1) {
        if (ctx.hash_rc == -1) {
            ESP_LOGW(TAG, "SET WEB PASSWORD: length must be 8..63");
            return PSVC_E_INVALID;
        }
        return PSVC_E_INTERNAL;   /* -3: this board cannot hash; nothing was committed */
    }
    return from_ops(rc);
}

int master_web_set_password(const char *pw) {
    return psvc_rc_to_net_legacy(psvc_web_password_set(pw, PSVC_LOCK_LEGACY_MS));
}

/* ---- Wi-Fi scan ---- */

psvc_rc_t psvc_wifi_scan(wifi_scan_t *out, int cap, int *n, uint32_t lock_ms) {
    if (n) *n = 0;
    if (!out || cap <= 0 || !n) return PSVC_E_INTERNAL;
    if (mcfg_ops_lock(lock_ms) != 0) return PSVC_E_BUSY;
    int got = wifi_mgr_scan(out, cap);
    mcfg_ops_unlock();
    if (got < 0) return PSVC_E_INTERNAL;
    *n = got;
    return PSVC_OK;
}

/* ---- node binding ----
 * node_mgr owns the ztab, so the binding is written there (and to NVS) rather
 * than through the mcfg path -- no mcfg lock, since node_mgr_seed_mac
 * serializes on nmgr_lock() internally. -1 is an out-of-range zone, which
 * master_cmds reports as ERR ZONE_UNKNOWN. */
static int net_seed_mac(uint8_t zone, const uint8_t mac[6]) {
    return node_mgr_seed_mac(zone, mac);
}

static const net_ops_t MASTER_NET_OPS = {
    .get_mcfg         = net_get_mcfg,
    .set_sta          = net_set_sta,
    .set_ap           = net_set_ap,
    .set_web_password = master_web_set_password,
    .set_tz           = net_set_tz,
    .wifi_status      = wifi_mgr_status,
    .seed_mac         = net_seed_mac,
};

const net_ops_t *master_net_ops(void) {
    return &MASTER_NET_OPS;
}
```

- [ ] **Step 4: Wire the component**

`components/panel_svc/CMakeLists.txt`: replace the `list(APPEND ...)` line and the `REQUIRES`/`PRIV_REQUIRES` lines so the file's body reads:

```cmake
set(PSVC_SRCS "")
if("${CMAKE_PROJECT_NAME}" STREQUAL "hillgrow_master")
    list(APPEND PSVC_SRCS "psvc_rc.c" "psvc_mcfg.c" "psvc_net.c")
endif()

idf_component_register(
    SRCS ${PSVC_SRCS}
    INCLUDE_DIRS "."
    REQUIRES hg_cfg hg_mcfg master_cmds wifi_mgr
    PRIV_REQUIRES mcfg_ops time_core hg_json time_svc http_auth node_mgr
)
```

`master/main/CMakeLists.txt` becomes:

```cmake
idf_component_register(SRCS "app_main.c" "app_if_master.c" "cmd_table_master.c" INCLUDE_DIRS "."
    REQUIRES board cli cmd_task cmd_common app_common notify app_update esp_timer nvs_flash ota_trial ring_link node_mgr master_cmds wifi_mgr fw_srv http_srv http_auth hg_mcfg time_svc alarm_mgr mcfg_ops cp_ota panel_svc)
```

(The comment block under it at `:4-8` stays as it is.)

In `master/main/cmd_table_master.c`, line 10 becomes `#include "psvc_net.h"`. In the comment at `:43`, `(net_ops_master.c)` becomes `(components/panel_svc/psvc_net.c)`.

In `components/http_srv/CMakeLists.txt`, the `REQUIRES` line becomes:

```cmake
    REQUIRES esp_http_server cmd_core cmd_task web_auth hg_mcfg web_assets fw_srv mcfg_ops http_auth panel_svc
```

- [ ] **Step 5: Drop the two externs and move the scan**

In `components/http_srv/http_api.c`:
- delete lines 15 (`#include "mcfg_ops.h"`) and 21-27 (the `net_ops_master.{h,c} live in master/main ...` comment and `extern const net_ops_t *master_net_ops(void);`);
- after `#include "master_cmds.h"` (`:16`) add `#include "psvc_net.h"   /* master_net_ops(), psvc_wifi_scan() -- components/panel_svc */`;
- replace the body of `h_wifi_scan()` from its comment down to `mcfg_ops_unlock();` plus the `if (n < 0) {...}` block (`:176-194`) with:

```c
    /* wifi_mgr_scan() parks the single radio for the whole scan, so it must not
     * run concurrently with a master-config apply (SET WIFI STA/AP/TZ, SET WEB
     * PASSWORD, a zone-0 PUT or a panel save): psvc_wifi_scan() holds the
     * components/mcfg_ops lock across the scan, which every one of those takes
     * across its own commit AND apply. 100 ms try, exactly as before. */
    wifi_scan_t out[20];
    int n = 0;
    psvc_rc_t src = psvc_wifi_scan(out, 20, &n, PSVC_LOCK_WEB_MS);
    if (src == PSVC_E_BUSY) {
        http_srv_error(req, 409, "BUSY", NULL);
        return http_srv_done(req, 0);
    }
    if (src != PSVC_OK) {
        http_srv_error(req, 500, "INTERNAL", NULL);
        return http_srv_done(req, 0);
    }
```

The cJSON array build that follows (`:196-213`) is unchanged.

In `components/http_srv/http_login.c`, delete lines 125-130 (the comment and `extern int master_web_set_password(const char *pw);`). After `#include "http_srv_internal.h"` (`:6`) add `#include "psvc_net.h"   /* master_web_set_password() -- components/panel_svc */`.

- [ ] **Step 6: Put the zone-0 PUT on the shared edit**

In `components/http_srv/http_api_cfg.c`:
- delete lines 10-12 (`#include "wifi_mgr.h"`, `#include "time_svc.h"`, `#include "time_core.h"`);
- delete lines 17-26 (the `components/mcfg_ops owns ...` comment and `#include "mcfg_ops.h"`);
- after `#include "http_srv_internal.h"` add `#include "psvc_mcfg.h"   /* psvc_mcfg_edit() -- the ONE master-config RMW, components/panel_svc */`;
- replace the comment above `cfg_put_zone0()` and the whole function (`:185-249`) with:

```c
/* zone 0: the master's own mcfg, through panel_svc's ONE master-config
 * read-modify-write -- the same code the panel's Config editor and the CLI's
 * NET/TIME rows run, so a web PUT and a console SET WIFI or a panel save can
 * never interleave their snapshot -> modify -> commit -> apply sequences
 * (panel plan Task 4; this replaces the hand-rolled copy follow-ups item 6
 * recorded). Observable behaviour is unchanged: 100 ms try-lock (409 BUSY),
 * merge (400 BAD_JSON / INVALID_FIELD + path), validate (400 VALIDATION +
 * path), commit (-1 -> 400 VALIDATION without a path, -2 -> 503 STORAGE),
 * then wifi_mgr_apply() + time_svc_apply_mcfg() under the lock, then 200. */
static esp_err_t cfg_put_zone0(httpd_req_t *req, const char *body) {
    char err[64] = "";
    psvc_rc_t rc = psvc_mcfg_edit(psvc_mcfg_json_fn, (void *)body, PSVC_LOCK_WEB_MS,
                                  "PUT CONFIG ZONE0", err, sizeof err);
    switch (rc) {
    case PSVC_OK:              http_srv_json(req, 200, "{\"ok\":true}");                     break;
    case PSVC_E_BUSY:          http_srv_error(req, 409, "BUSY", NULL);                       break;
    case PSVC_E_BAD_JSON:      http_srv_error(req, 400, "BAD_JSON", NULL);                   break;
    case PSVC_E_INVALID_FIELD: http_srv_error(req, 400, "INVALID_FIELD", err);               break;
    case PSVC_E_VALIDATION:    http_srv_error(req, 400, "VALIDATION", err[0] ? err : NULL);  break;
    case PSVC_E_STORAGE:       http_srv_error(req, 503, "STORAGE", NULL);                    break;
    default:                   http_srv_error(req, 500, "INTERNAL", NULL);                   break;
    }
    return http_srv_done(req, 1);
}
```

`#include "mcfg_store.h"` (`:9`) stays: `h_config_get` still calls `mcfg_get()` for the zone-0 export.

- [ ] **Step 7: Fix every comment that names the old file**

Run: `git -C C:\Projects\HillGrov grep -n net_ops_master -- components master tests`
Edit each hit's wording so it names `components/panel_svc/psvc_net.c` (or `psvc_net.h`). Do not change any code. The expected hits at this point:
- `components/http_srv/http_srv.h:22`: `net_ops_master's mcfg read-modify-write` becomes `panel_svc's mcfg read-modify-write`.
- `components/master_cmds/master_cmds.h:43`: `master/main/net_ops_master.c holds the production implementation` becomes `components/panel_svc/psvc_net.c holds the production implementation`.
- `components/mcfg_ops/mcfg_ops.c:9`: `Moved out of master/main/net_ops_master.c verbatim (Task 5)` becomes `Moved out of master/main/net_ops_master.c verbatim (migration Task 5; that file is now components/panel_svc/psvc_net.c)`.
- `components/http_auth/http_auth.c:19`: `net_ops_master.c)` becomes `components/panel_svc/psvc_net.c)`.
- `tests/host/fakes/esp_log.h:3` and `tests/host/fakes/freertos/FreeRTOS.h:3`: same wording change as `mcfg_ops.c`.

Re-run the grep. Expected: only the historical mentions inside the three sentences just rewritten (`was master/main/net_ops_master.c ... now psvc_net.c`) remain.

- [ ] **Step 8: Run the gates**

Run GATE-HOST. Expected: 37/37 tests pass (+0). `test_master_cmds` is unchanged and green through `fake_net_ops`.
Run GATE-ESP32. Expected: master, zone and rescue each print `Project build complete` with 0 warnings, and the lock diff is empty.
Run GATE-P4. Expected: `Project build complete`, 0 warnings, the target grep matches, then the lock checkout.
Run: `python C:\Projects\HillGrov\tools\web_test.py --selftest`. Expected: `SELFTEST OK (9 assertion groups)`.

The live MCFG, WIFI and LOGIN suites and `uart_test.py` run at the Stage 0 gate (steps 1 and 6). They are what proves the web and the CLI rows unchanged on the wire.

- [ ] **Step 9: Commit**

```powershell
git -C C:\Projects\HillGrov add components/panel_svc/psvc_net.c components/panel_svc/psvc_net.h components/panel_svc/CMakeLists.txt master/main/CMakeLists.txt master/main/cmd_table_master.c components/http_srv/CMakeLists.txt components/http_srv/http_api.c components/http_srv/http_login.c components/http_srv/http_api_cfg.c components/http_srv/http_srv.h components/master_cmds/master_cmds.h components/mcfg_ops/mcfg_ops.c components/http_auth/http_auth.c tests/host/fakes/esp_log.h tests/host/fakes/freertos/FreeRTOS.h
git -C C:\Projects\HillGrov commit -m "refactor(panel_svc): net ops and the zone-0 RMW leave master/main; drop both externs" -m "Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>"
```

### Task 5: Shared zone-config edit; the web's zone PUT and GET on it (Stage 0)

The web's zone PUT (`cfg_put_zone`, `http_api_cfg.c:118-183`) becomes a thin mapper over one shared edit. The panel will call the same edit with field edits instead of JSON. The load-bearing part is the hw_present rule (`http_api_cfg.c:132-149`). Validation must use the zone's hardware plane only when one is really cached, and pass NULL otherwise. The two ways to get this wrong are a false reject of every save and a pump-limit guard that is silently off, and both have happened. It moves verbatim into `psvc_zone_cfg_edit()`, and a host test pins it in both directions.

**Files:**
- Create: `components/panel_svc/psvc_zcfg.h`, `.c`; `tests/host/test_psvc_zcfg.c`; `tests/host/fakes/fake_nmgr_cfg_api.h`, `.c`. The fake provides scripted `node_mgr_cfg_busy`, `node_mgr_cfg_get` and `node_mgr_cfg_set`, with a `g_fnc` struct holding the returns, the cfg and hw, `hw_present`, call counters and the last set cfg.
- Modify:
  - `components/panel_svc/CMakeLists.txt`: add `psvc_zcfg.c`. `PRIV_REQUIRES` already has `node_mgr hg_json` (Tasks 3-4).
  - `components/http_srv/http_api_cfg.c`: `cfg_put_zone` becomes `psvc_zone_cfg_edit(zone, psvc_zone_json_fn, (void *)body, err, sizeof err, warn, sizeof warn)` plus a switch; the zone ≥ 1 branch of `h_config_get` uses `psvc_zone_cfg_get`.
  - `tests/host/CMakeLists.txt`: `hg_test(test_psvc_zcfg ...)` plus `cjson_host`.

**Interfaces:**
- Consumes:
  - `node_mgr_cfg_busy`, `node_mgr_cfg_get` and `node_mgr_cfg_set` (`node_mgr.h:85-88`);
  - `hg_json_merge_cfg` (`hg_json.h:43`), `hg_cfg_validate` (`hg_cfg.h:40`), `hg_field_base` and `hg_field_write` (`hg_cfg.h:28,36`), `hg_group_scope` (`hg_cfg.h:20`);
  - from Task 3: `psvc_fedit_t`, `psvc_fedits_t` and the `PSVC_EDIT_*` codes.
- Produces (`psvc_zcfg.h`):
  ```c
  typedef int (*psvc_zcfg_fn)(hg_zone_cfg_t *cfg, const hg_zone_hw_t *hw_or_null, void *ctx,
                              char *err, size_t errcap, char *warn, size_t warncap);   /* 0 proceed / PSVC_EDIT_* */
  psvc_rc_t psvc_zone_cfg_get(uint8_t zone, hg_zone_cfg_t *cfg, hg_zone_hw_t *hw, uint32_t *gen, int *hw_present);
       /* [WORKER] zone outside 1..HG_MAX_ZONES -> ZONE_UNKNOWN; node_mgr_cfg_get -1 -> NO_CACHE; hw zeroed when !*hw_present */
  psvc_rc_t psvc_zone_cfg_edit(uint8_t zone, psvc_zcfg_fn fn, void *ctx,
                               char *err, size_t errcap, char *warn, size_t warncap);
       /* [WORKER] order = http_api_cfg.c:118-160: range -> ZONE_UNKNOWN; node_mgr_cfg_busy -> BUSY;
          node_mgr_cfg_get(fresh copy, &hw_present) -1 -> NO_CACHE; fn(&cfg, hw_present ? &hw : NULL, ...) 1/2/3 ->
          BAD_JSON / INVALID_FIELD(err) / VALIDATION(err); hg_cfg_validate(&cfg, hw_present ? &hw : NULL, err) -> VALIDATION;
          node_mgr_cfg_set: -1 ZONE_UNKNOWN, -2 BUSY, -3 ZONE_NOT_ONLINE, 0 PSVC_OK (queued; the tick pushes) */
  int psvc_zone_cfg_busy(uint8_t zone);   /* [WORKER] node_mgr_cfg_busy */
  int psvc_zone_json_fn(hg_zone_cfg_t *cfg, const hg_zone_hw_t *hw_or_null, void *ctx /* const char *json */,
                        char *err, size_t errcap, char *warn, size_t warncap);
       /* hg_json_merge_cfg(hw_or_null, cfg, json, ...): -1 BAD_JSON, -2 INVALID_FIELD, -3 VALIDATION, 0 -> 0 (warn filled) */
  int psvc_zone_fields_fn(hg_zone_cfg_t *cfg, const hg_zone_hw_t *hw_or_null, void *ctx /* const psvc_fedits_t * */,
                          char *err, size_t errcap, char *warn, size_t warncap);
       /* per edit: hg_group_scope/idx -> hg_field_base(group, idx, NULL, cfg); a hardware-plane group (HG_G_HW, HG_G_HWSHELF,
          HG_G_CAL) -> PSVC_EDIT_INVALID_FIELD err "hw.<GROUP>.<KEY>"; hg_field_write != 0 -> PSVC_EDIT_INVALID_FIELD with the
          web merge's path shape: "cfg.<GROUP>.<KEY>" | "cfg.shelf[<i>].<GROUP>.<KEY>" | "cfg.aux[<i>].AUX.<KEY>" */
  ```
  The `cfg_put_zone` mapping, identical to today's:

  | `psvc_rc_t` | Response |
  |---|---|
  | `OK` | 202 `{"queued":true,"warnings":warn}` via cJSON |
  | `BUSY` | 409 |
  | `NO_CACHE` | 404 |
  | `BAD_JSON` | 400 |
  | `INVALID_FIELD` | 400 with path |
  | `VALIDATION` | 400 with path |
  | `ZONE_UNKNOWN` | 404 |
  | `ZONE_NOT_ONLINE` | 409 |
  | anything else | 500 |
- Interface additions (host fake only): `typedef struct {...} fake_nmgr_cfg_t; extern fake_nmgr_cfg_t g_fnc; void fake_nmgr_cfg_reset(void);` in `fakes/fake_nmgr_cfg_api.h`.

For the web path, `hg_json_merge_cfg()` already validates (`hg_json.h:39-42`), so the edit's own `hg_cfg_validate()` runs a second, identical check on the same data with the same `hw` argument. It can only return what the merge already returned. So the web's observable answers are unchanged, and the panel's field path gets the same check.

- [ ] **Step 1: Write the fake**

`tests/host/fakes/fake_nmgr_cfg_api.h`:

```c
#pragma once
#include <stdint.h>
#include "hg_cfg_types.h"

/* Scripted stand-ins for node_mgr's three §4.4 config primitives
 * (node_mgr.h:85-88), for psvc_zcfg.c's host test. node_mgr_cfg_get() copies
 * g_fnc.cfg out (and g_fnc.hw when hw_present, else zeroes), exactly the
 * real primitive's documented contract; node_mgr_cfg_set() records what it
 * was handed. Reset before every test with fake_nmgr_cfg_reset(). */
typedef struct {
    int           busy_rc;      /* node_mgr_cfg_busy() return (default 0) */
    int           get_rc;       /* node_mgr_cfg_get() return (default 0 = CFG cached) */
    int           set_rc;       /* node_mgr_cfg_set() return (default 0 = queued) */
    int           hw_present;   /* default 1 */
    uint32_t      gen;          /* reported cfg_gen (default 7) */
    hg_zone_cfg_t cfg;          /* the cached CFG plane (default hg_defaults_cfg) */
    hg_zone_hw_t  hw;           /* the cached HW plane (default hg_defaults_hw) */
    int           busy_calls, get_calls, set_calls;
    uint8_t       last_zone;
    hg_zone_cfg_t last_set;     /* the cfg the last node_mgr_cfg_set() was handed */
} fake_nmgr_cfg_t;

extern fake_nmgr_cfg_t g_fnc;
void fake_nmgr_cfg_reset(void);
```

`tests/host/fakes/fake_nmgr_cfg_api.c`:

```c
#include <string.h>
#include "hg_cfg.h"
#include "node_mgr.h"   /* the real prototypes, so a signature drift fails the host build */
#include "fake_nmgr_cfg_api.h"

fake_nmgr_cfg_t g_fnc;

void fake_nmgr_cfg_reset(void) {
    memset(&g_fnc, 0, sizeof g_fnc);
    g_fnc.hw_present = 1;
    g_fnc.gen = 7;
    hg_defaults_cfg(&g_fnc.cfg);
    hg_defaults_hw(&g_fnc.hw);
}

int node_mgr_cfg_busy(uint8_t zone) {
    g_fnc.busy_calls++;
    g_fnc.last_zone = zone;
    return g_fnc.busy_rc;
}

int node_mgr_cfg_get(uint8_t zone, hg_zone_cfg_t *cfg, hg_zone_hw_t *hw, uint32_t *cfg_gen, int *hw_present) {
    g_fnc.get_calls++;
    g_fnc.last_zone = zone;
    if (g_fnc.get_rc != 0) return g_fnc.get_rc;
    *cfg = g_fnc.cfg;
    if (hw) {
        if (g_fnc.hw_present) *hw = g_fnc.hw;
        else memset(hw, 0, sizeof *hw);
    }
    if (cfg_gen) *cfg_gen = g_fnc.gen;
    if (hw_present) *hw_present = g_fnc.hw_present;
    return 0;
}

int node_mgr_cfg_set(uint8_t zone, const hg_zone_cfg_t *cfg) {
    g_fnc.set_calls++;
    g_fnc.last_zone = zone;
    g_fnc.last_set = *cfg;
    return g_fnc.set_rc;
}
```

- [ ] **Step 2: Write the failing test**

`tests/host/test_psvc_zcfg.c`:

```c
/* psvc_zone_cfg_edit() is the ONE zone-config write for both faces. What this
   pins: the web's exact check order (busy before any read, then the cache),
   the hw_present rule in BOTH directions (what_we_learned: a presence signal
   must be proven with a positive assertion, not only its absence), per-field
   last-writer-wins on a fresh copy, the hardware plane refused, and the web's
   error paths and codes. */
#include <stdio.h>
#include <string.h>
#include "unity.h"
#include "hg_cfg.h"
#include "psvc_zcfg.h"
#include "fake_nmgr_cfg_api.h"

void setUp(void) { fake_nmgr_cfg_reset(); }
void tearDown(void) {}

static const hg_field_t *zrow(uint8_t group, const char *key) {
    for (int i = 0; i < HG_FIELD_COUNT; i++)
        if (HG_FIELDS[i].group == group && strcmp(HG_FIELDS[i].key, key) == 0) return &HG_FIELDS[i];
    return NULL;
}

static psvc_fedit_t zedit(uint8_t group, int idx, const char *key, const char *text) {
    psvc_fedit_t e;
    memset(&e, 0, sizeof e);
    e.group = group;
    e.idx = (int8_t)idx;
    e.f = zrow(group, key);
    snprintf(e.text, sizeof e.text, "%s", text);
    return e;
}

static psvc_rc_t edit(uint8_t zone, const psvc_fedit_t *e, int n, char *err, size_t cap) {
    psvc_fedits_t set = { e, n };
    char warn[64];
    return psvc_zone_cfg_edit(zone, psvc_zone_fields_fn, &set, err, cap, warn, sizeof warn);
}

static psvc_rc_t put_json(uint8_t zone, const char *json, char *err, size_t cap) {
    char warn[128];
    return psvc_zone_cfg_edit(zone, psvc_zone_json_fn, (void *)json, err, cap, warn, sizeof warn);
}

static void test_busy_refuses_before_any_read(void) {
    g_fnc.busy_rc = 1;
    psvc_fedit_t e = zedit(HG_G_WATER, 0, "TARGET", "55");
    char err[96] = "";
    TEST_ASSERT_EQUAL_INT(PSVC_E_BUSY, edit(2, &e, 1, err, sizeof err));
    TEST_ASSERT_EQUAL_INT(0, g_fnc.get_calls);
    TEST_ASSERT_EQUAL_INT(0, g_fnc.set_calls);
}

static void test_no_cache_is_no_cache(void) {
    g_fnc.get_rc = -1;
    psvc_fedit_t e = zedit(HG_G_WATER, 0, "TARGET", "55");
    char err[96] = "";
    TEST_ASSERT_EQUAL_INT(PSVC_E_NO_CACHE, edit(2, &e, 1, err, sizeof err));
    TEST_ASSERT_EQUAL_INT(0, g_fnc.set_calls);
}

static void test_zone_out_of_range_is_zone_unknown(void) {
    psvc_fedit_t e = zedit(HG_G_WATER, 0, "TARGET", "55");
    char err[96] = "";
    TEST_ASSERT_EQUAL_INT(PSVC_E_ZONE_UNKNOWN, edit(0, &e, 1, err, sizeof err));
    TEST_ASSERT_EQUAL_INT(PSVC_E_ZONE_UNKNOWN, edit(HG_MAX_ZONES + 1, &e, 1, err, sizeof err));
    TEST_ASSERT_EQUAL_INT(0, g_fnc.busy_calls);
}

/* hw present: dose 200 s against the zone's own pump_max_run_s 60 must be
   refused -- the flood guard is ON. */
static void test_hw_present_enforces_the_pump_limit(void) {
    g_fnc.hw_present = 1;
    psvc_fedit_t e = zedit(HG_G_WATER, 0, "DOSE_S", "200");
    char err[96] = "";
    TEST_ASSERT_EQUAL_INT(PSVC_E_VALIDATION, edit(2, &e, 1, err, sizeof err));
    TEST_ASSERT_EQUAL_STRING("shelf[0].water.dose_s", err);
    TEST_ASSERT_EQUAL_INT(0, g_fnc.set_calls);
}

/* hw absent: the same edit must NOT be refused against an all-zero profile. */
static void test_hw_absent_skips_the_hardware_checks(void) {
    g_fnc.hw_present = 0;
    psvc_fedit_t e = zedit(HG_G_WATER, 0, "DOSE_S", "200");
    char err[96] = "x";
    TEST_ASSERT_EQUAL_INT(PSVC_OK, edit(2, &e, 1, err, sizeof err));
    TEST_ASSERT_EQUAL_STRING("", err);
    TEST_ASSERT_EQUAL_INT(1, g_fnc.set_calls);
    TEST_ASSERT_EQUAL_UINT16(200, g_fnc.last_set.shelf[0].water.dose_s);
}

/* The panel displayed hyst 5; meanwhile another writer set it to 9. A save of
   TARGET alone applies to the FRESH copy, so the other writer's field survives. */
static void test_fields_apply_to_a_fresh_copy(void) {
    g_fnc.cfg.shelf[0].water.hyst_pct = 9;
    psvc_fedit_t e = zedit(HG_G_WATER, 0, "TARGET", "55");
    char err[96] = "";
    TEST_ASSERT_EQUAL_INT(PSVC_OK, edit(2, &e, 1, err, sizeof err));
    TEST_ASSERT_EQUAL_UINT8(55, g_fnc.last_set.shelf[0].water.target_pct);
    TEST_ASSERT_EQUAL_UINT8(9, g_fnc.last_set.shelf[0].water.hyst_pct);
    TEST_ASSERT_EQUAL_UINT8(2, g_fnc.last_zone);
}

static void test_hardware_plane_is_refused(void) {
    psvc_fedit_t e = zedit(HG_G_HW, -1, "SHELVES", "2");
    char err[96] = "";
    TEST_ASSERT_EQUAL_INT(PSVC_E_INVALID_FIELD, edit(2, &e, 1, err, sizeof err));
    TEST_ASSERT_EQUAL_STRING("hw.HW.SHELVES", err);
    psvc_fedit_t c = zedit(HG_G_CAL, 1, "DRY_A", "2900");
    TEST_ASSERT_EQUAL_INT(PSVC_E_INVALID_FIELD, edit(2, &c, 1, err, sizeof err));
    TEST_ASSERT_EQUAL_STRING("hw.CAL.DRY_A", err);
    TEST_ASSERT_EQUAL_INT(0, g_fnc.set_calls);
}

static void test_field_write_failure_uses_the_merge_path_shape(void) {
    psvc_fedit_t a = zedit(HG_G_WATER, 1, "TARGET", "101");
    char err[96] = "";
    TEST_ASSERT_EQUAL_INT(PSVC_E_INVALID_FIELD, edit(2, &a, 1, err, sizeof err));
    TEST_ASSERT_EQUAL_STRING("cfg.shelf[1].WATER.TARGET", err);
    psvc_fedit_t b = zedit(HG_G_ZONECFG, -1, "LINKLOSS_S", "5");
    TEST_ASSERT_EQUAL_INT(PSVC_E_INVALID_FIELD, edit(2, &b, 1, err, sizeof err));
    TEST_ASSERT_EQUAL_STRING("cfg.ZONECFG.LINKLOSS_S", err);
    psvc_fedit_t c = zedit(HG_G_AUX, 0, "MODE", "BOGUS");
    TEST_ASSERT_EQUAL_INT(PSVC_E_INVALID_FIELD, edit(2, &c, 1, err, sizeof err));
    TEST_ASSERT_EQUAL_STRING("cfg.aux[0].AUX.MODE", err);
}

static void test_set_codes_map_like_the_web(void) {
    psvc_fedit_t e = zedit(HG_G_WATER, 0, "TARGET", "55");
    char err[96];
    g_fnc.set_rc = -1; TEST_ASSERT_EQUAL_INT(PSVC_E_ZONE_UNKNOWN,    edit(2, &e, 1, err, sizeof err));
    g_fnc.set_rc = -2; TEST_ASSERT_EQUAL_INT(PSVC_E_BUSY,            edit(2, &e, 1, err, sizeof err));
    g_fnc.set_rc = -3; TEST_ASSERT_EQUAL_INT(PSVC_E_ZONE_NOT_ONLINE, edit(2, &e, 1, err, sizeof err));
}

static void test_json_paths_match_the_web(void) {
    char err[96] = "";
    TEST_ASSERT_EQUAL_INT(PSVC_E_INVALID_FIELD,
        put_json(2, "{\"cfg\":{\"shelf\":[null,{\"WATER\":{\"TARGET\":101}}]}}", err, sizeof err));
    TEST_ASSERT_EQUAL_STRING("cfg.shelf[1].WATER.TARGET", err);
    TEST_ASSERT_EQUAL_INT(PSVC_E_VALIDATION,
        put_json(2, "{\"cfg\":{\"shelf\":[null,{\"LIGHT\":{\"OFF\":\"06:00\"}}]}}", err, sizeof err));
    TEST_ASSERT_EQUAL_STRING("shelf[1].light.off", err);
    TEST_ASSERT_EQUAL_INT(PSVC_E_BAD_JSON, put_json(2, "{", err, sizeof err));
    TEST_ASSERT_EQUAL_INT(0, g_fnc.set_calls);
}

static void test_json_hw_key_is_a_warning_not_a_write(void) {
    char err[96] = "", warn[128] = "";
    TEST_ASSERT_EQUAL_INT(PSVC_OK, psvc_zone_cfg_edit(2, psvc_zone_json_fn,
        (void *)"{\"hw\":{\"HW\":{\"SHELVES\":1}}}", err, sizeof err, warn, sizeof warn));
    TEST_ASSERT_NOT_NULL(strstr(warn, "hw.HW.SHELVES readonly"));
    TEST_ASSERT_EQUAL_INT(1, g_fnc.set_calls);
}

static void test_get_ranges_and_hw_presence(void) {
    hg_zone_cfg_t cfg; hg_zone_hw_t hw; uint32_t gen = 0; int hp = -1;
    TEST_ASSERT_EQUAL_INT(PSVC_E_ZONE_UNKNOWN, psvc_zone_cfg_get(9, &cfg, &hw, &gen, &hp));
    g_fnc.hw_present = 0;
    memset(&hw, 0xAA, sizeof hw);
    TEST_ASSERT_EQUAL_INT(PSVC_OK, psvc_zone_cfg_get(3, &cfg, &hw, &gen, &hp));
    TEST_ASSERT_EQUAL_INT(0, hp);
    TEST_ASSERT_EQUAL_UINT32(7, gen);
    TEST_ASSERT_EQUAL_UINT8(0, hw.shelf_count);   /* zeroed, never stale bytes */
    g_fnc.get_rc = -1;
    TEST_ASSERT_EQUAL_INT(PSVC_E_NO_CACHE, psvc_zone_cfg_get(3, &cfg, &hw, &gen, &hp));
    g_fnc.busy_rc = 1;
    TEST_ASSERT_EQUAL_INT(1, psvc_zone_cfg_busy(3));
}

int main(void) { UNITY_BEGIN();
    RUN_TEST(test_busy_refuses_before_any_read);
    RUN_TEST(test_no_cache_is_no_cache);
    RUN_TEST(test_zone_out_of_range_is_zone_unknown);
    RUN_TEST(test_hw_present_enforces_the_pump_limit);
    RUN_TEST(test_hw_absent_skips_the_hardware_checks);
    RUN_TEST(test_fields_apply_to_a_fresh_copy);
    RUN_TEST(test_hardware_plane_is_refused);
    RUN_TEST(test_field_write_failure_uses_the_merge_path_shape);
    RUN_TEST(test_set_codes_map_like_the_web);
    RUN_TEST(test_json_paths_match_the_web);
    RUN_TEST(test_json_hw_key_is_a_warning_not_a_write);
    RUN_TEST(test_get_ranges_and_hw_presence);
    return UNITY_END(); }
```

Append to `tests/host/CMakeLists.txt`:

```cmake
# psvc_zcfg.c over the REAL hg_cfg validation and hg_json merge; only node_mgr's
# three §4.4 primitives are scripted (fakes/fake_nmgr_cfg_api.c).
hg_test(test_psvc_zcfg ${COMP}/panel_svc/psvc_zcfg.c ${COMP}/panel_svc/psvc_rc.c ${HG_CFG_SRC}
        ${COMP}/hg_json/hg_json_cfg.c ${COMP}/hg_mcfg/hg_mcfg.c ${COMP}/hg_blob/hg_blob.c fakes/fake_nmgr_cfg_api.c)
target_link_libraries(test_psvc_zcfg cjson_host)
```

- [ ] **Step 3: Run it to verify it fails**

Run the one-test command with `-R test_psvc_zcfg`.
Expected: configure fails with `Cannot find source file: .../components/panel_svc/psvc_zcfg.c`.

- [ ] **Step 4: Implement the shared zone edit**

`components/panel_svc/psvc_zcfg.h`:

```c
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "hg_cfg.h"
#include "psvc_rc.h"
#include "psvc_edit.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The ONE zone-config read and write for both faces, over node_mgr's §4.4
 * primitives. The web's PUT /api/config?zone=N calls psvc_zone_cfg_edit() with
 * psvc_zone_json_fn; the panel's Config editor with psvc_zone_fields_fn.
 * Threading: [WORKER] -- node_mgr's lock waits forever and is held across NVS
 * writes (node_mgr.c:40-41), so never from the LVGL task. */

typedef int (*psvc_zcfg_fn)(hg_zone_cfg_t *cfg, const hg_zone_hw_t *hw_or_null, void *ctx,
                            char *err, size_t errcap, char *warn, size_t warncap);   /* 0 proceed / PSVC_EDIT_* */

/* zone outside 1..HG_MAX_ZONES -> ZONE_UNKNOWN; node_mgr_cfg_get -1 -> NO_CACHE.
 * hw may be NULL; when *hw_present is 0 the struct is zeroed, never stale.
 * gen and hw_present may be NULL. */
psvc_rc_t psvc_zone_cfg_get(uint8_t zone, hg_zone_cfg_t *cfg, hg_zone_hw_t *hw, uint32_t *gen, int *hw_present);

/* The web's exact order (http_api_cfg.c:118-160): range -> ZONE_UNKNOWN;
 * node_mgr_cfg_busy -> BUSY; node_mgr_cfg_get(FRESH copy, &hw_present) -1 ->
 * NO_CACHE; fn(&cfg, hw_present ? &hw : NULL, ...) 1/2/3 -> BAD_JSON /
 * INVALID_FIELD(err) / VALIDATION(err); hg_cfg_validate(&cfg,
 * hw_present ? &hw : NULL, err) -> VALIDATION; node_mgr_cfg_set: -1
 * ZONE_UNKNOWN, -2 BUSY, -3 ZONE_NOT_ONLINE, 0 PSVC_OK (queued -- the node_mgr
 * tick pushes; watch psvc_zone_cfg_busy() for "landed"). err and warn are
 * always written ("" when nothing to say). */
psvc_rc_t psvc_zone_cfg_edit(uint8_t zone, psvc_zcfg_fn fn, void *ctx,
                             char *err, size_t errcap, char *warn, size_t warncap);

int psvc_zone_cfg_busy(uint8_t zone);   /* node_mgr_cfg_busy: 1 while a write is queued or on the wire */

/* ctx = const char *json. hg_json_merge_cfg(hw_or_null, cfg, json, ...): -1
 * BAD_JSON, -2 INVALID_FIELD, -3 VALIDATION, 0 -> 0 with warn filled ("hw.*
 * readonly" and unknown keys). */
int psvc_zone_json_fn(hg_zone_cfg_t *cfg, const hg_zone_hw_t *hw_or_null, void *ctx,
                      char *err, size_t errcap, char *warn, size_t warncap);

/* ctx = const psvc_fedits_t *. The hardware plane (HG_G_HW, HG_G_HWSHELF,
 * HG_G_CAL) is refused with err "hw.<GROUP>.<KEY>" -- the panel never writes
 * it (system spec §4.4). A write failure reports the web merge's path shape:
 * "cfg.<GROUP>.<KEY>" | "cfg.shelf[<i>].<GROUP>.<KEY>" | "cfg.aux[<i>].AUX.<KEY>". */
int psvc_zone_fields_fn(hg_zone_cfg_t *cfg, const hg_zone_hw_t *hw_or_null, void *ctx,
                        char *err, size_t errcap, char *warn, size_t warncap);

#ifdef __cplusplus
}
#endif
```

`components/panel_svc/psvc_zcfg.c`:

```c
#include <stdio.h>
#include <string.h>
#include "hg_json.h"
#include "node_mgr.h"
#include "psvc_zcfg.h"

psvc_rc_t psvc_zone_cfg_get(uint8_t zone, hg_zone_cfg_t *cfg, hg_zone_hw_t *hw, uint32_t *gen, int *hw_present) {
    if (zone < 1 || zone > HG_MAX_ZONES || !cfg) return PSVC_E_ZONE_UNKNOWN;
    uint32_t g = 0;
    int hp = 0;
    if (node_mgr_cfg_get(zone, cfg, hw, &g, &hp) != 0) return PSVC_E_NO_CACHE;
    if (hw && !hp) memset(hw, 0, sizeof *hw);
    if (gen) *gen = g;
    if (hw_present) *hw_present = hp;
    return PSVC_OK;
}

int psvc_zone_cfg_busy(uint8_t zone) { return node_mgr_cfg_busy(zone); }

/* node_mgr_cfg_get treats the HW plane as best-effort -- when it is absent or
 * its envelope will not unwrap it zeroes the struct and returns 0, signalling
 * the absence through hw_present. Handing hg_cfg_validate that all-zero
 * profile rejected EVERY save in the first seconds after a zone reboot (the
 * default dose_s=20 compared against pump_max_run_s=0). The first fix keyed on
 * hw_gen, which is structurally always 0 (the HW plane carries no generation
 * on the wire), so it passed NULL on every save and silently disabled the pump
 * limits -- a permanent false accept on a flood guard. Presence comes from the
 * cache's own validity, never a gen. (Moved verbatim from http_api_cfg.c's
 * cfg_put_zone, panel plan Task 5.) */
psvc_rc_t psvc_zone_cfg_edit(uint8_t zone, psvc_zcfg_fn fn, void *ctx,
                             char *err, size_t errcap, char *warn, size_t warncap) {
    char err_scratch[2], warn_scratch[2];
    if (!err || errcap == 0)   { err = err_scratch;   errcap = sizeof err_scratch; }
    if (!warn || warncap == 0) { warn = warn_scratch; warncap = sizeof warn_scratch; }
    err[0] = '\0';
    warn[0] = '\0';
    if (zone < 1 || zone > HG_MAX_ZONES) return PSVC_E_ZONE_UNKNOWN;
    if (!fn) return PSVC_E_INTERNAL;
    if (node_mgr_cfg_busy(zone)) return PSVC_E_BUSY;

    hg_zone_cfg_t cfg;
    hg_zone_hw_t  hw;
    int           hw_present = 0;
    if (node_mgr_cfg_get(zone, &cfg, &hw, NULL, &hw_present) != 0) return PSVC_E_NO_CACHE;
    const hg_zone_hw_t *hwp = hw_present ? &hw : NULL;

    int frc = fn(&cfg, hwp, ctx, err, errcap, warn, warncap);
    if (frc == PSVC_EDIT_BAD_JSON)      return PSVC_E_BAD_JSON;
    if (frc == PSVC_EDIT_INVALID_FIELD) return PSVC_E_INVALID_FIELD;
    if (frc == PSVC_EDIT_VALIDATION)    return PSVC_E_VALIDATION;
    if (frc != 0)                       return PSVC_E_INTERNAL;

    if (hg_cfg_validate(&cfg, hwp, err, errcap) != 0) return PSVC_E_VALIDATION;

    switch (node_mgr_cfg_set(zone, &cfg)) {
    case 0:  return PSVC_OK;
    case -1: return PSVC_E_ZONE_UNKNOWN;
    case -2: return PSVC_E_BUSY;
    case -3: return PSVC_E_ZONE_NOT_ONLINE;
    default: return PSVC_E_INTERNAL;
    }
}

int psvc_zone_json_fn(hg_zone_cfg_t *cfg, const hg_zone_hw_t *hw_or_null, void *ctx,
                      char *err, size_t errcap, char *warn, size_t warncap) {
    int rc = hg_json_merge_cfg(hw_or_null, cfg, (const char *)ctx, err, errcap, warn, warncap);
    switch (rc) {
    case 0:  return 0;
    case -1: return PSVC_EDIT_BAD_JSON;
    case -2: return PSVC_EDIT_INVALID_FIELD;
    default: return PSVC_EDIT_VALIDATION;
    }
}

static int is_hw_group(uint8_t g) { return g == HG_G_HW || g == HG_G_HWSHELF || g == HG_G_CAL; }

static void cfg_path(char *err, size_t errcap, const hg_field_t *f, int idx) {
    if (!err || !errcap) return;
    int scope = hg_group_scope(f->group);
    if (scope == 1)      snprintf(err, errcap, "cfg.shelf[%d].%s.%s", idx, HG_GROUP_NAMES[f->group], f->key);
    else if (scope == 2) snprintf(err, errcap, "cfg.aux[%d].%s.%s", idx, HG_GROUP_NAMES[f->group], f->key);
    else                 snprintf(err, errcap, "cfg.%s.%s", HG_GROUP_NAMES[f->group], f->key);
}

int psvc_zone_fields_fn(hg_zone_cfg_t *cfg, const hg_zone_hw_t *hw_or_null, void *ctx,
                        char *err, size_t errcap, char *warn, size_t warncap) {
    (void)hw_or_null;   /* the validator after this fn is what uses hw -- a field write never does */
    (void)warn;
    (void)warncap;
    const psvc_fedits_t *set = (const psvc_fedits_t *)ctx;
    if (!set || (set->n > 0 && !set->e)) return PSVC_EDIT_INVALID_FIELD;
    for (int i = 0; i < set->n; i++) {
        const psvc_fedit_t *e = &set->e[i];
        const hg_field_t *f = e->f;
        if (!f || f->group >= HG_G_COUNT) {
            if (err && errcap) snprintf(err, errcap, "cfg.?");
            return PSVC_EDIT_INVALID_FIELD;
        }
        if (is_hw_group(f->group)) {
            if (err && errcap) snprintf(err, errcap, "hw.%s.%s", HG_GROUP_NAMES[f->group], f->key);
            return PSVC_EDIT_INVALID_FIELD;
        }
        int idx = hg_group_scope(f->group) == 0 ? -1 : e->idx;
        void *base = hg_field_base(f->group, idx, NULL, cfg);
        char text[PSVC_FEDIT_TEXT_MAX];
        memcpy(text, e->text, sizeof text);
        text[sizeof text - 1] = '\0';
        if (!base || hg_field_write(f, base, text) != 0) {
            cfg_path(err, errcap, f, idx);
            return PSVC_EDIT_INVALID_FIELD;
        }
    }
    return 0;
}
```

`components/panel_svc/CMakeLists.txt`: the `list(APPEND ...)` line becomes:

```cmake
    list(APPEND PSVC_SRCS "psvc_rc.c" "psvc_mcfg.c" "psvc_net.c" "psvc_zcfg.c")
```

- [ ] **Step 5: Run it to verify it passes**

Run the one-test command with `-R test_psvc_zcfg`.
Expected: `100% tests passed, 0 tests failed out of 1` (12 cases).

- [ ] **Step 6: Put the web's zone PUT and GET on it**

In `components/http_srv/http_api_cfg.c`:
- after `#include "psvc_mcfg.h"` add `#include "psvc_zcfg.h"   /* the ONE zone-config edit -- components/panel_svc */`;
- in `h_config_get()`, replace the `if (node_mgr_cfg_get((uint8_t)zone, &cfg, &hw, &cfg_gen, &hw_present) != 0) {` line (`:82`) with `if (psvc_zone_cfg_get((uint8_t)zone, &cfg, &hw, &cfg_gen, &hw_present) != PSVC_OK) {`. The 404 `NO_CACHE` body under it is unchanged, and so are the `hw_present` log and the export.
- replace `cfg_put_zone()` (`:114-183`, its comment included) with:

```c
/* zone 1..8, through panel_svc's ONE zone-config edit (the panel's Config
 * editor runs the same function with field edits instead of JSON). Merge
 * applies the "cfg" plane only -- "hw" is read-only from the web (system spec
 * §4.4) and comes back as warnings, never merged. The hw_present rule and the
 * check order now live in psvc_zcfg.c. Status mapping unchanged. */
static esp_err_t cfg_put_zone(httpd_req_t *req, uint8_t zone, const char *body) {
    char err[96]  = "";
    char warn[256] = "";
    psvc_rc_t rc = psvc_zone_cfg_edit(zone, psvc_zone_json_fn, (void *)body, err, sizeof err, warn, sizeof warn);
    switch (rc) {
    case PSVC_OK:                break;
    case PSVC_E_BUSY:            http_srv_error(req, 409, "BUSY", NULL);            return http_srv_done(req, 1);
    case PSVC_E_NO_CACHE:        http_srv_error(req, 404, "NO_CACHE", NULL);        return http_srv_done(req, 1);
    case PSVC_E_BAD_JSON:        http_srv_error(req, 400, "BAD_JSON", NULL);        return http_srv_done(req, 1);
    case PSVC_E_INVALID_FIELD:   http_srv_error(req, 400, "INVALID_FIELD", err);    return http_srv_done(req, 1);
    case PSVC_E_VALIDATION:      http_srv_error(req, 400, "VALIDATION", err);       return http_srv_done(req, 1);
    case PSVC_E_ZONE_UNKNOWN:    http_srv_error(req, 404, "ZONE_UNKNOWN", NULL);    return http_srv_done(req, 1);
    case PSVC_E_ZONE_NOT_ONLINE: http_srv_error(req, 409, "ZONE_NOT_ONLINE", NULL); return http_srv_done(req, 1);
    default:                     http_srv_error(req, 500, "INTERNAL", NULL);        return http_srv_done(req, 1);
    }

    /* Review fix round 1 (CRITICAL #2): warn embeds client-typed JSON keys
     * verbatim (hg_json_internal.h's unknown-key branch), so the response is
     * built through cJSON, which escapes it, never through a format string. */
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "queued", 1);
    cJSON_AddStringToObject(resp, "warnings", warn);
    char *body_out = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    if (!body_out) {
        http_srv_error(req, 500, "INTERNAL", NULL);
        return http_srv_done(req, 1);
    }
    http_srv_json(req, 202, body_out);
    cJSON_free(body_out);
    return http_srv_done(req, 1);
}
```

`#include "node_mgr.h"` (`:8`) stays: `HG_MAX_ZONES` in `h_config_put()` comes through it.

- [ ] **Step 7: Run the gates**

Run GATE-HOST. Expected: 38/38 tests pass (+1).
Run GATE-ESP32. Expected: master, zone and rescue each print `Project build complete` with 0 warnings, and the lock diff is empty.
Run GATE-P4. Expected: `Project build complete`, 0 warnings, the target grep matches, then the lock checkout.

The live CONFIG suite (`web_test.py --only config`, part of the full run at Stage 0 gate step 6) proves the zone PUT end to end: TARGET 55/61, 101 → 400 with its path, and the hw key → 202 with a warning and nothing merged.

- [ ] **Step 8: Commit**

```powershell
git -C C:\Projects\HillGrov add components/panel_svc/psvc_zcfg.h components/panel_svc/psvc_zcfg.c components/panel_svc/CMakeLists.txt components/http_srv/http_api_cfg.c tests/host/test_psvc_zcfg.c tests/host/fakes/fake_nmgr_cfg_api.h tests/host/fakes/fake_nmgr_cfg_api.c tests/host/CMakeLists.txt
git -C C:\Projects\HillGrov commit -m "refactor(panel_svc): one zone-config edit for both faces" -m "Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>"
```

### Task 6: P4 panel dependencies, sdkconfig keys and the `panel_ui` gating idiom (Stage 0)

This adds the bench-proven panel stack to the P4 master only, without breaking the ESP32 master, the zone, the rescue app or the future `rescue_p4`.

**Why the lock needs care (read this before Step 1).** The component manager rewrites the **project's** `master/dependencies.lock` for whichever target was configured last, whatever `-B` says (commit `ee83c0b`). The committed lock is the ESP32 resolution: `target: esp32`, direct deps cjson and mdns only.
- Every P4 build therefore dirties it, and GATE-P4 ends with `git checkout -- master/dependencies.lock`.
- For a P4-only dependency this means no committed file ever records its version. The manifest's P4-gated pin is the only guard, which is why the BSP, lvgl and the adapter are pinned **exactly**, and why Step 7 compares the resolved P4 set against the bench-proven table.
- Any manifest edit, even a P4-gated one, changes the ESP32 lock's `manifest_hash`. The lock must be regenerated by an ESP32 reconfigure and committed **with** the manifest. Then a second reconfigure must leave `git diff` empty (the `eeb3c33` lesson: verify with a reconfigure, not by eye).
- An ESP32 build purges the P4-only packages from the shared `master/managed_components/`. The next P4 configure re-extracts about 18 of them.

(D7, a per-target lock file, would retire the checkout reflex. It needs the owner's OK, so this plan keeps Option A.)

**Files:**
- Modify: `master/main/idf_component.yml`. Append, in the file's existing esp_hosted style:
  ```yaml
  # P4 only: the 7" panel. Pinned EXACTLY -- no committed lock ever records the P4 resolution (the committed
  # master/dependencies.lock is the ESP32 one), so this manifest is the only guard. 3.0.1 / 9.5.0 / 0.6.4 is the
  # bench-proven set (touch mapping, lock semantics); a bump is a deliberate, bench-verified act.
  waveshare/esp32_p4_wifi6_touch_lcd_7b:
    version: "==3.0.1"
    rules:
      - if: "target == esp32p4"
  lvgl/lvgl:                      # panel_ui calls lv_* directly -- declared, not ridden on the BSP's ~9.5.0
    version: "==9.5.0"
    rules:
      - if: "target == esp32p4"
  espressif/esp_lvgl_adapter:     # the BSP only says ~0.6
    version: "==0.6.4"
    rules:
      - if: "target == esp32p4"
  ```
- Modify: `master/dependencies.lock`, regenerated by an ESP32 reconfigure. Only `manifest_hash` changes. Commit it together with the manifest.
- Modify: `master/sdkconfig.defaults.esp32p4`. Append:
  ```
  CONFIG_BSP_ERROR_CHECK=n
  CONFIG_LV_FONT_MONTSERRAT_20=y
  CONFIG_LV_FONT_MONTSERRAT_28=y
  CONFIG_LV_FONT_MONTSERRAT_48=y
  CONFIG_LV_FONT_DEFAULT_MONTSERRAT_20=y
  ```
  Give each a comment saying why.
- Create: `components/panel_ui/CMakeLists.txt`, `components/panel_ui/panel_ui.h`, `components/panel_ui/panel_ui.c`. This version logs `panel: LVGL %d.%d.%d linked; display not started` and returns -1. `panel_services_start()` returns 0.
- Modify: `master/main/CMakeLists.txt`: `REQUIRES` gains `panel_ui`.
- Modify: `master/main/app_main.c`, adding all three under `#if CONFIG_IDF_TARGET_ESP32P4`:
  - `#include "panel_ui.h"`;
  - `(void)panel_start();` immediately after `ota_trial_start(1);`;
  - `(void)panel_services_start();` immediately after `node_mgr_start();`.

**Interfaces:**
- Consumes: nothing.
- Produces:
  ```c
  /* panel_ui.h -- LVGL-free; the only panel symbols app_main sees */
  int panel_start(void);           /* display + touch + LVGL task + first screen; soft on every failure: 0 lit / -1 dark,
                                      boot continues either way */
  int panel_services_start(void);  /* worker + poller; 0 / -1; the panel shows "starting" until the first snapshot */
  ```
  The `panel_ui` CMake idiom (all later tasks append to `PANEL_SRCS`):
  ```cmake
  set(PANEL_SRCS "")
  idf_build_get_property(panel_target IDF_TARGET)
  if("${CMAKE_PROJECT_NAME}" STREQUAL "hillgrow_master" AND "${panel_target}" STREQUAL "esp32p4")
      set(PANEL_SRCS "panel_ui.c")
  endif()
  idf_component_register(SRCS ${PANEL_SRCS} INCLUDE_DIRS "."
      REQUIRES panel_svc hg_cfg hg_mcfg ring_proto cmd_core
      PRIV_REQUIRES cmd_task node_mgr alarm_mgr wifi_mgr time_svc app_common state_snap notify board hg_blob
                    nvs_flash esp_timer esp_driver_ledc fatfs sdmmc esp_driver_sdmmc)
  # every name above exists in every app build (the early requirements pass evaluates this file for zone/rescue too);
  # panel_ui never requires http_srv -- the web's quarantine count reaches it through psvc_state (Task 9)
  if(PANEL_SRCS)
      idf_component_optional_requires(PRIVATE waveshare__esp32_p4_wifi6_touch_lcd_7b lvgl__lvgl
                                              espressif__esp_lvgl_adapter espressif__esp_lcd_touch)
      target_compile_definitions(${COMPONENT_LIB} PRIVATE LV_LVGL_H_INCLUDE_SIMPLE)
  endif()
  ```
  The bench-proven P4 set, from the spike's resolved lock. It is the reference Step 7 compares against on every P4 configure:

  | Package | Version |
  |---|---|
  | BSP | 3.0.1 |
  | lvgl | 9.5.0 |
  | esp_lvgl_adapter | 0.6.4 |
  | esp_lcd_ek79007 | 2.0.2~1 |
  | esp_lcd_touch | 1.2.1 |
  | esp_lcd_touch_gt911 | 1.2.1 |
  | esp_codec_dev | 1.5.11 |
  | esp_lv_decoder | 0.4.3 |
  | esp_lv_fs | 1.0.1 |
  | esp_mmap_assets | 2.0.1 |
  | esp_new_jpeg | 1.0.2 |
  | freetype | 2.14.3~1 |
  | libpng | 1.6.58~1 |
  | zlib | 1.3.2~1 |
  | button | 4.2.1 |
  | knob | 1.1.0 |
  | usb | 1.5.0 |
  | cmake_utilities | 0.5.3 |

**What the gates prove.**
- GATE-P4 proves the pinned stack resolves and the gated component links LVGL.
- GATE-ESP32 proves the three other apps are untouched, and that the ESP32 image does not grow.
- The boot log line proves the P4 image really carries LVGL.

No host test is possible here (+0).

- [ ] **Step 1: Record the ESP32 master image size**

The last GATE-ESP32 (Task 5) left `master/build` current.
Run: `(Get-Item C:\Projects\HillGrov\master\build\hillgrow_master.bin).Length`
Record the number in the task report as `esp32_bin_before`.

- [ ] **Step 2: Pin the panel stack in the manifest**

Append the YAML block from **Files** above to the end of `master/main/idf_component.yml`, at the same two-space indent as `espressif/esp_wifi_remote:` (it goes under `dependencies:`).

- [ ] **Step 3: Regenerate the ESP32 lock and prove it consistent**

```powershell
& C:\esp\v6.0.1\esp-idf\export.ps1
idf.py -C C:\Projects\HillGrov\master reconfigure
git -C C:\Projects\HillGrov diff -- master/dependencies.lock
```
Expected: the diff changes exactly one line, `manifest_hash:`. If `target:` or any package changed, stop: the rule gating is wrong.

```powershell
git -C C:\Projects\HillGrov add master/main/idf_component.yml master/dependencies.lock
idf.py -C C:\Projects\HillGrov\master reconfigure
git -C C:\Projects\HillGrov diff --stat -- master/dependencies.lock
```
Expected: the last command prints nothing. The staged lock is what a fresh reconfigure produces.

- [ ] **Step 4: Add the P4 sdkconfig keys**

Append to `master/sdkconfig.defaults.esp32p4`:

```
# The Waveshare BSP's error macros default to ESP_ERROR_CHECK/assert: with this
# y, a loose ribbon or a missing GT911 would ABORT the greenhouse controller
# (system spec 3.3 forbids aborting). n turns them into returned errors, which
# panel_hw.c handles softly; panel_hw.c #errors if this ever comes back y,
# because sdkconfig.defaults silently ignores a misspelt name.
CONFIG_BSP_ERROR_CHECK=n
# LVGL's Montserrat ships only at 14; each size is a separate compiled bitmap.
# 20 is the body text, 28 the context line and headings, 48 the home date line.
CONFIG_LV_FONT_MONTSERRAT_20=y
CONFIG_LV_FONT_MONTSERRAT_28=y
CONFIG_LV_FONT_MONTSERRAT_48=y
# Body text at arm's length on a 7" 1024x600 panel.
CONFIG_LV_FONT_DEFAULT_MONTSERRAT_20=y
```

- [ ] **Step 5: Create the gated component**

`components/panel_ui/CMakeLists.txt`:

```cmake
# components/panel_ui: the Master v2 7" LVGL face. Sources ONLY for the P4
# master: components/ is in EXTRA_COMPONENT_DIRS for master, zone and rescue
# (and will be for rescue_p4), and the panel must never land in any of them
# (recovery design decision 4: no panel in rescue). Two gates, both needed:
#   - CMAKE_PROJECT_NAME keeps it out of zone/rescue/rescue_p4;
#   - IDF_TARGET keeps it out of the ESP32 (DevKitC) master.
# SRCS gating is safe in this late pass (wifi_mgr :25-28, http_srv :11-16).
# REQUIRES/PRIV_REQUIRES are NOT gated -- IDF evaluates them in an early pass
# where a CONFIG_/project-name gate silently comes out empty
# (cp_ota/CMakeLists.txt:17-31, wifi_mgr/CMakeLists.txt:14-19) -- so they
# list only components present in every app build. The P4-only managed
# components (BSP, lvgl, adapter, touch) attach through
# idf_component_optional_requires() after registration, the cp_ota idiom:
# it checks BUILD_COMPONENTS and is a no-op wherever they are absent.
set(PANEL_SRCS "")
idf_build_get_property(panel_target IDF_TARGET)
if("${CMAKE_PROJECT_NAME}" STREQUAL "hillgrow_master" AND "${panel_target}" STREQUAL "esp32p4")
    set(PANEL_SRCS "panel_ui.c")
endif()

idf_component_register(SRCS ${PANEL_SRCS} INCLUDE_DIRS "."
    REQUIRES panel_svc hg_cfg hg_mcfg ring_proto cmd_core
    PRIV_REQUIRES cmd_task node_mgr alarm_mgr wifi_mgr time_svc app_common state_snap notify board hg_blob
                  nvs_flash esp_timer esp_driver_ledc fatfs sdmmc esp_driver_sdmmc)
# every name above exists in every app build (the early requirements pass evaluates this file for zone/rescue too);
# panel_ui never requires http_srv -- the web's quarantine count reaches it through psvc_state (Task 9)
if(PANEL_SRCS)
    idf_component_optional_requires(PRIVATE waveshare__esp32_p4_wifi6_touch_lcd_7b lvgl__lvgl
                                            espressif__esp_lvgl_adapter espressif__esp_lcd_touch)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE LV_LVGL_H_INCLUDE_SIMPLE)
endif()
```

`components/panel_ui/panel_ui.h`:

```c
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* The Master v2 7" touch panel (spec 2026-09-18-master-v2-panel-ui-design.md).
 * LVGL-free on purpose: these two are the only panel symbols app_main sees,
 * and the header compiles on every target (the component is include-only
 * everywhere but the P4 master).
 *
 * Placement (recovery-design compatible, anchored by call name, never by line):
 *   panel_start()          immediately after ota_trial_start(1) and before the
 *                          first radio call (today wifi_mgr_start(); later the
 *                          recovery plan's explicit esp_hosted_init());
 *   panel_services_start() immediately after node_mgr_start(), before the
 *                          cp_ota_sync() gate.
 * The panel never calls cp_ota_sync() or any esp_hosted_* function, and it is
 * not part of ota_trial_drivers_ok() (D17). */

/* Display + touch + LVGL task + first screen. Soft on EVERY failure: 0 = lit,
 * -1 = dark, and boot continues either way (system spec 3.3). */
int panel_start(void);

/* The panel worker and the 1 Hz poller. 0 / -1 (-1 when the panel is dark).
 * The panel shows "starting" until the first snapshot lands. */
int panel_services_start(void);

#ifdef __cplusplus
}
#endif
```

`components/panel_ui/panel_ui.c`:

```c
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
```

(`LVGL_VERSION_*` are lvgl's own version macros from `lv_version.h`, always present. They are used instead of `lv_version_major()` so this stub needs nothing but the header.)

`master/main/CMakeLists.txt` line 2 becomes:

```cmake
    REQUIRES board cli cmd_task cmd_common app_common notify app_update esp_timer nvs_flash ota_trial ring_link node_mgr master_cmds wifi_mgr fw_srv http_srv http_auth hg_mcfg time_svc alarm_mgr mcfg_ops cp_ota panel_svc panel_ui)
```

- [ ] **Step 6: Call it from app_main, P4 only**

In `master/main/app_main.c`, after `#include "alarm_mgr.h"` (`:24`) add:

```c
#if CONFIG_IDF_TARGET_ESP32P4
#include "panel_ui.h"     /* the 7" panel -- P4 master only (components/panel_ui) */
#endif
```

Immediately after the line `ota_trial_start(1);` add:

```c
#if CONFIG_IDF_TARGET_ESP32P4
    /* The 7" panel comes up BEFORE the first radio call, so a silent C6 (or,
     * after the recovery plan lands, its bounded esp_hosted retry loop) never
     * leaves the operator looking at a dark screen. About 283 ms (the spike's
     * figure) ahead of the AP and the ring (D19). Soft on every failure; the
     * return value is informational only. */
    (void)panel_start();
#endif
```

Immediately after the line `node_mgr_start();` add:

```c
#if CONFIG_IDF_TARGET_ESP32P4
    /* Panel worker + poller, once node_mgr owns the node table and BEFORE the
     * cp_ota_sync() gate below, which can block this task for 15-45 s. */
    (void)panel_services_start();
#endif
```

- [ ] **Step 7: Build for the P4 and compare the resolved set**

The defaults file changed, so the existing P4 sdkconfig must go first. An existing sdkconfig overrides the defaults.

```powershell
Remove-Item C:\Projects\HillGrov\master\build_p4\sdkconfig
& C:\esp\v6.0.1\esp-idf\export.ps1
idf.py -C C:\Projects\HillGrov\master -B C:\Projects\HillGrov\master\build_p4 -DIDF_TARGET=esp32p4 -DSDKCONFIG=C:\Projects\HillGrov\master\build_p4\sdkconfig -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.esp32p4" build
Select-String -Path C:\Projects\HillGrov\master\build_p4\sdkconfig -Pattern 'CONFIG_IDF_TARGET="esp32p4"'
```
Expected: `Project build complete` with 0 warnings, and the pattern matches.

**Before** the lock checkout, print the P4 resolution and paste it into the task report:

```powershell
$cur = $null; Get-Content C:\Projects\HillGrov\master\dependencies.lock | ForEach-Object { if ($_ -match '^  (\S+):$') { $cur = $Matches[1] } elseif ($_ -match '^    version: (\S+)$' -and $cur) { "$cur $($Matches[1])"; $cur = $null } }
```
Expected: every package in the bench-proven table above appears at exactly that version. `espressif/esp_hosted`, `espressif/esp_wifi_remote`, `espressif/eppp_link`, cjson, mdns and `idf` also appear, which is expected. **Stop on any version difference** (D8), and report it rather than continuing.

Then:

```powershell
git -C C:\Projects\HillGrov checkout -- master/dependencies.lock
```

- [ ] **Step 8: Prove the keys took**

```powershell
Select-String -Path C:\Projects\HillGrov\master\build_p4\sdkconfig -Pattern '# CONFIG_BSP_ERROR_CHECK is not set','CONFIG_LV_FONT_MONTSERRAT_20=y','CONFIG_LV_FONT_MONTSERRAT_28=y','CONFIG_LV_FONT_MONTSERRAT_48=y','CONFIG_LV_FONT_DEFAULT_MONTSERRAT_20=y'
```
Expected: five matching lines, one per pattern.

- [ ] **Step 9: Run the other gates**

Run GATE-ESP32. Expected: master, zone and rescue each print `Project build complete` with 0 warnings, and the lock diff is empty.
Then: `(Get-Item C:\Projects\HillGrov\master\build\hillgrow_master.bin).Length`. Expected: equal to `esp32_bin_before`. On the ESP32 master, `panel_ui` is include-only and both calls are compiled out.
Run GATE-HOST. Expected: 38/38 tests pass.

GATE-ESP32 purged the P4 packages again. Rebuild the P4 image for flashing by re-running the Step 7 build command (without the `Remove-Item`), then `git -C C:\Projects\HillGrov checkout -- master/dependencies.lock`.

- [ ] **Step 10: Flash and read the boot line**

Run FLASH-P4 (Global notes above: `GET VERSION` must show `VALID` first; dry run first). Open the console with `C:\Python311\python -m serial.tools.miniterm COM28 115200` and press the board's RESET button.
Expected: the boot log contains `I (...) panel: LVGL 9.5.0 linked; display not started`, and boot continues to the CLI prompt as before. Close the console (Ctrl+]).

- [ ] **Step 11: Commit**

```powershell
git -C C:\Projects\HillGrov add master/main/idf_component.yml master/dependencies.lock master/sdkconfig.defaults.esp32p4 components/panel_ui/CMakeLists.txt components/panel_ui/panel_ui.h components/panel_ui/panel_ui.c master/main/CMakeLists.txt master/main/app_main.c
git -C C:\Projects\HillGrov commit -m "build(master): pin the P4 panel stack; add the gated panel_ui component" -m "Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>"
```

### Task 7: Panel bring-up: soft-error `panel_hw`, the lock wrapper, touch diagnostics, the boot screen and the I²C pin fix (Stage 0)

The panel lights on the real master. Every BSP failure is soft. Touch is proven by a finger rather than by a log line.

**Files:**
- Create:
  - `components/panel_ui/panel_hw.h`, `.c`; `panel_lock.h`, `.c`; `pnl_touch.h`, `.c` (P); `scr_diag.h`, `.c`; `pnl_palette.h` (P); `pnl_theme.h`, `.c`.
  - `tests/host/test_pnl_touch.c`.
- Modify:
  - `components/panel_ui/panel_ui.c`: the real `panel_start()`.
  - `components/panel_ui/CMakeLists.txt`.
  - `components/board/board.h`: the P4 block becomes `HG_GPIO_I2C_SDA 7`, `HG_GPIO_I2C_SCL 8` (`:18-19`).
  - `docs/pin-mapping.md`: line 72 becomes SCL 8, SDA 7.
  - `tests/host/CMakeLists.txt`: include `${COMP}/panel_ui`; add `hg_test(test_pnl_touch ${COMP}/panel_ui/pnl_touch.c)`.

**Interfaces:**
- Consumes:
  - from Task 6: `panel_start` and `panel_services_start`;
  - BSP and adapter: `esp_lv_adapter_init`, `bsp_display_new_with_handles(NULL, &h)`, `esp_lv_adapter_register_display`, `bsp_touch_new`, `esp_lv_adapter_register_touch`, `esp_lv_adapter_start`, `bsp_display_lock` and `bsp_display_unlock`; LVGL's `lv_mem_add_pool` (builtin allocator, `src/stdlib/lv_mem.h:60`) with `heap_caps_malloc(..., MALLOC_CAP_SPIRAM)` for the overflow pool;
  - `esp_lcd_touch_read_data` and `esp_lcd_touch_get_data` (`esp_lcd_touch` 1.2.1), plus `esp_lcd_touch_set_swap_xy`, `esp_lcd_touch_set_mirror_x` and `esp_lcd_touch_set_mirror_y`;
  - LVGL `lv_indev_set_read_cb`.
- Produces:
  ```c
  /* pnl_touch.h (pure) */
  #define PNL_ORIENT_NORMAL  0   /* display ESP_LV_ADAPTER_ROTATE_180 + touch passthrough: the bench-proven mapping */
  #define PNL_ORIENT_FLIPPED 1   /* display ESP_LV_ADAPTER_ROTATE_0 + a 180-degree touch flip done here, clamped */
  void pnl_touch_map(uint8_t orient, uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t *ox, uint16_t *oy);
       /* NORMAL: clamp to [0,w-1] x [0,h-1]; FLIPPED: ox = x >= w ? 0 : w-1-x, oy = y >= h ? 0 : h-1-y (never wraps) */

  /* panel_lock.h */
  #define PANEL_LOCK_MS 200u
  bool panel_lock(uint32_t timeout_ms);    /* bsp_display_lock(timeout_ms ? timeout_ms : 1); false -> caller makes NO lv_* call */
  void panel_unlock(void);

  /* panel_hw.h (glue) */
  typedef struct { uint8_t lit, touch_ok, orient; uint32_t up_ms; uint32_t reads, read_errs, points; int last_err;
                   uint16_t last_x, last_y; } panel_hw_status_t;
  int  panel_hw_start(uint8_t orient);   /* #error if CONFIG_BSP_ERROR_CHECK; _Static_assert(HG_GPIO_I2C_SDA == BSP_I2C_SDA &&
       HG_GPIO_I2C_SCL == BSP_I2C_SCL); adapter cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG() with task_core_id 1, task_priority 3;
       a PANEL_LV_PSRAM_POOL (256 KB) PSRAM pool added with lv_mem_add_pool() right after esp_lv_adapter_init() (soft);
       display profile exactly the BSP's (MIPI_DSI, 1024x600, buffer_height 50, use_psram false, TRIPLE_PARTIAL);
       touch flags {0,0,0} + custom_touch_read (counts both rcs, maps with pnl_touch_map); a touch failure leaves the display
       up with touch_ok 0; 0 display up / -1 no display. Backlight stays at 0 until the caller sets it. */
  void panel_hw_status(panel_hw_status_t *out);   /* [ANY] */
  lv_display_t *panel_hw_display(void);
  lv_indev_t   *panel_hw_touch(void);             /* NULL when touch_ok == 0 */
  int  panel_hw_brightness(uint8_t pct);          /* 0..100 via ledc_set_duty/ledc_update_duty directly; logs nothing at INFO */

  /* pnl_palette.h (pure; web/app.css dark theme) */
  #define PNL_C_BG 0x121812
  #define PNL_C_CARD 0x1C241C
  #define PNL_C_TEXT 0xE6EFE6
  #define PNL_C_MUTED 0x9FB09F
  #define PNL_C_BORDER 0x2C382C
  #define PNL_C_ACCENT 0x2F7D32
  #define PNL_C_OK 0x2F7D32
  #define PNL_C_DEGRADED 0xB8860B
  #define PNL_C_OFFLINE 0xC0392B
  #define PNL_C_UPDATING 0x2B6FB8
  #define PNL_C_EMPTY 0x8A8A8A
  #define PNL_C_WARN 0x9A6B00
  #define PNL_C_OK_TEXT 0x5FBF62
  #define PNL_C_OFFLINE_TEXT 0xEB6F63
  #define PNL_C_WARN_TEXT 0xE0AD3D

  /* pnl_theme.h (glue) */
  void       pnl_theme_init(lv_display_t *disp);   /* lv_theme_default_init(dark, accent PNL_C_ACCENT, Montserrat 20) */
  lv_color_t pnl_health_color(node_health_t h);    /* ONLINE ok / DEGRADED / OFFLINE / UPDATING / EMPTY */

  /* scr_diag.h (glue) */
  void scr_diag_build(lv_obj_t *parent);   /* version, internal heap free/min, 5 touch targets, live touch counters */
  ```
  The `panel_start()` sequence:
  1. Log internal free/min.
  2. `panel_hw_start(PNL_ORIENT_NORMAL)`. On -1, log `panel: no display` and return -1.
  3. `panel_lock(2000)`; `pnl_theme_init`; `scr_diag_build(lv_screen_active())`; `panel_unlock()`.
  4. `panel_hw_brightness(80)`.
  5. Log `panel: up in %u ms (lvgl c1/p3, touch %s)` and internal free/min.
- Interface additions:
  - `panel_hw.h`: `#define PANEL_HW_W 1024` and `#define PANEL_HW_H 600`, the native panel size passed to `pnl_touch_map()`.
  - `pnl_theme.h`: `void pnl_theme_card(lv_obj_t *o)` (the web's card look: `PNL_C_CARD` background, `PNL_C_BORDER` 1 px border, radius 8, padding 12) and `lv_obj_t *pnl_label(lv_obj_t *parent, const char *text, const lv_font_t *font, uint32_t hex)` (a label in one call). Every later screen uses both.
  - Deviation: the custom touch read is installed with LVGL's own `lv_indev_set_read_cb()` on the indev that `esp_lv_adapter_register_touch()` returns, rather than through `esp_lv_adapter_set_touch_callbacks()`. The adapter source is not on disk to confirm that function's signature, and the LVGL call is public API with a known signature. Either way the result is the same: our callback reads the GT911, counts both return codes and maps the point. The mirrors are switched off on the touch handle itself with `esp_lcd_touch_set_mirror_*()`, because `bsp_touch_new(NULL, ...)` turns them on by default (the double flip the spike found).

**What is provable where.**
- `pnl_touch.c` is pure and host-tested: the corners, the centre and the out-of-range clamp for both orientations.
- Everything else here is BSP, adapter and LVGL glue. GATE-P4 proves it compiles with `-Werror` against the pinned stack, including the `#error` and the `_Static_assert`.
- Only the bench proves it lights, maps touch correctly and survives a missing GT911: Step 11 now, and Stage 0 gate steps 3 and 4.

- [ ] **Step 1: Write the failing touch-map test**

`tests/host/test_pnl_touch.c`:

```c
/* The panel's touch mapping. NORMAL is the bench-proven passthrough (display
   rotated 180 in the adapter, GT911 already reports in that frame) and only
   clamps; FLIPPED does the 180-degree flip itself. Neither may ever wrap: the
   esp_lcd_touch mirrors do x_max - x on a uint16_t with no clamp, and a point
   outside the panel then becomes ~65000 (what_we_learned 2026-09-18). */
#include "unity.h"
#include "pnl_touch.h"

void setUp(void) {}
void tearDown(void) {}

#define W 1024
#define H 600

static void map(uint8_t o, uint16_t x, uint16_t y, uint16_t *ox, uint16_t *oy) {
    *ox = 0xBEEF; *oy = 0xBEEF;
    pnl_touch_map(o, x, y, W, H, ox, oy);
}

static void test_normal_is_passthrough(void) {
    uint16_t x, y;
    map(PNL_ORIENT_NORMAL, 0, 0, &x, &y);         TEST_ASSERT_EQUAL_UINT16(0, x);    TEST_ASSERT_EQUAL_UINT16(0, y);
    map(PNL_ORIENT_NORMAL, 1023, 0, &x, &y);      TEST_ASSERT_EQUAL_UINT16(1023, x); TEST_ASSERT_EQUAL_UINT16(0, y);
    map(PNL_ORIENT_NORMAL, 0, 599, &x, &y);       TEST_ASSERT_EQUAL_UINT16(0, x);    TEST_ASSERT_EQUAL_UINT16(599, y);
    map(PNL_ORIENT_NORMAL, 1023, 599, &x, &y);    TEST_ASSERT_EQUAL_UINT16(1023, x); TEST_ASSERT_EQUAL_UINT16(599, y);
    map(PNL_ORIENT_NORMAL, 512, 300, &x, &y);     TEST_ASSERT_EQUAL_UINT16(512, x);  TEST_ASSERT_EQUAL_UINT16(300, y);
}

static void test_flipped_turns_corners_around(void) {
    uint16_t x, y;
    map(PNL_ORIENT_FLIPPED, 0, 0, &x, &y);        TEST_ASSERT_EQUAL_UINT16(1023, x); TEST_ASSERT_EQUAL_UINT16(599, y);
    map(PNL_ORIENT_FLIPPED, 1023, 0, &x, &y);     TEST_ASSERT_EQUAL_UINT16(0, x);    TEST_ASSERT_EQUAL_UINT16(599, y);
    map(PNL_ORIENT_FLIPPED, 0, 599, &x, &y);      TEST_ASSERT_EQUAL_UINT16(1023, x); TEST_ASSERT_EQUAL_UINT16(0, y);
    map(PNL_ORIENT_FLIPPED, 1023, 599, &x, &y);   TEST_ASSERT_EQUAL_UINT16(0, x);    TEST_ASSERT_EQUAL_UINT16(0, y);
    map(PNL_ORIENT_FLIPPED, 512, 300, &x, &y);    TEST_ASSERT_EQUAL_UINT16(511, x);  TEST_ASSERT_EQUAL_UINT16(299, y);
}

static void test_out_of_range_clamps_and_never_wraps(void) {
    uint16_t x, y;
    map(PNL_ORIENT_NORMAL, 1100, 700, &x, &y);    TEST_ASSERT_EQUAL_UINT16(1023, x); TEST_ASSERT_EQUAL_UINT16(599, y);
    map(PNL_ORIENT_FLIPPED, 1100, 700, &x, &y);   TEST_ASSERT_EQUAL_UINT16(0, x);    TEST_ASSERT_EQUAL_UINT16(0, y);
    map(PNL_ORIENT_FLIPPED, 1024, 600, &x, &y);   TEST_ASSERT_EQUAL_UINT16(0, x);    TEST_ASSERT_EQUAL_UINT16(0, y);
    map(PNL_ORIENT_FLIPPED, 65535, 65535, &x, &y);
    TEST_ASSERT_TRUE(x < W && y < H);
}

static void test_unknown_orientation_behaves_as_normal(void) {
    uint16_t x, y;
    map(7, 10, 20, &x, &y);                       TEST_ASSERT_EQUAL_UINT16(10, x);   TEST_ASSERT_EQUAL_UINT16(20, y);
}

static void test_zero_size_panel_yields_origin(void) {
    uint16_t x = 5, y = 5;
    pnl_touch_map(PNL_ORIENT_FLIPPED, 10, 10, 0, 0, &x, &y);
    TEST_ASSERT_EQUAL_UINT16(0, x);
    TEST_ASSERT_EQUAL_UINT16(0, y);
}

int main(void) { UNITY_BEGIN();
    RUN_TEST(test_normal_is_passthrough);
    RUN_TEST(test_flipped_turns_corners_around);
    RUN_TEST(test_out_of_range_clamps_and_never_wraps);
    RUN_TEST(test_unknown_orientation_behaves_as_normal);
    RUN_TEST(test_zero_size_panel_yields_origin);
    return UNITY_END(); }
```

In `tests/host/CMakeLists.txt`, add `${COMP}/panel_ui` to `include_directories(...)` (after `${COMP}/panel_svc ${COMP}/time_svc`). Append:

```cmake
# panel_ui's PURE helpers only (panel plan Global Constraints, "Pure files"):
# they include no lvgl.h / bsp / esp_* / freertos header, so they compile here.
hg_test(test_pnl_touch ${COMP}/panel_ui/pnl_touch.c)
```

- [ ] **Step 2: Run it to verify it fails**

Run the one-test command with `-R test_pnl_touch`.
Expected: configure fails with `Cannot find source file: .../components/panel_ui/pnl_touch.c`.

- [ ] **Step 3: Implement the mapping**

`components/panel_ui/pnl_touch.h`:

```c
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Touch orientation (pure). Rotation and touch mirroring are two separate
 * knobs (what_we_learned 2026-09-18); this owns the touch half. Never enable
 * the esp_lcd_touch mirrors instead: they compute x_max - x on a uint16_t with
 * no clamp. */
#define PNL_ORIENT_NORMAL  0   /* display ESP_LV_ADAPTER_ROTATE_180 + touch passthrough: the bench-proven mapping */
#define PNL_ORIENT_FLIPPED 1   /* display ESP_LV_ADAPTER_ROTATE_0 + a 180-degree touch flip done here, clamped */

/* NORMAL (and any unknown value): clamp to [0,w-1] x [0,h-1].
 * FLIPPED: ox = x >= w ? 0 : w-1-x, oy = y >= h ? 0 : h-1-y. Never wraps.
 * w or h of 0 -> (0,0). */
void pnl_touch_map(uint8_t orient, uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t *ox, uint16_t *oy);

#ifdef __cplusplus
}
#endif
```

`components/panel_ui/pnl_touch.c`:

```c
#include "pnl_touch.h"

void pnl_touch_map(uint8_t orient, uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t *ox, uint16_t *oy) {
    if (w == 0 || h == 0) { *ox = 0; *oy = 0; return; }
    if (orient == PNL_ORIENT_FLIPPED) {
        *ox = (x >= w) ? 0 : (uint16_t)(w - 1u - x);
        *oy = (y >= h) ? 0 : (uint16_t)(h - 1u - y);
    } else {
        *ox = (x >= w) ? (uint16_t)(w - 1u) : x;
        *oy = (y >= h) ? (uint16_t)(h - 1u) : y;
    }
}
```

- [ ] **Step 4: Run it to verify it passes**

Run the one-test command with `-R test_pnl_touch`.
Expected: `100% tests passed, 0 tests failed out of 1`.

- [ ] **Step 5: Fix the P4 I²C pin names**

`components/board/board.h` lines 18-19 become:

```c
#define HG_GPIO_I2C_SDA    7    /* the board's own I2C header, level-shifted; = BSP_I2C_SDA (GT911 proven) */
#define HG_GPIO_I2C_SCL    8    /* = BSP_I2C_SCL; any future P4 I2C user shares bsp_i2c_get_handle() (D6) */
```

`docs/pin-mapping.md` line 72 becomes:

```
| I²C header | SCL **8**, SDA **7** (level-shifted to `D_SCL`/`D_SDA`) | BSP `BSP_I2C_SCL`/`BSP_I2C_SDA`, proven by the working GT911; the earlier SCL 7 / SDA 8 was swapped |
```

Nothing uses `HG_GPIO_I2C_*` on either target today (`git -C C:\Projects\HillGrov grep -n HG_GPIO_I2C` hits only `board.h`). `panel_hw.c`'s `_Static_assert` below keeps the two in step from now on. Note for SP2: the ES7210 codec's default address 0x40 is the PCA9685's.

- [ ] **Step 6: Confirm the BSP and adapter identifiers against the pinned sources**

The spike that proved them has been deleted, and `master/managed_components` holds only cjson and mdns after an ESP32 build. Re-extract the P4 set, read the three headers, then restore the lock:

```powershell
& C:\esp\v6.0.1\esp-idf\export.ps1
idf.py -C C:\Projects\HillGrov\master -B C:\Projects\HillGrov\master\build_p4 -DIDF_TARGET=esp32p4 -DSDKCONFIG=C:\Projects\HillGrov\master\build_p4\sdkconfig -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.esp32p4" reconfigure
git -C C:\Projects\HillGrov checkout -- master/dependencies.lock
Get-ChildItem -Recurse C:\Projects\HillGrov\master\managed_components\waveshare__esp32_p4_wifi6_touch_lcd_7b\include, C:\Projects\HillGrov\master\managed_components\espressif__esp_lvgl_adapter\include, C:\Projects\HillGrov\master\managed_components\espressif__esp_lcd_touch\include -Filter *.h | Select-Object FullName
```

Open `master\managed_components\waveshare__esp32_p4_wifi6_touch_lcd_7b\esp32_p4_wifi6_touch_lcd_7b.c` and read `bsp_display_start_with_config()`: the adapter/display/touch sequence at about `:564-631` is what `panel_hw_start()` repeats, with soft errors. Check each identifier the code in Step 7 uses against the headers, and record the verdict for each in the task report:
- `bsp/esp-bsp.h` and the `BSP_I2C_SDA`, `BSP_I2C_SCL`, `BSP_LCD_H_RES` and `BSP_LCD_V_RES` macros;
- `bsp_lcd_handles_t` and its `.panel` / `.io` members; `bsp_display_new_with_handles`; `bsp_touch_new`;
- `esp_lv_adapter_config_t`, `ESP_LV_ADAPTER_DEFAULT_CONFIG()`, `.task_core_id` and `.task_priority`;
- `esp_lv_adapter_display_config_t`, `ESP_LV_ADAPTER_DISPLAY_MIPI_DEFAULT_CONFIG()`, `.profile.buffer_height`, `.profile.use_psram` and `.tear_avoid_mode`;
- `ESP_LV_ADAPTER_ROTATE_0` / `_180` and `ESP_LV_ADAPTER_TEAR_AVOID_MODE_TRIPLE_PARTIAL`;
- `esp_lv_adapter_touch_config_t`, `ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG()`, `esp_lv_adapter_register_display`, `esp_lv_adapter_register_touch` and `esp_lv_adapter_start`;
- `esp_lcd_touch_point_data_t` (`.x`, `.y`), `esp_lcd_touch_get_data` and `esp_lcd_touch_read_data`.

**Where a header spells one differently, use the header's spelling and keep the same values**: MIPI-DSI, 1024x600, `buffer_height` 50, `use_psram` false, TRIPLE_PARTIAL, core 1, prio 3. If the BSP initialises the display config through a different macro, copy its initialiser verbatim and then override only those fields. The behaviour is binding; the spelling is what this step confirms.

- [ ] **Step 7: Write the hardware layer and the lock wrapper**

`components/panel_ui/panel_hw.h`:

```c
#pragma once
#include <stdint.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Display, touch and backlight for the Waveshare ESP32-P4-WIFI6-Touch-LCD-7B
 * (EK79007 1024x600 over MIPI-DSI, GT911 on I2C port 1), brought up through
 * the registry BSP 3.0.1 and esp_lvgl_adapter 0.6.4 with SOFT errors
 * everywhere: the greenhouse controller never aborts over its screen.
 * Deliberately NOT bsp_display_start()/_with_config(): those call
 * ESP_ERROR_CHECK(esp_lv_adapter_start()) unconditionally, return NULL for
 * the whole display when only touch failed, and init LEDC twice. */

#define PANEL_HW_W 1024   /* native panel size, the frame pnl_touch_map() clamps to */
#define PANEL_HW_H 600
#define PANEL_LV_PSRAM_POOL (256u * 1024u)   /* LVGL overflow pool in PSRAM, added by panel_hw_start() */

typedef struct {
    uint8_t  lit, touch_ok, orient;
    uint32_t up_ms;                       /* bring-up time */
    uint32_t reads, read_errs, points;    /* touch reads, failed reads (either rc), reads with >= 1 point */
    int      last_err;                    /* last failing esp_err_t, 0 if none */
    uint16_t last_x, last_y;              /* last mapped point */
} panel_hw_status_t;

/* Brings the display up (and touch, if the GT911 answers). adapter task on
 * core 1 at prio 3, display profile exactly the BSP's, touch mirrors OFF on
 * the handle, and our own read callback installed (both esp_lcd_touch return
 * codes counted, point mapped by pnl_touch_map). A touch failure leaves the
 * display up with touch_ok 0. 0 display up / -1 no display. The backlight
 * stays at 0 until the caller calls panel_hw_brightness(). Call once, from
 * app_main (before the LVGL task exists). */
int  panel_hw_start(uint8_t orient);
void panel_hw_status(panel_hw_status_t *out);   /* [ANY] */
lv_display_t *panel_hw_display(void);
lv_indev_t   *panel_hw_touch(void);             /* NULL when touch_ok == 0 */

/* 0..100 (clamped) via ledc_set_duty/ledc_update_duty on the BSP's own LEDC
 * channel, duty 1023*pct/100 -- never bsp_display_brightness_set(), which logs
 * at INFO on UART0, the machine-parsed CLI. 0 ok / -1 (display not up, or LEDC
 * refused). */
int  panel_hw_brightness(uint8_t pct);

#ifdef __cplusplus
}
#endif
```

`components/panel_ui/panel_hw.c`:

```c
#include <string.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "driver/ledc.h"
#include "lvgl.h"
#include "bsp/esp-bsp.h"
#include "esp_lv_adapter.h"
#include "esp_lcd_touch.h"
#include "board.h"
#include "pnl_touch.h"
#include "panel_hw.h"

#if CONFIG_BSP_ERROR_CHECK
#error "CONFIG_BSP_ERROR_CHECK=y turns every BSP failure into an abort of the greenhouse controller (system spec 3.3) -- keep CONFIG_BSP_ERROR_CHECK=n in master/sdkconfig.defaults.esp32p4"
#endif

/* The one translation unit that sees both headers: board.h's P4 I2C names must
 * be the bus the BSP actually drives (the GT911 proves the BSP's). */
_Static_assert(HG_GPIO_I2C_SDA == BSP_I2C_SDA && HG_GPIO_I2C_SCL == BSP_I2C_SCL,
               "board.h HG_GPIO_I2C_* disagree with the BSP's I2C pins");

static const char *TAG = "panel_hw";

static lv_display_t          *s_disp;
static lv_indev_t            *s_touch;
static esp_lcd_touch_handle_t s_tp;
static portMUX_TYPE           s_mux = portMUX_INITIALIZER_UNLOCKED;
static panel_hw_status_t      s_st;
static uint32_t               s_log_ms;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

/* LVGL indev read callback -- runs on the LVGL task. A failed touch read is
 * otherwise invisible (what_we_learned 2026-09-18), so both return codes are
 * counted and a failure is logged at most once per 5 s (UART0 is the CLI). */
static void touch_read(lv_indev_t *indev, lv_indev_data_t *data) {
    (void)indev;
    esp_err_t r_read = esp_lcd_touch_read_data(s_tp);
    esp_lcd_touch_point_data_t pt[1];
    uint8_t cnt = 0;
    esp_err_t r_get = ESP_FAIL;
    if (r_read == ESP_OK) r_get = esp_lcd_touch_get_data(s_tp, pt, &cnt, 1);
    int pressed = (r_read == ESP_OK && r_get == ESP_OK && cnt > 0);
    uint16_t x = 0, y = 0;
    if (pressed) pnl_touch_map(s_st.orient, pt[0].x, pt[0].y, PANEL_HW_W, PANEL_HW_H, &x, &y);
    int err = (r_read != ESP_OK) ? (int)r_read : (r_get != ESP_OK ? (int)r_get : 0);

    portENTER_CRITICAL(&s_mux);
    s_st.reads++;
    if (err) { s_st.read_errs++; s_st.last_err = err; }
    if (pressed) { s_st.points++; s_st.last_x = x; s_st.last_y = y; }
    uint32_t errs = s_st.read_errs;
    uint16_t lx = s_st.last_x, ly = s_st.last_y;
    portEXIT_CRITICAL(&s_mux);

    if (err) {
        uint32_t t = now_ms();
        if (t - s_log_ms >= 5000) {
            s_log_ms = t;
            ESP_LOGW(TAG, "touch read failed: read %s, get %s (%u failed reads so far)",
                     esp_err_to_name(r_read), esp_err_to_name(r_get), (unsigned)errs);
        }
    }
    data->point.x = lx;   /* on release LVGL uses the last pressed point */
    data->point.y = ly;
    data->state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

int panel_hw_start(uint8_t orient) {
    memset(&s_st, 0, sizeof s_st);
    s_st.orient = (orient == PNL_ORIENT_FLIPPED) ? PNL_ORIENT_FLIPPED : PNL_ORIENT_NORMAL;
    uint32_t t0 = now_ms();

    esp_lv_adapter_config_t acfg = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    acfg.task_core_id  = 1;   /* away from ring_rx (c0/p6), cmd_task/httpd (c0/p5) */
    acfg.task_priority = 3;   /* below node_mgr (c1/p4): a long redraw never starves the ring logic */
    esp_err_t rc = esp_lv_adapter_init(&acfg);
    if (rc != ESP_OK) { ESP_LOGE(TAG, "esp_lv_adapter_init: %s", esp_err_to_name(rc)); return -1; }

    /* LVGL 9.5.0 defaults: LV_USE_ASSERT_MALLOC on and LV_ASSERT_HANDLER `while(1);` (lv_conf_internal.h:1491-1496,
     * no Kconfig override). A NULL lv_malloc would spin the LVGL task (c1/p3) for good, starve IDLE1, and the TWDT
     * (IDLE1 watched, 8 s, panic) would reboot the greenhouse controller -- in an OTA trial that retires the image.
     * So the builtin TLSF allocator gets a PSRAM overflow pool on top of its internal CONFIG_LV_MEM_SIZE_KILOBYTES
     * pool: exhaustion then costs speed, not a reset. lv_init() ran inside esp_lv_adapter_init(); the LVGL task does
     * not exist until esp_lv_adapter_start(), so no lock is needed here. Soft on failure, like everything here. */
    void *lv_extra = heap_caps_malloc(PANEL_LV_PSRAM_POOL, MALLOC_CAP_SPIRAM);
    if (lv_extra && lv_mem_add_pool(lv_extra, PANEL_LV_PSRAM_POOL)) {
        ESP_LOGI(TAG, "LVGL pool: %u KB internal + %u KB PSRAM overflow", (unsigned)CONFIG_LV_MEM_SIZE_KILOBYTES,
                 (unsigned)(PANEL_LV_PSRAM_POOL / 1024u));
    } else {
        if (lv_extra) heap_caps_free(lv_extra);
        ESP_LOGW(TAG, "LVGL PSRAM overflow pool unavailable -- LVGL has only its %u KB internal pool",
                 (unsigned)CONFIG_LV_MEM_SIZE_KILOBYTES);
    }

    bsp_lcd_handles_t h;
    memset(&h, 0, sizeof h);
    rc = bsp_display_new_with_handles(NULL, &h);
    if (rc != ESP_OK) { ESP_LOGE(TAG, "bsp_display_new_with_handles: %s", esp_err_to_name(rc)); return -1; }

    esp_lv_adapter_display_config_t dcfg = ESP_LV_ADAPTER_DISPLAY_MIPI_DEFAULT_CONFIG(
        h.panel, h.io, BSP_LCD_H_RES, BSP_LCD_V_RES,
        s_st.orient == PNL_ORIENT_FLIPPED ? ESP_LV_ADAPTER_ROTATE_0 : ESP_LV_ADAPTER_ROTATE_180);
    dcfg.profile.buffer_height = 50;       /* the BSP's own profile: 1024x50x2 B internal draw buffer */
    dcfg.profile.use_psram     = false;
    dcfg.tear_avoid_mode       = ESP_LV_ADAPTER_TEAR_AVOID_MODE_TRIPLE_PARTIAL;
    s_disp = esp_lv_adapter_register_display(&dcfg);
    if (!s_disp) { ESP_LOGE(TAG, "esp_lv_adapter_register_display failed"); return -1; }

    rc = bsp_touch_new(NULL, &s_tp);
    if (rc == ESP_OK && s_tp) {
        /* bsp_touch_new(NULL) turns BOTH mirrors on; with the display rotated
         * 180 that flips touch twice (the spike's bug). Off, all three. */
        esp_lcd_touch_set_swap_xy(s_tp, false);
        esp_lcd_touch_set_mirror_x(s_tp, false);
        esp_lcd_touch_set_mirror_y(s_tp, false);
        esp_lv_adapter_touch_config_t tcfg = ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(s_disp, s_tp);
        s_touch = esp_lv_adapter_register_touch(&tcfg);
        if (s_touch) {
            lv_indev_set_read_cb(s_touch, touch_read);   /* ours: counts both rcs, clamps, maps orientation */
            s_st.touch_ok = 1;
        } else {
            ESP_LOGE(TAG, "esp_lv_adapter_register_touch failed -- display only");
        }
    } else {
        ESP_LOGE(TAG, "bsp_touch_new: %s -- display only (GT911 not answering?)", esp_err_to_name(rc));
        s_tp = NULL;
    }

    rc = esp_lv_adapter_start();
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "esp_lv_adapter_start: %s", esp_err_to_name(rc));
        s_disp = NULL;
        s_touch = NULL;
        s_st.touch_ok = 0;
        return -1;
    }
    s_st.lit = 1;
    s_st.up_ms = now_ms() - t0;
    return 0;
}

void panel_hw_status(panel_hw_status_t *out) {
    portENTER_CRITICAL(&s_mux);
    *out = s_st;
    portEXIT_CRITICAL(&s_mux);
}

lv_display_t *panel_hw_display(void) { return s_disp; }
lv_indev_t   *panel_hw_touch(void)   { return s_st.touch_ok ? s_touch : NULL; }

int panel_hw_brightness(uint8_t pct) {
    if (!s_st.lit) return -1;
    if (pct > 100) pct = 100;
    uint32_t duty = 1023u * pct / 100u;
    ledc_channel_t ch = (ledc_channel_t)CONFIG_BSP_DISPLAY_BRIGHTNESS_LEDC_CH;
    if (ledc_set_duty(LEDC_LOW_SPEED_MODE, ch, duty) != ESP_OK) return -1;
    if (ledc_update_duty(LEDC_LOW_SPEED_MODE, ch) != ESP_OK) return -1;
    return 0;
}
```

`components/panel_ui/panel_lock.h`:

```c
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The display lock, never ignored. A lv_* call is made in exactly two places:
 * inside an lv_timer/event callback on the LVGL task (the adapter's recursive
 * lock is already held there), or after panel_lock() returned true. Ignoring
 * a failed take panics the board (esp_lv_adapter_lock(751)), and
 * bsp_display_lock(0) means "try ONCE", not "forever" -- so 0 is promoted to 1.
 * Nothing blocking is ever called while it is held; the worker and the poller
 * never take it. */
#define PANEL_LOCK_MS 200u

bool panel_lock(uint32_t timeout_ms);   /* false -> the caller makes NO lv_* call */
void panel_unlock(void);

#ifdef __cplusplus
}
#endif
```

`components/panel_ui/panel_lock.c`:

```c
#include "bsp/esp-bsp.h"
#include "panel_lock.h"

bool panel_lock(uint32_t timeout_ms) {
    return bsp_display_lock(timeout_ms ? timeout_ms : 1u);
}

void panel_unlock(void) {
    bsp_display_unlock();
}
```

- [ ] **Step 8: Write the palette, the theme and the diagnostics screen**

`components/panel_ui/pnl_palette.h`:

```c
#pragma once
/* The web UI's dark palette (web/app.css), as 0xRRGGBB for lv_color_hex().
 * Pure: shared by the theme and every screen, so the two faces look like one
 * product (spec Decision 1: "one design language"). */
#define PNL_C_BG            0x121812
#define PNL_C_CARD          0x1C241C
#define PNL_C_TEXT          0xE6EFE6
#define PNL_C_MUTED         0x9FB09F
#define PNL_C_BORDER        0x2C382C
#define PNL_C_ACCENT        0x2F7D32
#define PNL_C_OK            0x2F7D32
#define PNL_C_DEGRADED      0xB8860B
#define PNL_C_OFFLINE       0xC0392B
#define PNL_C_UPDATING      0x2B6FB8
#define PNL_C_EMPTY         0x8A8A8A
#define PNL_C_WARN          0x9A6B00
#define PNL_C_OK_TEXT       0x5FBF62
#define PNL_C_OFFLINE_TEXT  0xEB6F63
#define PNL_C_WARN_TEXT     0xE0AD3D
```

`components/panel_ui/pnl_theme.h`:

```c
#pragma once
#include <stdint.h>
#include "lvgl.h"
#include "ring_proto.h"   /* node_health_t */

#ifdef __cplusplus
extern "C" {
#endif

/* LVGL-side styling from pnl_palette.h. [LVGL] -- call with the display lock
 * held or from the LVGL task. */
void       pnl_theme_init(lv_display_t *disp);   /* lv_theme_default_init(dark, accent PNL_C_ACCENT, Montserrat 20) */
lv_color_t pnl_health_color(node_health_t h);    /* ONLINE ok / DEGRADED / OFFLINE / UPDATING / EMPTY */
void       pnl_theme_card(lv_obj_t *o);          /* the web's card: PNL_C_CARD bg, 1 px PNL_C_BORDER, radius 8, pad 12 */
lv_obj_t  *pnl_label(lv_obj_t *parent, const char *text, const lv_font_t *font, uint32_t hex);  /* one-call label */

#ifdef __cplusplus
}
#endif
```

`components/panel_ui/pnl_theme.c`:

```c
#include "lvgl.h"
#include "pnl_palette.h"
#include "pnl_theme.h"

void pnl_theme_init(lv_display_t *disp) {
    lv_theme_t *th = lv_theme_default_init(disp, lv_color_hex(PNL_C_ACCENT), lv_color_hex(PNL_C_UPDATING),
                                           true, &lv_font_montserrat_20);
    if (th) lv_display_set_theme(disp, th);
    lv_obj_t *scr = lv_display_get_screen_active(disp);
    if (scr) {
        lv_obj_set_style_bg_color(scr, lv_color_hex(PNL_C_BG), 0);
        lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
        lv_obj_set_style_text_color(scr, lv_color_hex(PNL_C_TEXT), 0);
    }
}

lv_color_t pnl_health_color(node_health_t h) {
    switch (h) {
    case NODE_H_ONLINE:   return lv_color_hex(PNL_C_OK);
    case NODE_H_DEGRADED: return lv_color_hex(PNL_C_DEGRADED);
    case NODE_H_OFFLINE:  return lv_color_hex(PNL_C_OFFLINE);
    case NODE_H_UPDATING: return lv_color_hex(PNL_C_UPDATING);
    default:              return lv_color_hex(PNL_C_EMPTY);
    }
}

void pnl_theme_card(lv_obj_t *o) {
    lv_obj_set_style_bg_color(o, lv_color_hex(PNL_C_CARD), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(o, lv_color_hex(PNL_C_BORDER), 0);
    lv_obj_set_style_border_width(o, 1, 0);
    lv_obj_set_style_radius(o, 8, 0);
    lv_obj_set_style_pad_all(o, 12, 0);
}

lv_obj_t *pnl_label(lv_obj_t *parent, const char *text, const lv_font_t *font, uint32_t hex) {
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text ? text : "");
    if (font) lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(hex), 0);
    return l;
}
```

`components/panel_ui/scr_diag.h`:

```c
#pragma once
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The panel diagnostics screen: firmware version, internal heap free/min, five
 * touch targets (the four corners and the centre) and live touch counters.
 * The stage gates' touch test: the target under the finger must light, never
 * the opposite one. [LVGL] */
void scr_diag_build(lv_obj_t *parent);

#ifdef __cplusplus
}
#endif
```

`components/panel_ui/scr_diag.c`:

```c
#include <stdio.h>
#include <stdint.h>
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "lvgl.h"
#include "panel_hw.h"
#include "pnl_palette.h"
#include "pnl_theme.h"
#include "scr_diag.h"

#define TGT_N 5
#define TGT_W 150
#define TGT_H 90
#define TGT_M 12

static const char *const TGT_NAME[TGT_N] = { "top left", "top right", "bottom left", "bottom right", "centre" };
static const lv_align_t  TGT_ALIGN[TGT_N] = { LV_ALIGN_TOP_LEFT, LV_ALIGN_TOP_RIGHT, LV_ALIGN_BOTTOM_LEFT,
                                             LV_ALIGN_BOTTOM_RIGHT, LV_ALIGN_CENTER };

static lv_obj_t   *s_heap, *s_touch;
static lv_obj_t   *s_tgt[TGT_N], *s_tgt_lbl[TGT_N];
static uint16_t    s_hits[TGT_N];
static lv_timer_t *s_tick;

static void tgt_paint(int i) {
    char buf[40];
    snprintf(buf, sizeof buf, "%s\nhit %u", TGT_NAME[i], (unsigned)s_hits[i]);
    lv_label_set_text(s_tgt_lbl[i], buf);
    lv_obj_set_style_bg_color(s_tgt[i], lv_color_hex(s_hits[i] ? PNL_C_OK : PNL_C_CARD), 0);
}

static void tgt_cb(lv_event_t *e) {
    intptr_t i = (intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= TGT_N || !s_tgt[i]) return;
    s_hits[i]++;
    tgt_paint((int)i);
}

static void reset_cb(lv_event_t *e) {
    (void)e;
    for (int i = 0; i < TGT_N; i++) { s_hits[i] = 0; if (s_tgt[i]) tgt_paint(i); }
}

/* 200 ms, on the LVGL task: heap and touch counters. Cheap reads only. */
static void diag_tick(lv_timer_t *t) {
    (void)t;
    if (!s_heap || !s_touch) return;
    char buf[160];
    snprintf(buf, sizeof buf, "Internal heap: free %u KB, min %u KB",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024));
    lv_label_set_text(s_heap, buf);
    panel_hw_status_t st;
    panel_hw_status(&st);
    snprintf(buf, sizeof buf, "Touch %s | reads %u | errors %u (last %d) | points %u | last %u,%u",
             st.touch_ok ? "ok" : "UNAVAILABLE", (unsigned)st.reads, (unsigned)st.read_errs, st.last_err,
             (unsigned)st.points, (unsigned)st.last_x, (unsigned)st.last_y);
    lv_label_set_text(s_touch, buf);
}

void scr_diag_build(lv_obj_t *parent) {
    char buf[96];
    snprintf(buf, sizeof buf, "HillGrow master v%s -- panel diagnostics", esp_app_get_description()->version);
    lv_obj_t *title = pnl_label(parent, buf, &lv_font_montserrat_20, PNL_C_TEXT);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 16);

    s_heap = pnl_label(parent, "", &lv_font_montserrat_20, PNL_C_MUTED);
    lv_obj_align(s_heap, LV_ALIGN_TOP_MID, 0, 48);
    s_touch = pnl_label(parent, "", &lv_font_montserrat_20, PNL_C_MUTED);
    lv_obj_set_width(s_touch, 560);   /* a label's default long mode wraps at its width */
    lv_obj_align(s_touch, LV_ALIGN_TOP_MID, 0, 80);

    for (int i = 0; i < TGT_N; i++) {
        s_tgt[i] = lv_button_create(parent);
        lv_obj_set_size(s_tgt[i], TGT_W, TGT_H);
        lv_obj_align(s_tgt[i], TGT_ALIGN[i], i == 4 ? 0 : (i % 2 ? -TGT_M : TGT_M), i == 4 ? 0 : (i < 2 ? TGT_M : -TGT_M));
        s_tgt_lbl[i] = lv_label_create(s_tgt[i]);
        lv_obj_center(s_tgt_lbl[i]);
        lv_obj_add_event_cb(s_tgt[i], tgt_cb, LV_EVENT_PRESSED, (void *)(intptr_t)i);
        tgt_paint(i);
    }

    lv_obj_t *rst = lv_button_create(parent);
    lv_obj_t *rl = lv_label_create(rst);
    lv_label_set_text(rl, "Reset targets");
    lv_obj_align(rst, LV_ALIGN_BOTTOM_MID, 0, -TGT_M);
    lv_obj_add_event_cb(rst, reset_cb, LV_EVENT_CLICKED, NULL);

    s_tick = lv_timer_create(diag_tick, 200, NULL);
    diag_tick(s_tick);
}
```

(`s_tick` is kept so that Task 12 can delete it when the shell tears this screen down.)

- [ ] **Step 9: The real `panel_start()`**

Replace the whole of `components/panel_ui/panel_ui.c` with:

```c
#include <stdint.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "lvgl.h"
#include "panel_hw.h"
#include "panel_lock.h"
#include "pnl_touch.h"
#include "pnl_theme.h"
#include "scr_diag.h"
#include "panel_ui.h"

static const char *TAG = "panel";
static uint8_t s_lit;

/* Internal RAM is the pool that can run out on the P4 (PSRAM hides it from
 * esp_get_minimum_free_heap_size()); every gate records these two numbers. */
static void log_heap(const char *when) {
    ESP_LOGI(TAG, "internal heap %s: free %u, min %u", when,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
}

int panel_start(void) {
    int64_t t0 = esp_timer_get_time();
    log_heap("before panel");
    if (panel_hw_start(PNL_ORIENT_NORMAL) != 0) {
        ESP_LOGE(TAG, "no display");
        return -1;
    }
    if (!panel_lock(2000)) {
        ESP_LOGE(TAG, "display lock not taken in 2000 ms -- panel left dark");
        return -1;
    }
    pnl_theme_init(panel_hw_display());
    scr_diag_build(lv_screen_active());
    panel_unlock();
    (void)panel_hw_brightness(80);
    s_lit = 1;

    panel_hw_status_t st;
    panel_hw_status(&st);
    ESP_LOGI(TAG, "up in %u ms (lvgl c1/p3, touch %s)",
             (unsigned)((esp_timer_get_time() - t0) / 1000), st.touch_ok ? "ok" : "UNAVAILABLE");
    log_heap("after panel");
    return 0;
}

int panel_services_start(void) {
    return s_lit ? 0 : -1;
}
```

`components/panel_ui/CMakeLists.txt`: the `set(PANEL_SRCS ...)` line becomes:

```cmake
    set(PANEL_SRCS "panel_ui.c" "panel_hw.c" "panel_lock.c" "pnl_touch.c" "pnl_theme.c" "scr_diag.c")
```

- [ ] **Step 10: Run the gates**

Run GATE-HOST. Expected: 39/39 tests pass (+1).
Run GATE-P4. Expected: `Project build complete` with 0 warnings, the target grep matches, then the lock checkout. This proves the `#error` guard stayed quiet, the `_Static_assert` holds against the BSP header, and every identifier confirmed in Step 6 compiles.
Run GATE-ESP32. Expected: master, zone and rescue each print `Project build complete` with 0 warnings, and the lock diff is empty.
Re-run the GATE-P4 build line and then the lock checkout, so that `build_p4` is current for flashing (GATE-ESP32 purged the P4 packages).

- [ ] **Step 11: Bench spot check (the full check is the Stage 0 gate)**

Run FLASH-P4. With the console open (`C:\Python311\python -m serial.tools.miniterm COM28 115200`), press RESET.
Expected:
- the log shows `panel: up in N ms (lvgl c1/p3, touch ok)` and two `internal heap` lines;
- the panel lights with the diagnostics screen;
- tapping each of the five targets with a finger lights that target, never the opposite one;
- the counter line shows reads > 0 and errors 0.

If a tap lights the diagonally opposite target, the adapter's own indev path already applies the rotation. Stop and report it: do not "fix" it with the `esp_lcd_touch` mirrors. Close the console.

- [ ] **Step 12: Commit**

```powershell
git -C C:\Projects\HillGrov add components/panel_ui/panel_hw.h components/panel_ui/panel_hw.c components/panel_ui/panel_lock.h components/panel_ui/panel_lock.c components/panel_ui/pnl_touch.h components/panel_ui/pnl_touch.c components/panel_ui/pnl_palette.h components/panel_ui/pnl_theme.h components/panel_ui/pnl_theme.c components/panel_ui/scr_diag.h components/panel_ui/scr_diag.c components/panel_ui/panel_ui.c components/panel_ui/CMakeLists.txt components/board/board.h docs/pin-mapping.md tests/host/test_pnl_touch.c tests/host/CMakeLists.txt
git -C C:\Projects\HillGrov commit -m "feat(panel_ui): soft-error panel bring-up, lock wrapper, touch diagnostics; fix P4 I2C pin names" -m "Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>"
```

### Task 8: Panel worker and result mailbox; THE RULE proven on glass (Stage 0)

*"The LVGL task must never make a blocking call."* Every write and every command is handed to one worker task. Its result comes back through a mailbox that an `lv_timer` drains on the LVGL task, so the worker never touches the display lock. Here the machinery is proven with a deliberately blocking 3 s job while a spinner keeps turning.

**Files:**
- Create: `components/panel_ui/pnl_worker.h`, `.c`.
- Modify:
  - `components/panel_ui/scr_diag.c`: add a spinner and a "Blocking job (3 s)" button. Its job runs `vTaskDelay(pdMS_TO_TICKS(3000))`, and `done` shows the elapsed ms plus the UI ticks counted by a 16 ms `lv_timer`.
  - `components/panel_ui/panel_ui.c`: `panel_services_start()` → `pnl_worker_start()`.
  - `components/panel_ui/CMakeLists.txt`.

**Interfaces:**
- Consumes: `panel_lock` (Task 7) and `psvc_rc_t` (Task 3).
- Produces (`pnl_worker.h`):
  ```c
  typedef struct pnl_job pnl_job_t;
  typedef void (*pnl_job_run_fn)(pnl_job_t *j);    /* runs on pnl_work: may block; never calls lv_* */
  typedef void (*pnl_job_done_fn)(pnl_job_t *j);   /* runs on the LVGL task (the drain timer); may call lv_* */
  #define PNL_JOB_ARG_MAX 160   /* by value: ssid 33 + pass 65, TZ 48, a password 64, a SET TIME line; bigger payloads
                                   (console line + reply, edit sets, doc buffers) go by pointer to module-owned storage */
  #define PNL_JOB_OUT_MAX 256
  #define PNL_JOB_POOL    8     /* the pool memsets every job's arg/out after done() returns -- secrets never linger */
  struct pnl_job {
      pnl_job_run_fn  run;
      pnl_job_done_fn done;
      uint32_t        screen_gen;          /* pnl_screen_gen() at submit; done() must not touch widgets if it changed */
      uint32_t        t_submit_ms, t_done_ms;
      psvc_rc_t       rc;                  /* psvc_* results */
      int             irc;                 /* int-returning results (pnl_cmd_run, pnl_sd_mount) */
      char            err[96];             /* path / detail */
      uint8_t         arg[PNL_JOB_ARG_MAX];/* copied at submit */
      uint8_t         out[PNL_JOB_OUT_MAX];/* run() -> done() */
  };
  void     pnl_worker_start(void);    /* task pnl_work c0/p2/8192 internal, NOT TWDT-subscribed; pool + 2 queues; 20 ms drain timer */
  int      pnl_worker_submit(pnl_job_run_fn run, pnl_job_done_fn done, const void *arg, size_t arg_len);
           /* [LVGL] 0 queued / -1 pool full or arg_len > PNL_JOB_ARG_MAX. Any buffer referenced by pointer inside arg belongs
              to the worker from submit until done() returns -- the submitter must not touch it meanwhile. */
  int      pnl_worker_pending(void);  /* [ANY] jobs queued or running */
  uint32_t pnl_worker_stack_free(void); /* [ANY] pnl_work's stack high-water mark in bytes (uxTaskGetStackHighWaterMark;
                                           IDF counts bytes), 0 before the task exists -- shown on Panel > About */
  uint32_t pnl_screen_gen(void);      /* [ANY] */
  void     pnl_screen_gen_bump(void); /* [LVGL] called by the shell on every teardown */
  int      pnl_on_lvgl_task(void);    /* [ANY] 1 when pcTaskGetName(NULL) is "lvgl" -- [WORKER] panel functions log ERROR and
                                         refuse (-1) when this is 1 (tripwire for THE RULE) */
  ```

**What is provable where.** This is FreeRTOS and LVGL glue with no pure logic to host-test (+0). GATE-P4 proves it compiles. The bench proves THE RULE: during a 3 s blocking job the spinner keeps turning and the UI tick timer runs about 187 times (3000 / 16), and the gate requires at least 150.

Design notes that bind later tasks:
- The worker is **not** TWDT-subscribed. Its jobs make esp_hosted RPCs that can exceed the 8 s TWDT (recovery design §6.1: no subscription is held across an RPC). The one exception is Task 30's `psvc_fw_install()`, which subscribes itself for its bounded flash loop only.
- Jobs run **one at a time**, in submit order. A slow Wi-Fi scan (up to 30 s on the P4) therefore delays the next save, and the UI shows it as pending.
- `done()` runs on the LVGL task. It must be as short as any other callback (≤ 200 ms) and must check `j->screen_gen == pnl_screen_gen()` before touching any widget: the screen that submitted the job may have been torn down meanwhile. The one allowed alternative: a module whose teardown sets every widget pointer to NULL may skip the check if its `done()` first stores the outcome in module state and then draws only through NULL-safe helpers, and its build redraws that state. The System sections use it (Tasks 23 and 32), so a result that lands after the operator left by the rail and came back reaches the rebuilt widgets instead of leaving a pending state on screen.
- The pool clears `arg`, `out` and `err` after every `done()`, so a password passed by value never outlives its job.

- [ ] **Step 1: Write the worker**

`components/panel_ui/pnl_worker.h`:

```c
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "psvc_rc.h"

#ifdef __cplusplus
extern "C" {
#endif

/* THE RULE (spec "The rule"): the LVGL task never makes a blocking call. Every
 * write and every command is a job: run() executes on the pnl_work task (may
 * block: mutexes, NVS, esp_hosted RPCs, ring round trips; never calls lv_*),
 * then done() executes on the LVGL task from a 20 ms drain timer (may call
 * lv_*; must be short). The worker never takes the display lock -- results
 * come back through a queue, the "mailbox".
 *
 * Thread tags used across panel_ui: [LVGL] only on the LVGL task (or under
 * panel_lock()); [WORKER] only on pnl_work / pnl_poll / pnl_wifi; [ANY] safe
 * everywhere. */

typedef struct pnl_job pnl_job_t;
typedef void (*pnl_job_run_fn)(pnl_job_t *j);    /* runs on pnl_work: may block; never calls lv_* */
typedef void (*pnl_job_done_fn)(pnl_job_t *j);   /* runs on the LVGL task (the drain timer); may call lv_* */

#define PNL_JOB_ARG_MAX 160   /* by value: ssid 33 + pass 65, TZ 48, a password 64, a SET TIME line; bigger payloads
                                 (console line + reply, edit sets, doc buffers) go by pointer to module-owned storage */
#define PNL_JOB_OUT_MAX 256
#define PNL_JOB_POOL    8     /* the pool memsets every job's arg/out after done() returns -- secrets never linger */

struct pnl_job {
    pnl_job_run_fn  run;
    pnl_job_done_fn done;
    uint32_t        screen_gen;          /* pnl_screen_gen() at submit; done() must not touch widgets if it changed */
    uint32_t        t_submit_ms, t_done_ms;
    psvc_rc_t       rc;                  /* psvc_* results */
    int             irc;                 /* int-returning results (pnl_cmd_run, pnl_sd_mount) */
    char            err[96];             /* path / detail */
    uint8_t         arg[PNL_JOB_ARG_MAX];/* copied at submit */
    uint8_t         out[PNL_JOB_OUT_MAX];/* run() -> done() */
};

/* Task pnl_work (core 0, prio 2, 8192 B internal stack, NOT TWDT-subscribed),
 * the job pool, the request and done queues, and the 20 ms drain lv_timer
 * (created under panel_lock(2000)). Idempotent. Call from
 * panel_services_start(). */
void     pnl_worker_start(void);

/* [LVGL] 0 queued / -1 pool full, arg_len > PNL_JOB_ARG_MAX, run NULL or the
 * worker not started. arg is copied by value. Any buffer referenced BY POINTER
 * inside arg belongs to the worker from submit until done() returns -- the
 * submitter must not touch it meanwhile. */
int      pnl_worker_submit(pnl_job_run_fn run, pnl_job_done_fn done, const void *arg, size_t arg_len);

int      pnl_worker_pending(void);  /* [ANY] jobs queued or running (or run, awaiting done()) */

/* [ANY] Smallest free stack pnl_work has had, in bytes (ESP-IDF's
 * uxTaskGetStackHighWaterMark counts bytes); 0 before the task exists. The
 * stack is chosen, not measured: the Stage 3 and 4 gates record this. */
uint32_t pnl_worker_stack_free(void);

uint32_t pnl_screen_gen(void);      /* [ANY] */
void     pnl_screen_gen_bump(void); /* [LVGL] called by the shell on every teardown */

/* [ANY] 1 when the calling task is the LVGL adapter's ("lvgl"). Every [WORKER]
 * function in panel_ui checks it first, logs ERROR and refuses (-1) when it is
 * 1: the tripwire for THE RULE. */
int      pnl_on_lvgl_task(void);

#ifdef __cplusplus
}
#endif
```

`components/panel_ui/pnl_worker.c`:

```c
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "panel_lock.h"
#include "pnl_worker.h"

static const char *TAG = "pnl_work";

static pnl_job_t         s_pool[PNL_JOB_POOL];   /* internal RAM: jobs carry secrets and run() stacks point into them */
static uint8_t           s_used[PNL_JOB_POOL];
static QueueHandle_t     s_req, s_done;
static portMUX_TYPE      s_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t s_gen;
static volatile int      s_pending;
static lv_timer_t       *s_drain;
static TaskHandle_t      s_work_task;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static void worker_task(void *arg) {
    (void)arg;
    for (;;) {
        pnl_job_t *j = NULL;
        if (xQueueReceive(s_req, &j, portMAX_DELAY) != pdTRUE || !j) continue;
        if (j->run) j->run(j);
        j->t_done_ms = now_ms();
        (void)xQueueSend(s_done, &j, portMAX_DELAY);   /* depth == pool size: never actually waits */
    }
}

static void job_release(pnl_job_t *j) {
    memset(j->arg, 0, sizeof j->arg);   /* secrets never linger past their job */
    memset(j->out, 0, sizeof j->out);
    memset(j->err, 0, sizeof j->err);
    int i = (int)(j - s_pool);
    portENTER_CRITICAL(&s_mux);
    s_used[i] = 0;
    s_pending--;
    portEXIT_CRITICAL(&s_mux);
}

/* 20 ms, on the LVGL task: the mailbox. */
static void drain_cb(lv_timer_t *t) {
    (void)t;
    pnl_job_t *j = NULL;
    while (xQueueReceive(s_done, &j, 0) == pdTRUE) {
        if (!j) continue;
        if (j->done) j->done(j);
        job_release(j);
    }
}

void pnl_worker_start(void) {
    if (s_req) return;
    s_req  = xQueueCreate(PNL_JOB_POOL, sizeof(pnl_job_t *));
    s_done = xQueueCreate(PNL_JOB_POOL, sizeof(pnl_job_t *));
    if (!s_req || !s_done) { ESP_LOGE(TAG, "job queues unavailable -- the panel cannot write or run commands"); return; }
    if (xTaskCreatePinnedToCore(worker_task, "pnl_work", 8192, NULL, 2, &s_work_task, 0) != pdPASS) {
        s_work_task = NULL;
        ESP_LOGE(TAG, "pnl_work task not created -- the panel cannot write or run commands");
        return;
    }
    if (!panel_lock(2000)) {
        ESP_LOGE(TAG, "display lock not taken -- job results will not be delivered");
        return;
    }
    s_drain = lv_timer_create(drain_cb, 20, NULL);
    panel_unlock();
    ESP_LOGI(TAG, "worker up (c0/p2, %d-job pool, not TWDT-subscribed)", PNL_JOB_POOL);
}

int pnl_worker_submit(pnl_job_run_fn run, pnl_job_done_fn done, const void *arg, size_t arg_len) {
    if (!s_req || !s_drain || !run || arg_len > PNL_JOB_ARG_MAX || (arg_len && !arg)) return -1;
    int slot = -1;
    portENTER_CRITICAL(&s_mux);
    for (int i = 0; i < PNL_JOB_POOL; i++) {
        if (!s_used[i]) { s_used[i] = 1; slot = i; s_pending++; break; }
    }
    portEXIT_CRITICAL(&s_mux);
    if (slot < 0) return -1;

    pnl_job_t *j = &s_pool[slot];
    memset(j, 0, sizeof *j);
    j->run = run;
    j->done = done;
    j->screen_gen = s_gen;
    j->t_submit_ms = now_ms();
    j->rc = PSVC_OK;
    if (arg_len) memcpy(j->arg, arg, arg_len);
    if (xQueueSend(s_req, &j, 0) != pdTRUE) { job_release(j); return -1; }
    return 0;
}

int      pnl_worker_pending(void)  { return s_pending; }
uint32_t pnl_worker_stack_free(void) { return s_work_task ? (uint32_t)uxTaskGetStackHighWaterMark(s_work_task) : 0; }
uint32_t pnl_screen_gen(void)      { return s_gen; }
void     pnl_screen_gen_bump(void) { s_gen++; }

int pnl_on_lvgl_task(void) {
    const char *n = pcTaskGetName(NULL);
    return n && strcmp(n, "lvgl") == 0;
}
```

- [ ] **Step 2: Prove THE RULE on the diagnostics screen**

In `components/panel_ui/scr_diag.c`:
- add to the includes: `#include "freertos/FreeRTOS.h"`, `#include "freertos/task.h"`, `#include "esp_log.h"` and `#include "pnl_worker.h"`;
- after `static lv_timer_t *s_tick;` add:

```c
static const char *TAG = "scr_diag";
static lv_obj_t   *s_job_lbl, *s_job_btn;
static lv_timer_t *s_ui_tick;               /* 16 ms: counts LVGL cycles while a job runs */
static uint32_t    s_ticks, s_ticks_at_submit;
static uint8_t     s_job_busy;

static void ui_tick(lv_timer_t *t) { (void)t; s_ticks++; }

/* [WORKER] deliberately blocks for 3 s -- if the UI keeps ticking meanwhile,
 * the worker/mailbox split holds. */
static void job_block_run(pnl_job_t *j) {
    if (pnl_on_lvgl_task()) {
        ESP_LOGE(TAG, "blocking job ran on the LVGL task -- THE RULE is broken");
        j->irc = -1;
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(3000));
    j->irc = 0;
}

/* [LVGL] via the drain timer. */
static void job_block_done(pnl_job_t *j) {
    s_job_busy = 0;
    if (j->screen_gen != pnl_screen_gen() || !s_job_lbl) return;   /* this screen was torn down meanwhile */
    char buf[96];
    if (j->irc != 0) snprintf(buf, sizeof buf, "FAILED: the job ran on the LVGL task");
    else snprintf(buf, sizeof buf, "done after %u ms, %u UI ticks during the job",
                  (unsigned)(j->t_done_ms - j->t_submit_ms), (unsigned)(s_ticks - s_ticks_at_submit));
    lv_label_set_text(s_job_lbl, buf);
}

static void job_btn_cb(lv_event_t *e) {
    (void)e;
    if (s_job_busy) return;
    s_ticks_at_submit = s_ticks;
    if (pnl_worker_submit(job_block_run, job_block_done, NULL, 0) != 0) {
        lv_label_set_text(s_job_lbl, "worker unavailable (not started or pool full)");
        return;
    }
    s_job_busy = 1;
    lv_label_set_text(s_job_lbl, "running... (the spinner must keep turning)");
}
```

- in `scr_diag_build()`, before `s_tick = lv_timer_create(diag_tick, 200, NULL);` add:

```c
    lv_obj_t *spin = lv_spinner_create(parent);
    lv_obj_set_size(spin, 72, 72);
    lv_obj_align(spin, LV_ALIGN_LEFT_MID, 190, 0);

    s_job_btn = lv_button_create(parent);
    lv_obj_t *jl = lv_label_create(s_job_btn);
    lv_label_set_text(jl, "Blocking job (3 s)");
    lv_obj_align(s_job_btn, LV_ALIGN_RIGHT_MID, -170, -30);
    lv_obj_add_event_cb(s_job_btn, job_btn_cb, LV_EVENT_CLICKED, NULL);

    s_job_lbl = pnl_label(parent, "tap to prove the LVGL task never blocks", &lv_font_montserrat_20, PNL_C_MUTED);
    lv_obj_set_width(s_job_lbl, 300);
    lv_obj_align(s_job_lbl, LV_ALIGN_RIGHT_MID, -120, 40);

    s_ui_tick = lv_timer_create(ui_tick, 16, NULL);
```

In `components/panel_ui/panel_ui.c`, add `#include "pnl_worker.h"` and replace `panel_services_start()` with:

```c
int panel_services_start(void) {
    if (!s_lit) return -1;   /* a dark panel runs no worker: nothing could ever submit to it */
    pnl_worker_start();
    return 0;
}
```

`components/panel_ui/CMakeLists.txt`: the `set(PANEL_SRCS ...)` line becomes:

```cmake
    set(PANEL_SRCS "panel_ui.c" "panel_hw.c" "panel_lock.c" "pnl_touch.c" "pnl_theme.c" "scr_diag.c" "pnl_worker.c")
```

- [ ] **Step 3: Run the gates**

Run GATE-HOST. Expected: 39/39 tests pass (+0).
Run GATE-P4. Expected: `Project build complete` with 0 warnings, the target grep matches, then the lock checkout.
Run GATE-ESP32. Expected: master, zone and rescue each print `Project build complete` with 0 warnings, and the lock diff is empty. Then re-run the GATE-P4 build line and the lock checkout, for flashing.

- [ ] **Step 4: Flash and prove THE RULE on glass**

Run FLASH-P4, then watch the boot with the console open.
Expected:
- the log shows `pnl_work: worker up (c0/p2, 8-job pool, not TWDT-subscribed)` after `node_mgr` starts;
- on glass, tapping "Blocking job (3 s)" keeps the spinner turning without a hitch;
- after about 3 s the label reads `done after ~3000 ms, N UI ticks during the job`, with N ≥ 150.

This is also Stage 0 gate step 5.

- [ ] **Step 5: Commit**

```powershell
git -C C:\Projects\HillGrov add components/panel_ui/pnl_worker.h components/panel_ui/pnl_worker.c components/panel_ui/scr_diag.c components/panel_ui/panel_ui.c components/panel_ui/CMakeLists.txt
git -C C:\Projects\HillGrov commit -m "feat(panel_ui): worker task and result mailbox -- the LVGL task never blocks" -m "Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>"
```

### Stage 0 bench gate (owner, about 40 min)

Setup: the P4 master on COM28 and both zones on the ring. The PC is joined to the master's AP `HillGrow` (`netsh wlan show interfaces` shows it). Use the bench's current web password in place of `hillgrow1` below if it differs. One process at a time holds COM28: close the console before running `uart_test.py`.

1. **The net describes today's firmware.** Run this before flashing anything from this stage (the master is still on its pre-Stage-0 image):
   `python C:\Projects\HillGrov\tools\web_test.py 192.168.7.7 --password hillgrow1 --only mcfg`
   - Pass: every MCFG line is PASS and the summary says `N/N passed`.
   - Fail: any FAIL. Stop: either the suite is wrong or today's firmware differs from its description, and Tasks 3-5 must not be judged against it.
   - Phones on the AP are logged out; that is expected.
2. **Flash.** In the console (`C:\Python311\python -m serial.tools.miniterm COM28 115200`), `GET VERSION` must show the running slot `VALID`. Quit the console (Ctrl+]). Then:
   ```powershell
   python C:\Projects\HillGrov\tools\flash_app.py --app master --target esp32p4 --build-dir C:\Projects\HillGrov\master\build_p4 --port COM28 --dry-run
   python C:\Projects\HillGrov\tools\flash_app.py --app master --target esp32p4 --build-dir C:\Projects\HillGrov\master\build_p4 --port COM28
   ```
   - Pass: every path the dry run prints contains `build_p4`, and the real run ends with the tool's success line.
3. **Boot.** Open the console and press RESET.
   - Pass: the panel lights on the diagnostics screen, showing the version and internal heap free/min. The log shows, in order:
     - `panel: internal heap before panel: ...`;
     - `panel_hw: LVGL pool: 64 KB internal + 256 KB PSRAM overflow` (a `W` line saying the overflow pool is unavailable is a Fail);
     - `panel: up in N ms (lvgl c1/p3, touch ok)`;
     - `panel: internal heap after panel: ...`;
     - later, after node_mgr starts, `pnl_work: worker up ...`.

     There are no repeated `ESP32_P4_EV` INFO lines, and boot reaches the CLI prompt.
   - Benign: one `W ledc: GPIO 32 is not usable, maybe conflict with others`. Do not chase it.
   - Fail: a dark panel, `touch UNAVAILABLE`, an abort/backtrace, or a reboot loop.
4. **Touch, with a finger.** Tap the four corner targets and the centre target, one at a time.
   - Pass: the target under the finger turns green and counts `hit 1`. The opposite one never lights. The counter line shows `reads` > 0 and `errors 0`.
   - Fail: a mirrored target lights, a tap does nothing, or errors > 0.
5. **THE RULE.** Tap "Blocking job (3 s)".
   - Pass: the spinner keeps turning smoothly for the whole wait, then the label reads `done after ~3000 ms, N UI ticks during the job` with N ≥ 150.
   - Fail: the spinner stalls, N < 150, or `FAILED: the job ran on the LVGL task`.
6. **Regression net, on the new image.** Close the console.
   - Run `python C:\Projects\HillGrov\tools\web_test.py 192.168.7.7 --password hillgrow1`. This is all the standard suites, MCFG and LOGIN included; LOGIN deliberately locks the web out for about 60 s at the end. Pass: `N/N passed`.
   - Run `C:\Python311\python C:\Projects\HillGrov\tools\uart_test.py COM28 --role MASTER`. Pass: all PASS.
   - Reopen the console and type `GET TZ`, then `SET TZ <the value GET TZ printed>`. Pass: the answer is `OK TZ <that value>`.
7. **Budgets, written into the Stage 0 report.**
   - Internal heap free/min before and after `panel_start`, from the two log lines. Pass: min after ≥ 64 KB (D25).
   - Image size against the 4 MB slot: `(Get-Item C:\Projects\HillGrov\master\build_p4\hillgrow_master.bin).Length` against 4194304.
   - The `panel: up in N ms` figure.

The stage passes only if every step passes. Record each step's evidence (log excerpts, web_test summaries, heap numbers) in the Stage 0 report.

---

## Stage 1 — Shell and read-only views: home screen, left-rail app, Dashboard, Zone, Alarms

### Task 9: Extract the state gather into `panel_svc`; publish upload progress there; register the web's quarantine count (Stage 1)

`state_snap` is only a JSON writer. The work of *filling* a snapshot lives inline in the HTTP handler `h_state` (`http_api.c:61-156`), and it reads two `http_srv`-internal getters. The panel needs the same data as a struct, so the gather moves into `panel_svc` and becomes an owning struct. `/api/state` becomes `psvc_state_fill()` → `psvc_state_to_snap()` → `state_snap_write()`, with the same bytes on the wire.

**Files:**
- Create: `components/panel_svc/psvc_state.h`, `psvc_state.c` (G), `psvc_state_conv.c` (P), `psvc_fw.h`, `psvc_fw.c` (P, progress only); `tests/host/test_psvc_state.c`.
- Modify:
  - `components/http_srv/http_api.c`: `h_state` = `static psvc_state_t s;` → `psvc_state_fill(&s, PSVC_FILL_ALL)` → `psvc_state_to_snap(&s, &m)` → `state_snap_write(&m, s.node, HG_MAX_ZONES, &s.ring, s.cfg_sync_failed, s.now_ms, chunk_writer, req)`. The bytes on the wire are unchanged.
  - `components/http_srv/http_upload.c`: the static `progress()` becomes `psvc_fw_progress_set()`; delete `http_upload_progress()` (`:76-91`).
  - `components/http_srv/http_upload.h`: drop `http_upload_progress` and its comment (`:29-42`), and point at `psvc_fw.h`.
  - `components/http_srv/http_srv.c`: `http_srv_start()` calls `psvc_state_set_web_quarantine_fn(http_cmd_quarantined)`.
  - `components/panel_svc/CMakeLists.txt`: add the new sources. `REQUIRES` gains `ring_proto wifi_mgr state_snap` (`wifi_mgr` is already there from Task 4). `PRIV_REQUIRES` gains `node_mgr alarm_mgr app_common esp_timer esp_app_format heap`.
  - `tests/host/CMakeLists.txt`: `hg_test(test_psvc_state ...)` plus `cjson_host`.

**Interfaces:**
- Consumes: exactly what `h_state` reads today (`http_api.c:61-145`):
  - `esp_app_get_description`, `hg_app_uptime_s`, `esp_get_minimum_free_heap_size`, `hg_app_time_get_noted`, `hg_app_time_is_set`;
  - `time_svc_utc_offset`, `wifi_mgr_status`, `hg_app_fw_info`, `node_mgr_fw_status`, `alarm_mgr_active_count` and `alarm_mgr_total`;
  - `mcfg_get()->flags`, `node_mgr_get` ×8, `node_mgr_cfg_sync_failed` ×8, `node_mgr_ring_status`;
  - `esp_timer_get_time`;
  - plus `heap_caps_get_free_size` and `heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)`.
- Produces:
  ```c
  /* psvc_state.h */
  typedef struct {
      char          version[32];
      uint32_t      uptime_s, heap_min_kb;              /* heap_min_kb = /api/state's figure (includes PSRAM on the P4) */
      uint32_t      heap_int_free_kb, heap_int_min_kb;  /* MALLOC_CAP_INTERNAL -- panel About only, not in /api/state */
      char          time[20];                           /* UTC "YYYY-MM-DD HH:MM:SS" (first 19 chars of hg_app_time_get_noted) */
      char          time_src[8];                        /* "NTP" | "SET" | "NONE" */
      uint8_t       time_is_set;                        /* hg_app_time_is_set() */
      int32_t       utc_offset_s;                       /* time_svc_utc_offset() */
      wifi_status_t wifi;                               /* untouched when PSVC_FILL_SKIP_WIFI */
      char          fw_slot[16], fw_state[16], fw_other[16];   /* hg_app_fw_info tokens; fw_state "PENDING" = OTA trial */
      char          upload_kind[8];                     /* "" | "master" | "zone" (psvc_fw_progress) */
      uint8_t       upload_pct;
      char          fleet_line[40];                     /* "IDLE" | "<z> PRECHECK|UPDATING|WAIT_HB" */
      int           alarms_active, alarms_total;
      uint8_t       web_default, ap_default;            /* MCFG_F_WEB_DEFAULT / MCFG_F_AP_DEFAULT */
      uint8_t       web_cmd_quarantined;                /* the registered web hook; 0 when the web never started */
      hg_node_t     node[HG_MAX_ZONES];                 /* slot = id-1; memset first; used == 0 => not enrolled */
      uint8_t       cfg_sync_failed[HG_MAX_ZONES];
      ring_status_t ring;
      uint32_t      now_ms;                             /* esp_timer ms (node_mgr's clock) */
  } psvc_state_t;
  #define PSVC_FILL_ALL       0u
  #define PSVC_FILL_SKIP_WIFI 1u
  void psvc_state_fill(psvc_state_t *out, uint32_t flags);            /* [WORKER] */
  void psvc_state_to_snap(const psvc_state_t *s, snap_master_t *m);   /* pure: m's const char* point into s (s outlives m's use) */
  void psvc_state_set_web_quarantine_fn(uint8_t (*fn)(void));          /* [ANY] http_srv_start() registers http_cmd_quarantined */
  void psvc_parse_time_noted(const char *noted, char time_out[20], char src_out[8]);          /* pure, h_state's parse */
  void psvc_parse_fw_info(const char *info, char slot[16], char state[16], char other[16]);   /* pure, h_state's sscanf */

  /* psvc_fw.h (this task's part; Task 30 extends the file) */
  void psvc_fw_progress_set(const char *kind, uint32_t pct);   /* kind: string literal "" | "master" | "zone"; pct stored first */
  int  psvc_fw_progress(const char **kind, uint8_t *pct);      /* [ANY] lock-free; 1 while an install streams; outs always written */
  ```

The pure half is host-tested:
- the two parsers against the real string shapes, including short and garbage input;
- `to_snap` followed by `state_snap_write`, which must produce **byte-identical JSON** to a hand-built `snap_master_t` holding the same values;
- progress publication and its clamp.

`psvc_state.c` is glue, proven by the builds and by `web_test --only state` at the Stage 1 gate (and by the full run's STATE, UPLOADS and fleet checks).

- [ ] **Step 1: Write the failing test**

`tests/host/test_psvc_state.c`:

```c
/* The panel and /api/state now share ONE gather (psvc_state_fill) and ONE
   conversion to the web's JSON block (psvc_state_to_snap). The conversion is
   pure and is pinned here to produce EXACTLY the bytes a hand-built
   snap_master_t does -- that is the web's "same bytes on the wire" guarantee
   for this extraction. The two text parsers are h_state's own, moved. */
#include <stdio.h>
#include <string.h>
#include "unity.h"
#include "cJSON.h"
#include "state_snap.h"
#include "psvc_state.h"
#include "psvc_fw.h"

void setUp(void) { psvc_fw_progress_set("", 0); }
void tearDown(void) {}

typedef struct { char buf[6144]; size_t len; } sink_t;
static int sink(void *ctx, const char *b, size_t n) {
    sink_t *s = (sink_t *)ctx;
    if (s->len + n >= sizeof s->buf) return -1;
    memcpy(s->buf + s->len, b, n);
    s->len += n;
    s->buf[s->len] = '\0';
    return 0;
}

static void test_parse_time_noted_real_shape(void) {
    char t[20], src[8];
    psvc_parse_time_noted("2026-09-23 10:00:00 NTP 12", t, src);
    TEST_ASSERT_EQUAL_STRING("2026-09-23 10:00:00", t);
    TEST_ASSERT_EQUAL_STRING("NTP", src);
    psvc_parse_time_noted("1970-01-01 00:03:28 NONE 0", t, src);
    TEST_ASSERT_EQUAL_STRING("NONE", src);
}

static void test_parse_time_noted_short_and_garbage(void) {
    char t[20], src[8];
    psvc_parse_time_noted("1970-01", t, src);
    TEST_ASSERT_EQUAL_STRING("1970-01", t);
    TEST_ASSERT_EQUAL_STRING("", src);
    psvc_parse_time_noted("", t, src);
    TEST_ASSERT_EQUAL_STRING("", t);
    TEST_ASSERT_EQUAL_STRING("", src);
    psvc_parse_time_noted(NULL, t, src);
    TEST_ASSERT_EQUAL_STRING("", t);
    psvc_parse_time_noted("2026-09-23 10:00:00 AVERYLONGSOURCE 1", t, src);
    TEST_ASSERT_EQUAL_STRING("AVERYLO", src);   /* %7s: bounded, never overflows src[8] */
}

static void test_parse_fw_info(void) {
    char slot[16], state[16], other[16];
    psvc_parse_fw_info("0.4.0 ota_0 PENDING NONE", slot, state, other);
    TEST_ASSERT_EQUAL_STRING("ota_0", slot);
    TEST_ASSERT_EQUAL_STRING("PENDING", state);
    TEST_ASSERT_EQUAL_STRING("NONE", other);
    psvc_parse_fw_info("0.4.0 ota_1", slot, state, other);
    TEST_ASSERT_EQUAL_STRING("ota_1", slot);
    TEST_ASSERT_EQUAL_STRING("", state);
    psvc_parse_fw_info("garbage", slot, state, other);
    TEST_ASSERT_EQUAL_STRING("", slot);
    psvc_parse_fw_info(NULL, slot, state, other);
    TEST_ASSERT_EQUAL_STRING("", slot);
}

static void fill_node(hg_node_t *n, uint8_t id) {
    memset(n, 0, sizeof *n);
    n->used = 1;
    n->id = id;
    snprintf(n->name, sizeof n->name, "Z%u\"q", (unsigned)id);   /* a quote: the escaper must run the same both ways */
    n->health = NODE_H_ONLINE;
    n->last_hb_ms = 1000;
    n->hb.fw_min = 1;
    n->hb.n_shelves = 1;
    n->hb.shelf[0].pct_a = 41;
}

static void test_to_snap_is_byte_identical_to_a_hand_built_block(void) {
    static psvc_state_t s;
    memset(&s, 0, sizeof s);
    snprintf(s.version, sizeof s.version, "0.5.0");
    s.uptime_s = 1234; s.heap_min_kb = 33000;
    snprintf(s.time, sizeof s.time, "2026-09-23 10:00:00");
    snprintf(s.time_src, sizeof s.time_src, "NTP");
    s.wifi.sta_up = 1;
    snprintf(s.wifi.sta_ip, sizeof s.wifi.sta_ip, "192.168.1.5");
    snprintf(s.wifi.sta_ssid, sizeof s.wifi.sta_ssid, "house");
    s.wifi.rssi = -61;
    s.wifi.sta_reason[0] = '\0';
    snprintf(s.wifi.ap_ssid, sizeof s.wifi.ap_ssid, "HillGrow");
    s.wifi.ap_clients = 2;
    snprintf(s.wifi.ap_ip, sizeof s.wifi.ap_ip, "192.168.7.7");
    snprintf(s.fw_slot, sizeof s.fw_slot, "ota_1");
    snprintf(s.fw_state, sizeof s.fw_state, "PENDING");
    snprintf(s.fw_other, sizeof s.fw_other, "NONE");
    snprintf(s.upload_kind, sizeof s.upload_kind, "zone");
    s.upload_pct = 40;
    snprintf(s.fleet_line, sizeof s.fleet_line, "2 UPDATING");
    s.alarms_active = 1; s.alarms_total = 9;
    s.web_default = 1; s.ap_default = 0;
    s.web_cmd_quarantined = 2;
    fill_node(&s.node[1], 2);
    s.cfg_sync_failed[1] = 1;
    s.ring.state = RING_ST_OK; s.ring.size = 2; s.ring.online_mask = 6;
    s.now_ms = 5000;

    snap_master_t conv;
    psvc_state_to_snap(&s, &conv);
    static sink_t a;
    memset(&a, 0, sizeof a);
    TEST_ASSERT_EQUAL_INT(0, state_snap_write(&conv, s.node, HG_MAX_ZONES, &s.ring, s.cfg_sync_failed, s.now_ms, sink, &a));

    snap_master_t m;
    memset(&m, 0, sizeof m);
    m.version = "0.5.0"; m.uptime_s = 1234; m.heap_min_kb = 33000;
    snprintf(m.time, sizeof m.time, "2026-09-23 10:00:00");
    m.time_src = "NTP";
    m.sta.up = 1;
    snprintf(m.sta.ip, sizeof m.sta.ip, "192.168.1.5");
    snprintf(m.sta.ssid, sizeof m.sta.ssid, "house");
    m.sta.rssi = -61;
    snprintf(m.ap.ssid, sizeof m.ap.ssid, "HillGrow");
    m.ap.clients = 2;
    snprintf(m.ap.ip, sizeof m.ap.ip, "192.168.7.7");
    m.fw.slot = "ota_1"; m.fw.state = "PENDING"; m.fw.other = "NONE";
    m.fw.upload_kind = "zone"; m.fw.upload_pct = 40;
    m.fleet_line = "2 UPDATING";
    m.alarms_active = 1; m.alarms_total = 9;
    m.web_default = 1; m.ap_default = 0;
    m.cmd_quarantined = 2;
    static sink_t b;
    memset(&b, 0, sizeof b);
    TEST_ASSERT_EQUAL_INT(0, state_snap_write(&m, s.node, HG_MAX_ZONES, &s.ring, s.cfg_sync_failed, s.now_ms, sink, &b));

    TEST_ASSERT_EQUAL_STRING(b.buf, a.buf);

    cJSON *root = cJSON_Parse(a.buf);
    TEST_ASSERT_NOT_NULL(root);
    cJSON *master = cJSON_GetObjectItem(root, "master");
    TEST_ASSERT_EQUAL_INT(2, cJSON_GetObjectItem(cJSON_GetObjectItem(master, "http"), "cmd_quarantined")->valueint);
    TEST_ASSERT_EQUAL_STRING("zone", cJSON_GetObjectItem(cJSON_GetObjectItem(master, "fw"), "upload_kind")->valuestring);
    TEST_ASSERT_EQUAL_STRING("PENDING", cJSON_GetObjectItem(cJSON_GetObjectItem(master, "fw"), "state")->valuestring);
    TEST_ASSERT_EQUAL_INT(1, cJSON_GetArraySize(cJSON_GetObjectItem(root, "nodes")));
    cJSON_Delete(root);
}

static void test_progress_publication_and_clamp(void) {
    const char *k = "x";
    uint8_t p = 99;
    TEST_ASSERT_EQUAL_INT(0, psvc_fw_progress(&k, &p));
    TEST_ASSERT_EQUAL_STRING("", k);
    TEST_ASSERT_EQUAL_UINT8(0, p);
    psvc_fw_progress_set("zone", 150);
    TEST_ASSERT_EQUAL_INT(1, psvc_fw_progress(&k, &p));
    TEST_ASSERT_EQUAL_STRING("zone", k);
    TEST_ASSERT_EQUAL_UINT8(100, p);
    psvc_fw_progress_set(NULL, 0);
    TEST_ASSERT_EQUAL_INT(0, psvc_fw_progress(&k, &p));
    TEST_ASSERT_EQUAL_STRING("", k);
    TEST_ASSERT_EQUAL_INT(0, psvc_fw_progress(NULL, NULL));   /* NULL outs are allowed */
}

int main(void) { UNITY_BEGIN();
    RUN_TEST(test_parse_time_noted_real_shape);
    RUN_TEST(test_parse_time_noted_short_and_garbage);
    RUN_TEST(test_parse_fw_info);
    RUN_TEST(test_to_snap_is_byte_identical_to_a_hand_built_block);
    RUN_TEST(test_progress_publication_and_clamp);
    return UNITY_END(); }
```

Append to `tests/host/CMakeLists.txt`:

```cmake
# psvc_state_conv.c (pure: the snap_master_t conversion and h_state's two text
# parsers) against the REAL state_snap writer; psvc_fw.c's progress
# publication. psvc_state.c itself is IDF glue (node_mgr, wifi_mgr, heap).
hg_test(test_psvc_state ${COMP}/panel_svc/psvc_state_conv.c ${COMP}/panel_svc/psvc_fw.c ${COMP}/state_snap/state_snap.c)
target_link_libraries(test_psvc_state cjson_host)
```

- [ ] **Step 2: Run it to verify it fails**

Run the one-test command with `-R test_psvc_state`.
Expected: configure fails with `Cannot find source file: .../components/panel_svc/psvc_state_conv.c`.

- [ ] **Step 3: Implement the struct, the conversion and the progress publication**

`components/panel_svc/psvc_state.h`:

```c
#pragma once
#include <stdint.h>
#include "hg_cfg_types.h"   /* HG_MAX_ZONES */
#include "ring_proto.h"     /* hg_node_t, ring_status_t */
#include "wifi_mgr.h"       /* wifi_status_t */
#include "state_snap.h"     /* snap_master_t */

#ifdef __cplusplus
extern "C" {
#endif

/* The ONE state gather, for /api/state and the panel's 1 Hz poller alike --
 * extracted from h_state (http_api.c), which it now serves. Unlike
 * snap_master_t this struct OWNS its strings, so a copy can be published to
 * another task. */
typedef struct {
    char          version[32];
    uint32_t      uptime_s, heap_min_kb;              /* heap_min_kb = /api/state's figure (includes PSRAM on the P4) */
    uint32_t      heap_int_free_kb, heap_int_min_kb;  /* MALLOC_CAP_INTERNAL -- panel About only, not in /api/state */
    char          time[20];                           /* UTC "YYYY-MM-DD HH:MM:SS" (first 19 chars of hg_app_time_get_noted) */
    char          time_src[8];                        /* "NTP" | "SET" | "NONE" */
    uint8_t       time_is_set;                        /* hg_app_time_is_set() */
    int32_t       utc_offset_s;                       /* time_svc_utc_offset() */
    wifi_status_t wifi;                               /* untouched when PSVC_FILL_SKIP_WIFI */
    char          fw_slot[16], fw_state[16], fw_other[16];   /* hg_app_fw_info tokens; fw_state "PENDING" = OTA trial */
    char          upload_kind[8];                     /* "" | "master" | "zone" (psvc_fw_progress) */
    uint8_t       upload_pct;
    char          fleet_line[40];                     /* "IDLE" | "<z> PRECHECK|UPDATING|WAIT_HB" */
    int           alarms_active, alarms_total;
    uint8_t       web_default, ap_default;            /* MCFG_F_WEB_DEFAULT / MCFG_F_AP_DEFAULT */
    uint8_t       web_cmd_quarantined;                /* the registered web hook; 0 when the web never started */
    hg_node_t     node[HG_MAX_ZONES];                 /* slot = id-1; memset first; used == 0 => not enrolled */
    uint8_t       cfg_sync_failed[HG_MAX_ZONES];
    ring_status_t ring;
    uint32_t      now_ms;                             /* esp_timer ms (node_mgr's clock) */
} psvc_state_t;

#define PSVC_FILL_ALL       0u
#define PSVC_FILL_SKIP_WIFI 1u   /* keep out->wifi as the caller left it: wifi_mgr_status() is two esp_hosted RPCs
                                    of up to 5 s each on the P4, so the panel reads it on its own 5 s task */

/* [WORKER] 17 node_mgr lock round trips (portMAX_DELAY, held across NVS
 * writes), an otadata read, the fleet mutex and -- unless SKIP_WIFI -- two
 * RPCs. Never on the LVGL task. */
void psvc_state_fill(psvc_state_t *out, uint32_t flags);

/* Pure. m's const char* members point INTO s, so s must outlive every use of
 * m (h_state's static s does; so does a stack s used within one call). */
void psvc_state_to_snap(const psvc_state_t *s, snap_master_t *m);

/* [ANY] http_srv_start() registers http_cmd_quarantined here, so the panel can
 * show the web's degraded-slot count without panel_ui requiring http_srv. */
void psvc_state_set_web_quarantine_fn(uint8_t (*fn)(void));

/* Pure: h_state's own parses of hg_app_time_get_noted()'s
 * "YYYY-MM-DD HH:MM:SS <SRC> <age>" and hg_app_fw_info()'s
 * "<ver> <slot> <state> <other>". Outs always written ("" when absent). */
void psvc_parse_time_noted(const char *noted, char time_out[20], char src_out[8]);
void psvc_parse_fw_info(const char *info, char slot[16], char state[16], char other[16]);

#ifdef __cplusplus
}
#endif
```

`components/panel_svc/psvc_state_conv.c`:

```c
#include <stdio.h>
#include <string.h>
#include "psvc_state.h"

/* Pure (host-tested): no IDF headers. */

void psvc_parse_time_noted(const char *noted, char time_out[20], char src_out[8]) {
    time_out[0] = '\0';
    src_out[0] = '\0';
    if (!noted) return;
    size_t tlen = strlen(noted);
    size_t tcopy = tlen < 19 ? tlen : 19;   /* bounded memcpy, not snprintf -- h_state's format-truncation lesson */
    memcpy(time_out, noted, tcopy);
    time_out[tcopy] = '\0';
    if (tlen >= 19) (void)sscanf(noted + 19, " %7s", src_out);
}

void psvc_parse_fw_info(const char *info, char slot[16], char state[16], char other[16]) {
    slot[0] = state[0] = other[0] = '\0';
    if (!info) return;
    (void)sscanf(info, "%*s %15s %15s %15s", slot, state, other);
}

void psvc_state_to_snap(const psvc_state_t *s, snap_master_t *m) {
    memset(m, 0, sizeof *m);
    m->version     = s->version;
    m->uptime_s    = s->uptime_s;
    m->heap_min_kb = s->heap_min_kb;
    memcpy(m->time, s->time, sizeof m->time);
    m->time[sizeof m->time - 1] = '\0';
    m->time_src = s->time_src;
    m->sta.up = s->wifi.sta_up;
    snprintf(m->sta.ip, sizeof m->sta.ip, "%s", s->wifi.sta_ip);
    snprintf(m->sta.ssid, sizeof m->sta.ssid, "%s", s->wifi.sta_ssid);
    m->sta.rssi = s->wifi.rssi;
    snprintf(m->sta.reason, sizeof m->sta.reason, "%s", s->wifi.sta_reason);
    snprintf(m->ap.ssid, sizeof m->ap.ssid, "%s", s->wifi.ap_ssid);
    m->ap.clients = s->wifi.ap_clients;
    snprintf(m->ap.ip, sizeof m->ap.ip, "%s", s->wifi.ap_ip);
    m->fw.slot        = s->fw_slot;
    m->fw.state       = s->fw_state;
    m->fw.other       = s->fw_other;
    m->fw.upload_kind = s->upload_kind;
    m->fw.upload_pct  = s->upload_pct;
    m->fleet_line     = s->fleet_line;
    m->alarms_active  = s->alarms_active;
    m->alarms_total   = s->alarms_total;
    m->web_default    = s->web_default;
    m->ap_default     = s->ap_default;
    m->cmd_quarantined = s->web_cmd_quarantined;
}
```

`components/panel_svc/psvc_fw.h`:

```c
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Firmware-install state shared by every face. This task: upload progress,
 * moved out of http_upload.c so /api/state and the panel read ONE value and a
 * phone can watch a panel install (and the reverse). Task 30 adds the install
 * core to this header.
 *
 * Lock-free by construction: two word-sized stores by the one installer, two
 * loads by any reader. kind must be a string literal ("" | "master" | "zone"),
 * so the pointer stays valid however the install ends; pct is stored FIRST, so
 * a reader never sees a kind without its pct. */
void psvc_fw_progress_set(const char *kind, uint32_t pct);   /* NULL kind == "" */

/* [ANY] 1 while an install streams, else 0. *kind ("" when idle) and *pct
 * (0..100, clamped) are always written; either pointer may be NULL. */
int  psvc_fw_progress(const char **kind, uint8_t *pct);

#ifdef __cplusplus
}
#endif
```

`components/panel_svc/psvc_fw.c`:

```c
#include "psvc_fw.h"

static const char *volatile s_kind = "";
static volatile uint32_t    s_pct;

void psvc_fw_progress_set(const char *kind, uint32_t pct) {
    s_pct  = pct;
    s_kind = kind ? kind : "";   /* published last: a reader never sees a kind without a pct */
}

int psvc_fw_progress(const char **kind, uint8_t *pct) {
    const char *k = s_kind;
    uint32_t    p = s_pct;
    if (kind) *kind = k ? k : "";
    if (pct)  *pct  = (uint8_t)(p > 100 ? 100 : p);
    return (k && *k) ? 1 : 0;
}
```

- [ ] **Step 4: Run it to verify it passes**

Comment out nothing: the row names only files that now exist. Run the one-test command with `-R test_psvc_state`.
Expected: `100% tests passed, 0 tests failed out of 1` (5 cases).

- [ ] **Step 5: Write the gather**

`components/panel_svc/psvc_state.c`:

```c
#include <stdio.h>
#include <string.h>
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "app_if_common.h"
#include "mcfg_store.h"
#include "time_svc.h"
#include "wifi_mgr.h"
#include "node_mgr.h"
#include "alarm_mgr.h"
#include "psvc_fw.h"
#include "psvc_state.h"

static uint8_t (*volatile s_quar_fn)(void);

void psvc_state_set_web_quarantine_fn(uint8_t (*fn)(void)) { s_quar_fn = fn; }

/* Exactly what h_state gathered (http_api.c:61-145 before panel plan Task 9),
 * in the same order, plus the two internal-heap figures the panel shows. */
void psvc_state_fill(psvc_state_t *out, uint32_t flags) {
    wifi_status_t keep = out->wifi;
    memset(out, 0, sizeof *out);   /* load-bearing: node_mgr_get() leaves an unused slot untouched */

    snprintf(out->version, sizeof out->version, "%s", esp_app_get_description()->version);
    out->uptime_s         = hg_app_uptime_s();
    out->heap_min_kb      = esp_get_minimum_free_heap_size() / 1024;
    out->heap_int_free_kb = (uint32_t)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024);
    out->heap_int_min_kb  = (uint32_t)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024);

    char tbuf[48];
    hg_app_time_get_noted(tbuf, sizeof tbuf);
    psvc_parse_time_noted(tbuf, out->time, out->time_src);
    out->time_is_set  = hg_app_time_is_set() ? 1 : 0;
    out->utc_offset_s = time_svc_utc_offset();

    if (flags & PSVC_FILL_SKIP_WIFI) out->wifi = keep;
    else wifi_mgr_status(&out->wifi);

    char fwbuf[96];
    hg_app_fw_info(fwbuf, sizeof fwbuf);
    psvc_parse_fw_info(fwbuf, out->fw_slot, out->fw_state, out->fw_other);

    const char *kind = "";
    uint8_t pct = 0;
    (void)psvc_fw_progress(&kind, &pct);
    snprintf(out->upload_kind, sizeof out->upload_kind, "%s", kind);
    out->upload_pct = pct;

    node_mgr_fw_status(out->fleet_line, sizeof out->fleet_line);

    out->alarms_active = alarm_mgr_active_count();
    out->alarms_total  = alarm_mgr_total();

    uint8_t f = mcfg_get()->flags;
    out->web_default = (f & MCFG_F_WEB_DEFAULT) ? 1 : 0;
    out->ap_default  = (f & MCFG_F_AP_DEFAULT)  ? 1 : 0;

    uint8_t (*q)(void) = s_quar_fn;
    out->web_cmd_quarantined = q ? q() : 0;

    for (int i = 0; i < HG_MAX_ZONES; i++) (void)node_mgr_get(i, &out->node[i]);
    for (int i = 0; i < HG_MAX_ZONES; i++)
        out->cfg_sync_failed[i] = (uint8_t)node_mgr_cfg_sync_failed((uint8_t)(i + 1));
    node_mgr_ring_status(&out->ring);

    out->now_ms = (uint32_t)(esp_timer_get_time() / 1000);   /* same clock as node_mgr's last_hb_ms */
}
```

`components/panel_svc/CMakeLists.txt` now reads, in full:

```cmake
# components/panel_svc: the shared service layer BOTH faces call -- the web's
# HTTP handlers and the P4 panel (spec Decision 4: "Both faces call the layer
# beneath HTTP"). Master-only sources, include-only on zone/rescue: the
# http_srv pattern. REQUIRES stays unconditional (evaluated in IDF's early
# requirements pass, where CMAKE_PROJECT_NAME is not yet set) and names only
# components present in every app build.
set(PSVC_SRCS "")
if("${CMAKE_PROJECT_NAME}" STREQUAL "hillgrow_master")
    list(APPEND PSVC_SRCS "psvc_rc.c" "psvc_mcfg.c" "psvc_net.c" "psvc_zcfg.c"
                          "psvc_state.c" "psvc_state_conv.c" "psvc_fw.c")
endif()

idf_component_register(
    SRCS ${PSVC_SRCS}
    INCLUDE_DIRS "."
    REQUIRES hg_cfg hg_mcfg master_cmds wifi_mgr ring_proto state_snap
    PRIV_REQUIRES mcfg_ops time_core hg_json time_svc http_auth node_mgr alarm_mgr app_common
                  esp_timer esp_app_format heap
)
```

- [ ] **Step 6: Put `/api/state`, the upload and the web's hook on it**

In `components/http_srv/http_api.c`, add `#include "psvc_state.h"` after `#include "psvc_net.h"`, and replace the whole of `h_state()` (`:61-156`) with:

```c
esp_err_t h_state(httpd_req_t *req) {
    /* The gather is panel_svc's (panel plan Task 9): the panel's poller runs
     * the same psvc_state_fill(). ~1.9 KB, so static -- httpd serves every
     * socket from ONE task, and h_state can never run twice at once. */
    static psvc_state_t s;
    psvc_state_fill(&s, PSVC_FILL_ALL);
    snap_master_t m;
    psvc_state_to_snap(&s, &m);   /* m points into s, which outlives the write below */

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    int rc = state_snap_write(&m, s.node, HG_MAX_ZONES, &s.ring, s.cfg_sync_failed, s.now_ms, chunk_writer, req);
    if (rc != 0) return ESP_FAIL;   /* a chunk send already failed; nothing more to send */

    httpd_resp_send_chunk(req, NULL, 0);
    return http_srv_done(req, 0);   /* GET: no request body to drain */
}
```

In `components/http_srv/http_upload.c`:
- delete lines 76-91 (the `/* ---- progress ... */` block: `s_kind`, `s_pct`, `progress()` and `http_upload_progress()`);
- after `#include "http_upload.h"` (`:10`) add `#include "psvc_fw.h"   /* progress lives in panel_svc: /api/state and the panel read one value */`;
- replace the three calls:
  - `progress(kind, 0);` becomes `psvc_fw_progress_set(kind, 0);`
  - `progress(kind, (uint32_t)((uint64_t)got * 100 / req->content_len));` becomes `psvc_fw_progress_set(kind, (uint32_t)((uint64_t)got * 100 / req->content_len));`
  - `progress("", 0);` becomes `psvc_fw_progress_set("", 0);`

In `components/http_srv/http_upload.h`, replace lines 29-42 (the `/api/state's view of an upload in flight` comment and `int http_upload_progress(...)`) with:

```c
/* Upload progress is published through panel_svc (psvc_fw.h:
 * psvc_fw_progress_set() by the installer, psvc_fw_progress() by /api/state
 * and the panel), so every face reads ONE value. */
```

In `components/http_srv/http_srv.c`:
- add `#include "psvc_state.h"` after `#include "node_mgr.h"` (`:7`);
- immediately after `if (http_cmd_init() != 0) return -1;` (`:306`), add:

```c
    /* The web's degraded-slot count reaches the shared state gather (and so
     * the panel) through this hook: panel_ui never requires http_srv. */
    psvc_state_set_web_quarantine_fn(http_cmd_quarantined);
```

- [ ] **Step 7: Run the gates**

Run GATE-HOST. Expected: 40/40 tests pass (+1).
Run GATE-ESP32. Expected: master, zone and rescue each print `Project build complete` with 0 warnings, and the lock diff is empty.
Run GATE-P4. Expected: `Project build complete`, 0 warnings, the target grep matches, then the lock checkout.
Confirm nothing still calls the deleted getter: `git -C C:\Projects\HillGrov grep -n http_upload_progress -- components master`. Expected: no output.

The live proof is Stage 1 gate step 6: `web_test --only state`, with the fixture shape unchanged.

- [ ] **Step 8: Commit**

```powershell
git -C C:\Projects\HillGrov add components/panel_svc/psvc_state.h components/panel_svc/psvc_state.c components/panel_svc/psvc_state_conv.c components/panel_svc/psvc_fw.h components/panel_svc/psvc_fw.c components/panel_svc/CMakeLists.txt components/http_srv/http_api.c components/http_srv/http_upload.c components/http_srv/http_upload.h components/http_srv/http_srv.c tests/host/test_psvc_state.c tests/host/CMakeLists.txt
git -C C:\Projects\HillGrov commit -m "refactor(panel_svc): one state gather for /api/state and the panel" -m "Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>"
```

### Task 10: Pure presentation helpers: time, formatting, home maths (Stage 1)

Everything the screens compute, and nothing they draw: local time without libc TZ (the master's clock is UTC, and nothing calls `setenv("TZ")`), the web's names and formats in ASCII, the status-band layout, and the schedule sentence. All of it is host-tested.

**Files:**
- Create: `components/panel_ui/pnl_time.h`, `.c`; `pnl_fmt.h`, `.c`; `pnl_home.h`, `.c`; `tests/host/test_pnl_time.c`, `test_pnl_fmt.c`, `test_pnl_home.c`.
- Modify:
  - `components/state_snap/state_snap.h`, `.c`: the static `health_name()` (`state_snap.c:55-63`) becomes public as `state_snap_health_name()`, plus a new `state_snap_ring_state_name()` used by `ss_ring()`. The JSON output is unchanged.
  - `tests/host/test_state_snap.c`: one case for the two names.
  - `components/panel_ui/CMakeLists.txt`: the three `.c` files join `PANEL_SRCS`, so GATE-P4 compiles them with `-Werror` on the target too.
  - `tests/host/CMakeLists.txt`.

**Interfaces:**
- Consumes: `hg_node_t`, `node_health_t` and `ring_state_t` (`ring_proto.h:208-239`), `wifi_status_t` (`wifi_mgr.h:32-41`), `hg_zone_cfg_t` and `hg_zone_hw_t` (`hg_cfg_types.h`), and `HG_MAX_SHELVES`.
- Produces:
  ```c
  /* state_snap.h additions (pure) */
  const char *state_snap_health_name(node_health_t h);        /* "ONLINE" "DEGRADED" "OFFLINE" "UPDATING" "EMPTY" */
  const char *state_snap_ring_state_name(ring_state_t s);     /* "OK" "OPEN" "IDLE" */

  /* pnl_time.h (pure) */
  typedef struct { uint8_t valid; int year, mon /*1..12*/, mday, wday /*0=Sunday*/, hour, min, sec, minute_of_day; } pnl_local_t;
  void pnl_local_time(int64_t utc_s, int32_t offset_s, int time_is_set, pnl_local_t *out);   /* valid = time_is_set; civil maths, no libc tz */
  int  pnl_fmt_clock(const pnl_local_t *t, char *out, size_t cap);   /* "14:32" | "--:--" */
  int  pnl_fmt_date(const pnl_local_t *t, char *out, size_t cap);    /* "Thursday 18 September" | "Clock not set" */
  int  pnl_set_time_line(int y, int mo, int d, int h, int mi, int32_t offset_s, char *out, size_t cap);
       /* local -> UTC "SET TIME YYYY-MM-DD HH:MM:00"; 0 / -1 outside 2020..2099 or a bad date (the master's own range) */

  /* pnl_fmt.h (pure; ASCII only) */
  void pnl_zone_name(const hg_node_t *n, char out[17]);                      /* name, or "Z<id>" when empty (app.js:788) */
  int  pnl_fmt_age(uint32_t now_s, uint32_t stamp_s, char *out, size_t cap); /* the web's fmtAge: "12s ago" | "4m 3s ago" | "3h 5m ago"
                                                                                | "2d 4h ago"; a stamp newer than now clamps to "0s ago" */
  int  pnl_fmt_sta(const wifi_status_t *w, char *out, size_t cap);           /* "192.168.1.5 | house | -61 dBm" | "STA down: <reason|-->" */
  int  pnl_fmt_ap(const wifi_status_t *w, char *out, size_t cap);            /* "HillGrow | 2 client(s) | 192.168.7.7" */
  int  pnl_fmt_master_time(const char *utc19, const char *src, int32_t offset_s, int is_set, char *out, size_t cap);
       /* local "YYYY-MM-DD HH:MM:SS (UTC+02:00) NTP" | "Clock not set (NONE)" -- the web shows UTC unlabelled (D28) */
  typedef struct { int soil_pct, light_pct, pump_s; uint8_t any; } pnl_readings_t;
  void pnl_node_readings(const hg_node_t *n, pnl_readings_t *out);           /* app.js:603-617 shelfTotals over min(n_shelves,4) */
  int  pnl_fmt_reading(const pnl_readings_t *r, int which /*0 soil 1 light 2 pump*/, char *out, size_t cap);  /* "41%" "12s" "--" */

  /* pnl_home.h (pure) */
  typedef enum { PNL_TILE_MIN = 0, PNL_TILE_MID, PNL_TILE_FULL } pnl_tile_detail_t;
       /* MIN: id + dot + soil; MID: name + dot + soil; FULL: name + dot + soil + light */
  typedef struct { int tile_w, gap, x0; pnl_tile_detail_t detail; } pnl_band_t;
  #define PNL_BAND_WIDTH 976   /* 1024 - 2 x 24 margin */
  #define PNL_BAND_GAP   8
  #define PNL_TILE_MAX_W 240
  int pnl_band_layout(int n_tiles, int width, pnl_band_t *out);
      /* 0 / -1 (n outside 1..HG_MAX_ZONES); tile_w = min(PNL_TILE_MAX_W, (width-(n-1)*gap)/n), row centred (x0);
         tile_w < 140 -> MIN, < 200 -> MID, else FULL (8 tiles -> 115 px MIN; 1 tile -> 240 px FULL) */
  #define PNL_SCHED_MAX (HG_MAX_ZONES * HG_MAX_SHELVES)
  typedef struct {
      uint16_t light_on[PNL_SCHED_MAX], light_off[PNL_SCHED_MAX]; int n_light;   /* enabled shelves of enrolled zones */
      uint16_t water_start[PNL_SCHED_MAX], water_end[PNL_SCHED_MAX]; int n_water;/* WATER.MODE AUTO shelves */
  } pnl_sched_t;
  void pnl_sched_reset(pnl_sched_t *s);
  void pnl_sched_add_zone(pnl_sched_t *s, const hg_zone_cfg_t *cfg, const hg_zone_hw_t *hw_or_null);
       /* shelves i < (hw ? hw->shelf_count : HG_MAX_SHELVES) with enabled != 0 */
  int  pnl_lights_on_now(const pnl_sched_t *s, int minute_of_day);   /* on<off: on<=now<off; on>off wraps midnight */
  int  pnl_ctx_line(const pnl_sched_t *s, const pnl_local_t *t, char *out, size_t cap);
       /* !valid -> "Clock not set"; a light on -> "Lights off in 2h 14m" (earliest off; "in 14m" under an hour);
          else a light schedule -> "Lights on at 06:00" (soonest on); else a watering window -> "Next watering 06:00"
          (or "Watering window open until 18:00" inside one, "Watering on demand" when start == end); else "" */
  ```
- Interface additions: `int64_t pnl_utc_from_civil(int y, int mo, int d, int h, int mi, int s)` and `int pnl_days_in_month(int y, int mo)` in `pnl_time.h`. `pnl_fmt_master_time()` and `pnl_set_time_line()` share them. Task 23's Set-clock dialog relies on the date check `pnl_set_time_line()` makes with them (it refuses 31 February).
- Web-parity details of the formatters (the Produces block above states them; an earlier outline showed single-unit ages and "--"):
  - `pnl_fmt_age()` reproduces the web's `fmtAge()` (`app.js:876-882`): `"12s ago"`, `"4m 3s ago"`, `"3h 5m ago"`, `"2d 4h ago"`. It clamps a stamp newer than now to `"0s ago"`, as the web does ("the two documents are fetched by two independent polls"), rather than printing `"--"`. The Alarms screen shows `"--"` only before the first snapshot, exactly like the web's `HG.alarmAgo`.
  - `pnl_fmt_ap()` says `"2 client(s)"`, the web's wording (`app.js:781`).
  - Every formatter replaces bytes outside printable ASCII (for example a UTF-8 SSID) with `?`. The built-in Montserrat has no glyphs for them (Global Constraints, "UI text").

- [ ] **Step 1: Write the failing tests**

`tests/host/test_pnl_time.c`:

```c
/* The panel's own civil-time maths. The master's clock is UTC (nothing calls
   setenv("TZ")/tzset), so local = time(NULL) + time_svc_utc_offset(), done here
   with no libc tz. An unset clock must read as unset, never as a plausible
   wrong time (spec "Error handling"). */
#include <string.h>
#include "unity.h"
#include "pnl_time.h"

void setUp(void) {}
void tearDown(void) {}

static pnl_local_t at_utc(int y, int mo, int d, int h, int mi, int s, int32_t off) {
    pnl_local_t t;
    pnl_local_time(pnl_utc_from_civil(y, mo, d, h, mi, s), off, 1, &t);
    return t;
}

static void test_civil_anchor_points(void) {
    TEST_ASSERT_EQUAL_INT64(0, pnl_utc_from_civil(1970, 1, 1, 0, 0, 0));
    TEST_ASSERT_EQUAL_INT64(946684800, pnl_utc_from_civil(2000, 1, 1, 0, 0, 0));
    pnl_local_t t;
    pnl_local_time(0, 0, 1, &t);
    TEST_ASSERT_EQUAL_INT(1970, t.year); TEST_ASSERT_EQUAL_INT(1, t.mon); TEST_ASSERT_EQUAL_INT(1, t.mday);
    TEST_ASSERT_EQUAL_INT(4, t.wday);   /* Thursday */
}

static void test_known_weekday_and_date_text(void) {
    pnl_local_t t = at_utc(2025, 9, 18, 12, 0, 0, 0);
    TEST_ASSERT_EQUAL_INT(4, t.wday);
    char b[40];
    pnl_fmt_date(&t, b, sizeof b);
    TEST_ASSERT_EQUAL_STRING("Thursday 18 September", b);
}

static void test_offset_across_midnight_and_year(void) {
    pnl_local_t t = at_utc(2025, 12, 31, 23, 30, 0, 3600);
    TEST_ASSERT_EQUAL_INT(2026, t.year); TEST_ASSERT_EQUAL_INT(1, t.mon); TEST_ASSERT_EQUAL_INT(1, t.mday);
    TEST_ASSERT_EQUAL_INT(0, t.hour);    TEST_ASSERT_EQUAL_INT(30, t.min);
    TEST_ASSERT_EQUAL_INT(4, t.wday);    /* 2026-01-01 is a Thursday */
    t = at_utc(2026, 1, 1, 3, 0, 0, -18000);
    TEST_ASSERT_EQUAL_INT(2025, t.year); TEST_ASSERT_EQUAL_INT(12, t.mon); TEST_ASSERT_EQUAL_INT(31, t.mday);
    TEST_ASSERT_EQUAL_INT(22, t.hour);
    TEST_ASSERT_EQUAL_INT(3, t.wday);    /* Wednesday */
}

static void test_month_boundary_and_leap_day(void) {
    pnl_local_t t = at_utc(2026, 4, 30, 23, 59, 0, 60);
    TEST_ASSERT_EQUAL_INT(5, t.mon); TEST_ASSERT_EQUAL_INT(1, t.mday); TEST_ASSERT_EQUAL_INT(0, t.hour);
    t = at_utc(2028, 2, 28, 23, 0, 0, 7200);
    TEST_ASSERT_EQUAL_INT(2, t.mon); TEST_ASSERT_EQUAL_INT(29, t.mday); TEST_ASSERT_EQUAL_INT(1, t.hour);
    TEST_ASSERT_EQUAL_INT(2, t.wday);    /* Tuesday */
    char b[40];
    pnl_fmt_date(&t, b, sizeof b);
    TEST_ASSERT_EQUAL_STRING("Tuesday 29 February", b);
    TEST_ASSERT_EQUAL_INT(29, pnl_days_in_month(2024, 2));
    TEST_ASSERT_EQUAL_INT(28, pnl_days_in_month(2100, 2));
    TEST_ASSERT_EQUAL_INT(29, pnl_days_in_month(2000, 2));
    TEST_ASSERT_EQUAL_INT(30, pnl_days_in_month(2026, 4));
    TEST_ASSERT_EQUAL_INT(0, pnl_days_in_month(2026, 13));
}

static void test_clock_text_and_minute_of_day(void) {
    pnl_local_t t = at_utc(2026, 9, 18, 12, 32, 5, 7200);
    char b[16];
    pnl_fmt_clock(&t, b, sizeof b);
    TEST_ASSERT_EQUAL_STRING("14:32", b);
    TEST_ASSERT_EQUAL_INT(14 * 60 + 32, t.minute_of_day);
    TEST_ASSERT_EQUAL_INT(5, t.sec);
}

static void test_unset_clock_is_honest(void) {
    pnl_local_t t;
    pnl_local_time(pnl_utc_from_civil(2026, 9, 18, 12, 0, 0), 7200, 0, &t);
    TEST_ASSERT_EQUAL_UINT8(0, t.valid);
    char b[32];
    pnl_fmt_clock(&t, b, sizeof b);
    TEST_ASSERT_EQUAL_STRING("--:--", b);
    pnl_fmt_date(&t, b, sizeof b);
    TEST_ASSERT_EQUAL_STRING("Clock not set", b);
}

static void test_set_time_line_converts_local_to_utc(void) {
    char b[40];
    TEST_ASSERT_EQUAL_INT(0, pnl_set_time_line(2026, 9, 18, 14, 32, 7200, b, sizeof b));
    TEST_ASSERT_EQUAL_STRING("SET TIME 2026-09-18 12:32:00", b);
    TEST_ASSERT_EQUAL_INT(0, pnl_set_time_line(2026, 12, 31, 21, 0, -18000, b, sizeof b));
    TEST_ASSERT_EQUAL_STRING("SET TIME 2027-01-01 02:00:00", b);
    TEST_ASSERT_EQUAL_INT(0, pnl_set_time_line(2028, 2, 29, 10, 0, 0, b, sizeof b));
    TEST_ASSERT_EQUAL_STRING("SET TIME 2028-02-29 10:00:00", b);
}

static void test_set_time_line_refuses_out_of_range(void) {
    char b[40];
    TEST_ASSERT_EQUAL_INT(-1, pnl_set_time_line(2019, 12, 31, 12, 0, 0, b, sizeof b));
    TEST_ASSERT_EQUAL_INT(-1, pnl_set_time_line(2100, 1, 1, 0, 0, 0, b, sizeof b));
    TEST_ASSERT_EQUAL_INT(-1, pnl_set_time_line(2026, 2, 29, 12, 0, 0, b, sizeof b));
    TEST_ASSERT_EQUAL_INT(-1, pnl_set_time_line(2026, 13, 1, 12, 0, 0, b, sizeof b));
    TEST_ASSERT_EQUAL_INT(-1, pnl_set_time_line(2026, 1, 1, 24, 0, 0, b, sizeof b));
    TEST_ASSERT_EQUAL_INT(-1, pnl_set_time_line(2026, 1, 1, 12, 60, 0, b, sizeof b));
    TEST_ASSERT_EQUAL_INT(-1, pnl_set_time_line(2020, 1, 1, 0, 30, 7200, b, sizeof b));    /* UTC lands in 2019 */
    TEST_ASSERT_EQUAL_INT(-1, pnl_set_time_line(2099, 12, 31, 23, 30, -3600, b, sizeof b));/* UTC lands in 2100 */
}

int main(void) { UNITY_BEGIN();
    RUN_TEST(test_civil_anchor_points);
    RUN_TEST(test_known_weekday_and_date_text);
    RUN_TEST(test_offset_across_midnight_and_year);
    RUN_TEST(test_month_boundary_and_leap_day);
    RUN_TEST(test_clock_text_and_minute_of_day);
    RUN_TEST(test_unset_clock_is_honest);
    RUN_TEST(test_set_time_line_converts_local_to_utc);
    RUN_TEST(test_set_time_line_refuses_out_of_range);
    return UNITY_END(); }
```

`tests/host/test_pnl_fmt.c`:

```c
/* Every word and number the panel shows about zones, the master and Wi-Fi,
   in the web's vocabulary (map-parity A11/A12) -- and ASCII only: the built-in
   Montserrat renders nothing for U+2014/U+00B7 or a UTF-8 SSID, so every output
   here is checked character by character. */
#include <stdio.h>
#include <string.h>
#include "unity.h"
#include "pnl_fmt.h"

void setUp(void) {}
void tearDown(void) {}

static void assert_ascii(const char *s) {
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        TEST_ASSERT_TRUE_MESSAGE(*p >= 0x20 && *p <= 0x7E, s);
}

static void test_zone_name(void) {
    hg_node_t n;
    memset(&n, 0, sizeof n);
    n.id = 3;
    char b[17];
    pnl_zone_name(&n, b);
    TEST_ASSERT_EQUAL_STRING("Z3", b);
    snprintf(n.name, sizeof n.name, "Tomatoes");
    pnl_zone_name(&n, b);
    TEST_ASSERT_EQUAL_STRING("Tomatoes", b);
    memcpy(n.name, "caf\xC3\xA9", 6);
    pnl_zone_name(&n, b);
    TEST_ASSERT_EQUAL_STRING("caf??", b);
    memset(n.name, 'x', sizeof n.name);   /* no terminator in the 16-byte field */
    pnl_zone_name(&n, b);
    TEST_ASSERT_EQUAL_INT(16, (int)strlen(b));
    assert_ascii(b);
}

static void test_age_matches_the_web(void) {
    char b[24];
    pnl_fmt_age(1000, 988, b, sizeof b);                       TEST_ASSERT_EQUAL_STRING("12s ago", b);
    pnl_fmt_age(1000, 757, b, sizeof b);                       TEST_ASSERT_EQUAL_STRING("4m 3s ago", b);
    pnl_fmt_age(20000, 20000 - 11100, b, sizeof b);            TEST_ASSERT_EQUAL_STRING("3h 5m ago", b);
    pnl_fmt_age(200000, 200000 - (2 * 86400 + 4 * 3600), b, sizeof b);
                                                               TEST_ASSERT_EQUAL_STRING("2d 4h ago", b);
    pnl_fmt_age(1000, 1005, b, sizeof b);                      TEST_ASSERT_EQUAL_STRING("0s ago", b);
    assert_ascii(b);
}

static void test_sta_and_ap_lines(void) {
    wifi_status_t w;
    memset(&w, 0, sizeof w);
    char b[96];
    w.sta_up = 1;
    snprintf(w.sta_ip, sizeof w.sta_ip, "192.168.1.5");
    snprintf(w.sta_ssid, sizeof w.sta_ssid, "house");
    w.rssi = -61;
    pnl_fmt_sta(&w, b, sizeof b);                              TEST_ASSERT_EQUAL_STRING("192.168.1.5 | house | -61 dBm", b);
    assert_ascii(b);
    w.sta_up = 0;
    snprintf(w.sta_reason, sizeof w.sta_reason, "AUTH_FAIL");
    pnl_fmt_sta(&w, b, sizeof b);                              TEST_ASSERT_EQUAL_STRING("STA down: AUTH_FAIL", b);
    w.sta_reason[0] = '\0';
    pnl_fmt_sta(&w, b, sizeof b);                              TEST_ASSERT_EQUAL_STRING("STA down: --", b);
    snprintf(w.ap_ssid, sizeof w.ap_ssid, "HillGrow");
    w.ap_clients = 2;
    snprintf(w.ap_ip, sizeof w.ap_ip, "192.168.7.7");
    pnl_fmt_ap(&w, b, sizeof b);                               TEST_ASSERT_EQUAL_STRING("HillGrow | 2 client(s) | 192.168.7.7", b);
    memcpy(w.ap_ssid, "Gr\xC3\xBCn", 6);
    pnl_fmt_ap(&w, b, sizeof b);
    assert_ascii(b);
}

static void test_master_time_is_local_and_labelled(void) {
    char b[64];
    pnl_fmt_master_time("2026-09-18 12:32:05", "NTP", 7200, 1, b, sizeof b);
    TEST_ASSERT_EQUAL_STRING("2026-09-18 14:32:05 (UTC+02:00) NTP", b);
    pnl_fmt_master_time("2026-09-18 12:32:05", "SET", -18000, 1, b, sizeof b);
    TEST_ASSERT_EQUAL_STRING("2026-09-18 07:32:05 (UTC-05:00) SET", b);
    pnl_fmt_master_time("2026-09-18 12:32:05", "NTP", 19800, 1, b, sizeof b);
    TEST_ASSERT_EQUAL_STRING("2026-09-18 18:02:05 (UTC+05:30) NTP", b);
    pnl_fmt_master_time("1970-01-01 00:03:28", "NONE", 7200, 0, b, sizeof b);
    TEST_ASSERT_EQUAL_STRING("Clock not set (NONE)", b);
    pnl_fmt_master_time("garbage", "SET", 0, 1, b, sizeof b);
    TEST_ASSERT_EQUAL_STRING("garbage SET", b);
    assert_ascii(b);
}

static void test_readings_follow_shelf_totals(void) {
    hg_node_t n;
    memset(&n, 0, sizeof n);
    pnl_readings_t r;
    char b[16];
    pnl_node_readings(&n, &r);                                 /* no shelves */
    TEST_ASSERT_EQUAL_UINT8(0, r.any);
    pnl_fmt_reading(&r, 0, b, sizeof b);                       TEST_ASSERT_EQUAL_STRING("--", b);
    n.hb.n_shelves = 2;
    pnl_node_readings(&n, &r);                                 /* shelves, all zero */
    TEST_ASSERT_EQUAL_UINT8(0, r.any);
    n.hb.shelf[0].pct_a = 40; n.hb.shelf[0].pct_b = 42;
    n.hb.shelf[1].pump_today_s = 30;
    pnl_node_readings(&n, &r);
    TEST_ASSERT_EQUAL_UINT8(1, r.any);
    TEST_ASSERT_EQUAL_INT(21, r.soil_pct);                     /* round(82/4 = 20.5) = 21, JS Math.round */
    TEST_ASSERT_EQUAL_INT(0, r.light_pct);
    TEST_ASSERT_EQUAL_INT(30, r.pump_s);
    pnl_fmt_reading(&r, 0, b, sizeof b);                       TEST_ASSERT_EQUAL_STRING("21%", b);
    pnl_fmt_reading(&r, 1, b, sizeof b);                       TEST_ASSERT_EQUAL_STRING("0%", b);
    pnl_fmt_reading(&r, 2, b, sizeof b);                       TEST_ASSERT_EQUAL_STRING("30s", b);
    pnl_fmt_reading(&r, 7, b, sizeof b);                       TEST_ASSERT_EQUAL_STRING("--", b);
    n.hb.n_shelves = 9;                                        /* never trust the count past 4 */
    n.hb.shelf[3].white = 100; n.hb.shelf[3].red = 100;
    pnl_node_readings(&n, &r);
    TEST_ASSERT_EQUAL_INT(25, r.light_pct);                    /* 200 / (2 x 4) */
    assert_ascii(b);
}

int main(void) { UNITY_BEGIN();
    RUN_TEST(test_zone_name);
    RUN_TEST(test_age_matches_the_web);
    RUN_TEST(test_sta_and_ap_lines);
    RUN_TEST(test_master_time_is_local_and_labelled);
    RUN_TEST(test_readings_follow_shelf_totals);
    return UNITY_END(); }
```

`tests/host/test_pnl_home.c`:

```c
/* The home screen's maths: the status band divides the width so 1..8 tiles
   all look deliberate (spec "Status band"), and the context line is "the most
   useful sentence a greenhouse clock can carry" -- honest when the clock is
   unset. */
#include <string.h>
#include "unity.h"
#include "hg_cfg.h"
#include "pnl_home.h"

void setUp(void) {}
void tearDown(void) {}

static pnl_local_t at(int h, int m) {
    pnl_local_t t;
    memset(&t, 0, sizeof t);
    t.valid = 1; t.hour = h; t.min = m; t.minute_of_day = h * 60 + m;
    return t;
}

static void test_band_layout_for_every_count(void) {
    static const int W[9]  = { 0, 240, 240, 240, 238, 188, 156, 132, 115 };
    static const int X0[9] = { 0, 368, 244, 120,   0,   2,   0,   2,   0 };
    static const pnl_tile_detail_t D[9] = { PNL_TILE_MIN, PNL_TILE_FULL, PNL_TILE_FULL, PNL_TILE_FULL, PNL_TILE_FULL,
                                            PNL_TILE_MID, PNL_TILE_MID, PNL_TILE_MIN, PNL_TILE_MIN };
    for (int n = 1; n <= 8; n++) {
        pnl_band_t b;
        TEST_ASSERT_EQUAL_INT(0, pnl_band_layout(n, PNL_BAND_WIDTH, &b));
        TEST_ASSERT_EQUAL_INT_MESSAGE(W[n], b.tile_w, "tile_w");
        TEST_ASSERT_EQUAL_INT_MESSAGE(X0[n], b.x0, "x0");
        TEST_ASSERT_EQUAL_INT_MESSAGE(D[n], b.detail, "detail");
        TEST_ASSERT_EQUAL_INT(PNL_BAND_GAP, b.gap);
        TEST_ASSERT_TRUE(b.x0 + n * b.tile_w + (n - 1) * b.gap <= PNL_BAND_WIDTH);
    }
    pnl_band_t b;
    TEST_ASSERT_EQUAL_INT(-1, pnl_band_layout(0, PNL_BAND_WIDTH, &b));
    TEST_ASSERT_EQUAL_INT(-1, pnl_band_layout(9, PNL_BAND_WIDTH, &b));
}

static pnl_sched_t one_zone(int on_h, int off_h) {
    hg_zone_cfg_t cfg;
    hg_defaults_cfg(&cfg);
    cfg.shelf[0].enabled = 1;
    cfg.shelf[0].light.on_min = (uint16_t)(on_h * 60);
    cfg.shelf[0].light.off_min = (uint16_t)(off_h * 60);
    pnl_sched_t s;
    pnl_sched_reset(&s);
    pnl_sched_add_zone(&s, &cfg, NULL);
    return s;
}

static void test_lights_sentences(void) {
    pnl_sched_t s = one_zone(6, 22);
    char b[48];
    pnl_local_t t = at(19, 46);
    pnl_ctx_line(&s, &t, b, sizeof b);  TEST_ASSERT_EQUAL_STRING("Lights off in 2h 14m", b);
    t = at(21, 46);
    pnl_ctx_line(&s, &t, b, sizeof b);  TEST_ASSERT_EQUAL_STRING("Lights off in 14m", b);
    t = at(23, 0);
    pnl_ctx_line(&s, &t, b, sizeof b);  TEST_ASSERT_EQUAL_STRING("Lights on at 06:00", b);
    t = at(5, 59);
    pnl_ctx_line(&s, &t, b, sizeof b);  TEST_ASSERT_EQUAL_STRING("Lights on at 06:00", b);
}

static void test_wrap_around_midnight(void) {
    pnl_sched_t s = one_zone(20, 4);
    TEST_ASSERT_EQUAL_INT(1, pnl_lights_on_now(&s, 1 * 60));
    TEST_ASSERT_EQUAL_INT(1, pnl_lights_on_now(&s, 21 * 60));
    TEST_ASSERT_EQUAL_INT(0, pnl_lights_on_now(&s, 12 * 60));
    TEST_ASSERT_EQUAL_INT(0, pnl_lights_on_now(&s, 4 * 60));   /* off is exclusive */
    char b[48];
    pnl_local_t t = at(1, 0);
    pnl_ctx_line(&s, &t, b, sizeof b);  TEST_ASSERT_EQUAL_STRING("Lights off in 3h 0m", b);
}

static void test_earliest_off_and_soonest_on_win(void) {
    pnl_sched_t s;
    pnl_sched_reset(&s);
    s.n_light = 2;
    s.light_on[0] = 6 * 60;  s.light_off[0] = 22 * 60;
    s.light_on[1] = 8 * 60;  s.light_off[1] = 20 * 60;
    char b[48];
    pnl_local_t t = at(10, 0);
    pnl_ctx_line(&s, &t, b, sizeof b);  TEST_ASSERT_EQUAL_STRING("Lights off in 10h 0m", b);
    t = at(23, 0);
    pnl_ctx_line(&s, &t, b, sizeof b);  TEST_ASSERT_EQUAL_STRING("Lights on at 06:00", b);
}

static void test_watering_sentences(void) {
    pnl_sched_t s;
    pnl_sched_reset(&s);
    s.n_water = 1;
    s.water_start[0] = 6 * 60; s.water_end[0] = 18 * 60;
    char b[48];
    pnl_local_t t = at(5, 0);
    pnl_ctx_line(&s, &t, b, sizeof b);  TEST_ASSERT_EQUAL_STRING("Next watering 06:00", b);
    t = at(10, 0);
    pnl_ctx_line(&s, &t, b, sizeof b);  TEST_ASSERT_EQUAL_STRING("Watering window open until 18:00", b);
    s.water_end[0] = s.water_start[0];   /* equal = always (hg_water_cfg_t) */
    pnl_ctx_line(&s, &t, b, sizeof b);  TEST_ASSERT_EQUAL_STRING("Watering on demand", b);
}

static void test_nothing_to_say_and_unset_clock(void) {
    pnl_sched_t s;
    pnl_sched_reset(&s);
    char b[48] = "x";
    pnl_local_t t = at(10, 0);
    TEST_ASSERT_EQUAL_INT(0, pnl_ctx_line(&s, &t, b, sizeof b));
    TEST_ASSERT_EQUAL_STRING("", b);
    s = one_zone(6, 22);
    t.valid = 0;
    pnl_ctx_line(&s, &t, b, sizeof b);  TEST_ASSERT_EQUAL_STRING("Clock not set", b);
}

static void test_add_zone_respects_enable_mode_and_shelf_count(void) {
    hg_zone_cfg_t cfg;
    hg_defaults_cfg(&cfg);                   /* all shelves disabled; water mode AUTO */
    cfg.shelf[0].enabled = 1;
    cfg.shelf[1].enabled = 1;
    cfg.shelf[1].water.mode = 0;             /* OFF: no watering window */
    hg_zone_hw_t hw;
    hg_defaults_hw(&hw);
    hw.shelf_count = 1;
    pnl_sched_t s;
    pnl_sched_reset(&s);
    pnl_sched_add_zone(&s, &cfg, &hw);
    TEST_ASSERT_EQUAL_INT(1, s.n_light);     /* shelf 1 is beyond the hardware */
    TEST_ASSERT_EQUAL_INT(1, s.n_water);
    pnl_sched_reset(&s);
    pnl_sched_add_zone(&s, &cfg, NULL);
    TEST_ASSERT_EQUAL_INT(2, s.n_light);
    TEST_ASSERT_EQUAL_INT(1, s.n_water);     /* shelf 1's water is OFF */
    for (int z = 0; z < 20; z++) pnl_sched_add_zone(&s, &cfg, NULL);
    TEST_ASSERT_EQUAL_INT(PNL_SCHED_MAX, s.n_light);   /* capacity-bounded */
}

int main(void) { UNITY_BEGIN();
    RUN_TEST(test_band_layout_for_every_count);
    RUN_TEST(test_lights_sentences);
    RUN_TEST(test_wrap_around_midnight);
    RUN_TEST(test_earliest_off_and_soonest_on_win);
    RUN_TEST(test_watering_sentences);
    RUN_TEST(test_nothing_to_say_and_unset_clock);
    RUN_TEST(test_add_zone_respects_enable_mode_and_shelf_count);
    return UNITY_END(); }
```

In `tests/host/test_state_snap.c`, add before `int main(void)`:

```c
/* Panel plan Task 10: the two vocabulary helpers are public now (the panel
   shows the same words), and ss_node/ss_ring use them -- the JSON is unchanged. */
static void test_health_and_ring_state_names(void) {
    TEST_ASSERT_EQUAL_STRING("ONLINE",   state_snap_health_name(NODE_H_ONLINE));
    TEST_ASSERT_EQUAL_STRING("DEGRADED", state_snap_health_name(NODE_H_DEGRADED));
    TEST_ASSERT_EQUAL_STRING("OFFLINE",  state_snap_health_name(NODE_H_OFFLINE));
    TEST_ASSERT_EQUAL_STRING("UPDATING", state_snap_health_name(NODE_H_UPDATING));
    TEST_ASSERT_EQUAL_STRING("EMPTY",    state_snap_health_name(NODE_H_EMPTY));
    TEST_ASSERT_EQUAL_STRING("EMPTY",    state_snap_health_name((node_health_t)99));
    TEST_ASSERT_EQUAL_STRING("OK",   state_snap_ring_state_name(RING_ST_OK));
    TEST_ASSERT_EQUAL_STRING("OPEN", state_snap_ring_state_name(RING_ST_OPEN));
    TEST_ASSERT_EQUAL_STRING("IDLE", state_snap_ring_state_name(RING_ST_IDLE));
    TEST_ASSERT_EQUAL_STRING("IDLE", state_snap_ring_state_name((ring_state_t)9));
}
```

and `RUN_TEST(test_health_and_ring_state_names);` as the last line before `return UNITY_END();`.

Append to `tests/host/CMakeLists.txt`:

```cmake
hg_test(test_pnl_time ${COMP}/panel_ui/pnl_time.c)
hg_test(test_pnl_fmt  ${COMP}/panel_ui/pnl_fmt.c ${COMP}/panel_ui/pnl_time.c)
hg_test(test_pnl_home ${COMP}/panel_ui/pnl_home.c ${COMP}/panel_ui/pnl_time.c ${COMP}/hg_cfg/hg_cfg_defaults.c)
```

- [ ] **Step 2: Run them to verify they fail**

Run the one-test command with `-R "test_pnl_(time|fmt|home)"`.
Expected: configure fails with `Cannot find source file: .../components/panel_ui/pnl_time.c` (and the other two).

- [ ] **Step 3: Implement `pnl_time`**

`components/panel_ui/pnl_time.h`:

```c
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Local civil time for the panel (pure). The master's clock and SET TIME are
 * UTC -- nothing calls setenv("TZ")/tzset -- so the panel's local time is
 * time(NULL) + time_svc_utc_offset(), computed here with Howard Hinnant's
 * days-from-civil algorithm, no libc tz. While the clock is unset every text
 * says so rather than showing a plausible wrong time. */

typedef struct { uint8_t valid; int year, mon /*1..12*/, mday, wday /*0=Sunday*/, hour, min, sec, minute_of_day; } pnl_local_t;

void pnl_local_time(int64_t utc_s, int32_t offset_s, int time_is_set, pnl_local_t *out);   /* valid = time_is_set */
int  pnl_fmt_clock(const pnl_local_t *t, char *out, size_t cap);   /* "14:32" | "--:--" */
int  pnl_fmt_date(const pnl_local_t *t, char *out, size_t cap);    /* "Thursday 18 September" | "Clock not set" */

/* Local wall time -> the master's UTC "SET TIME YYYY-MM-DD HH:MM:00". 0, or -1
 * when the local date/time is invalid or either it or its UTC equivalent falls
 * outside 2020..2099 (the master's own SET TIME range). */
int  pnl_set_time_line(int y, int mo, int d, int h, int mi, int32_t offset_s, char *out, size_t cap);

int64_t pnl_utc_from_civil(int y, int mo, int d, int h, int mi, int s);   /* seconds since 1970-01-01 00:00:00 */
int     pnl_days_in_month(int y, int mo);                                  /* 28..31; 0 for a month outside 1..12 */

#ifdef __cplusplus
}
#endif
```

`components/panel_ui/pnl_time.c`:

```c
#include <stdio.h>
#include <string.h>
#include "pnl_time.h"

static const char *const WDAY[7] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
static const char *const MON[12] = { "January", "February", "March", "April", "May", "June", "July",
                                     "August", "September", "October", "November", "December" };

/* Days since 1970-01-01 for a proleptic Gregorian date (H. Hinnant). */
static int64_t days_from_civil(int y, int m, int d) {
    y -= (m <= 2);
    int era = (y >= 0 ? y : y - 399) / 400;
    int yoe = y - era * 400;                          /* [0, 399] */
    int mp  = m > 2 ? m - 3 : m + 9;                  /* [0, 11], March-based */
    int doy = (153 * mp + 2) / 5 + d - 1;             /* [0, 365] */
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;  /* [0, 146096] */
    return (int64_t)era * 146097 + doe - 719468;
}

static void civil_from_days(int64_t z, int *y, int *m, int *d) {
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int doe = (int)(z - era * 146097);                                  /* [0, 146096] */
    int yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;    /* [0, 399] */
    int doy = doe - (365 * yoe + yoe / 4 - yoe / 100);                  /* [0, 365] */
    int mp  = (5 * doy + 2) / 153;                                      /* [0, 11] */
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp < 10 ? mp + 3 : mp - 9;
    *y = (int)(yoe + era * 400) + (*m <= 2);
}

int pnl_days_in_month(int y, int mo) {
    static const int DIM[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (mo < 1 || mo > 12) return 0;
    if (mo == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) return 29;
    return DIM[mo - 1];
}

int64_t pnl_utc_from_civil(int y, int mo, int d, int h, int mi, int s) {
    return days_from_civil(y, mo, d) * 86400 + (int64_t)h * 3600 + (int64_t)mi * 60 + s;
}

void pnl_local_time(int64_t utc_s, int32_t offset_s, int time_is_set, pnl_local_t *out) {
    memset(out, 0, sizeof *out);
    out->valid = time_is_set ? 1 : 0;
    int64_t t = utc_s + offset_s;
    int64_t days = t >= 0 ? t / 86400 : -((-t + 86399) / 86400);   /* floor division */
    int sod = (int)(t - days * 86400);                              /* [0, 86399] */
    civil_from_days(days, &out->year, &out->mon, &out->mday);
    out->wday = (int)(((days % 7) + 11) % 7);                       /* 1970-01-01 (day 0) was a Thursday */
    out->hour = sod / 3600;
    out->min  = (sod / 60) % 60;
    out->sec  = sod % 60;
    out->minute_of_day = sod / 60;
}

int pnl_fmt_clock(const pnl_local_t *t, char *out, size_t cap) {
    if (!t || !t->valid) return snprintf(out, cap, "--:--");
    return snprintf(out, cap, "%02d:%02d", t->hour, t->min);
}

int pnl_fmt_date(const pnl_local_t *t, char *out, size_t cap) {
    if (!t || !t->valid || t->wday < 0 || t->wday > 6 || t->mon < 1 || t->mon > 12)
        return snprintf(out, cap, "Clock not set");
    return snprintf(out, cap, "%s %d %s", WDAY[t->wday], t->mday, MON[t->mon - 1]);
}

int pnl_set_time_line(int y, int mo, int d, int h, int mi, int32_t offset_s, char *out, size_t cap) {
    if (y < 2020 || y > 2099 || mo < 1 || mo > 12 || d < 1 || d > pnl_days_in_month(y, mo) ||
        h < 0 || h > 23 || mi < 0 || mi > 59) return -1;
    pnl_local_t u;
    pnl_local_time(pnl_utc_from_civil(y, mo, d, h, mi, 0) - offset_s, 0, 1, &u);
    if (u.year < 2020 || u.year > 2099) return -1;
    snprintf(out, cap, "SET TIME %04d-%02d-%02d %02d:%02d:00", u.year, u.mon, u.mday, u.hour, u.min);
    return 0;
}
```

- [ ] **Step 4: Implement `pnl_fmt`**

`components/panel_ui/pnl_fmt.h`:

```c
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "ring_proto.h"   /* hg_node_t */
#include "wifi_mgr.h"     /* wifi_status_t */

#ifdef __cplusplus
extern "C" {
#endif

/* The web's words and numbers, for the panel (pure; ASCII only -- any byte
 * outside 0x20..0x7E, e.g. a UTF-8 SSID, becomes '?', because the built-in
 * Montserrat has no glyph for it). Every function writes a NUL-terminated
 * string and returns snprintf's count (or 0). */

void pnl_zone_name(const hg_node_t *n, char out[17]);                      /* name, or "Z<id>" when empty (app.js:788) */
int  pnl_fmt_age(uint32_t now_s, uint32_t stamp_s, char *out, size_t cap); /* the web's fmtAge + " ago": "12s ago",
                                                                              "4m 3s ago", "3h 5m ago", "2d 4h ago";
                                                                              a stamp newer than now clamps to "0s ago" */
int  pnl_fmt_sta(const wifi_status_t *w, char *out, size_t cap);           /* "192.168.1.5 | house | -61 dBm" | "STA down: <reason|-->" */
int  pnl_fmt_ap(const wifi_status_t *w, char *out, size_t cap);            /* "HillGrow | 2 client(s) | 192.168.7.7" */

/* The master's UTC "YYYY-MM-DD HH:MM:SS" shown as LOCAL time and labelled:
 * "2026-09-18 14:32:05 (UTC+02:00) NTP". !is_set -> "Clock not set (<src>)".
 * An unparseable utc19 is shown as-is: "<utc19> <src>". The web shows UTC
 * unlabelled (D28); that fix is out of scope. */
int  pnl_fmt_master_time(const char *utc19, const char *src, int32_t offset_s, int is_set, char *out, size_t cap);

typedef struct { int soil_pct, light_pct, pump_s; uint8_t any; } pnl_readings_t;
void pnl_node_readings(const hg_node_t *n, pnl_readings_t *out);           /* app.js:603-617 shelfTotals over min(n_shelves,4) */
int  pnl_fmt_reading(const pnl_readings_t *r, int which /*0 soil 1 light 2 pump*/, char *out, size_t cap);  /* "41%" "12s" "--" */

#ifdef __cplusplus
}
#endif
```

`components/panel_ui/pnl_fmt.c`:

```c
#include <stdio.h>
#include <string.h>
#include "pnl_time.h"
#include "pnl_fmt.h"

/* Copies at most n bytes of src (stopping at NUL) into dst[cap], replacing
 * anything outside printable ASCII with '?'. */
static void ascii_copy(char *dst, size_t cap, const char *src, size_t n) {
    size_t o = 0;
    for (size_t i = 0; i < n && src[i] && o + 1 < cap; i++) {
        unsigned char c = (unsigned char)src[i];
        dst[o++] = (c >= 0x20 && c <= 0x7E) ? (char)c : '?';
    }
    dst[o] = '\0';
}

void pnl_zone_name(const hg_node_t *n, char out[17]) {
    if (n->name[0]) ascii_copy(out, 17, n->name, sizeof n->name);
    else snprintf(out, 17, "Z%u", (unsigned)n->id);
}

int pnl_fmt_age(uint32_t now_s, uint32_t stamp_s, char *out, size_t cap) {
    unsigned s = (unsigned)(now_s >= stamp_s ? now_s - stamp_s : 0u);
    if (s < 60u)    return snprintf(out, cap, "%us ago", s);
    if (s < 3600u)  return snprintf(out, cap, "%um %us ago", s / 60u, s % 60u);
    if (s < 86400u) return snprintf(out, cap, "%uh %um ago", s / 3600u, (s % 3600u) / 60u);
    return snprintf(out, cap, "%ud %uh ago", s / 86400u, (s % 86400u) / 3600u);
}

int pnl_fmt_sta(const wifi_status_t *w, char *out, size_t cap) {
    if (w->sta_up) {
        char ip[16], ssid[33];
        ascii_copy(ip, sizeof ip, w->sta_ip, sizeof w->sta_ip);
        ascii_copy(ssid, sizeof ssid, w->sta_ssid, sizeof w->sta_ssid);
        return snprintf(out, cap, "%s | %s | %d dBm", ip, ssid, (int)w->rssi);
    }
    char reason[24];
    ascii_copy(reason, sizeof reason, w->sta_reason, sizeof w->sta_reason);
    return snprintf(out, cap, "STA down: %s", reason[0] ? reason : "--");
}

int pnl_fmt_ap(const wifi_status_t *w, char *out, size_t cap) {
    char ssid[33], ip[16];
    ascii_copy(ssid, sizeof ssid, w->ap_ssid, sizeof w->ap_ssid);
    ascii_copy(ip, sizeof ip, w->ap_ip, sizeof w->ap_ip);
    return snprintf(out, cap, "%s | %u client(s) | %s", ssid, (unsigned)w->ap_clients, ip);
}

int pnl_fmt_master_time(const char *utc19, const char *src, int32_t offset_s, int is_set, char *out, size_t cap) {
    char s[8];
    ascii_copy(s, sizeof s, src ? src : "", 8);
    if (!is_set) return snprintf(out, cap, "Clock not set (%s)", s[0] ? s : "NONE");
    int y, mo, d, h, mi, se;
    if (!utc19 || sscanf(utc19, "%4d-%2d-%2d %2d:%2d:%2d", &y, &mo, &d, &h, &mi, &se) != 6) {
        char raw[20];
        ascii_copy(raw, sizeof raw, utc19 ? utc19 : "", 19);
        return snprintf(out, cap, "%s %s", raw, s);
    }
    pnl_local_t t;
    pnl_local_time(pnl_utc_from_civil(y, mo, d, h, mi, se), offset_s, 1, &t);
    int32_t a = offset_s < 0 ? -offset_s : offset_s;
    return snprintf(out, cap, "%04d-%02d-%02d %02d:%02d:%02d (UTC%c%02d:%02d) %s",
                    t.year, t.mon, t.mday, t.hour, t.min, t.sec,
                    offset_s < 0 ? '-' : '+', (int)(a / 3600), (int)((a % 3600) / 60), s);
}

void pnl_node_readings(const hg_node_t *n, pnl_readings_t *out) {
    memset(out, 0, sizeof *out);
    int ns = n->hb.n_shelves > 4 ? 4 : n->hb.n_shelves;
    if (ns <= 0) return;
    int soil = 0, light = 0, pump = 0, any = 0;
    for (int i = 0; i < ns; i++) {
        const hg_hb_shelf_t *s = &n->hb.shelf[i];
        if (s->pct_a || s->pct_b || s->white || s->red || s->pump_today_s) any = 1;
        soil  += s->pct_a + s->pct_b;
        light += s->white + s->red;
        pump  += s->pump_today_s;
    }
    out->any = (uint8_t)any;
    out->soil_pct  = (soil + ns) / (2 * ns);    /* Math.round(sum / (2 * ns)), half up */
    out->light_pct = (light + ns) / (2 * ns);
    out->pump_s    = pump;
}

int pnl_fmt_reading(const pnl_readings_t *r, int which, char *out, size_t cap) {
    if (!r || !r->any) return snprintf(out, cap, "--");
    switch (which) {
    case 0:  return snprintf(out, cap, "%d%%", r->soil_pct);
    case 1:  return snprintf(out, cap, "%d%%", r->light_pct);
    case 2:  return snprintf(out, cap, "%ds", r->pump_s);
    default: return snprintf(out, cap, "--");
    }
}
```

- [ ] **Step 5: Implement `pnl_home`**

`components/panel_ui/pnl_home.h`:

```c
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "hg_cfg_types.h"   /* HG_MAX_ZONES, HG_MAX_SHELVES, hg_zone_cfg_t, hg_zone_hw_t */
#include "pnl_time.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Home-screen maths (pure). */

typedef enum { PNL_TILE_MIN = 0, PNL_TILE_MID, PNL_TILE_FULL } pnl_tile_detail_t;
     /* MIN: id + dot + soil; MID: name + dot + soil; FULL: name + dot + soil + light */
typedef struct { int tile_w, gap, x0; pnl_tile_detail_t detail; } pnl_band_t;

#define PNL_BAND_WIDTH 976   /* 1024 - 2 x 24 margin */
#define PNL_BAND_GAP   8
#define PNL_TILE_MAX_W 240

/* 0 / -1 (n outside 1..HG_MAX_ZONES or width <= 0). tile_w =
 * min(PNL_TILE_MAX_W, (width - (n-1)*gap) / n), the row centred (x0);
 * tile_w < 140 -> MIN, < 200 -> MID, else FULL (8 tiles -> 115 px MIN;
 * 1 tile -> 240 px FULL). */
int pnl_band_layout(int n_tiles, int width, pnl_band_t *out);

#define PNL_SCHED_MAX (HG_MAX_ZONES * HG_MAX_SHELVES)
typedef struct {
    uint16_t light_on[PNL_SCHED_MAX], light_off[PNL_SCHED_MAX]; int n_light;   /* enabled shelves of enrolled zones */
    uint16_t water_start[PNL_SCHED_MAX], water_end[PNL_SCHED_MAX]; int n_water;/* WATER.MODE AUTO shelves */
} pnl_sched_t;

void pnl_sched_reset(pnl_sched_t *s);
/* Adds shelves i < (hw ? hw->shelf_count : HG_MAX_SHELVES) with enabled != 0;
 * a shelf's watering window only when WATER.MODE is AUTO (1). Bounded by
 * PNL_SCHED_MAX. */
void pnl_sched_add_zone(pnl_sched_t *s, const hg_zone_cfg_t *cfg, const hg_zone_hw_t *hw_or_null);

/* 1 when any light is on at minute_of_day: on<off: on<=now<off; on>off wraps
 * midnight; on==off (the validator forbids it) never. */
int  pnl_lights_on_now(const pnl_sched_t *s, int minute_of_day);

/* !valid -> "Clock not set"; a light on -> "Lights off in 2h 14m" (earliest
 * off; "Lights off in 14m" under an hour); else a light schedule -> "Lights on
 * at 06:00" (soonest on); else watering: "Watering on demand" when a window
 * has start == end (always open), "Watering window open until 18:00" inside a
 * window, else "Next watering 06:00"; else "" (returns 0). */
int  pnl_ctx_line(const pnl_sched_t *s, const pnl_local_t *t, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
```

`components/panel_ui/pnl_home.c`:

```c
#include <stdio.h>
#include <string.h>
#include "pnl_home.h"

int pnl_band_layout(int n, int width, pnl_band_t *out) {
    if (!out || n < 1 || n > HG_MAX_ZONES || width <= 0) return -1;
    int w = (width - (n - 1) * PNL_BAND_GAP) / n;
    if (w > PNL_TILE_MAX_W) w = PNL_TILE_MAX_W;
    out->tile_w = w;
    out->gap = PNL_BAND_GAP;
    out->x0 = (width - (n * w + (n - 1) * PNL_BAND_GAP)) / 2;
    out->detail = w < 140 ? PNL_TILE_MIN : (w < 200 ? PNL_TILE_MID : PNL_TILE_FULL);
    return 0;
}

void pnl_sched_reset(pnl_sched_t *s) { memset(s, 0, sizeof *s); }

void pnl_sched_add_zone(pnl_sched_t *s, const hg_zone_cfg_t *cfg, const hg_zone_hw_t *hw_or_null) {
    int ns = hw_or_null ? hw_or_null->shelf_count : HG_MAX_SHELVES;
    if (ns > HG_MAX_SHELVES) ns = HG_MAX_SHELVES;
    for (int i = 0; i < ns; i++) {
        const hg_shelf_cfg_t *sh = &cfg->shelf[i];
        if (!sh->enabled) continue;
        if (s->n_light < PNL_SCHED_MAX) {
            s->light_on[s->n_light]  = sh->light.on_min;
            s->light_off[s->n_light] = sh->light.off_min;
            s->n_light++;
        }
        if (sh->water.mode == 1 && s->n_water < PNL_SCHED_MAX) {
            s->water_start[s->n_water] = sh->water.win_start_min;
            s->water_end[s->n_water]   = sh->water.win_end_min;
            s->n_water++;
        }
    }
}

static int in_window(int start, int end, int now) {
    if (start < end) return now >= start && now < end;
    if (start > end) return now >= start || now < end;
    return 0;
}

static int until(int target, int now) { return (target - now + 1440) % 1440; }

int pnl_lights_on_now(const pnl_sched_t *s, int now) {
    for (int i = 0; i < s->n_light; i++)
        if (in_window(s->light_on[i], s->light_off[i], now)) return 1;
    return 0;
}

int pnl_ctx_line(const pnl_sched_t *s, const pnl_local_t *t, char *out, size_t cap) {
    if (!t || !t->valid) return snprintf(out, cap, "Clock not set");
    int now = t->minute_of_day;

    int best = -1;
    for (int i = 0; i < s->n_light; i++) {
        if (!in_window(s->light_on[i], s->light_off[i], now)) continue;
        int d = until(s->light_off[i], now);
        if (best < 0 || d < best) best = d;
    }
    if (best >= 0) {
        if (best >= 60) return snprintf(out, cap, "Lights off in %dh %dm", best / 60, best % 60);
        return snprintf(out, cap, "Lights off in %dm", best);
    }

    int on_at = -1;
    best = -1;
    for (int i = 0; i < s->n_light; i++) {
        if (s->light_on[i] == s->light_off[i]) continue;
        int d = until(s->light_on[i], now);
        if (best < 0 || d < best) { best = d; on_at = s->light_on[i]; }
    }
    if (on_at >= 0) return snprintf(out, cap, "Lights on at %02d:%02d", on_at / 60, on_at % 60);

    for (int i = 0; i < s->n_water; i++)
        if (s->water_start[i] == s->water_end[i]) return snprintf(out, cap, "Watering on demand");

    int end_at = -1;
    best = -1;
    for (int i = 0; i < s->n_water; i++) {
        if (!in_window(s->water_start[i], s->water_end[i], now)) continue;
        int d = until(s->water_end[i], now);
        if (best < 0 || d < best) { best = d; end_at = s->water_end[i]; }
    }
    if (end_at >= 0) return snprintf(out, cap, "Watering window open until %02d:%02d", end_at / 60, end_at % 60);

    int start_at = -1;
    best = -1;
    for (int i = 0; i < s->n_water; i++) {
        int d = until(s->water_start[i], now);
        if (best < 0 || d < best) { best = d; start_at = s->water_start[i]; }
    }
    if (start_at >= 0) return snprintf(out, cap, "Next watering %02d:%02d", start_at / 60, start_at % 60);

    if (cap) out[0] = '\0';
    return 0;
}
```

- [ ] **Step 6: Make the two names public in `state_snap`**

In `components/state_snap/state_snap.h`, add after `typedef int (*snap_write_fn)...` (`:33`):

```c
/* The web's vocabulary (map-parity A12), shared with the panel. Pure.
 * Anything unknown is "EMPTY" / "IDLE", exactly what the JSON has always said. */
const char *state_snap_health_name(node_health_t h);   /* "ONLINE" "DEGRADED" "OFFLINE" "UPDATING" "EMPTY" */
const char *state_snap_ring_state_name(ring_state_t s);/* "OK" "OPEN" "IDLE" */
```

In `components/state_snap/state_snap.c`:
- replace `static const char *health_name(node_health_t h) {   /* mirrors master_cmds.c's health_name() */` (`:55`) with `const char *state_snap_health_name(node_health_t h) {   /* mirrors master_cmds.c's health_name() */`;
- add after that function:

```c
const char *state_snap_ring_state_name(ring_state_t s) {
    return s == RING_ST_OPEN ? "OPEN" : s == RING_ST_OK ? "OK" : "IDLE";
}
```

- in `ss_node()`, `jstr(buf, cap, off, health_name(nd->health))` becomes `jstr(buf, cap, off, state_snap_health_name(nd->health))`;
- in `ss_ring()`, replace `const char *state = rs->state == RING_ST_OPEN ? "OPEN" : rs->state == RING_ST_OK ? "OK" : "IDLE";` with `const char *state = state_snap_ring_state_name(rs->state);`.

- [ ] **Step 7: Run the tests to verify they pass**

Run the one-test command with `-R "test_pnl_(time|fmt|home)|test_state_snap"`.
Expected: `100% tests passed, 0 tests failed out of 4`.

- [ ] **Step 8: Compile the helpers on the target too**

`components/panel_ui/CMakeLists.txt`: the `set(PANEL_SRCS ...)` line becomes:

```cmake
    set(PANEL_SRCS "panel_ui.c" "panel_hw.c" "panel_lock.c" "pnl_touch.c" "pnl_theme.c" "scr_diag.c" "pnl_worker.c"
                   "pnl_time.c" "pnl_fmt.c" "pnl_home.c")
```

Run GATE-HOST. Expected: 43/43 tests pass (+3).
Run GATE-ESP32. Expected: master, zone and rescue each print `Project build complete` with 0 warnings, and the lock diff is empty. `state_snap` is shared by every app.
Run GATE-P4. Expected: `Project build complete`, 0 warnings, the target grep matches, then the lock checkout. The pure helpers compile under RISC-V GCC with `-Werror`.

- [ ] **Step 9: Commit**

```powershell
git -C C:\Projects\HillGrov add components/panel_ui/pnl_time.h components/panel_ui/pnl_time.c components/panel_ui/pnl_fmt.h components/panel_ui/pnl_fmt.c components/panel_ui/pnl_home.h components/panel_ui/pnl_home.c components/panel_ui/CMakeLists.txt components/state_snap/state_snap.h components/state_snap/state_snap.c tests/host/test_pnl_time.c tests/host/test_pnl_fmt.c tests/host/test_pnl_home.c tests/host/test_state_snap.c tests/host/CMakeLists.txt
git -C C:\Projects\HillGrov commit -m "feat(panel_ui): pure time, formatting and home-screen helpers" -m "Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>"
```

### Task 11: Poller: 1 Hz snapshot, 5 s Wi-Fi status, schedules, LVGL heartbeat (Stage 1)

Reads leave the LVGL task too, not only writes:
- `nmgr_lock` waits forever and is held across NVS writes;
- on the P4, `wifi_mgr_status()` is two esp_hosted RPCs of up to 5 s each.

So a poller task gathers everything once a second and publishes a **copy**, and the LVGL task only ever reads that copy. A separate 5 s Wi-Fi task keeps a silent C6 from freezing ring and zone data (D23). The poller also watches the LVGL task: the LVGL task is deliberately not TWDT-subscribed (D5), so a freeze is reported as an alarm rather than a reboot.

**Files:**
- Create: `components/panel_ui/pnl_poll.h`, `.c`.
- Modify:
  - `components/panel_ui/panel_ui.c`:
    - `panel_services_start()` → `pnl_poll_start()` after `pnl_worker_start()`;
    - a 1 s `lv_timer` calls `pnl_lvgl_heartbeat()`.
  - `components/panel_ui/scr_diag.c`: add a "poll seq N" line.
  - `components/panel_ui/CMakeLists.txt`.

**Interfaces:**
- Consumes:
  - from Task 9: `psvc_state_fill` and `psvc_state_t`;
  - from Task 5: `psvc_zone_cfg_get` and `psvc_zone_cfg_busy`;
  - from Task 10: `pnl_sched_*`;
  - `wifi_mgr_status` (`wifi_mgr.h:56`) and `notify_emit` (`notify.h:25`).
- Produces (`pnl_poll.h`):
  ```c
  typedef struct {
      psvc_state_t st;                       /* st.wifi comes from pnl_wifi (5 s) -- a silent C6 costs 5 s per RPC */
      uint8_t      cfg_busy[HG_MAX_ZONES];   /* psvc_zone_cfg_busy per used zone ("save landed" = 0) */
      pnl_sched_t  sched;                    /* rebuilt every 10 s or when any used node's hb.cfg_gen changed */
      uint8_t      panel_cmd_quarantined;    /* Task 22 fills it; 0 before */
      uint8_t      started;                  /* 0 until the first fill */
      uint32_t     seq;
  } pnl_snap_t;
  void     pnl_poll_start(void);             /* tasks pnl_poll c0/p2/6144 and pnl_wifi c0/p1/4096, internal stacks, unsubscribed;
                                                double buffer in PSRAM guarded by a portMUX; seq++ per publish */
  void     pnl_poll_latest(pnl_snap_t *out); /* [ANY] copy of the last publish */
  uint32_t pnl_poll_seq(void);               /* [ANY] */
  void     pnl_poll_kick(void);              /* [ANY] xTaskNotifyGive -> refill now (after a save) */
  void     pnl_poll_stack_free(uint32_t *poll_b, uint32_t *wifi_b);   /* [ANY] stack high-water marks in bytes of
                                                pnl_poll and pnl_wifi (0 for a task that does not exist) -- Panel > About */
  void     pnl_lvgl_heartbeat(void);         /* [LVGL] 1 s timer; the poller logs ERROR and notify_emit(NTF_ALARM, 0,
                                                "W_PANEL_FROZEN ...") once after 10 s without a beat (active alarm "ALARM 0"),
                                                then notify_reset + "CLEARED PANEL_FROZEN" on the next beat (clears it) */
  ```

**What is provable where.** Tasks and queues only, with no pure logic beyond what Task 10 already tests (+0). GATE-P4 proves it compiles. The bench proves that `seq` advances about once a second, and keeps advancing while a web Wi-Fi scan parks the radio for up to 30 s. That is the whole point of the separate Wi-Fi task.

The double buffer is a private *stage* the poller fills without any lock, plus a published copy. The published copy is written and read with `memcpy` under one `portMUX`. At about 2.2 KB that is a few microseconds with interrupts off on one core, and never a mutex the LVGL task could wait on.

`pnl_snap_t` is about 2.2 KB. Callers keep it `static` or in PSRAM, never on the 8 KB LVGL stack. The shell (Task 12) holds the one LVGL-side copy.

- [ ] **Step 1: Write the poller**

`components/panel_ui/pnl_poll.h`:

```c
#pragma once
#include <stdint.h>
#include "hg_cfg_types.h"
#include "psvc_state.h"
#include "pnl_home.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The panel's read side (panel plan Global Constraints, "Reads leave the LVGL
 * task too"). pnl_poll gathers once a second on its own task and publishes a
 * COPY; the LVGL task only ever copies that out (pnl_poll_latest) and diffs it
 * against what it shows. ~2.2 KB: keep a pnl_snap_t static or in PSRAM, never
 * on the LVGL task's 8 KB stack. */
typedef struct {
    psvc_state_t st;                       /* st.wifi comes from pnl_wifi (5 s) -- a silent C6 costs 5 s per RPC */
    uint8_t      cfg_busy[HG_MAX_ZONES];   /* psvc_zone_cfg_busy per used zone ("save landed" = 0) */
    pnl_sched_t  sched;                    /* rebuilt every 10 s or when any used node's hb.cfg_gen changed */
    uint8_t      panel_cmd_quarantined;    /* Task 22 fills it; 0 before */
    uint8_t      started;                  /* 0 until the first fill */
    uint32_t     seq;
} pnl_snap_t;

/* Tasks pnl_poll (core 0, prio 2, 6144 B) and pnl_wifi (core 0, prio 1,
 * 4096 B), internal stacks, NEITHER TWDT-subscribed (both can sit in an
 * esp_hosted RPC or behind nmgr_lock). Stage + published copy in PSRAM,
 * guarded by a portMUX. Idempotent. */
void     pnl_poll_start(void);
void     pnl_poll_latest(pnl_snap_t *out); /* [ANY] copy of the last publish (all zero, started 0, before the first) */
uint32_t pnl_poll_seq(void);               /* [ANY] increments once per publish */
void     pnl_poll_kick(void);              /* [ANY] refill now instead of at the next second (after a save) */

/* [ANY] Smallest free stack, in bytes, pnl_poll and pnl_wifi have had
 * (uxTaskGetStackHighWaterMark); 0 for a task that was not created. Either
 * pointer may be NULL. */
void     pnl_poll_stack_free(uint32_t *poll_b, uint32_t *wifi_b);

/* [LVGL] Called by a 1 s lv_timer. The poller logs ERROR and emits
 * NOTIFY ALARM 0 W_PANEL_FROZEN LVGL task silent <N>s once after 10 s without
 * a beat (an ACTIVE alarm, key "ALARM 0"), and NOTIFY ALARM 0 CLEARED
 * PANEL_FROZEN on the next beat after that (D5), which clears it. */
void     pnl_lvgl_heartbeat(void);

#ifdef __cplusplus
}
#endif
```

`components/panel_ui/pnl_poll.c`:

```c
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "notify.h"
#include "wifi_mgr.h"
#include "psvc_state.h"
#include "psvc_zcfg.h"
#include "pnl_poll.h"

static const char *TAG = "pnl_poll";

#define POLL_PERIOD_MS   1000u
#define WIFI_PERIOD_MS   5000u
#define SCHED_PERIOD_MS 10000u
#define FROZEN_MS       10000u

static pnl_snap_t       *s_stage;   /* the poller's private buffer: filled with no lock held */
static pnl_snap_t       *s_pub;     /* the published copy: memcpy in and out under s_mux only */
static portMUX_TYPE      s_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t s_seq;
static TaskHandle_t      s_task;
static TaskHandle_t      s_wifi_task;

static wifi_status_t     s_wifi;
static portMUX_TYPE      s_wifi_mux = portMUX_INITIALIZER_UNLOCKED;

static volatile uint32_t s_beat_ms;   /* 0 = the LVGL heartbeat timer never ran (panel dark) */
static uint8_t           s_frozen;

static uint8_t           s_sched_valid;
static uint32_t          s_sched_ms;
static uint8_t           s_sched_used[HG_MAX_ZONES];
static uint32_t          s_sched_gen[HG_MAX_ZONES];
static hg_zone_cfg_t     s_cfg;       /* scratch for the schedule rebuild, off the 6 KB stack */
static hg_zone_hw_t      s_hw;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static int sched_stale(const pnl_snap_t *sn, uint32_t now) {
    if (!s_sched_valid || now - s_sched_ms >= SCHED_PERIOD_MS) return 1;
    for (int i = 0; i < HG_MAX_ZONES; i++) {
        const hg_node_t *n = &sn->st.node[i];
        if (n->used != s_sched_used[i]) return 1;
        if (n->used && n->hb.cfg_gen != s_sched_gen[i]) return 1;
    }
    return 0;
}

static void sched_rebuild(pnl_snap_t *sn, uint32_t now) {
    pnl_sched_reset(&sn->sched);
    for (int i = 0; i < HG_MAX_ZONES; i++) {
        const hg_node_t *n = &sn->st.node[i];
        s_sched_used[i] = n->used;
        s_sched_gen[i] = n->hb.cfg_gen;
        if (!n->used) continue;
        int hp = 0;
        if (psvc_zone_cfg_get((uint8_t)(i + 1), &s_cfg, &s_hw, NULL, &hp) != PSVC_OK) continue;
        pnl_sched_add_zone(&sn->sched, &s_cfg, hp ? &s_hw : NULL);
    }
    s_sched_ms = now;
    s_sched_valid = 1;
}

static void heartbeat_check(uint32_t now) {
    uint32_t beat = s_beat_ms;
    if (!beat) return;
    if (!s_frozen && now - beat > FROZEN_MS) {
        s_frozen = 1;
        ESP_LOGE(TAG, "LVGL task silent for %u ms -- the panel is frozen", (unsigned)(now - beat));
        /* alarm_mgr's active set keys on the first word: a W_ prefix activates "ALARM 0" (alarm_mgr.c:12-23), so the
         * band, the web banner and alarms_active all show it -- "PANEL FROZEN" would only reach the history ring. */
        notify_emit(NTF_ALARM, 0, "W_PANEL_FROZEN LVGL task silent %us", (unsigned)((now - beat) / 1000u));
    } else if (s_frozen && now - beat <= FROZEN_MS) {
        s_frozen = 0;
        ESP_LOGW(TAG, "LVGL task beating again");
        /* NTF_ALARM is rate-limited to 1 s per idx (notify.c:21-23) and a pnl_poll_kick() can bring the next poll
         * sooner: reset the latch so the clearing line is never dropped (it would leave the alarm up for good). */
        notify_reset(NTF_ALARM, 0);
        notify_emit(NTF_ALARM, 0, "CLEARED PANEL_FROZEN");   /* CLEARED is a clearing word: "ALARM 0" leaves the set */
    }
}

static void poll_task(void *arg) {
    (void)arg;
    for (;;) {
        uint32_t t0 = now_ms();
        psvc_state_fill(&s_stage->st, PSVC_FILL_SKIP_WIFI);
        portENTER_CRITICAL(&s_wifi_mux);
        s_stage->st.wifi = s_wifi;
        portEXIT_CRITICAL(&s_wifi_mux);
        for (int i = 0; i < HG_MAX_ZONES; i++)
            s_stage->cfg_busy[i] = s_stage->st.node[i].used
                                   ? (uint8_t)(psvc_zone_cfg_busy((uint8_t)(i + 1)) ? 1 : 0) : 0;
        if (sched_stale(s_stage, t0)) sched_rebuild(s_stage, t0);
        heartbeat_check(t0);
        s_stage->started = 1;
        s_stage->seq = s_seq + 1;

        portENTER_CRITICAL(&s_mux);
        memcpy(s_pub, s_stage, sizeof *s_pub);
        s_seq = s_stage->seq;
        portEXIT_CRITICAL(&s_mux);

        uint32_t spent = now_ms() - t0;
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(spent >= POLL_PERIOD_MS ? 1u : POLL_PERIOD_MS - spent));
    }
}

static void wifi_task(void *arg) {
    (void)arg;
    wifi_status_t w;
    for (;;) {
        memset(&w, 0, sizeof w);
        wifi_mgr_status(&w);   /* P4: two esp_hosted RPCs, up to 5 s each -- here and only here */
        portENTER_CRITICAL(&s_wifi_mux);
        s_wifi = w;
        portEXIT_CRITICAL(&s_wifi_mux);
        vTaskDelay(pdMS_TO_TICKS(WIFI_PERIOD_MS));
    }
}

void pnl_poll_start(void) {
    if (s_task) return;
    s_stage = heap_caps_calloc(1, sizeof(pnl_snap_t), MALLOC_CAP_SPIRAM);
    s_pub   = heap_caps_calloc(1, sizeof(pnl_snap_t), MALLOC_CAP_SPIRAM);
    if (!s_stage || !s_pub) {
        ESP_LOGE(TAG, "no PSRAM for the snapshot buffers -- the panel stays on \"starting\"");
        return;
    }
    if (xTaskCreatePinnedToCore(wifi_task, "pnl_wifi", 4096, NULL, 1, &s_wifi_task, 0) != pdPASS) {
        s_wifi_task = NULL;
        ESP_LOGE(TAG, "pnl_wifi task not created -- Wi-Fi lines stay blank");
    }
    if (xTaskCreatePinnedToCore(poll_task, "pnl_poll", 6144, NULL, 2, &s_task, 0) != pdPASS) {
        ESP_LOGE(TAG, "pnl_poll task not created -- the panel stays on \"starting\"");
        s_task = NULL;
        return;
    }
    ESP_LOGI(TAG, "poller up (1 Hz state, %u s Wi-Fi, not TWDT-subscribed)", (unsigned)(WIFI_PERIOD_MS / 1000));
}

void pnl_poll_latest(pnl_snap_t *out) {
    if (!s_pub) { memset(out, 0, sizeof *out); return; }
    portENTER_CRITICAL(&s_mux);
    memcpy(out, s_pub, sizeof *out);
    portEXIT_CRITICAL(&s_mux);
}

uint32_t pnl_poll_seq(void) { return s_seq; }

void pnl_poll_stack_free(uint32_t *poll_b, uint32_t *wifi_b) {
    if (poll_b) *poll_b = s_task ? (uint32_t)uxTaskGetStackHighWaterMark(s_task) : 0;
    if (wifi_b) *wifi_b = s_wifi_task ? (uint32_t)uxTaskGetStackHighWaterMark(s_wifi_task) : 0;
}

void pnl_poll_kick(void) {
    TaskHandle_t t = s_task;
    if (t) xTaskNotifyGive(t);
}

void pnl_lvgl_heartbeat(void) {
    uint32_t t = now_ms();
    s_beat_ms = t ? t : 1u;
}
```

- [ ] **Step 2: Start it, beat it, show it**

In `components/panel_ui/panel_ui.c`, add `#include "pnl_poll.h"`. Add above `panel_start()`:

```c
/* [LVGL] 1 s: proves the LVGL task is still turning (the poller watches it). */
static void heartbeat_cb(lv_timer_t *t) {
    (void)t;
    pnl_lvgl_heartbeat();
}
```

and replace `panel_services_start()` with:

```c
int panel_services_start(void) {
    if (!s_lit) return -1;   /* a dark panel runs no worker or poller */
    pnl_worker_start();
    pnl_poll_start();
    if (panel_lock(2000)) {
        (void)lv_timer_create(heartbeat_cb, 1000, NULL);
        panel_unlock();
    } else {
        ESP_LOGE(TAG, "display lock not taken -- no LVGL heartbeat, a frozen panel would go unreported");
    }
    return 0;
}
```

In `components/panel_ui/scr_diag.c`:
- add `#include "sdkconfig.h"` (`CONFIG_LV_MEM_SIZE_KILOBYTES`) and `#include "pnl_poll.h"`;
- change `static lv_obj_t   *s_heap, *s_touch;` to `static lv_obj_t   *s_heap, *s_touch, *s_poll;`;
- in `scr_diag_build()`, after the `s_touch` label's `lv_obj_align(...)`, add:

```c
    s_poll = pnl_label(parent, "Poller: not started", &lv_font_montserrat_20, PNL_C_MUTED);
    lv_obj_align(s_poll, LV_ALIGN_TOP_MID, 0, 136);
```

- at the end of `diag_tick()`, add:

```c
    if (s_poll) {
        /* The LVGL pool figure every stage gate records (Global Constraints,
         * "Memory": raise CONFIG_LV_MEM_SIZE_KILOBYTES only if max_used > 75 % of
         * it). The budget is the INTERNAL pool; mon.total_size also counts Task 7's
         * PSRAM overflow pool, so it is not the denominator. */
        lv_mem_monitor_t mon;
        lv_mem_monitor(&mon);
        snprintf(buf, sizeof buf, "Poller: seq %u | LVGL pool: max used %u of %u KB internal (+%u KB PSRAM)",
                 (unsigned)pnl_poll_seq(), (unsigned)(mon.max_used / 1024), (unsigned)CONFIG_LV_MEM_SIZE_KILOBYTES,
                 (unsigned)(mon.total_size / 1024 > CONFIG_LV_MEM_SIZE_KILOBYTES
                            ? mon.total_size / 1024 - CONFIG_LV_MEM_SIZE_KILOBYTES : 0));
        lv_label_set_text(s_poll, buf);
    }
```

`components/panel_ui/CMakeLists.txt`: the `set(PANEL_SRCS ...)` line becomes:

```cmake
    set(PANEL_SRCS "panel_ui.c" "panel_hw.c" "panel_lock.c" "pnl_touch.c" "pnl_theme.c" "scr_diag.c" "pnl_worker.c"
                   "pnl_time.c" "pnl_fmt.c" "pnl_home.c" "pnl_poll.c")
```

- [ ] **Step 3: Run the gates**

Run GATE-HOST. Expected: 43/43 tests pass.
Run GATE-P4. Expected: `Project build complete`, 0 warnings, the target grep matches, then the lock checkout.
Run GATE-ESP32. Expected: master, zone and rescue each print `Project build complete` with 0 warnings, and the lock diff is empty. Then re-run the GATE-P4 build line and the lock checkout, for flashing.

- [ ] **Step 4: Bench check: the poller never stalls behind the radio**

Run FLASH-P4, and watch the boot with the console. Expected: `pnl_poll: poller up (1 Hz state, 5 s Wi-Fi, not TWDT-subscribed)`.

On glass the diag line `Poller: seq N` should advance about once a second.

With the PC on the AP, start a scan: `python C:\Projects\HillGrov\tools\web_test.py 192.168.7.7 --password hillgrow1 --only wifi`, or tap Scan in the web UI's System page. While the scan runs, watch the panel: the `seq` number must keep advancing. The AP itself drops for the length of the scan, which is expected.

- [ ] **Step 5: Commit**

```powershell
git -C C:\Projects\HillGrov add components/panel_ui/pnl_poll.h components/panel_ui/pnl_poll.c components/panel_ui/panel_ui.c components/panel_ui/scr_diag.c components/panel_ui/CMakeLists.txt
git -C C:\Projects\HillGrov commit -m "feat(panel_ui): poller publishes snapshot copies; LVGL heartbeat alarm" -m "Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>"
```

### Task 12: Shell (left rail, navigation, destination registry) and the Dashboard (Stage 1)

The HillGrow app gets its persistent left rail and its five destinations (spec *The HillGrow app*). Screens are built lazily: only the open destination's widget tree exists, and it is deleted on navigation. The Dashboard lands with the web's content in the web's order.

**Files:**
- Create: `components/panel_ui/scr_shell.h`, `.c`; `scr_dashboard.c`.
- Modify:
  - `components/panel_ui/scr_diag.c`: add the ops const `PNL_SCR_DIAG` and its teardown.
  - `components/panel_ui/panel_ui.c`: build with `pnl_shell_start()` in place of `scr_diag_build`.
  - `components/panel_ui/CMakeLists.txt`.

**Interfaces:**
- Consumes:
  - from Task 11: `pnl_poll_latest` and `pnl_poll_seq`;
  - from Task 10: `pnl_fmt_*`, `pnl_zone_name`, `pnl_node_readings` and `state_snap_*_name`;
  - from Task 8: `pnl_screen_gen_bump`;
  - from Task 7: `pnl_theme_*`, `pnl_label` and `pnl_health_color`.
- Produces (`scr_shell.h`, glue):
  ```c
  typedef enum { PNL_DEST_HOME = 0, PNL_DEST_DASHBOARD, PNL_DEST_ZONE, PNL_DEST_CONFIG, PNL_DEST_ALARMS, PNL_DEST_SYSTEM,
                 PNL_DEST_PANEL, PNL_DEST_AUDIO, PNL_DEST_DIAG, PNL_DEST_COUNT } pnl_dest_t;
  typedef struct {
      const char *title;
      void (*build)(lv_obj_t *content, int arg);   /* LVGL task; widgets under content only */
      void (*update)(const pnl_snap_t *snap);      /* LVGL task; on every new poll seq; NULL allowed */
      void (*teardown)(void);                      /* LVGL task; before content is deleted: drop widget pointers */
      uint8_t in_rail;                             /* Dashboard, Zone, Config, Alarms, System */
  } pnl_screen_ops_t;
  extern const pnl_screen_ops_t PNL_SCR_HOME, PNL_SCR_DASHBOARD, PNL_SCR_ZONE, PNL_SCR_CONFIG, PNL_SCR_ALARMS,
                                PNL_SCR_SYSTEM, PNL_SCR_PANEL, PNL_SCR_AUDIO, PNL_SCR_DIAG, PNL_SCR_PLACEHOLDER;
       /* PLACEHOLDER shows "<title>: not available yet". Registry in scr_shell.c: one line per destination; each later
          task swaps its own line (HOME -> DIAG until Task 13; ZONE/CONFIG/ALARMS/SYSTEM/PANEL/AUDIO -> PLACEHOLDER) */
  void       pnl_shell_start(void);            /* root screen, rail (120 px) + content; 250 ms timer calls update() on seq change */
  void       pnl_nav_go(pnl_dest_t d, int arg);/* [LVGL] teardown -> pnl_screen_gen_bump -> build; arg: zone id for ZONE/CONFIG
                                                  (0 = master config, -1 = last used) */
  pnl_dest_t pnl_nav_current(void);
  int        pnl_nav_arg(void);
  void       pnl_label_set_if_changed(lv_obj_t *label, const char *text);   /* the "update only what changed" primitive */
  ```
  Dashboard (`PNL_SCR_DASHBOARD`), in the web's order:
  1. Ring banner: "Ring: OK", or "Ring: OPEN -- <blame>".
  2. Default-password banner: "Factory default password still in use (web, Wi-Fi AP) -- change it in System.", naming only the flags that are set, with a button to System.
  3. Master card: `v<version>`; `pnl_fmt_master_time`; STA and AP lines; "Heap min N KB".
  4. "N web console slot(s) degraded", when `web_cmd_quarantined > 0`.
  5. One card per used node: name, health badge, `fw x.y.z`, a stale pill, "last seen Ns ago", and Soil, Light and Pump. Tapping a card goes to `PNL_DEST_ZONE` with that id.
- Interface additions (`scr_shell.h`):
  - `const char *pnl_nav_title(void)`: the current destination's registry title, used by the shared placeholder.
  - `const pnl_snap_t *pnl_shell_snap(void)`: the shell's one LVGL-side copy of the last poll, valid on the LVGL task. `build()` reads it, so no screen puts a 2.2 KB copy on the LVGL stack.
  - `void pnl_obj_show(lv_obj_t *o, int show)`: toggle `LV_OBJ_FLAG_HIDDEN` only when it changes.
- Design decisions the executor must keep:
  - **Rail membership comes from the registry row**, not from `ops->in_rail`, because one ops struct (the placeholder) serves rail and non-rail destinations alike. Real screens still set `in_rail` truthfully.
  - `pnl_nav_go()` is **deferred** with `lv_async_call()`. A tap handler that navigates would otherwise delete the very object whose event is being dispatched.
  - Every destination builds into a fresh full-size **page** object, deleted on navigation. Screens may style their page freely (flex, padding, scroll) without leaking into the next screen.
  - Until Task 13, HOME is the diagnostics screen **shown with the rail**, so the rail is reachable from boot. Task 13 turns HOME into the full-screen home.
  - Non-rail destinations other than HOME get a "Home" button in the top-left corner, drawn by the shell above the page.
  - Every build and update is timed with `esp_timer_get_time()` and logs WARN over 200 ms.

**What is provable where.** LVGL glue only (+0 host tests). GATE-P4 compiles it. The bench proves the rail switches destinations, and that the Dashboard matches the web's dashboard value for value with two zones. Before each Stage 1 flash, the same checks are made on glass.

- [ ] **Step 1: Write the shell**

`components/panel_ui/scr_shell.h`:

```c
#pragma once
#include <stdint.h>
#include "lvgl.h"
#include "pnl_poll.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The panel shell: home + the HillGrow app's persistent left rail (spec "The
 * HillGrow app": five destinations, matching the web) + the panel-local
 * screens. Only the open destination's widget tree exists; it is built into a
 * fresh page on navigation and deleted on the next. Everything here is [LVGL]. */

typedef enum { PNL_DEST_HOME = 0, PNL_DEST_DASHBOARD, PNL_DEST_ZONE, PNL_DEST_CONFIG, PNL_DEST_ALARMS, PNL_DEST_SYSTEM,
               PNL_DEST_PANEL, PNL_DEST_AUDIO, PNL_DEST_DIAG, PNL_DEST_COUNT } pnl_dest_t;

typedef struct {
    const char *title;
    void (*build)(lv_obj_t *content, int arg);   /* LVGL task; widgets under content only */
    void (*update)(const pnl_snap_t *snap);      /* LVGL task; on every new poll seq; NULL allowed */
    void (*teardown)(void);                      /* LVGL task; before content is deleted: drop widget pointers */
    uint8_t in_rail;                             /* Dashboard, Zone, Config, Alarms, System */
} pnl_screen_ops_t;

extern const pnl_screen_ops_t PNL_SCR_HOME, PNL_SCR_DASHBOARD, PNL_SCR_ZONE, PNL_SCR_CONFIG, PNL_SCR_ALARMS,
                              PNL_SCR_SYSTEM, PNL_SCR_PANEL, PNL_SCR_AUDIO, PNL_SCR_DIAG, PNL_SCR_PLACEHOLDER;

/* Root screen, the 120 px rail and the content area; a 250 ms timer calls the
 * open screen's update() whenever pnl_poll_seq() moved; opens HOME. Call once,
 * under panel_lock(). */
void       pnl_shell_start(void);

/* teardown -> pnl_screen_gen_bump -> build, DEFERRED to the next LVGL cycle
 * (lv_async_call), so a tap handler may call it safely. arg: the zone id for
 * ZONE/CONFIG (0 = master config, -1 = the last one used there), else 0. */
void       pnl_nav_go(pnl_dest_t d, int arg);
pnl_dest_t pnl_nav_current(void);
int        pnl_nav_arg(void);
const char *pnl_nav_title(void);              /* the current destination's registry title */

/* The shell's LVGL-side copy of the last poll (never NULL after
 * pnl_shell_start(); started == 0 until the first publish). */
const pnl_snap_t *pnl_shell_snap(void);

void       pnl_label_set_if_changed(lv_obj_t *label, const char *text);   /* the "update only what changed" primitive */
void       pnl_obj_show(lv_obj_t *o, int show);                           /* toggles LV_OBJ_FLAG_HIDDEN only on change */

#ifdef __cplusplus
}
#endif
```

`components/panel_ui/scr_shell.c`:

```c
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "lvgl.h"
#include "pnl_palette.h"
#include "pnl_theme.h"
#include "pnl_worker.h"
#include "pnl_poll.h"
#include "scr_shell.h"

static const char *TAG = "pnl_shell";

#define RAIL_W       120
#define SCREEN_W     1024
#define SCREEN_H     600
#define CB_BUDGET_US 200000   /* any single build/update callback: <= 200 ms (both idle tasks are TWDT-watched) */

typedef struct { const char *title; const pnl_screen_ops_t *ops; uint8_t rail; } pnl_reg_t;

/* The destination registry: ONE line per destination. Later tasks swap their
 * own line: HOME (Task 13), ZONE (14), ALARMS (16), CONFIG (20), SYSTEM (23),
 * PANEL (13, then 26), AUDIO (13). rail = shown with the left rail. */
static const pnl_reg_t REG[PNL_DEST_COUNT] = {
    [PNL_DEST_HOME]      = { "Home",       &PNL_SCR_DIAG,        1 },   /* Task 13: &PNL_SCR_HOME, rail 0 */
    [PNL_DEST_DASHBOARD] = { "Dashboard",  &PNL_SCR_DASHBOARD,   1 },
    [PNL_DEST_ZONE]      = { "Zone",       &PNL_SCR_PLACEHOLDER, 1 },   /* Task 14 */
    [PNL_DEST_CONFIG]    = { "Config",     &PNL_SCR_PLACEHOLDER, 1 },   /* Task 20 */
    [PNL_DEST_ALARMS]    = { "Alarms",     &PNL_SCR_PLACEHOLDER, 1 },   /* Task 16 */
    [PNL_DEST_SYSTEM]    = { "System",     &PNL_SCR_PLACEHOLDER, 1 },   /* Task 23 */
    [PNL_DEST_PANEL]     = { "Panel",      &PNL_SCR_PLACEHOLDER, 0 },   /* Task 13: &PNL_SCR_DIAG; Task 26 */
    [PNL_DEST_AUDIO]     = { "Audio",      &PNL_SCR_PLACEHOLDER, 0 },   /* Task 13 */
    [PNL_DEST_DIAG]      = { "Touch test", &PNL_SCR_DIAG,        0 },
};

#define RAIL_N 6
static const pnl_dest_t   RAIL_DEST[RAIL_N] = { PNL_DEST_HOME, PNL_DEST_DASHBOARD, PNL_DEST_ZONE,
                                                PNL_DEST_CONFIG, PNL_DEST_ALARMS, PNL_DEST_SYSTEM };
static const char *const  RAIL_ICON[RAIL_N] = { LV_SYMBOL_HOME, LV_SYMBOL_LIST, LV_SYMBOL_EYE_OPEN,
                                                LV_SYMBOL_SETTINGS, LV_SYMBOL_BELL, LV_SYMBOL_WIFI };
static const char *const  RAIL_TEXT[RAIL_N] = { "Home", "Dashboard", "Zone", "Config", "Alarms", "System" };

static lv_obj_t   *s_rail, *s_content, *s_page, *s_back;
static lv_obj_t   *s_rail_btn[RAIL_N];
static pnl_dest_t  s_cur = PNL_DEST_COUNT;
static int         s_arg;
static int         s_last_arg[PNL_DEST_COUNT];
static pnl_snap_t *s_snap;
static uint32_t    s_seen_seq;
static lv_timer_t *s_timer;
static pnl_dest_t  s_pend_dest;
static int         s_pend_arg;
static uint8_t     s_pend;

void pnl_label_set_if_changed(lv_obj_t *label, const char *text) {
    if (!label || !text) return;
    const char *cur = lv_label_get_text(label);
    if (!cur || strcmp(cur, text) != 0) lv_label_set_text(label, text);
}

void pnl_obj_show(lv_obj_t *o, int show) {
    if (!o) return;
    int hidden = lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN) ? 1 : 0;
    if (show && hidden) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    else if (!show && !hidden) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

pnl_dest_t pnl_nav_current(void) { return s_cur; }
int        pnl_nav_arg(void)     { return s_arg; }
const char *pnl_nav_title(void)  { return s_cur < PNL_DEST_COUNT ? REG[s_cur].title : ""; }
const pnl_snap_t *pnl_shell_snap(void) { return s_snap; }

static void warn_slow(const char *what, int64_t us) {
    if (us > CB_BUDGET_US)
        ESP_LOGW(TAG, "%s %s took %u ms (budget 200)", pnl_nav_title(), what, (unsigned)(us / 1000));
}

static void rail_paint(void) {
    for (int i = 0; i < RAIL_N; i++) {
        if (!s_rail_btn[i]) continue;
        int on = (RAIL_DEST[i] == s_cur);
        lv_obj_set_style_bg_color(s_rail_btn[i], lv_color_hex(on ? PNL_C_ACCENT : PNL_C_CARD), 0);
    }
}

static void back_cb(lv_event_t *e) { (void)e; pnl_nav_go(PNL_DEST_HOME, 0); }

static void nav_apply(void *unused) {
    (void)unused;
    s_pend = 0;
    pnl_dest_t d = s_pend_dest;
    int arg = s_pend_arg;
    if (d >= PNL_DEST_COUNT) return;
    if ((d == PNL_DEST_ZONE || d == PNL_DEST_CONFIG)) {
        if (arg == -1) arg = s_last_arg[d];
        if (arg >= 0) s_last_arg[d] = arg;
    }

    if (s_cur < PNL_DEST_COUNT && REG[s_cur].ops->teardown) REG[s_cur].ops->teardown();
    pnl_screen_gen_bump();                         /* any job still in flight for the old screen must not touch it */
    if (s_page) { lv_obj_delete(s_page); s_page = NULL; }
    if (s_back) { lv_obj_delete(s_back); s_back = NULL; }

    s_cur = d;
    s_arg = arg;
    int rail = REG[d].rail;
    pnl_obj_show(s_rail, rail);
    lv_obj_set_pos(s_content, rail ? RAIL_W : 0, 0);
    lv_obj_set_size(s_content, rail ? SCREEN_W - RAIL_W : SCREEN_W, SCREEN_H);
    rail_paint();

    s_page = lv_obj_create(s_content);
    lv_obj_remove_style_all(s_page);
    lv_obj_set_size(s_page, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_text_color(s_page, lv_color_hex(PNL_C_TEXT), 0);

    pnl_poll_latest(s_snap);
    s_seen_seq = s_snap->seq;
    int64_t t0 = esp_timer_get_time();
    REG[d].ops->build(s_page, arg);
    warn_slow("build", esp_timer_get_time() - t0);

    if (!rail && d != PNL_DEST_HOME) {
        s_back = lv_button_create(s_content);
        lv_obj_t *l = lv_label_create(s_back);
        lv_label_set_text(l, LV_SYMBOL_LEFT " Home");
        lv_obj_align(s_back, LV_ALIGN_TOP_LEFT, 8, 8);
        lv_obj_add_event_cb(s_back, back_cb, LV_EVENT_CLICKED, NULL);
    }
}

void pnl_nav_go(pnl_dest_t d, int arg) {
    if (d >= PNL_DEST_COUNT) return;
    s_pend_dest = d;
    s_pend_arg = arg;
    if (!s_pend) {
        s_pend = 1;
        if (lv_async_call(nav_apply, NULL) != LV_RESULT_OK) s_pend = 0;
    }
}

static void rail_cb(lv_event_t *e) {
    pnl_dest_t d = (pnl_dest_t)(intptr_t)lv_event_get_user_data(e);
    pnl_nav_go(d, (d == PNL_DEST_ZONE || d == PNL_DEST_CONFIG) ? -1 : 0);
}

/* 250 ms: a new poll publish -> the open screen's update(), timed. */
static void tick_cb(lv_timer_t *t) {
    (void)t;
    if (s_cur >= PNL_DEST_COUNT || s_pend) return;
    uint32_t seq = pnl_poll_seq();
    if (seq == s_seen_seq) return;
    pnl_poll_latest(s_snap);
    s_seen_seq = seq;
    if (!REG[s_cur].ops->update) return;
    int64_t t0 = esp_timer_get_time();
    REG[s_cur].ops->update(s_snap);
    warn_slow("update", esp_timer_get_time() - t0);
}

void pnl_shell_start(void) {
    s_snap = heap_caps_calloc(1, sizeof(pnl_snap_t), MALLOC_CAP_SPIRAM);
    if (!s_snap) {
        static pnl_snap_t fallback;   /* only if PSRAM is gone -- keep the panel alive */
        s_snap = &fallback;
        ESP_LOGW(TAG, "no PSRAM for the shell snapshot -- using internal RAM");
    }
    for (int i = 0; i < PNL_DEST_COUNT; i++) s_last_arg[i] = -1;

    lv_obj_t *root = lv_screen_active();
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    s_rail = lv_obj_create(root);
    lv_obj_remove_style_all(s_rail);
    lv_obj_set_size(s_rail, RAIL_W, SCREEN_H);
    lv_obj_set_pos(s_rail, 0, 0);
    lv_obj_set_style_bg_color(s_rail, lv_color_hex(PNL_C_CARD), 0);
    lv_obj_set_style_bg_opa(s_rail, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_rail, 6, 0);
    lv_obj_set_style_pad_row(s_rail, 6, 0);
    lv_obj_set_flex_flow(s_rail, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(s_rail, LV_OBJ_FLAG_SCROLLABLE);
    for (int i = 0; i < RAIL_N; i++) {
        lv_obj_t *b = lv_button_create(s_rail);
        lv_obj_set_size(b, RAIL_W - 12, 88);
        lv_obj_set_flex_flow(b, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        pnl_label(b, RAIL_ICON[i], &lv_font_montserrat_28, PNL_C_TEXT);
        pnl_label(b, RAIL_TEXT[i], &lv_font_montserrat_14, PNL_C_TEXT);
        lv_obj_add_event_cb(b, rail_cb, LV_EVENT_CLICKED, (void *)(intptr_t)RAIL_DEST[i]);
        s_rail_btn[i] = b;
    }

    s_content = lv_obj_create(root);
    lv_obj_remove_style_all(s_content);
    lv_obj_set_style_bg_color(s_content, lv_color_hex(PNL_C_BG), 0);
    lv_obj_set_style_bg_opa(s_content, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_content, LV_OBJ_FLAG_SCROLLABLE);

    s_timer = lv_timer_create(tick_cb, 250, NULL);
    pnl_nav_go(PNL_DEST_HOME, 0);
}

/* ---- the shared placeholder: every destination whose task has not landed ---- */

static void ph_build(lv_obj_t *page, int arg) {
    (void)arg;
    char buf[64];
    snprintf(buf, sizeof buf, "%s: not available yet", pnl_nav_title());
    lv_obj_t *l = pnl_label(page, buf, &lv_font_montserrat_28, PNL_C_MUTED);
    lv_obj_center(l);
}

const pnl_screen_ops_t PNL_SCR_PLACEHOLDER = { "Placeholder", ph_build, NULL, NULL, 0 };
```

(`lv_font_montserrat_14` is LVGL's default-on size, which Task 6 leaves enabled. The rail's small captions use it, so "Dashboard" fits 108 px.)

- [ ] **Step 2: Give the diagnostics screen its ops**

In `components/panel_ui/scr_diag.c`:
- add `#include "scr_shell.h"`;
- append at the end of the file:

```c
/* ---- shell registration (Task 12) ---- */

static void diag_build(lv_obj_t *page, int arg) {
    (void)arg;
    scr_diag_build(page);
}

/* The timers reference this screen's labels, so they die with it. A blocking
 * job still in flight finds screen_gen changed and s_job_lbl NULL. */
static void diag_teardown(void) {
    if (s_tick)    { lv_timer_delete(s_tick);    s_tick = NULL; }
    if (s_ui_tick) { lv_timer_delete(s_ui_tick); s_ui_tick = NULL; }
    s_heap = s_touch = s_poll = s_job_lbl = s_job_btn = NULL;
    for (int i = 0; i < TGT_N; i++) { s_tgt[i] = NULL; s_tgt_lbl[i] = NULL; }
}

const pnl_screen_ops_t PNL_SCR_DIAG = { "Touch test", diag_build, NULL, diag_teardown, 0 };
```

- [ ] **Step 3: Write the Dashboard**

`components/panel_ui/scr_dashboard.c`:

```c
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "lvgl.h"
#include "state_snap.h"
#include "pnl_fmt.h"
#include "pnl_palette.h"
#include "pnl_theme.h"
#include "scr_shell.h"

/* The web dashboard (app.js:758-799), in its order: ring banner, default-
 * password banner, master card, degraded console slots, one card per ENROLLED
 * node. Built once; update() only rewrites what changed. */

typedef struct {
    lv_obj_t *card, *name, *health, *sub, *soil, *light, *pump;
    uint8_t   id;
    int       health_seen;
} dash_card_t;

static lv_obj_t   *s_start, *s_ring, *s_def, *s_def_lbl, *s_master, *s_mtitle, *s_time, *s_sta, *s_ap, *s_heap;
static lv_obj_t   *s_quar, *s_grid;
static dash_card_t s_card[HG_MAX_ZONES];
static int         s_ring_seen = -1;

static void card_cb(lv_event_t *e) {
    dash_card_t *c = (dash_card_t *)lv_event_get_user_data(e);
    if (c && c->id) pnl_nav_go(PNL_DEST_ZONE, c->id);
}

static void def_cb(lv_event_t *e) { (void)e; pnl_nav_go(PNL_DEST_SYSTEM, 0); }

/* A transparent, non-clickable flex row: taps fall through to the card. */
static lv_obj_t *row(lv_obj_t *parent) {
    lv_obj_t *r = lv_obj_create(parent);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    return r;
}

static void badge_style(lv_obj_t *l) {
    lv_obj_set_style_bg_opa(l, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(l, 4, 0);
    lv_obj_set_style_pad_hor(l, 6, 0);
    lv_obj_set_style_pad_ver(l, 2, 0);
}

static void dash_update(const pnl_snap_t *sn);

static void dash_build(lv_obj_t *page, int arg) {
    (void)arg;
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(page, 16, 0);
    lv_obj_set_style_pad_row(page, 12, 0);

    s_start = pnl_label(page, "Starting...", &lv_font_montserrat_28, PNL_C_MUTED);

    s_ring = pnl_label(page, "", &lv_font_montserrat_20, PNL_C_TEXT);
    lv_obj_set_width(s_ring, LV_PCT(100));
    badge_style(s_ring);
    lv_obj_set_style_pad_all(s_ring, 10, 0);

    s_def = lv_obj_create(page);
    pnl_theme_card(s_def);
    lv_obj_set_size(s_def, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_border_color(s_def, lv_color_hex(PNL_C_WARN), 0);
    lv_obj_set_flex_flow(s_def, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_def, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(s_def, LV_OBJ_FLAG_SCROLLABLE);
    s_def_lbl = pnl_label(s_def, "", &lv_font_montserrat_20, PNL_C_WARN_TEXT);
    lv_obj_set_width(s_def_lbl, 620);
    lv_obj_t *b = lv_button_create(s_def);
    pnl_label(b, "Open System", &lv_font_montserrat_20, PNL_C_TEXT);
    lv_obj_add_event_cb(b, def_cb, LV_EVENT_CLICKED, NULL);

    s_master = lv_obj_create(page);
    pnl_theme_card(s_master);
    lv_obj_set_size(s_master, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_master, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_master, 6, 0);
    lv_obj_remove_flag(s_master, LV_OBJ_FLAG_SCROLLABLE);
    s_mtitle = pnl_label(s_master, "", &lv_font_montserrat_28, PNL_C_TEXT);
    s_time   = pnl_label(s_master, "", &lv_font_montserrat_20, PNL_C_TEXT);
    s_sta    = pnl_label(s_master, "", &lv_font_montserrat_20, PNL_C_TEXT);
    s_ap     = pnl_label(s_master, "", &lv_font_montserrat_20, PNL_C_TEXT);
    s_heap   = pnl_label(s_master, "", &lv_font_montserrat_20, PNL_C_MUTED);

    s_quar = pnl_label(page, "", &lv_font_montserrat_20, PNL_C_WARN_TEXT);

    s_grid = lv_obj_create(page);
    lv_obj_remove_style_all(s_grid);
    lv_obj_set_size(s_grid, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(s_grid, 12, 0);
    lv_obj_set_style_pad_column(s_grid, 12, 0);
    lv_obj_remove_flag(s_grid, LV_OBJ_FLAG_CLICKABLE);

    for (int i = 0; i < HG_MAX_ZONES; i++) {
        dash_card_t *c = &s_card[i];
        memset(c, 0, sizeof *c);
        c->health_seen = -1;
        c->card = lv_obj_create(s_grid);
        pnl_theme_card(c->card);
        lv_obj_set_size(c->card, 280, 150);
        lv_obj_set_flex_flow(c->card, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(c->card, 6, 0);
        lv_obj_remove_flag(c->card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(c->card, card_cb, LV_EVENT_CLICKED, c);
        lv_obj_t *head = row(c->card);
        c->name   = pnl_label(head, "", &lv_font_montserrat_28, PNL_C_TEXT);
        c->health = pnl_label(head, "", &lv_font_montserrat_14, PNL_C_TEXT);
        badge_style(c->health);
        c->sub    = pnl_label(c->card, "", &lv_font_montserrat_14, PNL_C_MUTED);
        lv_obj_t *tiles = row(c->card);
        c->soil   = pnl_label(tiles, "", &lv_font_montserrat_20, PNL_C_TEXT);
        c->light  = pnl_label(tiles, "", &lv_font_montserrat_20, PNL_C_TEXT);
        c->pump   = pnl_label(tiles, "", &lv_font_montserrat_20, PNL_C_TEXT);
        lv_obj_add_flag(c->card, LV_OBJ_FLAG_HIDDEN);
    }
    dash_update(pnl_shell_snap());
}

static void dash_update(const pnl_snap_t *sn) {
    if (!s_grid || !sn) return;
    int up = sn->started ? 1 : 0;
    pnl_obj_show(s_start, !up);
    pnl_obj_show(s_ring, up);
    pnl_obj_show(s_master, up);
    pnl_obj_show(s_grid, up);
    if (!up) { pnl_obj_show(s_def, 0); pnl_obj_show(s_quar, 0); return; }

    const psvc_state_t *st = &sn->st;
    char buf[160], t[96];

    const char *rs = state_snap_ring_state_name(st->ring.state);
    if (st->ring.blame[0]) snprintf(buf, sizeof buf, "Ring: %s -- %.47s", rs, st->ring.blame);
    else snprintf(buf, sizeof buf, "Ring: %s", rs);
    pnl_label_set_if_changed(s_ring, buf);
    if ((int)st->ring.state != s_ring_seen) {
        s_ring_seen = (int)st->ring.state;
        uint32_t bg = st->ring.state == RING_ST_OK ? PNL_C_OK : st->ring.state == RING_ST_OPEN ? PNL_C_OFFLINE : PNL_C_CARD;
        lv_obj_set_style_bg_color(s_ring, lv_color_hex(bg), 0);
    }

    if (st->web_default || st->ap_default) {
        snprintf(buf, sizeof buf, "Factory default password still in use (%s) -- change it in System.",
                 st->web_default && st->ap_default ? "web, Wi-Fi AP" : st->web_default ? "web" : "Wi-Fi AP");
        pnl_label_set_if_changed(s_def_lbl, buf);
        pnl_obj_show(s_def, 1);
    } else {
        pnl_obj_show(s_def, 0);
    }

    snprintf(buf, sizeof buf, "Master  v%s", st->version);
    pnl_label_set_if_changed(s_mtitle, buf);
    pnl_fmt_master_time(st->time, st->time_src, st->utc_offset_s, st->time_is_set, t, sizeof t);
    snprintf(buf, sizeof buf, "Time: %s", t);
    pnl_label_set_if_changed(s_time, buf);
    pnl_fmt_sta(&st->wifi, t, sizeof t);
    snprintf(buf, sizeof buf, "STA: %s", t);
    pnl_label_set_if_changed(s_sta, buf);
    pnl_fmt_ap(&st->wifi, t, sizeof t);
    snprintf(buf, sizeof buf, "AP: %s", t);
    pnl_label_set_if_changed(s_ap, buf);
    snprintf(buf, sizeof buf, "Heap min %u KB", (unsigned)st->heap_min_kb);
    pnl_label_set_if_changed(s_heap, buf);

    if (st->web_cmd_quarantined > 0) {
        snprintf(buf, sizeof buf, "%u web console slot(s) degraded", (unsigned)st->web_cmd_quarantined);
        pnl_label_set_if_changed(s_quar, buf);
        pnl_obj_show(s_quar, 1);
    } else {
        pnl_obj_show(s_quar, 0);
    }

    int k = 0;
    for (int i = 0; i < HG_MAX_ZONES; i++) {
        const hg_node_t *n = &st->node[i];
        if (!n->used) continue;
        dash_card_t *c = &s_card[k++];
        c->id = n->id;
        char name[17];
        pnl_zone_name(n, name);
        pnl_label_set_if_changed(c->name, name);
        pnl_label_set_if_changed(c->health, state_snap_health_name(n->health));
        if ((int)n->health != c->health_seen) {
            c->health_seen = (int)n->health;
            lv_obj_set_style_bg_color(c->health, pnl_health_color(n->health), 0);
        }
        snprintf(buf, sizeof buf, "fw %u.%u.%u%s | last seen %us ago",
                 (unsigned)n->hb.fw_maj, (unsigned)n->hb.fw_min, (unsigned)n->hb.fw_patch,
                 n->health == NODE_H_OFFLINE ? "  stale" : "",
                 (unsigned)((st->now_ms - n->last_hb_ms) / 1000u));
        pnl_label_set_if_changed(c->sub, buf);
        pnl_readings_t r;
        pnl_node_readings(n, &r);
        pnl_fmt_reading(&r, 0, t, sizeof t); snprintf(buf, sizeof buf, "Soil %s", t);  pnl_label_set_if_changed(c->soil, buf);
        pnl_fmt_reading(&r, 1, t, sizeof t); snprintf(buf, sizeof buf, "Light %s", t); pnl_label_set_if_changed(c->light, buf);
        pnl_fmt_reading(&r, 2, t, sizeof t); snprintf(buf, sizeof buf, "Pump %s", t);  pnl_label_set_if_changed(c->pump, buf);
        pnl_obj_show(c->card, 1);
    }
    for (; k < HG_MAX_ZONES; k++) { s_card[k].id = 0; pnl_obj_show(s_card[k].card, 0); }
}

static void dash_teardown(void) {
    s_start = s_ring = s_def = s_def_lbl = s_master = s_mtitle = s_time = s_sta = s_ap = s_heap = NULL;
    s_quar = s_grid = NULL;
    memset(s_card, 0, sizeof s_card);
    s_ring_seen = -1;
}

const pnl_screen_ops_t PNL_SCR_DASHBOARD = { "Dashboard", dash_build, dash_update, dash_teardown, 1 };
```

(The node card's "stale" is written as a word inside the sub-line rather than a separate pill widget, to keep the card at seven labels. It appears exactly when the web's pill does: `link_stale`, which is health OFFLINE, `state_snap.c:162`.)

- [ ] **Step 4: Start the shell**

In `components/panel_ui/panel_ui.c`:
- replace `#include "scr_diag.h"` with `#include "scr_shell.h"`;
- in `panel_start()`, replace `scr_diag_build(lv_screen_active());` with `pnl_shell_start();`.

`components/panel_ui/CMakeLists.txt`: the `set(PANEL_SRCS ...)` line becomes:

```cmake
    set(PANEL_SRCS "panel_ui.c" "panel_hw.c" "panel_lock.c" "pnl_touch.c" "pnl_theme.c" "scr_diag.c" "pnl_worker.c"
                   "pnl_time.c" "pnl_fmt.c" "pnl_home.c" "pnl_poll.c" "scr_shell.c" "scr_dashboard.c")
```

- [ ] **Step 5: Run the gates**

Run GATE-HOST. Expected: 43/43 tests pass.
Run GATE-P4. Expected: `Project build complete`, 0 warnings, the target grep matches, then the lock checkout. The registry references only defined ops (`DIAG`, `DASHBOARD`, `PLACEHOLDER`), so the undefined externs are never linked.
Run GATE-ESP32. Expected: master, zone and rescue each print `Project build complete` with 0 warnings, and the lock diff is empty. Then re-run the GATE-P4 build line and the lock checkout, for flashing.

- [ ] **Step 6: Bench check**

Run FLASH-P4. On glass:
- The diagnostics screen shows with the rail.
- Each rail button opens its destination: Dashboard, and "Zone: not available yet" (and Config, Alarms and System likewise). Home returns to the diagnostics.
- The Dashboard's ring banner, master card (version, time with its `(UTC+hh:mm)` label, STA, AP, heap min) and both zone cards (name, health badge, fw, last seen, Soil/Light/Pump) match the web dashboard on a phone, value for value, allowing for the 1-2 s poll skew.
- Tapping a zone card opens the Zone placeholder.
- The console shows no `took N ms (budget 200)` WARN.

- [ ] **Step 7: Commit**

```powershell
git -C C:\Projects\HillGrov add components/panel_ui/scr_shell.h components/panel_ui/scr_shell.c components/panel_ui/scr_dashboard.c components/panel_ui/scr_diag.c components/panel_ui/panel_ui.c components/panel_ui/CMakeLists.txt
git -C C:\Projects\HillGrov commit -m "feat(panel_ui): left-rail shell and the dashboard" -m "Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>"
```

### Task 13: Clock font, home screen, Audio placeholder (Stage 1)

The panel is furniture most of the time, so the clock is the hero (spec Decision 5).
- Under the clock: the date, the one most useful sentence, and a status band with one tile per **enrolled** zone.
- Under the band: three icons.
- **Audio** is a placeholder that says so (SP7).
- **Panel** opens the touch test until Task 26.

**Files:**
- Create:
  - `components/panel_ui/font_clock_180.c`, generated and committed. Generate it after a GATE-P4 configure (an ESP32 build purges `master/managed_components/lvgl__lvgl`): `npx --yes lv_font_conv@1.5.2 --no-compress --no-prefilter --bpp 4 --size 180 --font C:\Projects\HillGrov\master\managed_components\lvgl__lvgl\scripts\built_in_font\Montserrat-Medium.ttf -r 0x20,0x2D,0x30-0x3A --format lvgl --force-fast-kern-format -o C:\Projects\HillGrov\components\panel_ui\font_clock_180.c`. The generated header records the options. If npx is unavailable, use route D2(b) (Step 2b).
  - `components/panel_ui/pnl_fonts.h` (`LV_FONT_DECLARE(font_clock_180);`), `scr_home.c`, `scr_audio.c`.
- Modify:
  - `components/panel_ui/scr_shell.c`: registry HOME → `PNL_SCR_HOME`, AUDIO → `PNL_SCR_AUDIO`; PANEL → `PNL_SCR_DIAG` until Task 26.
  - `components/panel_ui/CMakeLists.txt`.

**Interfaces:**
- Consumes:
  - from Task 10: `pnl_local_time`, `pnl_fmt_clock`, `pnl_fmt_date`, `pnl_ctx_line`, `pnl_band_layout`, `pnl_zone_name` and `pnl_node_readings`;
  - from Task 11: `pnl_poll_latest`;
  - from Task 12: `pnl_nav_go`, `pnl_shell_snap`, `pnl_label_set_if_changed` and `pnl_obj_show`.
- Produces: `PNL_SCR_HOME` and `PNL_SCR_AUDIO`. The home screen is full-screen, with no rail:
  - **Top bar:** "• ring OK/OPEN/IDLE" in the ring colour on the left; the time source (NTP/SET/NONE) and a Wi-Fi symbol on the right (STA up → normal; AP only → muted; neither → offline colour).
  - **Clock:** `font_clock_180`, updated by its own 1 s timer from `time(NULL)` plus the snapshot's offset.
  - **Date line:** Montserrat 48.
  - **Context line:** Montserrat 28.
  - **Status band:** tiles for used nodes only, laid out by `pnl_band_layout`, with a dot in `pnl_health_color`. Tapping a tile goes to that zone. With 0 enrolled, the band says "No zones enrolled".
  - **Alarms:** when `alarms_active > 0`, the band container turns `PNL_C_OFFLINE`, pulses its opacity with a 1 s `lv_anim`, and shows a badge "N alarm(s) -- tap to view". A tap on the band background or the badge goes to `PNL_DEST_ALARMS`; tiles still open zones.
  - **Icons:** HillGrow → `PNL_DEST_DASHBOARD`; Audio → `PNL_DEST_AUDIO`; Panel → `PNL_DEST_PANEL`.
  - **Audio text:** "Audio playback arrives with SP7 (media and storage: microSD, I2S, PCM5102A). Nothing to set up yet."
- Interface addition (`pnl_fonts.h`): `static inline const lv_font_t *pnl_font_clock(void)`. The home screen asks for the clock font through this one accessor, so route (b) of D2 changes only `pnl_fonts.h`/`.c` and never the screen.

The clock and date **never** show a plausible wrong time. `pnl_local_time()` carries `time_is_set` from the snapshot, and before the first snapshot that is 0, so the screen shows `--:--` and "Clock not set" until the poller has spoken. SAFE mode does not exist yet (system spec §3 is unbuilt), so the home screen has no takeover at all: alarms only turn the band red.

**What is provable where.** The maths is Task 10's (host-tested). The widget tree is glue: GATE-P4 compiles it and the font. Only glass proves the clock is readable across the room, that the band divides the width deliberately for the enrolled count, and that the pulse shows. Those checks come in Step 6 and Stage 1 gate steps 1-5.

- [ ] **Step 1: Extract the P4 packages (for the TTF)**

```powershell
& C:\esp\v6.0.1\esp-idf\export.ps1
idf.py -C C:\Projects\HillGrov\master -B C:\Projects\HillGrov\master\build_p4 -DIDF_TARGET=esp32p4 -DSDKCONFIG=C:\Projects\HillGrov\master\build_p4\sdkconfig -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.esp32p4" reconfigure
git -C C:\Projects\HillGrov checkout -- master/dependencies.lock
Test-Path C:\Projects\HillGrov\master\managed_components\lvgl__lvgl\scripts\built_in_font\Montserrat-Medium.ttf
```
Expected: `True`.

- [ ] **Step 2a: Generate the clock font (route D2(a), the default)**

```powershell
npx --yes lv_font_conv@1.5.2 --no-compress --no-prefilter --bpp 4 --size 180 --font C:\Projects\HillGrov\master\managed_components\lvgl__lvgl\scripts\built_in_font\Montserrat-Medium.ttf -r 0x20,0x2D,0x30-0x3A --format lvgl --force-fast-kern-format -o C:\Projects\HillGrov\components\panel_ui\font_clock_180.c
Select-String -Path C:\Projects\HillGrov\components\panel_ui\font_clock_180.c -Pattern 'font_clock_180','LV_LVGL_H_INCLUDE_SIMPLE' | Select-Object -First 3
```
Expected: the file exists (on the order of 100 KB of source). Its header comment records the options, it defines `font_clock_180`, and it includes `lvgl.h` under `LV_LVGL_H_INCLUDE_SIMPLE`, which `panel_ui`'s CMake defines. The glyph set is exactly space, `-`, `0`-`9` and `:`, which is everything `--:--` and `HH:MM` need.

`components/panel_ui/pnl_fonts.h`:

```c
#pragma once
#include "lvgl.h"

/* The home clock's face: a Montserrat-Medium subset (space, '-', '0'-'9', ':')
 * pre-rendered at 180 px, 4 bpp, by lv_font_conv 1.5.2 into font_clock_180.c
 * (D2 route a; the generated file records the exact command). Built-in
 * Montserrat stops at 48 px and bitmap fonts do not scale cleanly. */
LV_FONT_DECLARE(font_clock_180);

/* The one accessor screens use, so the font route can change in one place. */
static inline const lv_font_t *pnl_font_clock(void) { return &font_clock_180; }
```

- [ ] **Step 2b: Only if Step 2a cannot reach the npm registry: route D2(b), fully offline**

Skip this step when Step 2a produced the file.
1. Subset the TTF with the installed fontTools:
   ```powershell
   C:\Python311\python -m fontTools.subset C:\Projects\HillGrov\master\managed_components\lvgl__lvgl\scripts\built_in_font\Montserrat-Medium.ttf --text="0123456789:- " --output-file=C:\Projects\HillGrov\components\panel_ui\font_clock.ttf
   ```
2. Append to `master/sdkconfig.defaults.esp32p4`:
   ```
   # D2 route (b): the clock font rendered at runtime by LVGL's own stb_truetype.
   CONFIG_LV_USE_TINY_TTF=y
   ```
   Then run `Remove-Item C:\Projects\HillGrov\master\build_p4\sdkconfig`. After the next GATE-P4, `Select-String -Path C:\Projects\HillGrov\master\build_p4\sdkconfig -Pattern 'CONFIG_LV_USE_TINY_TTF=y'` must match.
3. In `components/panel_ui/CMakeLists.txt`, inside `if(PANEL_SRCS)`, add `target_add_binary_data(${COMPONENT_LIB} "font_clock.ttf" BINARY)`. In Step 5, list `"pnl_fonts.c"` in place of `"font_clock_180.c"`.
4. `pnl_fonts.h` becomes:
   ```c
   #pragma once
   #include "lvgl.h"
   /* D2 route (b): a Montserrat-Medium subset (space, '-', '0'-'9', ':') embedded as
    * font_clock.ttf and rasterised on first draw by lv_tiny_ttf, then cached. */
   const lv_font_t *pnl_font_clock(void);
   ```
   and add `components/panel_ui/pnl_fonts.c`:
   ```c
   #include <stddef.h>
   #include <stdint.h>
   #include "lvgl.h"
   #include "pnl_fonts.h"

   extern const uint8_t font_clock_ttf_start[] asm("_binary_font_clock_ttf_start");
   extern const uint8_t font_clock_ttf_end[]   asm("_binary_font_clock_ttf_end");

   /* [LVGL] Created once, on first use; Montserrat 48 if it cannot be. */
   const lv_font_t *pnl_font_clock(void) {
       static lv_font_t *f;
       if (!f) f = lv_tiny_ttf_create_data(font_clock_ttf_start, (size_t)(font_clock_ttf_end - font_clock_ttf_start), 180);
       return f ? f : &lv_font_montserrat_48;
   }
   ```
5. Stage `components/panel_ui/font_clock.ttf`, `components/panel_ui/pnl_fonts.c` and `master/sdkconfig.defaults.esp32p4` in Step 8 instead of `font_clock_180.c`. Record in the task report which route was taken.

- [ ] **Step 3: Write the home screen**

`components/panel_ui/scr_home.c`:

```c
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include "lvgl.h"
#include "state_snap.h"
#include "pnl_fmt.h"
#include "pnl_home.h"
#include "pnl_time.h"
#include "pnl_fonts.h"
#include "pnl_palette.h"
#include "pnl_theme.h"
#include "scr_shell.h"

/* Spec "Shell -- the home screen". Full screen, no rail. Layout (1024 x 600):
 *   top bar y 10 | clock y 36 (180 px) | date y 222 | context y 284 |
 *   band y 330 (118 px) | alarm badge y 452 | icons at the bottom. */
#define HOME_MARGIN 24
#define BAND_Y      330
#define BAND_H      118
#define TILE_H      98

static lv_obj_t   *s_ring, *s_src, *s_wifi, *s_clock, *s_date, *s_ctx, *s_band, *s_none, *s_badge;
static lv_obj_t   *s_tile[HG_MAX_ZONES], *s_dot[HG_MAX_ZONES], *s_l1[HG_MAX_ZONES], *s_l2[HG_MAX_ZONES], *s_l3[HG_MAX_ZONES];
static uint8_t     s_tile_id[HG_MAX_ZONES];
static int         s_tile_health[HG_MAX_ZONES];
static int         s_ntiles = -1;
static pnl_tile_detail_t s_detail;
static lv_timer_t *s_clock_timer;
static int         s_alarm_on = -1;
static uint32_t    s_ring_c, s_wifi_c;

static void nav_cb(lv_event_t *e) { pnl_nav_go((pnl_dest_t)(intptr_t)lv_event_get_user_data(e), 0); }

static void tile_cb(lv_event_t *e) {
    uint8_t id = (uint8_t)(intptr_t)lv_event_get_user_data(e);
    if (id) pnl_nav_go(PNL_DEST_ZONE, id);
}

static void band_cb(lv_event_t *e) {
    (void)e;
    if (s_alarm_on == 1) pnl_nav_go(PNL_DEST_ALARMS, 0);
}

/* A 1 s triangle wave on the band's background opacity, 255 -> 120 -> 255. */
static void band_opa_cb(void *var, int32_t v) {
    int32_t tri = v < 500 ? v : 1000 - v;
    lv_obj_set_style_bg_opa((lv_obj_t *)var, (lv_opa_t)(255 - tri * 135 / 500), 0);
}

static void set_color_if_changed(lv_obj_t *o, uint32_t *seen, uint32_t hex) {
    if (*seen == hex) return;
    *seen = hex;
    lv_obj_set_style_text_color(o, lv_color_hex(hex), 0);
}

/* 1 s: the clock, the date and the context line. time(NULL) is UTC; the
 * offset and "is the clock set at all" come from the last poll. */
static void clock_tick(lv_timer_t *t) {
    (void)t;
    if (!s_clock) return;
    const pnl_snap_t *sn = pnl_shell_snap();
    pnl_local_t lt;
    pnl_local_time((int64_t)time(NULL), sn->st.utc_offset_s, sn->st.time_is_set, &lt);
    char b[64];
    pnl_fmt_clock(&lt, b, sizeof b);
    pnl_label_set_if_changed(s_clock, b);
    pnl_fmt_date(&lt, b, sizeof b);
    pnl_label_set_if_changed(s_date, b);
    pnl_ctx_line(&sn->sched, &lt, b, sizeof b);
    pnl_label_set_if_changed(s_ctx, b);
}

static void tiles_rebuild(const uint8_t *ids, int n) {
    for (int i = 0; i < HG_MAX_ZONES; i++) {
        if (s_tile[i]) lv_obj_delete(s_tile[i]);
        s_tile[i] = s_dot[i] = s_l1[i] = s_l2[i] = s_l3[i] = NULL;
        s_tile_health[i] = -1;
    }
    s_ntiles = n;
    memcpy(s_tile_id, ids, (size_t)n);
    pnl_obj_show(s_none, n == 0);
    if (n == 0) return;
    pnl_band_t L;
    if (pnl_band_layout(n, PNL_BAND_WIDTH, &L) != 0) return;
    s_detail = L.detail;
    for (int k = 0; k < n; k++) {
        lv_obj_t *t = lv_obj_create(s_band);
        pnl_theme_card(t);
        lv_obj_set_size(t, L.tile_w, TILE_H);
        lv_obj_set_pos(t, L.x0 + k * (L.tile_w + L.gap), (BAND_H - TILE_H) / 2);
        lv_obj_remove_flag(t, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(t, tile_cb, LV_EVENT_CLICKED, (void *)(intptr_t)ids[k]);
        s_dot[k] = lv_obj_create(t);
        lv_obj_remove_style_all(s_dot[k]);
        lv_obj_set_size(s_dot[k], 16, 16);
        lv_obj_set_style_radius(s_dot[k], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(s_dot[k], LV_OPA_COVER, 0);
        lv_obj_remove_flag(s_dot[k], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_align(s_dot[k], LV_ALIGN_TOP_RIGHT, 0, 4);
        s_l1[k] = pnl_label(t, "", L.detail == PNL_TILE_MIN ? &lv_font_montserrat_20 : &lv_font_montserrat_28, PNL_C_TEXT);
        lv_obj_align(s_l1[k], LV_ALIGN_TOP_LEFT, 0, 0);
        s_l2[k] = pnl_label(t, "", &lv_font_montserrat_28, PNL_C_TEXT);
        lv_obj_align(s_l2[k], LV_ALIGN_BOTTOM_LEFT, 0, 0);
        if (L.detail == PNL_TILE_FULL) {
            s_l3[k] = pnl_label(t, "", &lv_font_montserrat_20, PNL_C_MUTED);
            lv_obj_align(s_l3[k], LV_ALIGN_BOTTOM_RIGHT, 0, -4);
        }
        s_tile[k] = t;
    }
}

static void band_update(const pnl_snap_t *sn) {
    uint8_t ids[HG_MAX_ZONES];
    int n = 0;
    for (int i = 0; i < HG_MAX_ZONES; i++)
        if (sn->st.node[i].used) ids[n++] = sn->st.node[i].id;
    if (n != s_ntiles || memcmp(ids, s_tile_id, (size_t)n) != 0) tiles_rebuild(ids, n);

    char b[32], r[16];
    for (int k = 0; k < s_ntiles; k++) {
        const hg_node_t *nd = &sn->st.node[s_tile_id[k] - 1];   /* slot = id - 1 */
        if (!nd->used || !s_tile[k]) continue;
        char name[17];
        if (s_detail == PNL_TILE_MIN) snprintf(name, sizeof name, "Z%u", (unsigned)nd->id);
        else pnl_zone_name(nd, name);
        pnl_label_set_if_changed(s_l1[k], name);
        if ((int)nd->health != s_tile_health[k]) {
            s_tile_health[k] = (int)nd->health;
            lv_obj_set_style_bg_color(s_dot[k], pnl_health_color(nd->health), 0);
        }
        pnl_readings_t rd;
        pnl_node_readings(nd, &rd);
        pnl_fmt_reading(&rd, 0, r, sizeof r);
        pnl_label_set_if_changed(s_l2[k], r);
        if (s_l3[k]) {
            pnl_fmt_reading(&rd, 1, r, sizeof r);
            snprintf(b, sizeof b, "Light %s", r);
            pnl_label_set_if_changed(s_l3[k], b);
        }
    }
}

static void alarm_update(const pnl_snap_t *sn) {
    int on = sn->st.alarms_active > 0 ? 1 : 0;
    if (on != s_alarm_on) {
        s_alarm_on = on;
        lv_anim_delete(s_band, band_opa_cb);
        if (on) {
            lv_obj_set_style_bg_opa(s_band, LV_OPA_COVER, 0);
            lv_anim_t a;
            lv_anim_init(&a);
            lv_anim_set_var(&a, s_band);
            lv_anim_set_values(&a, 0, 1000);
            lv_anim_set_duration(&a, 1000);
            lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
            lv_anim_set_exec_cb(&a, band_opa_cb);
            lv_anim_start(&a);
        } else {
            lv_obj_set_style_bg_opa(s_band, LV_OPA_TRANSP, 0);
        }
        pnl_obj_show(s_badge, on);
    }
    if (on) {
        char b[48];
        snprintf(b, sizeof b, "%d alarm(s) -- tap to view", sn->st.alarms_active);
        pnl_label_set_if_changed(s_badge, b);
    }
}

static void home_update(const pnl_snap_t *sn) {
    if (!s_band || !sn) return;
    if (sn->started) {
        const psvc_state_t *st = &sn->st;
        char b[48];
        snprintf(b, sizeof b, "\xE2\x80\xA2 ring %s", state_snap_ring_state_name(st->ring.state));
        pnl_label_set_if_changed(s_ring, b);
        set_color_if_changed(s_ring, &s_ring_c, st->ring.state == RING_ST_OK ? PNL_C_OK_TEXT
                                              : st->ring.state == RING_ST_OPEN ? PNL_C_OFFLINE_TEXT : PNL_C_MUTED);
        pnl_label_set_if_changed(s_src, st->time_src);
        set_color_if_changed(s_wifi, &s_wifi_c, st->wifi.sta_up ? PNL_C_TEXT
                                              : st->wifi.ap_ssid[0] ? PNL_C_MUTED : PNL_C_OFFLINE_TEXT);
        band_update(sn);
        alarm_update(sn);
    }
    clock_tick(NULL);
}

static lv_obj_t *icon(lv_obj_t *page, const char *text, pnl_dest_t d, int x) {
    lv_obj_t *b = lv_button_create(page);
    lv_obj_set_size(b, 240, 90);
    lv_obj_align(b, LV_ALIGN_BOTTOM_MID, x, -14);
    lv_obj_t *l = pnl_label(b, text, &lv_font_montserrat_28, PNL_C_TEXT);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, nav_cb, LV_EVENT_CLICKED, (void *)(intptr_t)d);
    return b;
}

static void home_build(lv_obj_t *page, int arg) {
    (void)arg;
    lv_obj_remove_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    s_ring_c = s_wifi_c = 0xFFFFFFFFu;

    s_ring = pnl_label(page, "\xE2\x80\xA2 ring --", &lv_font_montserrat_20, PNL_C_MUTED);
    lv_obj_align(s_ring, LV_ALIGN_TOP_LEFT, HOME_MARGIN, 10);
    lv_obj_t *tr = lv_obj_create(page);
    lv_obj_remove_style_all(tr);
    lv_obj_set_size(tr, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(tr, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(tr, 16, 0);
    lv_obj_remove_flag(tr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(tr, LV_ALIGN_TOP_RIGHT, -HOME_MARGIN, 10);
    s_src  = pnl_label(tr, "", &lv_font_montserrat_20, PNL_C_MUTED);
    s_wifi = pnl_label(tr, LV_SYMBOL_WIFI, &lv_font_montserrat_20, PNL_C_OFFLINE_TEXT);

    s_clock = pnl_label(page, "--:--", pnl_font_clock(), PNL_C_TEXT);
    lv_obj_align(s_clock, LV_ALIGN_TOP_MID, 0, 36);
    s_date = pnl_label(page, "", &lv_font_montserrat_48, PNL_C_TEXT);
    lv_obj_align(s_date, LV_ALIGN_TOP_MID, 0, 222);
    s_ctx = pnl_label(page, "", &lv_font_montserrat_28, PNL_C_MUTED);
    lv_obj_align(s_ctx, LV_ALIGN_TOP_MID, 0, 284);

    s_band = lv_obj_create(page);
    lv_obj_remove_style_all(s_band);
    lv_obj_set_size(s_band, PNL_BAND_WIDTH, BAND_H);
    lv_obj_align(s_band, LV_ALIGN_TOP_MID, 0, BAND_Y);
    lv_obj_set_style_radius(s_band, 10, 0);
    lv_obj_set_style_bg_color(s_band, lv_color_hex(PNL_C_OFFLINE), 0);
    lv_obj_set_style_bg_opa(s_band, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(s_band, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_band, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_band, band_cb, LV_EVENT_CLICKED, NULL);
    s_none = pnl_label(s_band, "No zones enrolled", &lv_font_montserrat_28, PNL_C_MUTED);
    lv_obj_center(s_none);
    lv_obj_add_flag(s_none, LV_OBJ_FLAG_HIDDEN);

    s_badge = pnl_label(page, "", &lv_font_montserrat_20, PNL_C_OFFLINE_TEXT);
    lv_obj_align(s_badge, LV_ALIGN_TOP_MID, 0, BAND_Y + BAND_H + 4);
    lv_obj_add_flag(s_badge, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_badge, band_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(s_badge, LV_OBJ_FLAG_HIDDEN);

    icon(page, LV_SYMBOL_LIST " HillGrow", PNL_DEST_DASHBOARD, -260);
    icon(page, LV_SYMBOL_AUDIO " Audio", PNL_DEST_AUDIO, 0);
    icon(page, LV_SYMBOL_SETTINGS " Panel", PNL_DEST_PANEL, 260);

    s_clock_timer = lv_timer_create(clock_tick, 1000, NULL);
    home_update(pnl_shell_snap());
}

static void home_teardown(void) {
    if (s_clock_timer) { lv_timer_delete(s_clock_timer); s_clock_timer = NULL; }
    if (s_band) lv_anim_delete(s_band, band_opa_cb);
    s_ring = s_src = s_wifi = s_clock = s_date = s_ctx = s_band = s_none = s_badge = NULL;
    for (int i = 0; i < HG_MAX_ZONES; i++) s_tile[i] = s_dot[i] = s_l1[i] = s_l2[i] = s_l3[i] = NULL;
    s_ntiles = -1;
    s_alarm_on = -1;
}

const pnl_screen_ops_t PNL_SCR_HOME = { "Home", home_build, home_update, home_teardown, 0 };
```

- [ ] **Step 4: Write the Audio placeholder**

`components/panel_ui/scr_audio.c`:

```c
#include "lvgl.h"
#include "pnl_palette.h"
#include "pnl_theme.h"
#include "scr_shell.h"

/* Spec Scope: "The Audio icon is a placeholder in this sub-project." SP7 owns
 * the player; this screen reserves its place and says so. */
static void audio_build(lv_obj_t *page, int arg) {
    (void)arg;
    lv_obj_t *l = pnl_label(page, "Audio playback arrives with SP7 (media and storage: microSD, I2S, PCM5102A). "
                                  "Nothing to set up yet.", &lv_font_montserrat_28, PNL_C_MUTED);
    lv_obj_set_width(l, 760);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(l);
}

const pnl_screen_ops_t PNL_SCR_AUDIO = { "Audio", audio_build, NULL, NULL, 0 };
```

- [ ] **Step 5: Swap the registry lines and build**

In `components/panel_ui/scr_shell.c`, replace these three `REG[]` lines:

```c
    [PNL_DEST_HOME]      = { "Home",       &PNL_SCR_DIAG,        1 },   /* Task 13: &PNL_SCR_HOME, rail 0 */
    [PNL_DEST_PANEL]     = { "Panel",      &PNL_SCR_PLACEHOLDER, 0 },   /* Task 13: &PNL_SCR_DIAG; Task 26 */
    [PNL_DEST_AUDIO]     = { "Audio",      &PNL_SCR_PLACEHOLDER, 0 },   /* Task 13 */
```

with:

```c
    [PNL_DEST_HOME]      = { "Home",       &PNL_SCR_HOME,        0 },
    [PNL_DEST_PANEL]     = { "Panel",      &PNL_SCR_DIAG,        0 },   /* the touch test until Task 26's &PNL_SCR_PANEL */
    [PNL_DEST_AUDIO]     = { "Audio",      &PNL_SCR_AUDIO,       0 },
```

`components/panel_ui/CMakeLists.txt`: the `set(PANEL_SRCS ...)` line becomes:

```cmake
    set(PANEL_SRCS "panel_ui.c" "panel_hw.c" "panel_lock.c" "pnl_touch.c" "pnl_theme.c" "scr_diag.c" "pnl_worker.c"
                   "pnl_time.c" "pnl_fmt.c" "pnl_home.c" "pnl_poll.c" "scr_shell.c" "scr_dashboard.c"
                   "font_clock_180.c" "scr_home.c" "scr_audio.c")
```

Run GATE-HOST. Expected: 43/43 tests pass.
Run GATE-P4. Expected: `Project build complete`, 0 warnings, the target grep matches, then the lock checkout. The generated font compiles under `-Werror`. If the generated file alone raises a warning, re-run Step 2a with `--lv-include lvgl.h` added; if that does not clear it, take route D2(b). Never silence `-Werror` for the file.
Run GATE-ESP32. Expected: master, zone and rescue each print `Project build complete` with 0 warnings, and the lock diff is empty. Then re-run the GATE-P4 build line and the lock checkout, for flashing.
Record `(Get-Item C:\Projects\HillGrov\master\build_p4\hillgrow_master.bin).Length` against 4194304 in the task report: the font's cost is visible here.

- [ ] **Step 6: Bench check**

Run FLASH-P4. On glass:
- Boot lands on the home screen.
- The clock is readable from across the room, and `NTP`/`SET`/`NONE` shows top right.
- With both zones enrolled, the band shows exactly two 240 px FULL tiles centred, with name, dot, soil and light. Tapping a tile opens the Zone placeholder.
- HillGrow opens the Dashboard with the rail, and the rail's Home returns.
- Audio shows the SP7 sentence, and its Home button returns.
- Panel opens the touch test.
- Unset clock: after a power cycle with no STA configured, before any `SET TIME`, the clock reads `--:--` and the date line "Clock not set".

- [ ] **Step 7: The alarm band (quick check; the full one is Stage 1 gate step 4)**

Hold zone 2 in reset (its EN button) for 15 s.
Expected:
- the band turns red and pulses;
- `1 alarm(s) -- tap to view` appears;
- tapping the band background opens the Alarms placeholder, while tapping a tile still opens Zone;
- after release, the band returns to normal once the zone is ONLINE again.

- [ ] **Step 8: Commit**

```powershell
git -C C:\Projects\HillGrov add components/panel_ui/font_clock_180.c components/panel_ui/pnl_fonts.h components/panel_ui/scr_home.c components/panel_ui/scr_audio.c components/panel_ui/scr_shell.c components/panel_ui/CMakeLists.txt
git -C C:\Projects\HillGrov commit -m "feat(panel_ui): clock-hero home screen, status band, Audio placeholder" -m "Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>"
```

### Task 14: Zone view, read-only (Stage 1)

This is the web's zone page (`app.js:801-834`) row for row. It covers the 14 detail rows and the shelf telemetry table, with a way into that zone's Config. Task 22 adds the console and the Replace-board form into the area this task leaves under the table.

**Files:**
- Create: `components/panel_ui/scr_zone.h` (interface addition, below), `components/panel_ui/scr_zone.c`.
- Modify: `components/panel_ui/scr_shell.c` (registry ZONE); `components/panel_ui/CMakeLists.txt`.

**Interfaces:**
- Consumes: `pnl_snap_t` (Task 11); `pnl_fmt_*` and `pnl_zone_name` (Task 10); `pnl_nav_go`, `pnl_nav_arg`, `pnl_shell_snap`, `pnl_label_set_if_changed` and `pnl_obj_show` (Task 12); `state_snap_health_name` (Task 10).
- Produces: `PNL_SCR_ZONE` (arg = zone id; -1 means the last viewed, else the first used zone):
  - A zone selector row for the used zones.
  - "Zone N not found." when the zone is not used.
  - The web's 14 rows: MAC, Health, Firmware, Config gen, Hops, Link, Link stale (yes/no), Config sync (OK/FAILED), Last heard (`(now_ms-last_hb_ms)/1000`s ago), Uptime (s), Heap (KB), Resets, Faults (`0x%llx`), Mode.
  - The shelf table: #, Soil A, Soil B, White, Red, Out on/off, Pump s, over `min(hb.n_shelves, 4)`. With none: "No shelf telemetry.".
  - "Configure this zone" → `pnl_nav_go(PNL_DEST_CONFIG, zone)`.
  - Exported for Task 22: `lv_obj_t *scr_zone_extra_area(void)`, the container below the table where the console and Replace board sections go (NULL when no zone is shown). `uint8_t scr_zone_current(void)`.
- Interface addition: `components/panel_ui/scr_zone.h`, declaring the two exports above, so Task 22's `zone_console.c` and `zone_replace.c` include a header rather than extern-declaring.

**What is provable where.** LVGL glue (+0 host tests). Every value comes from the same `hg_node_t` fields `state_snap.c:152-202` serialises, so parity is a matter of the right field in the right row. The bench proves it: both zones compared with the web's zone page, row for row.

`lv_table` redraws the whole table on any cell change. So cells are compared first and only written when their text differs, which is the table form of `pnl_label_set_if_changed()`.

- [ ] **Step 1: Write the zone view**

`components/panel_ui/scr_zone.h`:

```c
#pragma once
#include <stdint.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The zone view's hooks for Task 22 (console + Replace board). [LVGL] */
lv_obj_t *scr_zone_extra_area(void);   /* the container below the shelf table; NULL when no zone is shown */
uint8_t   scr_zone_current(void);      /* the zone id on screen, 0 when none */

#ifdef __cplusplus
}
#endif
```

`components/panel_ui/scr_zone.c`:

```c
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include "lvgl.h"
#include "state_snap.h"
#include "pnl_fmt.h"
#include "pnl_palette.h"
#include "pnl_theme.h"
#include "scr_shell.h"
#include "scr_zone.h"

/* The web's zone page (app.js:801-834): 14 rows, the shelf table, "Configure
 * this zone". Read-only; Task 22 adds the console and Replace board below. */

#define ZROWS 14
static const char *const ROW_NAME[ZROWS] = {
    "MAC", "Health", "Firmware", "Config gen", "Hops", "Link", "Link stale", "Config sync",
    "Last heard", "Uptime", "Heap", "Resets", "Faults", "Mode" };
static const char *const SHELF_HEAD[7] = { "#", "Soil A", "Soil B", "White", "Red", "Out", "Pump" };

static lv_obj_t *s_sel, *s_title, *s_notfound, *s_body, *s_rows, *s_shelves, *s_noshelf, *s_extra;
static lv_obj_t *s_sel_btn[HG_MAX_ZONES];
static uint8_t   s_sel_id[HG_MAX_ZONES];
static int       s_nsel = -1;
static uint8_t   s_zone;          /* shown now; 0 = none */
static uint8_t   s_last_zone;     /* survives teardown: "-1 = last viewed" */

lv_obj_t *scr_zone_extra_area(void) { return s_zone ? s_extra : NULL; }
uint8_t   scr_zone_current(void)    { return s_zone; }

static void sel_cb(lv_event_t *e) {
    uint8_t id = (uint8_t)(intptr_t)lv_event_get_user_data(e);
    if (id && id != s_zone) pnl_nav_go(PNL_DEST_ZONE, id);
}

static void cfg_cb(lv_event_t *e) {
    (void)e;
    if (s_zone) pnl_nav_go(PNL_DEST_CONFIG, s_zone);
}

static void cell_set(lv_obj_t *t, uint32_t r, uint32_t c, const char *txt) {
    const char *cur = lv_table_get_cell_value(t, r, c);
    if (!cur || strcmp(cur, txt) != 0) lv_table_set_cell_value(t, r, c, txt);
}

static void selector_update(const pnl_snap_t *sn) {
    uint8_t ids[HG_MAX_ZONES];
    int n = 0;
    for (int i = 0; i < HG_MAX_ZONES; i++) if (sn->st.node[i].used) ids[n++] = sn->st.node[i].id;
    if (n != s_nsel || memcmp(ids, s_sel_id, (size_t)n) != 0) {
        lv_obj_clean(s_sel);
        memset(s_sel_btn, 0, sizeof s_sel_btn);
        s_nsel = n;
        memcpy(s_sel_id, ids, (size_t)n);
        for (int k = 0; k < n; k++) {
            lv_obj_t *b = lv_button_create(s_sel);
            char name[17];
            pnl_zone_name(&sn->st.node[ids[k] - 1], name);
            pnl_label(b, name, &lv_font_montserrat_20, PNL_C_TEXT);
            lv_obj_add_event_cb(b, sel_cb, LV_EVENT_CLICKED, (void *)(intptr_t)ids[k]);
            s_sel_btn[k] = b;
        }
    }
    for (int k = 0; k < s_nsel; k++)
        if (s_sel_btn[k])
            lv_obj_set_style_bg_color(s_sel_btn[k], lv_color_hex(s_sel_id[k] == s_zone ? PNL_C_ACCENT : PNL_C_CARD), 0);
}

static void zone_update(const pnl_snap_t *sn) {
    if (!s_body || !sn) return;
    if (!sn->started) { pnl_label_set_if_changed(s_title, "Starting..."); pnl_obj_show(s_body, 0); return; }
    selector_update(sn);

    const psvc_state_t *st = &sn->st;
    const hg_node_t *n = (s_zone >= 1 && s_zone <= HG_MAX_ZONES) ? &st->node[s_zone - 1] : NULL;
    char b[64];
    if (!n || !n->used) {
        snprintf(b, sizeof b, "Zone %u not found.", (unsigned)s_zone);
        pnl_label_set_if_changed(s_notfound, b);
        pnl_obj_show(s_notfound, 1);
        pnl_obj_show(s_body, 0);
        pnl_label_set_if_changed(s_title, "Zone");
        return;
    }
    pnl_obj_show(s_notfound, 0);
    pnl_obj_show(s_body, 1);

    char name[17];
    pnl_zone_name(n, name);
    snprintf(b, sizeof b, "Zone %u -- %s", (unsigned)n->id, name);
    pnl_label_set_if_changed(s_title, b);

    char v[ZROWS][32];
    snprintf(v[0], sizeof v[0], "%02x:%02x:%02x:%02x:%02x:%02x",
             n->mac[0], n->mac[1], n->mac[2], n->mac[3], n->mac[4], n->mac[5]);
    snprintf(v[1], sizeof v[1], "%s", state_snap_health_name(n->health));
    snprintf(v[2], sizeof v[2], "%u.%u.%u", (unsigned)n->hb.fw_maj, (unsigned)n->hb.fw_min, (unsigned)n->hb.fw_patch);
    snprintf(v[3], sizeof v[3], "%u", (unsigned)n->hb.cfg_gen);
    snprintf(v[4], sizeof v[4], "%u", (unsigned)n->hops);
    snprintf(v[5], sizeof v[5], "%u", (unsigned)n->link_flags);
    snprintf(v[6], sizeof v[6], "%s", n->health == NODE_H_OFFLINE ? "yes" : "no");
    snprintf(v[7], sizeof v[7], "%s", st->cfg_sync_failed[n->id - 1] ? "FAILED" : "OK");
    snprintf(v[8], sizeof v[8], "%us ago", (unsigned)((st->now_ms - n->last_hb_ms) / 1000u));
    snprintf(v[9], sizeof v[9], "%us", (unsigned)n->hb.uptime_s);
    snprintf(v[10], sizeof v[10], "%u KB", (unsigned)n->hb.min_free_heap_kb);
    snprintf(v[11], sizeof v[11], "%u", (unsigned)n->hb.reset_reason);
    snprintf(v[12], sizeof v[12], "0x%" PRIx64, (uint64_t)n->hb.active_faults);
    snprintf(v[13], sizeof v[13], "%u", (unsigned)n->hb.mode);
    for (uint32_t r = 0; r < ZROWS; r++) cell_set(s_rows, r, 1, v[r]);

    int ns = n->hb.n_shelves > 4 ? 4 : n->hb.n_shelves;
    pnl_obj_show(s_shelves, ns > 0);
    pnl_obj_show(s_noshelf, ns == 0);
    if (ns > 0) {
        if (lv_table_get_row_count(s_shelves) != (uint32_t)(ns + 1)) lv_table_set_row_count(s_shelves, (uint32_t)(ns + 1));
        for (int i = 0; i < ns; i++) {
            const hg_hb_shelf_t *s = &n->hb.shelf[i];
            uint32_t r = (uint32_t)(i + 1);
            snprintf(b, sizeof b, "%d", i);                         cell_set(s_shelves, r, 0, b);
            snprintf(b, sizeof b, "%u", (unsigned)s->pct_a);        cell_set(s_shelves, r, 1, b);
            snprintf(b, sizeof b, "%u", (unsigned)s->pct_b);        cell_set(s_shelves, r, 2, b);
            snprintf(b, sizeof b, "%u", (unsigned)s->white);        cell_set(s_shelves, r, 3, b);
            snprintf(b, sizeof b, "%u", (unsigned)s->red);          cell_set(s_shelves, r, 4, b);
            cell_set(s_shelves, r, 5, s->out_flags ? "on" : "off");
            snprintf(b, sizeof b, "%us", (unsigned)s->pump_today_s);cell_set(s_shelves, r, 6, b);
        }
    }
}

static void zone_build(lv_obj_t *page, int arg) {
    const pnl_snap_t *sn = pnl_shell_snap();
    uint8_t z = 0;
    if (arg >= 1 && arg <= HG_MAX_ZONES) z = (uint8_t)arg;
    else if (s_last_zone) z = s_last_zone;
    else for (int i = 0; i < HG_MAX_ZONES && !z; i++) if (sn->st.node[i].used) z = sn->st.node[i].id;
    s_zone = z;
    if (z) s_last_zone = z;

    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(page, 16, 0);
    lv_obj_set_style_pad_row(page, 10, 0);

    s_sel = lv_obj_create(page);
    lv_obj_remove_style_all(s_sel);
    lv_obj_set_size(s_sel, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_sel, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(s_sel, 8, 0);
    lv_obj_set_style_pad_row(s_sel, 8, 0);

    s_title = pnl_label(page, "Zone", &lv_font_montserrat_28, PNL_C_TEXT);
    s_notfound = pnl_label(page, "", &lv_font_montserrat_20, PNL_C_MUTED);
    lv_obj_add_flag(s_notfound, LV_OBJ_FLAG_HIDDEN);

    s_body = lv_obj_create(page);
    lv_obj_remove_style_all(s_body);
    lv_obj_set_size(s_body, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_body, 10, 0);

    lv_obj_t *cfg = lv_button_create(s_body);
    pnl_label(cfg, "Configure this zone " LV_SYMBOL_RIGHT, &lv_font_montserrat_20, PNL_C_TEXT);
    lv_obj_add_event_cb(cfg, cfg_cb, LV_EVENT_CLICKED, NULL);

    s_rows = lv_table_create(s_body);
    lv_table_set_column_count(s_rows, 2);
    lv_table_set_row_count(s_rows, ZROWS);
    lv_table_set_column_width(s_rows, 0, 220);
    lv_table_set_column_width(s_rows, 1, 420);
    for (uint32_t r = 0; r < ZROWS; r++) {
        lv_table_set_cell_value(s_rows, r, 0, ROW_NAME[r]);
        lv_table_set_cell_value(s_rows, r, 1, "");
    }

    pnl_label(s_body, "Shelves", &lv_font_montserrat_28, PNL_C_TEXT);
    s_shelves = lv_table_create(s_body);
    lv_table_set_column_count(s_shelves, 7);
    lv_table_set_row_count(s_shelves, 1);
    for (uint32_t c = 0; c < 7; c++) {
        lv_table_set_column_width(s_shelves, c, c == 0 ? 60 : 110);
        lv_table_set_cell_value(s_shelves, 0, c, SHELF_HEAD[c]);
    }
    s_noshelf = pnl_label(s_body, "No shelf telemetry.", &lv_font_montserrat_20, PNL_C_MUTED);

    s_extra = lv_obj_create(s_body);   /* Task 22: console + Replace board */
    lv_obj_remove_style_all(s_extra);
    lv_obj_set_size(s_extra, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_extra, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_extra, 10, 0);

    zone_update(sn);
}

static void zone_teardown(void) {
    s_sel = s_title = s_notfound = s_body = s_rows = s_shelves = s_noshelf = s_extra = NULL;
    memset(s_sel_btn, 0, sizeof s_sel_btn);
    s_nsel = -1;
    s_zone = 0;
}

const pnl_screen_ops_t PNL_SCR_ZONE = { "Zone", zone_build, zone_update, zone_teardown, 1 };
```

- [ ] **Step 2: Swap the registry line and build**

In `components/panel_ui/scr_shell.c`, replace:

```c
    [PNL_DEST_ZONE]      = { "Zone",       &PNL_SCR_PLACEHOLDER, 1 },   /* Task 14 */
```

with:

```c
    [PNL_DEST_ZONE]      = { "Zone",       &PNL_SCR_ZONE,        1 },
```

`components/panel_ui/CMakeLists.txt`: add `"scr_zone.c"` to the end of the `set(PANEL_SRCS ...)` list (after `"scr_audio.c"`).

Run GATE-HOST. Expected: 43/43 tests pass.
Run GATE-P4. Expected: `Project build complete`, 0 warnings, the target grep matches, then the lock checkout.
Run GATE-ESP32. Expected: master, zone and rescue each print `Project build complete` with 0 warnings, and the lock diff is empty. Then re-run the GATE-P4 build line and the lock checkout, for flashing.

- [ ] **Step 3: Bench check**

Run FLASH-P4, then open the Zone view from the rail.
Expected:
- the first used zone shows, and the selector switches between both;
- for each zone, all 14 rows and the shelf table match the web's `#/zone/N` page on a phone (Last heard and Uptime within the 1-2 s poll skew);
- "Configure this zone" opens the Config placeholder;
- the dashboard's zone card opens that zone's view;
- the rail's Zone button reopens the last zone viewed.

- [ ] **Step 4: Commit**

```powershell
git -C C:\Projects\HillGrov add components/panel_ui/scr_zone.h components/panel_ui/scr_zone.c components/panel_ui/scr_shell.c components/panel_ui/CMakeLists.txt
git -C C:\Projects\HillGrov commit -m "feat(panel_ui): read-only zone view" -m "Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>"
```

### Task 15: `alarm_mgr` lock hooks and a struct snapshot (Stage 1)

`alarm_mgr` has no lock at all. Its sink runs inline in whichever task emits a NOTIFY (sometimes under `nmgr_lock`), while the httpd task reads `am_ring` and `am_active` unlocked for `/api/alarms`. The panel poller would add a third party. This task adds:
- an injected lock that is held only for copies and mutations lasting microseconds, never around formatting;
- a struct snapshot the panel can read without JSON.

A `portMUX` rather than a mutex, because the sink can run inside another lock's critical path.

**Files:**
- Modify:
  - `components/alarm_mgr/alarm_mgr.h`: `AM_ACTIVE_MAX` and `AM_KEY_MAX` move here from `alarm_mgr_internal.h`; add the new types and functions.
  - `components/alarm_mgr/alarm_mgr.c`: parse outside the lock, and mutate `am_ring`, `am_total` and `am_active` inside it.
  - `components/alarm_mgr/alarm_mgr_json.c`: copy under the lock into a static `am_snapshot_t` (single caller, httpd), then format outside the lock. The output is byte-identical.
  - `components/alarm_mgr/alarm_mgr_internal.h`: the two defines are removed.
  - `master/main/app_main.c`: `static portMUX_TYPE s_am_mux = portMUX_INITIALIZER_UNLOCKED;` with the enter and exit wrappers; `alarm_mgr_set_lock(am_lock, am_unlock);` immediately after `alarm_mgr_init()`.
  - `tests/host/test_alarm_mgr.c`.

**Interfaces:**
- Consumes: nothing new.
- Produces (`alarm_mgr.h`, pure):
  ```c
  #define AM_ACTIVE_MAX 16
  #define AM_KEY_MAX    16
  typedef struct { char key[AM_KEY_MAX]; char text[72]; uint32_t since_s; } am_active_view_t;
  typedef struct {
      am_active_view_t active[AM_ACTIVE_MAX]; int n_active;
      am_event_t       events[AM_EVENTS];     int n_events;   /* newest first, <= AM_EVENTS */
      uint32_t         total;
  } am_snapshot_t;
  void alarm_mgr_set_lock(void (*lock)(void), void (*unlock)(void));   /* NULL,NULL (default) = no lock; lock holds for copies
                                                                          only (microseconds) -- never around formatting */
  void alarm_mgr_copy(am_snapshot_t *out);                              /* [ANY] */
  /* alarm_mgr_active_count / alarm_mgr_total / alarm_mgr_json / alarm_mgr_sink: unchanged signatures, now lock-bracketed */
  ```

The static `am_snapshot_t` in `alarm_mgr_json()` costs about 6.6 KB of `.bss` on both masters. It is internal RAM on the ESP32 and on the P4 alike, since static data never goes to PSRAM. Record the ESP32 master's `GET STATUS` heap min before and after this task in the task report; the soak bar is ≥ 64 KB.

`alarm_mgr_init()` does not reset the hooks: app_main installs them once, right after init, before any sink can run.

- [ ] **Step 1: Write the failing tests**

In `tests/host/test_alarm_mgr.c`:
- add `#include <stdlib.h>` after `#include <string.h>`;
- replace `void tearDown(void) {}` with:

```c
void tearDown(void) {
    alarm_mgr_set_lock(NULL, NULL);   /* the hooks are process-wide: never leak one test's into the next */
    cJSON_InitHooks(NULL);
}
```

- add before `int main(void)`:

```c
/* ---- panel plan Task 15: lock hooks + the struct snapshot ---- */

static int s_lock_n, s_unlock_n, s_depth, s_max_depth, s_malloc_held;
static void fake_lock(void)   { s_lock_n++; s_depth++; if (s_depth > s_max_depth) s_max_depth = s_depth; }
static void fake_unlock(void) { s_unlock_n++; s_depth--; }
static void *count_malloc(size_t n) { if (s_depth > 0) s_malloc_held++; return malloc(n); }
static void count_free(void *p) { free(p); }
static void hooks_reset(void) { s_lock_n = s_unlock_n = s_depth = s_max_depth = s_malloc_held = 0; }

/* The struct snapshot says exactly what /api/alarms says: same active set in
   the same order, same events newest first, same total -- after the ring has
   wrapped (70 events into 64 slots). */
static void test_copy_matches_json_after_the_ring_wraps(void) {
    char line[64];
    for (int i = 0; i < 70; i++) {
        fake_clock_set(100 + (uint32_t)i);
        snprintf(line, sizeof line, "NOTIFY NODE %d %s\n", 1 + (i % 3), (i % 2) ? "DEGRADED" : "ONLINE");
        alarm_mgr_sink(NULL, line);
    }
    static am_snapshot_t snap;
    alarm_mgr_copy(&snap);
    TEST_ASSERT_EQUAL_UINT32(70, snap.total);
    TEST_ASSERT_EQUAL_INT(AM_EVENTS, snap.n_events);
    TEST_ASSERT_EQUAL_STRING("NODE 1 DEGRADED", snap.events[0].text);   /* i = 69, the newest */
    TEST_ASSERT_EQUAL_INT(2, snap.n_active);                            /* NODE 1 and NODE 2 end DEGRADED, NODE 3 ONLINE */

    static char buf[8192];
    TEST_ASSERT_GREATER_THAN_INT(0, alarm_mgr_json(buf, sizeof buf));
    cJSON *root = cJSON_Parse(buf);
    TEST_ASSERT_NOT_NULL(root);
    cJSON *events = cJSON_GetObjectItem(root, "events");
    TEST_ASSERT_EQUAL_INT(snap.n_events, cJSON_GetArraySize(events));
    for (int i = 0; i < snap.n_events; i++) {
        cJSON *e = cJSON_GetArrayItem(events, i);
        TEST_ASSERT_EQUAL_STRING(snap.events[i].text, cJSON_GetObjectItem(e, "text")->valuestring);
        TEST_ASSERT_EQUAL_UINT32(snap.events[i].at_s, (uint32_t)cJSON_GetObjectItem(e, "at_s")->valuedouble);
    }
    cJSON *active = cJSON_GetObjectItem(root, "active");
    TEST_ASSERT_EQUAL_INT(snap.n_active, cJSON_GetArraySize(active));
    for (int i = 0; i < snap.n_active; i++) {
        cJSON *a = cJSON_GetArrayItem(active, i);
        TEST_ASSERT_EQUAL_STRING(snap.active[i].key, cJSON_GetObjectItem(a, "key")->valuestring);
        TEST_ASSERT_EQUAL_STRING(snap.active[i].text, cJSON_GetObjectItem(a, "text")->valuestring);
        TEST_ASSERT_EQUAL_UINT32(snap.active[i].since_s, (uint32_t)cJSON_GetObjectItem(a, "since_s")->valuedouble);
    }
    TEST_ASSERT_EQUAL_INT((int)snap.total, alarm_mgr_total());
    cJSON_Delete(root);
}

/* Every entry point takes the injected lock exactly once, never nested, and
   always releases it; a malformed line is rejected before any lock. */
static void test_lock_hooks_balance_on_every_entry_point(void) {
    hooks_reset();
    alarm_mgr_set_lock(fake_lock, fake_unlock);
    alarm_mgr_sink(NULL, "NOTIFY NODE 2 DEGRADED\n");
    (void)alarm_mgr_active_count();
    (void)alarm_mgr_total();
    static am_snapshot_t snap;
    alarm_mgr_copy(&snap);
    static char buf[4096];
    (void)alarm_mgr_json(buf, sizeof buf);
    alarm_mgr_sink(NULL, "NOTIFY BOOT 0 0.5.0 POWERON\n");   /* event-only: still one locked mutation */
    alarm_mgr_sink(NULL, "garbage");                          /* malformed: no lock at all */
    TEST_ASSERT_EQUAL_INT(6, s_lock_n);                       /* sink, count, total, copy, json (its copy), sink */
    TEST_ASSERT_EQUAL_INT(s_lock_n, s_unlock_n);
    TEST_ASSERT_EQUAL_INT(1, s_max_depth);
    TEST_ASSERT_EQUAL_INT(0, s_depth);
}

/* The lock covers the copy only: every cJSON allocation during
   alarm_mgr_json happens with no lock held. */
static void test_json_formats_outside_the_lock(void) {
    alarm_mgr_sink(NULL, "NOTIFY NODE 2 DEGRADED\n");
    alarm_mgr_sink(NULL, "NOTIFY RING 0 OPEN Z2 dead or wire Z2->Z1\n");
    hooks_reset();
    alarm_mgr_set_lock(fake_lock, fake_unlock);
    cJSON_Hooks h = { count_malloc, count_free };
    cJSON_InitHooks(&h);
    static char buf[4096];
    TEST_ASSERT_GREATER_THAN_INT(0, alarm_mgr_json(buf, sizeof buf));
    TEST_ASSERT_EQUAL_INT(1, s_lock_n);
    TEST_ASSERT_EQUAL_INT(0, s_malloc_held);
}
```

- add to `main()`, after `RUN_TEST(test_long_line_truncated_safely);`:

```c
    RUN_TEST(test_copy_matches_json_after_the_ring_wraps);
    RUN_TEST(test_lock_hooks_balance_on_every_entry_point);
    RUN_TEST(test_json_formats_outside_the_lock);
```

- [ ] **Step 2: Run it to verify it fails**

Run the one-test command with `-R test_alarm_mgr`.
Expected: the host build stops with `error C2065: 'am_snapshot_t': undeclared identifier` (and C4013 for `alarm_mgr_set_lock` / `alarm_mgr_copy`).

- [ ] **Step 3: Implement the lock hooks and the snapshot**

`components/alarm_mgr/alarm_mgr.h`: after the `am_event_t` typedef (`:18-23`), add:

```c
#define AM_ACTIVE_MAX 16
#define AM_KEY_MAX    16   /* "<TYPE> <node>": longest type name (ALARM/WATER/LIGHT) is 5 + ' ' + up to 3 digits */

/* A consistent copy of everything /api/alarms exports, for a reader that wants
 * structs (the panel). active[] is in the same order the JSON lists it;
 * events[] newest first. ~6.6 KB: callers keep it static or in PSRAM. */
typedef struct { char key[AM_KEY_MAX]; char text[72]; uint32_t since_s; } am_active_view_t;
typedef struct {
    am_active_view_t active[AM_ACTIVE_MAX]; int n_active;
    am_event_t       events[AM_EVENTS];     int n_events;   /* newest first, <= AM_EVENTS */
    uint32_t         total;
} am_snapshot_t;

/* The sink runs inline in whichever task emits a NOTIFY (sometimes inside
 * another component's lock), and readers run on other tasks. Inject a lock:
 * NULL,NULL (the default) = none, which is what the host tests and a
 * single-task build use. The lock is held for copies and mutations only
 * (microseconds) -- never around formatting -- so a spinlock (portMUX) is the
 * right kind. Set it once, right after alarm_mgr_init(), before any sink can
 * run; alarm_mgr_init() does not reset it. */
void alarm_mgr_set_lock(void (*lock)(void), void (*unlock)(void));

void alarm_mgr_copy(am_snapshot_t *out);   /* [ANY] */
```

and change the `alarm_mgr_json` comment to end `..., newest first, <=AM_EVENTS]} -- the state is copied under the lock, then formatted outside it`.

`components/alarm_mgr/alarm_mgr_internal.h`: delete the two `#define`s (`AM_ACTIVE_MAX`, `AM_KEY_MAX`, `:7-8`). They now come from `alarm_mgr.h`, which this header includes.

In `components/alarm_mgr/alarm_mgr.c`:
- after `static uint32_t (*s_now_s)(void);` add:

```c
static void (*s_lock)(void);
static void (*s_unlock)(void);

static void am_lock(void)   { if (s_lock) s_lock(); }
static void am_unlock(void) { if (s_unlock) s_unlock(); }

void alarm_mgr_set_lock(void (*lock)(void), void (*unlock)(void)) {
    s_lock = lock;
    s_unlock = unlock;
}
```

- replace the whole body of `alarm_mgr_sink()` from `uint32_t now = s_now_s ? s_now_s() : 0;` to the end of the function with the following. Everything down to `am_lock()` is parsing on the private `work` copy; only the four mutations run under the lock.

```c
    uint32_t now = s_now_s ? s_now_s() : 0;
    char text[72];
    bcopy_trunc(text, sizeof text, text_start, strlen(text_start));

    /* Decide the active-set effect BEFORE taking the lock: parsing is the slow
     * part, and the lock must cover the mutations only. */
    int  action = 0;                   /* 0 none, 1 upsert, 2 clear */
    char key[AM_KEY_MAX] = "";
    if (type != NTF_BOOT && type != NTF_CMD && type != NTF_WIFI) {
        /* Fleet FW lines (and only FW -- see node_mgr_fleet.c) nest a
         * per-zone status after "ZONE <n>" (e.g. "ZONE 2 UPDATING"): peel
         * that off so the active-set key is "FW <n>" (the affected zone)
         * and the state word checked against ACT_WORDS/CLR_WORDS is the one
         * AFTER "ZONE <n>", not "ZONE" itself. Gated on type == NTF_FW so a
         * RING/NODE/SAFE/etc payload that merely happens to start with
         * "ZONE 2 ..." is never re-keyed this way. A non-numeric token after
         * "ZONE" (or no token at all) falls back to the plain rule: "ZONE"
         * is the state word, which matches neither list, so no active-set
         * effect. */
        size_t w1len, w2len;
        const char *w1after, *w2after;
        const char *w1 = next_word(rest, &w1len, &w1after);
        const char *state_word = w1;
        size_t state_len = w1len;
        uint32_t key_node = node;

        if (type == NTF_FW && w1len == 4 && memcmp(w1, "ZONE", 4) == 0 && *w1after == ' ') {
            const char *w2 = next_word(w1after + 1, &w2len, &w2after);
            unsigned zone_val;
            if (parse_u8_word(w2, w2len, &zone_val) == 0 && *w2after == ' ') {
                size_t w3len;
                const char *w3after;
                const char *w3 = next_word(w2after + 1, &w3len, &w3after);
                if (w3len > 0) {
                    key_node = zone_val;
                    state_word = w3;
                    state_len = w3len;
                }
            }
        }

        snprintf(key, sizeof key, "%s %u", notify_type_name(type), (unsigned)key_node);
        if (is_activate_word(state_word, state_len))   action = 1;
        else if (is_clear_word(state_word, state_len)) action = 2;
    }

    am_lock();
    am_event_t *ev = &am_ring[am_total % AM_EVENTS];
    ev->at_s = now;
    ev->type = (uint8_t)type;
    ev->node = node;
    memcpy(ev->text, text, sizeof ev->text);
    am_total++;
    if (action == 1)      active_upsert(key, text, now);
    else if (action == 2) active_clear(key);
    am_unlock();
}
```

- replace `alarm_mgr_active_count()` and `alarm_mgr_total()` with:

```c
int alarm_mgr_active_count(void) {
    int c = 0;
    am_lock();
    for (int i = 0; i < AM_ACTIVE_MAX; i++) if (am_active[i].used) c++;
    am_unlock();
    return c;
}

int alarm_mgr_total(void) {
    am_lock();
    uint32_t t = am_total;
    am_unlock();
    return (int)t;
}

void alarm_mgr_copy(am_snapshot_t *out) {
    if (!out) return;
    memset(out, 0, sizeof *out);   /* outside the lock */
    am_lock();
    int k = 0;
    for (int i = 0; i < AM_ACTIVE_MAX; i++) {
        if (!am_active[i].used) continue;
        memcpy(out->active[k].key, am_active[i].key, sizeof out->active[k].key);
        memcpy(out->active[k].text, am_active[i].text, sizeof out->active[k].text);
        out->active[k].since_s = am_active[i].since_s;
        k++;
    }
    out->n_active = k;
    uint32_t kept = am_total < AM_EVENTS ? am_total : AM_EVENTS;
    for (uint32_t i = 0; i < kept; i++) out->events[i] = am_ring[(am_total - 1 - i) % AM_EVENTS];   /* newest first */
    out->n_events = (int)kept;
    out->total = am_total;
    am_unlock();
}
```

Replace the whole of `components/alarm_mgr/alarm_mgr_json.c` with:

```c
#include <string.h>
#include "cJSON.h"
#include "alarm_mgr.h"
#include "alarm_mgr_internal.h"

int alarm_mgr_json(char *out, size_t cap) {
    /* Copied under the lock (inside alarm_mgr_copy), formatted outside it, so
     * the lock is held for microseconds and never across cJSON's allocations.
     * Static because it is ~6.6 KB and there is one caller: /api/alarms on the
     * single httpd task. */
    static am_snapshot_t s;
    alarm_mgr_copy(&s);

    cJSON *root = cJSON_CreateObject();

    cJSON *active = cJSON_CreateArray();
    for (int i = 0; i < s.n_active; i++) {
        cJSON *a = cJSON_CreateObject();
        cJSON_AddStringToObject(a, "key", s.active[i].key);
        cJSON_AddStringToObject(a, "text", s.active[i].text);
        cJSON_AddNumberToObject(a, "since_s", (double)s.active[i].since_s);
        cJSON_AddItemToArray(active, a);
    }
    cJSON_AddItemToObject(root, "active", active);

    cJSON *events = cJSON_CreateArray();
    for (int i = 0; i < s.n_events; i++) {   /* already newest first */
        cJSON *e = cJSON_CreateObject();
        cJSON_AddNumberToObject(e, "at_s", (double)s.events[i].at_s);
        cJSON_AddStringToObject(e, "text", s.events[i].text);
        cJSON_AddItemToArray(events, e);
    }
    cJSON_AddItemToObject(root, "events", events);

    cJSON_bool ok = cJSON_PrintPreallocated(root, out, (int)cap, 0);
    cJSON_Delete(root);
    return ok ? (int)strlen(out) : -1;
}
```

- [ ] **Step 4: Run it to verify it passes**

Run the one-test command with `-R test_alarm_mgr`.
Expected: `100% tests passed, 0 tests failed out of 1`. The 14 existing cases pass unchanged, which is the byte-identical JSON proof, and the 3 new ones pass too.

- [ ] **Step 5: Install the lock on the master**

In `master/main/app_main.c`, add after `static uint8_t  master_id_fn(void) { return 0; } ...` (`:30`):

```c
/* alarm_mgr's lock (panel plan Task 15): its sink runs inline in whichever
 * task emits a NOTIFY -- sometimes inside node_mgr's own lock -- while httpd
 * and the panel poller read it. A spinlock, because the critical sections are
 * a few copies long and a sink must never block. */
static portMUX_TYPE s_am_mux = portMUX_INITIALIZER_UNLOCKED;
static void am_lock(void)   { portENTER_CRITICAL(&s_am_mux); }
static void am_unlock(void) { portEXIT_CRITICAL(&s_am_mux); }
```

and immediately after the line `alarm_mgr_init(hg_app_uptime_s);` add:

```c
    alarm_mgr_set_lock(am_lock, am_unlock);   /* before the sink is registered below: nothing can emit yet */
```

- [ ] **Step 6: Run the gates**

Run GATE-HOST. Expected: 43/43 tests pass (+0 files; `test_alarm_mgr` has 3 more cases).
Run GATE-ESP32. Expected: master, zone and rescue each print `Project build complete` with 0 warnings, and the lock diff is empty. The zone links `alarm_mgr` too, with no hooks set there.
Run GATE-P4. Expected: `Project build complete`, 0 warnings, the target grep matches, then the lock checkout.

The live proof is `web_test --only state` plus the alarms page at the Stage 1 gate, where the web's alarm list must be unchanged.

- [ ] **Step 7: Commit**

```powershell
git -C C:\Projects\HillGrov add components/alarm_mgr/alarm_mgr.h components/alarm_mgr/alarm_mgr.c components/alarm_mgr/alarm_mgr_json.c components/alarm_mgr/alarm_mgr_internal.h master/main/app_main.c tests/host/test_alarm_mgr.c
git -C C:\Projects\HillGrov commit -m "fix(alarm_mgr): lock the sink against its readers; add a struct snapshot" -m "Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>"
```

### Task 16: Alarms view; the poller publishes alarm snapshots (Stage 1)

This is the web's alarms page (`app.js:896-911`): the active set with key, text and age, and the history newest first. Ages are `uptime - stamp`, because the stamps are absolute uptime (the bug the web fixed, `what_we_learned.md:155`). The poller copies `alarm_mgr` only when something new arrived, and the screen re-renders its lists only when that copy changes. Ages tick every poll.

**Files:**
- Create: `components/panel_ui/scr_alarms.c`.
- Modify:
  - `components/panel_ui/pnl_poll.h`, `.c`: call `alarm_mgr_copy` when `alarm_mgr_total()` changed since the last copy; keep a second PSRAM double buffer.
  - `components/panel_ui/scr_shell.c` (registry ALARMS); `components/panel_ui/CMakeLists.txt`.

**Interfaces:**
- Consumes: `alarm_mgr_copy` and `am_snapshot_t` (Task 15); `pnl_fmt_age` (Task 10); `pnl_shell_snap`, `pnl_obj_show` (Task 12).
- Produces:
  ```c
  void     pnl_poll_alarms(am_snapshot_t *out);   /* [ANY] copy of the last published alarm snapshot */
  uint32_t pnl_poll_alarms_seq(void);             /* [ANY] */
  ```
  And `PNL_SCR_ALARMS`, laid out like the web:
  - Active: key, text and "<age> ago";
  - History: newest first, "<age> ago" and text;
  - the empty states "No active alarms." and "No events.";
  - age = `st.uptime_s - stamp`, shown as "--" before the first snapshot.

Two `lv_table`s, not rows of labels. 64 history rows of labels would be about 200 LVGL objects (tens of KB of the 64 KB LVGL pool), while a table is one object with its cell strings. The screen's `am_snapshot_t` lives in PSRAM, allocated on build and freed on teardown.

**What is provable where.** Glue (+0). The age maths and wording are Task 10's host-tested `pnl_fmt_age()`, and the snapshot's equality with `/api/alarms` is Task 15's host test. The bench proves the blame line and its ages match the web's alarms page while a zone is held in reset (Stage 1 gate step 4).

- [ ] **Step 1: Publish alarm snapshots from the poller**

`components/panel_ui/pnl_poll.h`: add `#include "alarm_mgr.h"` after `#include "pnl_home.h"`, and before the `#ifdef __cplusplus` closing block add:

```c
/* [ANY] The last published alarm_mgr snapshot. The poller re-copies only when
 * alarm_mgr_total() moved (every NOTIFY bumps it), so this is cheap to poll.
 * ~6.6 KB: out lives in PSRAM or static storage, never on the LVGL stack. */
void     pnl_poll_alarms(am_snapshot_t *out);
uint32_t pnl_poll_alarms_seq(void);   /* [ANY] increments once per new alarm snapshot */
```

In `components/panel_ui/pnl_poll.c`, add `#include "alarm_mgr.h"`, and after `static hg_zone_hw_t      s_hw;` add:

```c
static am_snapshot_t    *s_am_stage, *s_am_pub;   /* PSRAM, same stage + published pattern as the state */
static portMUX_TYPE      s_am_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t s_am_seq;
static int64_t           s_am_total_seen = -1;

static void alarms_poll(void) {
    if (!s_am_stage || !s_am_pub) return;
    int total = alarm_mgr_total();
    if ((int64_t)total == s_am_total_seen) return;
    alarm_mgr_copy(s_am_stage);                    /* alarm_mgr's own lock, microseconds */
    s_am_total_seen = (int64_t)s_am_stage->total;
    portENTER_CRITICAL(&s_am_mux);
    memcpy(s_am_pub, s_am_stage, sizeof *s_am_pub);
    s_am_seq++;
    portEXIT_CRITICAL(&s_am_mux);
}
```

In `poll_task()`, immediately after `heartbeat_check(t0);`, add `alarms_poll();`. That puts it before the state publish, so a screen's `update()` for the new state seq already sees the new alarms seq.

In `pnl_poll_start()`, after the `s_pub` allocation and its NULL check, add:

```c
    s_am_stage = heap_caps_calloc(1, sizeof(am_snapshot_t), MALLOC_CAP_SPIRAM);
    s_am_pub   = heap_caps_calloc(1, sizeof(am_snapshot_t), MALLOC_CAP_SPIRAM);
    if (!s_am_stage || !s_am_pub) ESP_LOGE(TAG, "no PSRAM for the alarm snapshot -- the Alarms screen stays empty");
```

Append at the end of the file:

```c
void pnl_poll_alarms(am_snapshot_t *out) {
    if (!s_am_pub) { memset(out, 0, sizeof *out); return; }
    portENTER_CRITICAL(&s_am_mux);
    memcpy(out, s_am_pub, sizeof *out);
    portEXIT_CRITICAL(&s_am_mux);
}

uint32_t pnl_poll_alarms_seq(void) { return s_am_seq; }
```

- [ ] **Step 2: Write the Alarms screen**

`components/panel_ui/scr_alarms.c`:

```c
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "esp_heap_caps.h"
#include "lvgl.h"
#include "alarm_mgr.h"
#include "pnl_fmt.h"
#include "pnl_palette.h"
#include "pnl_theme.h"
#include "pnl_poll.h"
#include "scr_shell.h"

/* The web's alarms page (app.js:896-911). No acknowledge or clear: the web
 * writes nothing here either. */

static am_snapshot_t *s_am;          /* PSRAM, this screen's copy; freed on teardown */
static uint32_t       s_am_seen;
static uint8_t        s_have;        /* s_am holds at least one published snapshot */
static lv_obj_t      *s_act, *s_act_empty, *s_ev, *s_ev_empty, *s_oom;

static void cell_set(lv_obj_t *t, uint32_t r, uint32_t c, const char *txt) {
    const char *cur = lv_table_get_cell_value(t, r, c);
    if (!cur || strcmp(cur, txt) != 0) lv_table_set_cell_value(t, r, c, txt);
}

static void age_text(const pnl_snap_t *sn, uint32_t stamp, char *b, size_t cap) {
    if (!sn || !sn->started) { snprintf(b, cap, "--"); return; }   /* HG.alarmAgo: no uptime yet -> a dash */
    pnl_fmt_age(sn->st.uptime_s, stamp, b, cap);
}

/* Only when the poller published a new alarm snapshot. */
static void lists_render(void) {
    pnl_obj_show(s_act_empty, s_am->n_active == 0);
    pnl_obj_show(s_act, s_am->n_active > 0);
    if (s_am->n_active > 0) {
        if (lv_table_get_row_count(s_act) != (uint32_t)s_am->n_active) lv_table_set_row_count(s_act, (uint32_t)s_am->n_active);
        for (int i = 0; i < s_am->n_active; i++) {
            cell_set(s_act, (uint32_t)i, 0, s_am->active[i].key);
            cell_set(s_act, (uint32_t)i, 1, s_am->active[i].text);
        }
    }
    pnl_obj_show(s_ev_empty, s_am->n_events == 0);
    pnl_obj_show(s_ev, s_am->n_events > 0);
    if (s_am->n_events > 0) {
        if (lv_table_get_row_count(s_ev) != (uint32_t)s_am->n_events) lv_table_set_row_count(s_ev, (uint32_t)s_am->n_events);
        for (int i = 0; i < s_am->n_events; i++) cell_set(s_ev, (uint32_t)i, 1, s_am->events[i].text);
    }
}

/* Every poll: the ages move even when the lists do not. */
static void ages_render(const pnl_snap_t *sn) {
    char b[24];
    for (int i = 0; i < s_am->n_active; i++) {
        age_text(sn, s_am->active[i].since_s, b, sizeof b);
        cell_set(s_act, (uint32_t)i, 2, b);
    }
    for (int i = 0; i < s_am->n_events; i++) {
        age_text(sn, s_am->events[i].at_s, b, sizeof b);
        cell_set(s_ev, (uint32_t)i, 0, b);
    }
}

static void alarms_update(const pnl_snap_t *sn) {
    if (!s_am || !s_act) return;
    uint32_t seq = pnl_poll_alarms_seq();
    if (!s_have || seq != s_am_seen) {
        pnl_poll_alarms(s_am);
        s_am_seen = seq;
        s_have = 1;
        lists_render();
    }
    ages_render(sn);
}

static lv_obj_t *table(lv_obj_t *parent, const int32_t *widths, uint32_t cols) {
    lv_obj_t *t = lv_table_create(parent);
    lv_table_set_column_count(t, cols);
    lv_table_set_row_count(t, 1);
    for (uint32_t c = 0; c < cols; c++) lv_table_set_column_width(t, c, widths[c]);
    lv_obj_add_flag(t, LV_OBJ_FLAG_HIDDEN);
    return t;
}

static void alarms_build(lv_obj_t *page, int arg) {
    (void)arg;
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(page, 16, 0);
    lv_obj_set_style_pad_row(page, 10, 0);
    pnl_label(page, "Alarms", &lv_font_montserrat_28, PNL_C_TEXT);

    s_am = heap_caps_calloc(1, sizeof(am_snapshot_t), MALLOC_CAP_SPIRAM);
    if (!s_am) {
        s_oom = pnl_label(page, "Out of memory -- alarms unavailable. Try again shortly.", &lv_font_montserrat_20, PNL_C_WARN_TEXT);
        return;
    }
    s_have = 0;

    static const int32_t ACT_W[3] = { 140, 560, 160 };
    static const int32_t EV_W[2]  = { 160, 700 };
    pnl_label(page, "Active", &lv_font_montserrat_20, PNL_C_MUTED);
    s_act_empty = pnl_label(page, "No active alarms.", &lv_font_montserrat_20, PNL_C_MUTED);
    s_act = table(page, ACT_W, 3);
    pnl_label(page, "History", &lv_font_montserrat_20, PNL_C_MUTED);
    s_ev_empty = pnl_label(page, "No events.", &lv_font_montserrat_20, PNL_C_MUTED);
    s_ev = table(page, EV_W, 2);

    alarms_update(pnl_shell_snap());
}

static void alarms_teardown(void) {
    if (s_am) { heap_caps_free(s_am); s_am = NULL; }
    s_act = s_act_empty = s_ev = s_ev_empty = s_oom = NULL;
    s_have = 0;
}

const pnl_screen_ops_t PNL_SCR_ALARMS = { "Alarms", alarms_build, alarms_update, alarms_teardown, 1 };
```

- [ ] **Step 3: Swap the registry line and build**

In `components/panel_ui/scr_shell.c`, replace:

```c
    [PNL_DEST_ALARMS]    = { "Alarms",     &PNL_SCR_PLACEHOLDER, 1 },   /* Task 16 */
```

with:

```c
    [PNL_DEST_ALARMS]    = { "Alarms",     &PNL_SCR_ALARMS,      1 },
```

`components/panel_ui/CMakeLists.txt`: add `"scr_alarms.c"` to the end of the `set(PANEL_SRCS ...)` list (after `"scr_zone.c"`).

Run GATE-HOST. Expected: 43/43 tests pass.
Run GATE-P4. Expected: `Project build complete`, 0 warnings, the target grep matches, then the lock checkout.
Run GATE-ESP32. Expected: master, zone and rescue each print `Project build complete` with 0 warnings, and the lock diff is empty. Then re-run the GATE-P4 build line and the lock checkout, for flashing. Then run the Stage 1 gate below.

- [ ] **Step 4: Commit**

```powershell
git -C C:\Projects\HillGrov add components/panel_ui/scr_alarms.c components/panel_ui/pnl_poll.h components/panel_ui/pnl_poll.c components/panel_ui/scr_shell.c components/panel_ui/CMakeLists.txt
git -C C:\Projects\HillGrov commit -m "feat(panel_ui): alarms view" -m "Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>"
```

### Stage 1 bench gate (owner, about 45 min)

Setup:
- the P4 master on COM28, both zones on the ring, and the PC joined to the master's AP;
- a phone logged into the web UI, for side-by-side comparison;
- the Stage 1 build (Task 16's) flashed with FLASH-P4: `GET VERSION` shows `VALID` first, then the dry run, then the real run.

Close the console whenever `uart_test.py` or the web suite needs the port or the AP.

1. **The clock.**
   - (a) Take NTP away first: unplug the house AP, or on the phone's web Config → Master clear the STA SSID and save. Then power-cycle the master with no `SET TIME` sent. Pass: the home clock reads `--:--`, the date line reads "Clock not set", and the context line is "Clock not set". Fail: any plausible time.
   - (b) In the console, send `SET TIME <today's UTC date> <UTC time now>` (for example `SET TIME 2026-09-28 12:34:00`). Pass: within about 1 s the clock shows the correct **local** time, compared with a phone, and the date line shows today's local date.
   - Restore: plug the house AP back in, or on the phone's web System → Wi-Fi re-join the house network with its password. Then run (c).
   - (c) With the STA joined to the house network (internet), NTP gives the same result after a normal boot, and the top bar reads `NTP`.
2. **The context line.** On the phone, read LIGHT ON/OFF for every enabled shelf of zones 1 and 2 from the web Config pages. Pass: while any of those lights is on, the home context line reads "Lights off in Xh Ym" for the **earliest** OFF among the lit shelves (X/Y within a minute of hand arithmetic); otherwise it reads "Lights on at HH:MM" for the **soonest** ON among them. With no enabled shelf on either zone the line follows the watering rule, or is blank.
3. **The status band.** Pass: exactly one tile per enrolled zone (two here), 240 px each, centred, each with its name, health dot, soil and light.
   - Optional, only if the owner is happy to re-enrol a zone: note zone 2's MAC from the Zone view, then send `CLEAR NODE 2 CONFIRM` in the console. Pass: its tile disappears on the next poll and the remaining tile re-centres.
   - Restore with `SET NODE 2 MAC <that MAC>`. The board is adopted back into zone 2 on its next heartbeat (`node_mgr.h:90-100`). Pass: the tile returns.
4. **Alarms, end to end.** Hold zone 2 in reset (its EN button) and watch:
   - its tile's dot goes DEGRADED (amber) at about 5 s, then OFFLINE (red) at about 10 s;
   - the band turns red and pulses, with the badge "N alarm(s) -- tap to view";
   - tapping the band background (not a tile) opens Alarms.

   On Alarms:
   - Active lists `NODE 2` with its text and an age that ticks each second.
   - History lists the RING/NODE events newest first.
   - Compare with the phone's `#/alarms`. Pass: the same keys, the same texts, and ages within 2 s of each other.

   Release the zone. Pass: the tile returns to ONLINE (green), the band stops pulsing, and the badge disappears.
5. **Every destination, with a finger.** From Home, tap:
   - HillGrow → the rail appears;
   - Dashboard, then compare with the phone's dashboard field for field (ring banner, master card version/time/STA/AP/heap min, both zone cards);
   - Zone → zone 1, then zone 2 via the selector, then compare all 14 rows and the shelf table with `#/zone/1` and `#/zone/2`;
   - Alarms (as above);
   - Config → "Config: not available yet";
   - System → "System: not available yet";
   - Home (rail);
   - Audio → the SP7 sentence → Home;
   - Panel → the touch test, where the five-target check from Stage 0 passes again → Home.

   Pass: every tap visibly responds under the finger, lands where named, and the console shows no `took N ms (budget 200)` WARN.
6. **Regression and budgets.** With the panel running (leave it on Home):
   - Run `python C:\Projects\HillGrov\tools\web_test.py 192.168.7.7 --password hillgrow1 --only state`. Pass: all PASS; the `/api/state` shape is unchanged after the gather moved.
   - Run `python C:\Projects\HillGrov\tools\web_test.py 192.168.7.7 --password hillgrow1 --only mcfg`. Pass: all PASS.
   - After 10 minutes on Home, having visited every destination at least once, open Panel (the touch test). Record in the Stage 1 report the internal heap min from its heap line and the `LVGL pool: max used N of M KB` figure from its poller line.
   - Pass: internal min ≥ 64 KB (D25). The pool max must be ≤ 75 % of the internal pool (the `of M KB internal` figure); above that, raise `CONFIG_LV_MEM_SIZE_KILOBYTES` to 128 in `master/sdkconfig.defaults.esp32p4` (delete `build_p4\sdkconfig` first), but only if internal min stays ≥ 64 KB. Record either outcome.

The stage passes only if every step passes. Record each step's evidence (photos or notes of the glass, web_test summaries, heap numbers) in the Stage 1 report.

---

## Stage 2 — The type-driven config editor (generated from `hg_cfg_fields` / `hg_mcfg`)

Tasks 17-21. The editor is generated: `pcfg_gen` turns every field row of `HG_FIELDS` (zone) and `HG_MFIELDS`
(master) into a `pcfg_spec_t`, and `wdg_field` turns a spec into one LVGL row. No field is hand-built. A field
added to either table later renders on its own (generator defaults), and the Task 17 coverage test fails until the
new row gets its presentation entry.

### Task 17: Config-widget generator and presentation table; `hg_group_is_hw()` (Stage 2)

**Files:**
- Create: `components/panel_ui/pcfg_gen.h`, `components/panel_ui/pcfg_gen.c` (P)
- Create: `components/panel_ui/pcfg_pres.h`, `components/panel_ui/pcfg_pres.c` (P)
- Create: `tests/host/test_pcfg_gen.c`
- Modify: `components/hg_cfg/hg_cfg.h` (declaration after `hg_group_scope`, today line 20)
- Modify: `components/hg_cfg/hg_cfg_fields.c` (definition after `hg_group_scope()`, today lines 91-95)
- Modify: `tests/host/test_hg_cfg_fields.c` (new test + `RUN_TEST`)
- Modify: `tests/host/CMakeLists.txt` (append one `hg_test` row)
- Modify: `components/panel_ui/CMakeLists.txt` (append two sources)

**Interfaces:**
- Consumes: `HG_FIELDS`, `HG_FIELD_COUNT`, `HG_GROUP_NAMES`, `hg_group_find`, `hg_group_scope`, `hg_hhmm_parse`,
  `hg_hhmm_format`, `hg_field_base`, `hg_field_read`, `hg_field_write` (`components/hg_cfg/hg_cfg.h:16-38`);
  `HG_MFIELDS`, `HG_MFIELD_COUNT`, `HG_MGROUP_NAMES`, `hg_mgroup_t`, `hg_mcfg_is_secret`
  (`components/hg_mcfg/hg_mcfg.h:31-45`); `HG_NONE`, `HG_MAX_SHELVES`, `HG_MAX_AUX`, `hg_zone_hw_t`
  (`components/hg_cfg/hg_cfg_types.h`).
- Produces (one addition marked):
  ```c
  /* hg_cfg.h */
  int hg_group_is_hw(uint8_t group);   /* 1 for HG_G_HW, HG_G_HWSHELF, HG_G_CAL (the read-only hardware plane), else 0 */

  /* pcfg_gen.h (pure) */
  typedef enum { PCFG_TABLE_ZONE = 0, PCFG_TABLE_MASTER } pcfg_table_t;
  typedef enum { PCFG_K_STEPPER = 0, PCFG_K_SWITCH, PCFG_K_SEGMENTED, PCFG_K_ROLLER, PCFG_K_TEXT, PCFG_K_SECRET, PCFG_K_READONLY } pcfg_kind_t;
  typedef enum { PCFG_KB_NONE = 0, PCFG_KB_NUMERIC, PCFG_KB_TEXT, PCFG_KB_TEXT_NOSPACE, PCFG_KB_HOSTNAME, PCFG_KB_HEX } pcfg_kb_t;
  typedef enum { PCFG_ROLL_NONE = 0, PCFG_ROLL_HHMM, PCFG_ROLL_ENUM, PCFG_ROLL_PIN } pcfg_roll_t;
  typedef enum { PCFG_FMT_DEC = 0, PCFG_FMT_HEX, PCFG_FMT_HHMM, PCFG_FMT_ENUM, PCFG_FMT_PIN, PCFG_FMT_TEXT, PCFG_FMT_BOOL, PCFG_FMT_SCALED } pcfg_fmt_t;
  #define PCFG_MAX_OPTS 8
  #define PCFG_OPT_LEN  16
  typedef struct {
      pcfg_kind_t kind, edit_kind;   /* READONLY rows keep edit_kind for formatting */
      uint8_t     ftype, readonly;
      int32_t     min, max, step, big_step;
      int16_t     scale_div;         /* 1; 10 for LIGHT.DLI */
      int32_t     none_value;        /* 255 for PIN; -1 none */
      pcfg_roll_t roller;
      uint8_t     n_opts; char opts[PCFG_MAX_OPTS][PCFG_OPT_LEN];
      pcfg_kb_t   keyboard;
      uint8_t     min_len, max_len;
      pcfg_fmt_t  fmt;
      const char *label, *unit;      /* presentation, else key / "" */
      const char *zero_text;         /* "off" for LIGHT.DLI, NULL otherwise */
  } pcfg_spec_t;
  int     pcfg_spec_for(pcfg_table_t t, const hg_field_t *f, pcfg_spec_t *out);  /* 0 / -1 (NULL or unknown ftype -> READONLY+TEXT) */
  void    pcfg_tighten(pcfg_spec_t *s, const hg_field_t *f, int shelf, const hg_zone_hw_t *hw_or_null);
  int     pcfg_format(const pcfg_spec_t *s, const char *raw, char *out, size_t cap);
  int     pcfg_parse_raw(const pcfg_spec_t *s, const char *raw, int32_t *v);
  int     pcfg_raw_text(const pcfg_spec_t *s, int32_t v, char *out, size_t cap);
  int32_t pcfg_step(const pcfg_spec_t *s, int32_t cur, int dir, int big);
  int     pcfg_locate(pcfg_table_t t, const char *path, uint8_t *group, int *idx, const hg_field_t **row);

  /* pcfg_pres.h (pure) */
  typedef struct {
      uint8_t table, group; const char *key;
      const char *label, *unit;
      int32_t big_step;
      int16_t scale_div;
      uint8_t min_len, max_len;
      uint8_t keyboard;
      uint8_t fmt_hex;
      uint8_t readonly;
      const char *zero_text;      /* ADDITION: display text for raw 0 (LIGHT.DLI "off"); NULL = none */
  } pcfg_pres_t;
  extern const pcfg_pres_t PCFG_PRES[];
  extern const int         PCFG_PRES_COUNT;
  const pcfg_pres_t *pcfg_pres_find(pcfg_table_t t, uint8_t group, const char *key);
  ```
  `pcfg_pres_t.zero_text` is the one name this task adds to the outline. It keeps "0 means off" in the
  presentation table instead of hard-coding the DLI key in the generator.

  Semantics fixed by this task (later tasks rely on them):
  - `pcfg_spec_for` with `PCFG_TABLE_MASTER` and a row whose group is `>= HG_MG_COUNT`, or `PCFG_TABLE_ZONE` and
    `>= HG_G_COUNT`, returns -1 with a READONLY/TEXT spec. An ENUM whose `enums` is NULL, has more than
    `PCFG_MAX_OPTS` names, or has a name of `PCFG_OPT_LEN` or more characters also returns -1 with READONLY/TEXT.
  - `pcfg_format` returns 0, or -1 when `raw` does not parse for the spec's format; on -1, `out` holds `raw` verbatim,
    so the row always shows something.
  - `pcfg_parse_raw` returns -1 for TEXT and SECRET specs, which carry text, not numbers.
  - `pcfg_raw_text` returns -1 for an HHMM value outside 0..1439, an ENUM index outside 0..n_opts-1, and every
    text type. It does not range-check U8/U16/PIN: `hg_field_write` does that and answers -2.
  - `pcfg_locate` returns 0 with `*idx == -1` for scope-0 groups, for every master row, and for the validator's
    unindexed AUX path `aux.<key>` (`hg_cfg_validate.c:98` drops the aux index). The caller keeps its current aux
    index for that case.
  - `pcfg_tighten` must only be called for zone-table specs.

**What each check proves:** everything in this task is pure and host-tested. GATE-P4 proves the same files compile
warning-free under GCC for RISC-V. There is no on-glass check until Task 20.

- [ ] **Step 1: Write the failing test for `hg_group_is_hw()`**

  In `tests/host/test_hg_cfg_fields.c`, add this function before `int main(void)`:
  ```c
  /* The read-only hardware plane has one C home: hg_group_is_hw(). The web's isHwGroup()
   * (web/app.js:623) and hg_json's "hw" section encode the same three groups. */
  static void test_group_is_hw(void) {
      for (int g = 0; g < HG_G_COUNT; g++) {
          int want = (g == HG_G_HW || g == HG_G_HWSHELF || g == HG_G_CAL) ? 1 : 0;
          TEST_ASSERT_EQUAL_INT_MESSAGE(want, hg_group_is_hw((uint8_t)g), HG_GROUP_NAMES[g]);
      }
      TEST_ASSERT_EQUAL_INT(0, hg_group_is_hw((uint8_t)HG_G_COUNT));
      TEST_ASSERT_EQUAL_INT(0, hg_group_is_hw(255));
  }
  ```
  and in `main()` add, after `RUN_TEST(test_hhmm_truncation);`:
  ```c
      RUN_TEST(test_group_is_hw);
  ```

- [ ] **Step 2: Run it and watch it fail**

  Run the single host test with `<name>` = `test_hg_cfg_fields`.
  Expected: the build fails with `warning C4013: 'hg_group_is_hw' undefined; assuming extern returning int`,
  then `error LNK2019: unresolved external symbol hg_group_is_hw` and `fatal error LNK1120`.

- [ ] **Step 3: Implement `hg_group_is_hw()`**

  In `components/hg_cfg/hg_cfg.h`, after the `hg_group_scope` declaration line, add:
  ```c
  int  hg_group_is_hw(uint8_t group);                    /* 1 for HG_G_HW, HG_G_HWSHELF, HG_G_CAL (the read-only hardware plane), else 0 */
  ```
  In `components/hg_cfg/hg_cfg_fields.c`, after the closing brace of `hg_group_scope()`, add:
  ```c
  int hg_group_is_hw(uint8_t group) {
      return group == HG_G_HW || group == HG_G_HWSHELF || group == HG_G_CAL;
  }
  ```

- [ ] **Step 4: Run it and watch it pass**

  Run the single host test with `<name>` = `test_hg_cfg_fields`.
  Expected: `7 Tests 0 Failures 0 Ignored` and `100% tests passed, 0 tests failed out of 1`.

- [ ] **Step 5: Write the failing generator test**

  Append to `tests/host/CMakeLists.txt`:
  ```cmake
  # panel_ui's config-widget generator (Task 17): pure, linked against the real field tables.
  hg_test(test_pcfg_gen ${COMP}/panel_ui/pcfg_gen.c ${COMP}/panel_ui/pcfg_pres.c ${HG_CFG_SRC}
          ${COMP}/hg_mcfg/hg_mcfg.c ${COMP}/hg_blob/hg_blob.c)
  ```
  (`${COMP}/panel_ui` is already on the include path from Task 7.)

  Create `tests/host/test_pcfg_gen.c`. It implements the 35 cases of map-cfg §8 (`pw_*` renamed `pcfg_*`):
  ```c
  /* test_pcfg_gen.c -- the config-widget generator: given a field row, which widget kind, bounds,
   * read-only flag and text form come out (panel spec "Testing" 1). The 35 cases of map-cfg §8. */
  #include <stdio.h>
  #include <string.h>
  #include "unity.h"
  #include "hg_cfg.h"
  #include "hg_mcfg.h"
  #include "pcfg_gen.h"
  #include "pcfg_pres.h"

  static hg_zone_hw_t  hw;
  static hg_zone_cfg_t cfg;
  void setUp(void) { hg_defaults_hw(&hw); hg_defaults_cfg(&cfg); }
  void tearDown(void) {}

  static const hg_field_t *zrow(uint8_t g, const char *k) {
      for (int i = 0; i < HG_FIELD_COUNT; i++)
          if (HG_FIELDS[i].group == g && strcmp(HG_FIELDS[i].key, k) == 0) return &HG_FIELDS[i];
      TEST_FAIL_MESSAGE(k);
      return NULL;
  }
  static const hg_field_t *mrow(uint8_t g, const char *k) {
      for (int i = 0; i < HG_MFIELD_COUNT; i++)
          if (HG_MFIELDS[i].group == g && strcmp(HG_MFIELDS[i].key, k) == 0) return &HG_MFIELDS[i];
      TEST_FAIL_MESSAGE(k);
      return NULL;
  }
  static pcfg_spec_t zspec(uint8_t g, const char *k) {
      pcfg_spec_t s;
      TEST_ASSERT_EQUAL_INT_MESSAGE(0, pcfg_spec_for(PCFG_TABLE_ZONE, zrow(g, k), &s), k);
      return s;
  }
  static pcfg_spec_t mspec(uint8_t g, const char *k) {
      pcfg_spec_t s;
      TEST_ASSERT_EQUAL_INT_MESSAGE(0, pcfg_spec_for(PCFG_TABLE_MASTER, mrow(g, k), &s), k);
      return s;
  }
  static const char *fmt(const pcfg_spec_t *s, const char *raw) {
      static char b[48];
      pcfg_format(s, raw, b, sizeof b);
      return b;
  }
  static void assert_ascii(const char *s) {
      TEST_ASSERT_NOT_NULL(s);
      for (const char *p = s; *p; p++)
          TEST_ASSERT_TRUE_MESSAGE((unsigned char)*p >= 0x20 && (unsigned char)*p <= 0x7E, s);
  }

  /* 1 */
  static void test_every_zone_row_generates(void) {
      for (int i = 0; i < HG_FIELD_COUNT; i++) {
          const hg_field_t *f = &HG_FIELDS[i];
          pcfg_spec_t s;
          TEST_ASSERT_EQUAL_INT_MESSAGE(0, pcfg_spec_for(PCFG_TABLE_ZONE, f, &s), f->key);
          TEST_ASSERT_TRUE_MESSAGE(s.edit_kind < PCFG_K_READONLY, f->key);    /* a real editor for every row */
          TEST_ASSERT_TRUE_MESSAGE(s.min <= s.max, f->key);
          TEST_ASSERT_EQUAL_INT_MESSAGE(hg_group_is_hw(f->group), s.readonly, f->key);
          TEST_ASSERT_EQUAL_INT_MESSAGE(s.readonly ? PCFG_K_READONLY : s.edit_kind, s.kind, f->key);
          TEST_ASSERT_NOT_NULL(s.label);
          TEST_ASSERT_NOT_NULL(s.unit);
      }
  }
  /* 2 */
  static void test_every_master_row_text_or_secret(void) {
      for (int i = 0; i < HG_MFIELD_COUNT; i++) {
          pcfg_spec_t s;
          TEST_ASSERT_EQUAL_INT(0, pcfg_spec_for(PCFG_TABLE_MASTER, &HG_MFIELDS[i], &s));
          TEST_ASSERT_EQUAL_INT(0, s.readonly);
          TEST_ASSERT_TRUE_MESSAGE(s.kind == PCFG_K_TEXT || s.kind == PCFG_K_SECRET, HG_MFIELDS[i].key);
          TEST_ASSERT_EQUAL_INT(hg_mcfg_is_secret(&HG_MFIELDS[i]) ? PCFG_K_SECRET : PCFG_K_TEXT, s.kind);
      }
  }
  /* 3: the "field added later" tripwire */
  static int pres_count(pcfg_table_t t, uint8_t g, const char *k) {
      int n = 0;
      for (int i = 0; i < PCFG_PRES_COUNT; i++)
          if (PCFG_PRES[i].table == t && PCFG_PRES[i].group == g && strcmp(PCFG_PRES[i].key, k) == 0) n++;
      return n;
  }
  static void test_presentation_covers_every_row_exactly_once(void) {
      for (int i = 0; i < HG_FIELD_COUNT; i++)
          TEST_ASSERT_EQUAL_INT_MESSAGE(1, pres_count(PCFG_TABLE_ZONE, HG_FIELDS[i].group, HG_FIELDS[i].key), HG_FIELDS[i].key);
      for (int i = 0; i < HG_MFIELD_COUNT; i++)
          TEST_ASSERT_EQUAL_INT_MESSAGE(1, pres_count(PCFG_TABLE_MASTER, HG_MFIELDS[i].group, HG_MFIELDS[i].key), HG_MFIELDS[i].key);
      for (int p = 0; p < PCFG_PRES_COUNT; p++) {          /* no orphan: every entry names a real row */
          const pcfg_pres_t *e = &PCFG_PRES[p];
          const hg_field_t *tab = e->table == PCFG_TABLE_MASTER ? HG_MFIELDS : HG_FIELDS;
          int n = e->table == PCFG_TABLE_MASTER ? HG_MFIELD_COUNT : HG_FIELD_COUNT, found = 0;
          for (int i = 0; i < n; i++) if (tab[i].group == e->group && strcmp(tab[i].key, e->key) == 0) found++;
          TEST_ASSERT_EQUAL_INT_MESSAGE(1, found, e->key);
          TEST_ASSERT_TRUE_MESSAGE(e->label && e->label[0], e->key);
          assert_ascii(e->label);
          assert_ascii(e->unit ? e->unit : "");
      }
      TEST_ASSERT_EQUAL_INT(HG_FIELD_COUNT + HG_MFIELD_COUNT, PCFG_PRES_COUNT);
      TEST_ASSERT_NULL(pcfg_pres_find(PCFG_TABLE_ZONE, HG_G_WATER, "NO_SUCH_KEY"));
  }
  /* 4: pins today's tables -- update deliberately when a row is added */
  static void test_counts(void) {
      int ro = 0, ed = 0, sec = 0;
      for (int i = 0; i < HG_FIELD_COUNT; i++) {
          pcfg_spec_t s; pcfg_spec_for(PCFG_TABLE_ZONE, &HG_FIELDS[i], &s);
          if (s.kind == PCFG_K_READONLY) ro++; else ed++;
      }
      for (int i = 0; i < HG_MFIELD_COUNT; i++) {
          pcfg_spec_t s; pcfg_spec_for(PCFG_TABLE_MASTER, &HG_MFIELDS[i], &s);
          if (s.kind == PCFG_K_SECRET) sec++;
      }
      TEST_ASSERT_EQUAL_INT(23, ro);
      TEST_ASSERT_EQUAL_INT(35, ed);
      TEST_ASSERT_EQUAL_INT(2, sec);
  }
  /* 5 */
  static void test_enabled_is_switch(void) {
      pcfg_spec_t s = zspec(HG_G_SHELF, "ENABLED");
      TEST_ASSERT_EQUAL_INT(PCFG_K_SWITCH, s.kind);
      TEST_ASSERT_EQUAL_INT(PCFG_FMT_BOOL, s.fmt);
      TEST_ASSERT_EQUAL_STRING("on", fmt(&s, "1"));
      TEST_ASSERT_EQUAL_STRING("off", fmt(&s, "0"));
  }
  /* 6 */
  static void test_water_mode_segmented(void) {
      pcfg_spec_t s = zspec(HG_G_WATER, "MODE");
      TEST_ASSERT_EQUAL_INT(PCFG_K_SEGMENTED, s.kind);
      TEST_ASSERT_EQUAL_UINT8(2, s.n_opts);
      TEST_ASSERT_EQUAL_STRING("OFF", s.opts[0]);
      TEST_ASSERT_EQUAL_STRING("AUTO", s.opts[1]);
      TEST_ASSERT_EQUAL_INT32(0, s.min);
      TEST_ASSERT_EQUAL_INT32(1, s.max);
  }
  /* 7 */
  static void test_fan_mode_four_options(void) {
      pcfg_spec_t s = zspec(HG_G_FAN, "MODE");
      TEST_ASSERT_EQUAL_INT(PCFG_K_SEGMENTED, s.kind);
      TEST_ASSERT_EQUAL_UINT8(4, s.n_opts);
      TEST_ASSERT_EQUAL_STRING("CYCLE", s.opts[3]);
  }
  /* 8 */
  static void test_five_option_enum_is_roller(void) {
      static const hg_field_t syn5 = { HG_G_WATER, "SYN5", 0, HG_T_ENUM, 0, 4, "A|B|C|D|E" };
      pcfg_spec_t s;
      TEST_ASSERT_EQUAL_INT(0, pcfg_spec_for(PCFG_TABLE_ZONE, &syn5, &s));
      TEST_ASSERT_EQUAL_INT(PCFG_K_ROLLER, s.kind);
      TEST_ASSERT_EQUAL_INT(PCFG_ROLL_ENUM, s.roller);
      TEST_ASSERT_EQUAL_UINT8(5, s.n_opts);
      TEST_ASSERT_EQUAL_STRING("SYN5", s.label);          /* no presentation entry: the key */
  }
  /* 9 */
  static void test_soil_backend_readonly_segmented(void) {
      pcfg_spec_t s = zspec(HG_G_HW, "SOIL_BACKEND");
      TEST_ASSERT_EQUAL_INT(PCFG_K_READONLY, s.kind);
      TEST_ASSERT_EQUAL_INT(PCFG_K_SEGMENTED, s.edit_kind);
      TEST_ASSERT_EQUAL_INT(PCFG_FMT_ENUM, s.fmt);
      TEST_ASSERT_EQUAL_UINT8(1, s.readonly);
      TEST_ASSERT_EQUAL_STRING("ADS1115", fmt(&s, "ADS1115"));
  }
  /* 10 */
  static void test_hhmm_roller(void) {
      pcfg_spec_t s = zspec(HG_G_LIGHT, "ON");
      TEST_ASSERT_EQUAL_INT(PCFG_K_ROLLER, s.kind);
      TEST_ASSERT_EQUAL_INT(PCFG_ROLL_HHMM, s.roller);
      TEST_ASSERT_EQUAL_INT32(0, s.min);
      TEST_ASSERT_EQUAL_INT32(1439, s.max);
      TEST_ASSERT_EQUAL_INT32(1, s.step);
      TEST_ASSERT_EQUAL_STRING("06:30", fmt(&s, "390"));
      TEST_ASSERT_EQUAL_STRING("06:30", fmt(&s, "06:30"));
  }
  /* 11-14 */
  static void test_stepper_bounds(void) {
      pcfg_spec_t s = zspec(HG_G_WATER, "DOSE_S");
      TEST_ASSERT_EQUAL_INT(PCFG_K_STEPPER, s.kind);
      TEST_ASSERT_EQUAL_INT32(1, s.min);
      TEST_ASSERT_EQUAL_INT32(300, s.max);
      TEST_ASSERT_EQUAL_INT(PCFG_KB_NUMERIC, s.keyboard);
      TEST_ASSERT_EQUAL_STRING("s", s.unit);
      s = zspec(HG_G_WATER, "HYST");
      TEST_ASSERT_EQUAL_INT32(1, s.min);                  /* a non-zero min is kept */
      TEST_ASSERT_EQUAL_INT32(30, s.max);
      TEST_ASSERT_EQUAL_INT32(0, s.big_step);
      s = zspec(HG_G_VIB, "INTENSITY");
      TEST_ASSERT_EQUAL_INT32(20, s.min);
      TEST_ASSERT_EQUAL_INT32(100, s.max);
      TEST_ASSERT_EQUAL_STRING("%", s.unit);
      s = zspec(HG_G_WATER, "INTERVAL_MIN");
      TEST_ASSERT_EQUAL_INT32(10, s.min);
      TEST_ASSERT_EQUAL_INT32(1440, s.max);
      TEST_ASSERT_EQUAL_INT32(10, s.big_step);
      TEST_ASSERT_EQUAL_UINT8(4, s.max_len);              /* keypad: at most "1440" */
  }
  /* 15 */
  static void test_dli_scaled(void) {
      pcfg_spec_t s = zspec(HG_G_LIGHT, "DLI");
      TEST_ASSERT_EQUAL_INT16(10, s.scale_div);
      TEST_ASSERT_EQUAL_INT(PCFG_FMT_SCALED, s.fmt);
      TEST_ASSERT_EQUAL_STRING("12.5", fmt(&s, "125"));
      TEST_ASSERT_EQUAL_STRING("off", fmt(&s, "0"));
      TEST_ASSERT_EQUAL_STRING("mol/m2/d", s.unit);
  }
  /* 16 */
  static void test_step_clamps(void) {
      pcfg_spec_t s = zspec(HG_G_WATER, "DOSE_S");
      TEST_ASSERT_EQUAL_INT32(300, pcfg_step(&s, 300, +1, 0));
      TEST_ASSERT_EQUAL_INT32(1, pcfg_step(&s, 1, -1, 0));
      TEST_ASSERT_EQUAL_INT32(1, pcfg_step(&s, 5, -1, 1));
      TEST_ASSERT_EQUAL_INT32(21, pcfg_step(&s, 20, +1, 0));
      s = zspec(HG_G_WATER, "INTERVAL_MIN");
      TEST_ASSERT_EQUAL_INT32(1440, pcfg_step(&s, 1435, +1, 1));
      TEST_ASSERT_EQUAL_INT32(130, pcfg_step(&s, 120, +1, 1));
      TEST_ASSERT_EQUAL_INT32(120, pcfg_step(&s, 120, 0, 0));
  }
  /* 17 */
  static void test_pin_none(void) {
      pcfg_spec_t s = zspec(HG_G_HWSHELF, "PUMP");
      TEST_ASSERT_EQUAL_INT(PCFG_K_READONLY, s.kind);
      TEST_ASSERT_EQUAL_INT(PCFG_K_ROLLER, s.edit_kind);
      TEST_ASSERT_EQUAL_INT(PCFG_ROLL_PIN, s.roller);
      TEST_ASSERT_EQUAL_INT32(255, s.none_value);
      TEST_ASSERT_EQUAL_STRING("none", fmt(&s, "NONE"));
      TEST_ASSERT_EQUAL_STRING("3", fmt(&s, "3"));
      int32_t v;
      TEST_ASSERT_EQUAL_INT(0, pcfg_parse_raw(&s, "NONE", &v));
      TEST_ASSERT_EQUAL_INT32(255, v);
  }
  /* 18-19 */
  static void test_hex(void) {
      pcfg_spec_t s = zspec(HG_G_HW, "PCA_ADDR");
      TEST_ASSERT_EQUAL_INT(PCFG_K_READONLY, s.kind);
      TEST_ASSERT_EQUAL_INT(PCFG_FMT_HEX, s.fmt);
      TEST_ASSERT_EQUAL_STRING("0x40", fmt(&s, "64"));
      s = zspec(HG_G_HW, "PCF_ACTLOW");
      TEST_ASSERT_EQUAL_INT(PCFG_FMT_HEX, s.fmt);
      TEST_ASSERT_EQUAL_STRING("0xFFFF", fmt(&s, "65535"));
      TEST_ASSERT_EQUAL_STRING("0x0000", fmt(&s, "0"));
  }
  /* 20-25 */
  static void test_text_rows(void) {
      pcfg_spec_t s = zspec(HG_G_ZONECFG, "NAME");
      TEST_ASSERT_EQUAL_INT(PCFG_K_TEXT, s.kind);
      TEST_ASSERT_EQUAL_INT(PCFG_KB_TEXT_NOSPACE, s.keyboard);
      TEST_ASSERT_EQUAL_UINT8(15, s.max_len);
      TEST_ASSERT_EQUAL_UINT8(1, s.min_len);
      s = zspec(HG_G_SHELF, "CROP");
      TEST_ASSERT_EQUAL_INT(PCFG_KB_TEXT_NOSPACE, s.keyboard);
      TEST_ASSERT_EQUAL_UINT8(15, s.max_len);
      TEST_ASSERT_EQUAL_UINT8(0, s.min_len);
      s = mspec(HG_MG_WIFI, "STA_SSID");
      TEST_ASSERT_EQUAL_INT(PCFG_K_TEXT, s.kind);
      TEST_ASSERT_EQUAL_INT(PCFG_KB_TEXT, s.keyboard);
      TEST_ASSERT_EQUAL_UINT8(32, s.max_len);
      TEST_ASSERT_EQUAL_UINT8(0, s.min_len);
      s = mspec(HG_MG_WIFI, "AP_SSID");
      TEST_ASSERT_EQUAL_UINT8(32, s.max_len);
      TEST_ASSERT_EQUAL_UINT8(1, s.min_len);
      s = mspec(HG_MG_WIFI, "STA_PASS");
      TEST_ASSERT_EQUAL_INT(PCFG_K_SECRET, s.kind);
      TEST_ASSERT_EQUAL_UINT8(63, s.max_len);             /* the validator's 63, not the row's 64 (D12) */
      s = mspec(HG_MG_WIFI, "AP_PASS");
      TEST_ASSERT_EQUAL_INT(PCFG_K_SECRET, s.kind);
      TEST_ASSERT_EQUAL_UINT8(63, s.max_len);
      s = mspec(HG_MG_TIME, "TZ");
      TEST_ASSERT_EQUAL_INT(PCFG_K_TEXT, s.kind);
      TEST_ASSERT_EQUAL_UINT8(47, s.max_len);
      TEST_ASSERT_EQUAL_INT(PCFG_KB_TEXT_NOSPACE, s.keyboard);
      s = mspec(HG_MG_TIME, "NTP");
      TEST_ASSERT_EQUAL_INT(PCFG_KB_TEXT_NOSPACE, s.keyboard);
      TEST_ASSERT_EQUAL_UINT8(1, s.min_len);
      s = mspec(HG_MG_SYS, "HOSTNAME");
      TEST_ASSERT_EQUAL_INT(PCFG_KB_HOSTNAME, s.keyboard);
      TEST_ASSERT_EQUAL_UINT8(23, s.max_len);
      TEST_ASSERT_EQUAL_INT32(-1, pcfg_parse_raw(&s, "hillgrow", &(int32_t){0}));   /* text carries no number */
  }
  /* 26 */
  static void test_unknown_ftype_renders(void) {
      static const hg_field_t syn99 = { HG_G_ZONECFG, "SYN99", 0, 99, 0, 1, NULL };
      pcfg_spec_t s;
      TEST_ASSERT_EQUAL_INT(-1, pcfg_spec_for(PCFG_TABLE_ZONE, &syn99, &s));
      TEST_ASSERT_EQUAL_INT(PCFG_K_READONLY, s.kind);
      TEST_ASSERT_EQUAL_INT(PCFG_FMT_TEXT, s.fmt);
      TEST_ASSERT_EQUAL_STRING("SYN99", s.label);
      TEST_ASSERT_EQUAL_INT(-1, pcfg_spec_for(PCFG_TABLE_ZONE, NULL, &s));
      TEST_ASSERT_EQUAL_INT(PCFG_K_READONLY, s.kind);
      TEST_ASSERT_EQUAL_INT(-1, pcfg_spec_for(PCFG_TABLE_ZONE, &syn99, NULL));
  }
  /* 27: the same uint8_t group means different things in the two tables */
  static void test_group_id_resolved_per_table(void) {
      pcfg_spec_t m, z;
      TEST_ASSERT_NOT_NULL(pcfg_pres_find(PCFG_TABLE_MASTER, HG_MG_WIFI, "STA_SSID"));
      TEST_ASSERT_NULL(pcfg_pres_find(PCFG_TABLE_ZONE, HG_G_ZONECFG, "STA_SSID"));
      const hg_field_t *host = mrow(HG_MG_SYS, "HOSTNAME");     /* group 3: SYS here, WATER in the zone table */
      TEST_ASSERT_EQUAL_INT(0, pcfg_spec_for(PCFG_TABLE_MASTER, host, &m));
      TEST_ASSERT_EQUAL_INT(0, pcfg_spec_for(PCFG_TABLE_ZONE, host, &z));
      TEST_ASSERT_EQUAL_INT(PCFG_KB_HOSTNAME, m.keyboard);
      TEST_ASSERT_EQUAL_INT(PCFG_KB_TEXT, z.keyboard);          /* zone table: no entry, generator defaults */
      TEST_ASSERT_EQUAL_STRING("HOSTNAME", z.label);
      const hg_field_t *shelves = zrow(HG_G_HW, "SHELVES");     /* group 7: HW here, no such master group */
      TEST_ASSERT_EQUAL_INT(0, pcfg_spec_for(PCFG_TABLE_ZONE, shelves, &z));
      TEST_ASSERT_EQUAL_INT(PCFG_K_READONLY, z.kind);
      TEST_ASSERT_EQUAL_INT(-1, pcfg_spec_for(PCFG_TABLE_MASTER, shelves, &m));
      for (int i = 0; i < HG_MFIELD_COUNT; i++) {
          TEST_ASSERT_EQUAL_INT(0, pcfg_spec_for(PCFG_TABLE_MASTER, &HG_MFIELDS[i], &m));
          TEST_ASSERT_EQUAL_UINT8(0, m.readonly);
      }
  }
  /* 28 */
  static void test_tighten_dose(void) {
      const hg_field_t *dose = zrow(HG_G_WATER, "DOSE_S");
      pcfg_spec_t s = zspec(HG_G_WATER, "DOSE_S");
      hw.shelf[1].pump_max_run_s = 60;
      pcfg_tighten(&s, dose, 1, &hw);
      TEST_ASSERT_EQUAL_INT32(60, s.max);
      s = zspec(HG_G_WATER, "DOSE_S");
      pcfg_tighten(&s, dose, 1, NULL);
      TEST_ASSERT_EQUAL_INT32(300, s.max);
      hw.shelf[1].pump_max_run_s = 500;
      pcfg_tighten(&s, dose, 1, &hw);
      TEST_ASSERT_EQUAL_INT32(300, s.max);                /* never widens */
      pcfg_spec_t t = zspec(HG_G_WATER, "TARGET");
      hw.shelf[1].pump_max_run_s = 60;
      pcfg_tighten(&t, zrow(HG_G_WATER, "TARGET"), 1, &hw);
      TEST_ASSERT_EQUAL_INT32(100, t.max);                /* other rows untouched */
  }
  /* 29-33 plus the other path shapes the panel receives */
  static void locate_ok(pcfg_table_t t, const char *path, uint8_t g, int ix, const hg_field_t *row) {
      uint8_t og = 0xAA; int oi = 99; const hg_field_t *orow = NULL;
      TEST_ASSERT_EQUAL_INT_MESSAGE(0, pcfg_locate(t, path, &og, &oi, &orow), path);
      TEST_ASSERT_EQUAL_UINT8_MESSAGE(g, og, path);
      TEST_ASSERT_EQUAL_INT_MESSAGE(ix, oi, path);
      TEST_ASSERT_EQUAL_PTR_MESSAGE(row, orow, path);
  }
  static void locate_bad(pcfg_table_t t, const char *path) {
      uint8_t og; int oi; const hg_field_t *orow;
      TEST_ASSERT_EQUAL_INT_MESSAGE(-1, pcfg_locate(t, path, &og, &oi, &orow), path);
  }
  static void test_locate_shapes(void) {
      locate_ok(PCFG_TABLE_ZONE, "cfg.shelf[1].WATER.TARGET", HG_G_WATER, 1, zrow(HG_G_WATER, "TARGET"));
      locate_ok(PCFG_TABLE_ZONE, "shelf[2].light.off", HG_G_LIGHT, 2, zrow(HG_G_LIGHT, "OFF"));
      locate_ok(PCFG_TABLE_ZONE, "zonecfg.name", HG_G_ZONECFG, -1, zrow(HG_G_ZONECFG, "NAME"));
      locate_ok(PCFG_TABLE_ZONE, "cfg.ZONECFG.NAME", HG_G_ZONECFG, -1, zrow(HG_G_ZONECFG, "NAME"));
      locate_ok(PCFG_TABLE_ZONE, "shelf[0].enabled", HG_G_SHELF, 0, zrow(HG_G_SHELF, "ENABLED"));
      locate_ok(PCFG_TABLE_ZONE, "cfg.aux[0].AUX.MODE", HG_G_AUX, 0, zrow(HG_G_AUX, "MODE"));
      locate_ok(PCFG_TABLE_ZONE, "aux.pulse_s", HG_G_AUX, -1, zrow(HG_G_AUX, "PULSE_S"));
      locate_ok(PCFG_TABLE_ZONE, "shelf[0].water.dose_s", HG_G_WATER, 0, zrow(HG_G_WATER, "DOSE_S"));
      locate_ok(PCFG_TABLE_MASTER, "WIFI.AP_PASS", HG_MG_WIFI, -1, mrow(HG_MG_WIFI, "AP_PASS"));
      locate_ok(PCFG_TABLE_MASTER, "SYS.HOSTNAME", HG_MG_SYS, -1, mrow(HG_MG_SYS, "HOSTNAME"));
      locate_bad(PCFG_TABLE_ZONE, "hw.aux_pin");                  /* no row carries it: caller shows a banner */
      locate_bad(PCFG_TABLE_ZONE, "");
      locate_bad(PCFG_TABLE_ZONE, "cfg.shelf[4].WATER.TARGET");
      locate_bad(PCFG_TABLE_ZONE, "cfg.aux[2].AUX.MODE");
      locate_bad(PCFG_TABLE_ZONE, "shelf[1].AUX.MODE");
      locate_bad(PCFG_TABLE_ZONE, "cfg.WATER.BOGUS");
      locate_bad(PCFG_TABLE_ZONE, "WIFI.AP_PASS");
      locate_bad(PCFG_TABLE_MASTER, "BOGUS.KEY");
      locate_bad(PCFG_TABLE_MASTER, "WIFI");
      TEST_ASSERT_EQUAL_INT(-1, pcfg_locate(PCFG_TABLE_ZONE, NULL, &(uint8_t){0}, &(int){0}, &(const hg_field_t *){NULL}));
  }
  /* 34: the generator never produces a value the writer refuses */
  static void test_round_trip_every_editable_row(void) {
      int checked = 0;
      for (int i = 0; i < HG_FIELD_COUNT; i++) {
          const hg_field_t *f = &HG_FIELDS[i];
          pcfg_spec_t s; char raw[32], t[32]; int32_t v;
          TEST_ASSERT_EQUAL_INT(0, pcfg_spec_for(PCFG_TABLE_ZONE, f, &s));
          if (s.readonly) continue;
          void *base = hg_field_base(f->group, 0, &hw, &cfg);
          TEST_ASSERT_NOT_NULL(base);
          TEST_ASSERT_EQUAL_INT(0, hg_field_read(f, base, raw, sizeof raw));
          if (s.kind == PCFG_K_TEXT) {
              TEST_ASSERT_EQUAL_INT_MESSAGE(0, hg_field_write(f, base, raw), f->key);
              checked++;
              continue;
          }
          TEST_ASSERT_EQUAL_INT_MESSAGE(0, pcfg_parse_raw(&s, raw, &v), f->key);
          TEST_ASSERT_EQUAL_INT_MESSAGE(0, pcfg_raw_text(&s, v, t, sizeof t), f->key);
          TEST_ASSERT_EQUAL_STRING_MESSAGE(raw, t, f->key);
          TEST_ASSERT_EQUAL_INT_MESSAGE(0, hg_field_write(f, base, t), f->key);
          checked++;
      }
      TEST_ASSERT_EQUAL_INT(35, checked);
  }
  /* 35: bounds agree with the writer -- every reachable value accepted, min-1 / max+1 refused */
  static void test_numeric_bounds_agree_with_writer(void) {
      for (int i = 0; i < HG_FIELD_COUNT; i++) {
          const hg_field_t *f = &HG_FIELDS[i];
          if (f->type != HG_T_U8 && f->type != HG_T_U16) continue;
          pcfg_spec_t s; char t[16];
          TEST_ASSERT_EQUAL_INT(0, pcfg_spec_for(PCFG_TABLE_ZONE, f, &s));
          void *base = hg_field_base(f->group, 0, &hw, &cfg);
          TEST_ASSERT_NOT_NULL(base);
          TEST_ASSERT_EQUAL_INT(0, pcfg_raw_text(&s, s.min - 1, t, sizeof t));
          TEST_ASSERT_EQUAL_INT_MESSAGE(-2, hg_field_write(f, base, t), f->key);
          TEST_ASSERT_EQUAL_INT(0, pcfg_raw_text(&s, s.max + 1, t, sizeof t));
          TEST_ASSERT_EQUAL_INT_MESSAGE(-2, hg_field_write(f, base, t), f->key);
          int32_t v = s.min;
          for (int guard = 0; ; guard++) {
              TEST_ASSERT_TRUE(guard < 70000);
              TEST_ASSERT_EQUAL_INT(0, pcfg_raw_text(&s, v, t, sizeof t));
              TEST_ASSERT_EQUAL_INT_MESSAGE(0, hg_field_write(f, base, t), f->key);
              if (v == s.max) break;
              int32_t nv = pcfg_step(&s, v, +1, 1);
              TEST_ASSERT_TRUE_MESSAGE(nv > v, f->key);
              v = nv;
          }
      }
  }

  int main(void) {
      UNITY_BEGIN();
      RUN_TEST(test_every_zone_row_generates);
      RUN_TEST(test_every_master_row_text_or_secret);
      RUN_TEST(test_presentation_covers_every_row_exactly_once);
      RUN_TEST(test_counts);
      RUN_TEST(test_enabled_is_switch);
      RUN_TEST(test_water_mode_segmented);
      RUN_TEST(test_fan_mode_four_options);
      RUN_TEST(test_five_option_enum_is_roller);
      RUN_TEST(test_soil_backend_readonly_segmented);
      RUN_TEST(test_hhmm_roller);
      RUN_TEST(test_stepper_bounds);
      RUN_TEST(test_dli_scaled);
      RUN_TEST(test_step_clamps);
      RUN_TEST(test_pin_none);
      RUN_TEST(test_hex);
      RUN_TEST(test_text_rows);
      RUN_TEST(test_unknown_ftype_renders);
      RUN_TEST(test_group_id_resolved_per_table);
      RUN_TEST(test_tighten_dose);
      RUN_TEST(test_locate_shapes);
      RUN_TEST(test_round_trip_every_editable_row);
      RUN_TEST(test_numeric_bounds_agree_with_writer);
      return UNITY_END();
  }
  ```

- [ ] **Step 6: Run it and watch it fail**

  Run the single host test with `<name>` = `test_pcfg_gen`.
  Expected: configure fails with `CMake Error at CMakeLists.txt:<n> (add_executable): Cannot find source file:`
  naming `.../components/panel_ui/pcfg_gen.c`.

- [ ] **Step 7: Create `components/panel_ui/pcfg_gen.h`**

  ```c
  #pragma once
  /* pcfg_gen.h -- the type-driven config-widget generator (pure: no LVGL, no IDF headers).
   * Every row of HG_FIELDS (zone) and HG_MFIELDS (master) becomes a pcfg_spec_t; wdg_field.c builds the
   * LVGL row from the spec, never from the table directly. Values stay in hg_field_read/hg_field_write
   * text form ("raw") everywhere -- the spec only says how to show, bound and step them. */
  #include <stddef.h>
  #include <stdint.h>
  #include "hg_cfg.h"
  #ifdef __cplusplus
  extern "C" {
  #endif

  typedef enum { PCFG_TABLE_ZONE = 0, PCFG_TABLE_MASTER } pcfg_table_t;
  typedef enum { PCFG_K_STEPPER = 0, PCFG_K_SWITCH, PCFG_K_SEGMENTED, PCFG_K_ROLLER, PCFG_K_TEXT, PCFG_K_SECRET, PCFG_K_READONLY } pcfg_kind_t;
  typedef enum { PCFG_KB_NONE = 0, PCFG_KB_NUMERIC, PCFG_KB_TEXT, PCFG_KB_TEXT_NOSPACE, PCFG_KB_HOSTNAME, PCFG_KB_HEX } pcfg_kb_t;
  typedef enum { PCFG_ROLL_NONE = 0, PCFG_ROLL_HHMM, PCFG_ROLL_ENUM, PCFG_ROLL_PIN } pcfg_roll_t;
  typedef enum { PCFG_FMT_DEC = 0, PCFG_FMT_HEX, PCFG_FMT_HHMM, PCFG_FMT_ENUM, PCFG_FMT_PIN, PCFG_FMT_TEXT, PCFG_FMT_BOOL, PCFG_FMT_SCALED } pcfg_fmt_t;
  #define PCFG_MAX_OPTS 8
  #define PCFG_OPT_LEN  16
  typedef struct {
      pcfg_kind_t kind, edit_kind;   /* READONLY rows keep edit_kind for formatting */
      uint8_t     ftype, readonly;
      int32_t     min, max, step, big_step;
      int16_t     scale_div;         /* 1; 10 for LIGHT.DLI */
      int32_t     none_value;        /* 255 for PIN; -1 none */
      pcfg_roll_t roller;
      uint8_t     n_opts; char opts[PCFG_MAX_OPTS][PCFG_OPT_LEN];
      pcfg_kb_t   keyboard;
      uint8_t     min_len, max_len;
      pcfg_fmt_t  fmt;
      const char *label, *unit;      /* presentation, else key / "" */
      const char *zero_text;         /* "off" for LIGHT.DLI, NULL otherwise */
  } pcfg_spec_t;

  /* 0 / -1 (NULL row, unknown ftype, bad enum list or a group outside the table -> READONLY + TEXT: renders, never crashes) */
  int     pcfg_spec_for(pcfg_table_t t, const hg_field_t *f, pcfg_spec_t *out);
  /* Zone table only. WATER.DOSE_S: max = min(max, hw->shelf[shelf].pump_max_run_s); never widens; hw NULL -> unchanged */
  void    pcfg_tighten(pcfg_spec_t *s, const hg_field_t *f, int shelf, const hg_zone_hw_t *hw_or_null);
  /* hg_field_read text -> display ("06:30", "none", "12.5", "off", "0x40", "on"); 0 / -1 (unparseable: out = raw) */
  int     pcfg_format(const pcfg_spec_t *s, const char *raw, char *out, size_t cap);
  /* raw -> number (HHMM minutes, ENUM index, PIN 255 for NONE, BOOL 0/1); 0 / -1 (text kinds: always -1) */
  int     pcfg_parse_raw(const pcfg_spec_t *s, const char *raw, int32_t *v);
  /* number -> hg_field_write text; 0 / -1 (HHMM outside 0..1439, ENUM index out of range, text kinds) */
  int     pcfg_raw_text(const pcfg_spec_t *s, int32_t v, char *out, size_t cap);
  /* cur + dir * (big && big_step ? big_step : step), clamped to [min, max]; never wraps */
  int32_t pcfg_step(const pcfg_spec_t *s, int32_t cur, int dir, int big);
  /* An error path -> the row it names. 0 / -1. Shapes:
   *   "cfg.ZONECFG.NAME", "cfg.shelf[1].WATER.TARGET", "cfg.aux[0].AUX.MODE"   (merge / psvc_zone_fields_fn)
   *   "shelf[2].light.off", "shelf[0].enabled", "zonecfg.name", "aux.pulse_s"   (hg_cfg_validate; aux: idx -1)
   *   "WIFI.AP_PASS", "SYS.HOSTNAME"                                            (master)
   *   "hw.aux_pin" and anything unknown -> -1 (the caller shows a banner) */
  int     pcfg_locate(pcfg_table_t t, const char *path, uint8_t *group, int *idx, const hg_field_t **row);

  #ifdef __cplusplus
  }
  #endif
  ```

- [ ] **Step 8: Create `components/panel_ui/pcfg_pres.h` and `components/panel_ui/pcfg_pres.c`**

  `pcfg_pres.h`:
  ```c
  #pragma once
  /* pcfg_pres.h -- the panel's presentation table (pure): label, unit, coarse step, scale and text
   * bounds per (table, group, key). A row missing here still renders with generator defaults; the
   * test_pcfg_gen coverage case fails until it gets its entry. All text is printable ASCII. */
  #include <stdint.h>
  #include "pcfg_gen.h"
  #ifdef __cplusplus
  extern "C" {
  #endif

  typedef struct {
      uint8_t table, group; const char *key;
      const char *label, *unit;   /* ASCII */
      int32_t big_step;           /* 0 = default rule (10 when max-min > 100, else 0) */
      int16_t scale_div;          /* 0 = 1 */
      uint8_t min_len, max_len;   /* text; max_len 0 = row max */
      uint8_t keyboard;           /* pcfg_kb_t for text rows */
      uint8_t fmt_hex;            /* PCA_ADDR, PCF_ADDR, PCF_ACTLOW */
      uint8_t readonly;           /* extra override (none by default -- D11) */
      const char *zero_text;      /* display text for raw 0 (LIGHT.DLI "off"); NULL = none */
  } pcfg_pres_t;
  extern const pcfg_pres_t PCFG_PRES[];
  extern const int         PCFG_PRES_COUNT;
  const pcfg_pres_t *pcfg_pres_find(pcfg_table_t t, uint8_t group, const char *key);   /* NULL -> generator defaults */

  #ifdef __cplusplus
  }
  #endif
  ```
  `pcfg_pres.c` (58 zone rows + 7 master rows, in table order):
  ```c
  /* pcfg_pres.c -- one entry per field row of HG_FIELDS and HG_MFIELDS (test_pcfg_gen pins the coverage). */
  #include <string.h>
  #include "pcfg_pres.h"
  #include "hg_mcfg.h"

  #define ZP(g, k, lbl, u) .table = PCFG_TABLE_ZONE,   .group = HG_G_##g,  .key = k, .label = lbl, .unit = u
  #define MP(g, k, lbl, u) .table = PCFG_TABLE_MASTER, .group = HG_MG_##g, .key = k, .label = lbl, .unit = u

  const pcfg_pres_t PCFG_PRES[] = {
      /* ZONECFG */
      { ZP(ZONECFG, "NAME",             "Zone name", ""), .min_len = 1 },          /* validator: non-empty */
      { ZP(ZONECFG, "LINKLOSS_S",       "Link-loss timeout", "s"), .big_step = 10 },
      /* SHELF */
      { ZP(SHELF,   "CROP",             "Crop", "") },
      { ZP(SHELF,   "ENABLED",          "Shelf enabled", "") },
      { ZP(SHELF,   "PROFILE",          "Profile id (profiles not implemented yet)", "") },   /* D11 */
      /* LIGHT */
      { ZP(LIGHT,   "ON",               "Lights on", "") },
      { ZP(LIGHT,   "OFF",              "Lights off", "") },
      { ZP(LIGHT,   "WHITE",            "White", "%") },
      { ZP(LIGHT,   "RED",              "Red", "%") },
      { ZP(LIGHT,   "RAMP_MIN",         "Ramp", "min") },
      { ZP(LIGHT,   "DLI",              "Daily light target", "mol/m2/d"), .scale_div = 10, .zero_text = "off" },
      /* WATER */
      { ZP(WATER,   "MODE",             "Watering", "") },
      { ZP(WATER,   "TARGET",           "Target moisture", "%") },
      { ZP(WATER,   "HYST",             "Hysteresis", "%") },
      { ZP(WATER,   "SETTLE_MIN",       "Settle time", "min") },
      { ZP(WATER,   "DOSE_S",           "Dose", "s"), .big_step = 10 },
      { ZP(WATER,   "INTERVAL_MIN",     "Minimum interval", "min"), .big_step = 10 },
      { ZP(WATER,   "MAX_DOSES",        "Max doses", "/day") },
      { ZP(WATER,   "DIFF_MAX",         "Max sensor difference", "%") },
      { ZP(WATER,   "WIN_START",        "Window start", "") },
      { ZP(WATER,   "WIN_END",          "Window end", "") },
      /* FAN */
      { ZP(FAN,     "MODE",             "Fan", "") },
      { ZP(FAN,     "ON_MIN",           "On time", "min") },
      { ZP(FAN,     "PERIOD_MIN",       "Period", "min"), .big_step = 10 },
      /* VIB */
      { ZP(VIB,     "MODE",             "Pollination", "") },
      { ZP(VIB,     "INTENSITY",        "Intensity", "%") },
      { ZP(VIB,     "PULSE_S",          "Pulse", "s") },
      { ZP(VIB,     "INTERVAL_MIN",     "Interval", "min"), .big_step = 10 },
      { ZP(VIB,     "START",            "Start", "") },
      { ZP(VIB,     "END",              "End", "") },
      /* AUX */
      { ZP(AUX,     "MODE",             "Aux output", "") },
      { ZP(AUX,     "PULSE_S",          "Pulse", "s") },
      { ZP(AUX,     "INTERVAL_MIN",     "Interval", "min"), .big_step = 10 },
      { ZP(AUX,     "START",            "Start", "") },
      { ZP(AUX,     "END",              "End", "") },
      /* HW -- read-only hardware plane */
      { ZP(HW,      "SHELVES",          "Shelves", "") },
      { ZP(HW,      "PCA_ADDR",         "PCA9685 I2C address", ""), .fmt_hex = 1 },
      { ZP(HW,      "PCF_ADDR",         "PCF8575 I2C address", ""), .fmt_hex = 1 },
      { ZP(HW,      "SOIL_BACKEND",     "Soil sensor backend", "") },
      { ZP(HW,      "PCF_ACTLOW",       "PCF8575 active-low mask", ""), .fmt_hex = 1 },
      { ZP(HW,      "PCA_HZ",           "PWM frequency", "Hz") },
      /* HWSHELF -- read-only */
      { ZP(HWSHELF, "LED_W",            "White LED channel", "") },
      { ZP(HWSHELF, "LED_R",            "Red LED channel", "") },
      { ZP(HWSHELF, "PUMP",             "Pump pin", "") },
      { ZP(HWSHELF, "FAN",              "Fan pin", "") },
      { ZP(HWSHELF, "SOIL_A",           "Soil sensor A channel", "") },
      { ZP(HWSHELF, "SOIL_B",           "Soil sensor B channel", "") },
      { ZP(HWSHELF, "VIB",              "Vibrator channel", "") },
      { ZP(HWSHELF, "LED_MAX_W",        "White LED cap", "%") },
      { ZP(HWSHELF, "LED_MAX_R",        "Red LED cap", "%") },
      { ZP(HWSHELF, "PUMP_MAX_RUN_S",   "Pump max run", "s") },
      { ZP(HWSHELF, "PUMP_MAX_DAILY_S", "Pump max per day", "s") },
      /* CAL -- read-only */
      { ZP(CAL,     "DRY_A",            "Soil A dry", "mV") },
      { ZP(CAL,     "DRY_B",            "Soil B dry", "mV") },
      { ZP(CAL,     "WET_A",            "Soil A wet", "mV") },
      { ZP(CAL,     "WET_B",            "Soil B wet", "mV") },
      { ZP(CAL,     "MIN_OK",           "Plausible minimum", "mV") },
      { ZP(CAL,     "MAX_OK",           "Plausible maximum", "mV") },
      /* master (zone 0) -- WEB has no rows */
      { MP(WIFI,    "STA_SSID",         "Wi-Fi network (SSID)", "") },
      { MP(WIFI,    "STA_PASS",         "Wi-Fi password", ""), .max_len = 63 },           /* D12: validator max */
      { MP(WIFI,    "AP_SSID",          "Access point name", ""), .min_len = 1 },
      { MP(WIFI,    "AP_PASS",          "Access point password", ""), .max_len = 63 },    /* D12 */
      { MP(TIME,    "TZ",               "Time zone (POSIX TZ)", ""), .min_len = 1, .keyboard = PCFG_KB_TEXT_NOSPACE },
      { MP(TIME,    "NTP",              "NTP server", ""), .min_len = 1, .keyboard = PCFG_KB_TEXT_NOSPACE },
      { MP(SYS,     "HOSTNAME",         "Hostname", ""), .min_len = 1, .keyboard = PCFG_KB_HOSTNAME },
  };
  #undef ZP
  #undef MP
  const int PCFG_PRES_COUNT = (int)(sizeof PCFG_PRES / sizeof PCFG_PRES[0]);

  const pcfg_pres_t *pcfg_pres_find(pcfg_table_t t, uint8_t group, const char *key) {
      if (!key) return NULL;
      for (int i = 0; i < PCFG_PRES_COUNT; i++)
          if (PCFG_PRES[i].table == (uint8_t)t && PCFG_PRES[i].group == group && strcmp(PCFG_PRES[i].key, key) == 0)
              return &PCFG_PRES[i];
      return NULL;
  }
  ```

- [ ] **Step 9: Create `components/panel_ui/pcfg_gen.c`**

  ```c
  /* pcfg_gen.c -- the generator's rules, in order (plan Task 17):
   *  1 zone row in the hw plane -> READONLY (edit_kind still computed)   2 BOOL -> SWITCH
   *  3 ENUM -> SEGMENTED (<= 4 options) else ROLLER(ENUM)                 4 HHMM -> ROLLER(HHMM) 0..1439 step 1
   *  5 U8/U16 -> STEPPER, row bounds, step 1, big_step, NUMERIC keypad    6 PIN -> ROLLER(PIN), none 255
   *  7 STR16 -> TEXT, TEXT_NOSPACE, max 15 (its writer refuses spaces)    8 master secret STR -> SECRET, max 63
   *  9 other STR -> TEXT, keyboard from the presentation table. */
  #include <stdio.h>
  #include <stdlib.h>
  #include <string.h>
  #include "pcfg_gen.h"
  #include "pcfg_pres.h"
  #include "hg_mcfg.h"

  static int ci_eq(const char *a, const char *b) {
      for (; *a && *b; a++, b++) {
          char x = *a, y = *b;
          if (x >= 'a' && x <= 'z') x = (char)(x - 32);
          if (y >= 'a' && y <= 'z') y = (char)(y - 32);
          if (x != y) return 0;
      }
      return *a == *b;
  }
  static int prefix_ci(const char *s, const char *p) {
      for (; *p; s++, p++) {
          char x = *s, y = *p;
          if (x >= 'a' && x <= 'z') x = (char)(x - 32);
          if (y >= 'a' && y <= 'z') y = (char)(y - 32);
          if (x != y) return 0;
      }
      return 1;
  }
  static int parse_long(const char *s, long *out) {
      char *end;
      if (!s || !*s) return -1;
      long v = strtol(s, &end, 10);
      if (*end != '\0') return -1;
      *out = v;
      return 0;
  }
  static uint8_t digits(int32_t v) {
      uint8_t d = 1;
      if (v < 0) v = -v;
      while (v >= 10) { v /= 10; d++; }
      return d;
  }
  static void set_unknown(pcfg_spec_t *o) {
      o->kind = o->edit_kind = PCFG_K_READONLY;
      o->readonly = 1;
      o->fmt = PCFG_FMT_TEXT;
      o->roller = PCFG_ROLL_NONE;
      o->keyboard = PCFG_KB_NONE;
      o->n_opts = 0;
  }
  static int split_enums(const char *enums, pcfg_spec_t *o) {
      const char *p = enums;
      int n = 0;
      if (!p || !*p) return -1;
      for (;;) {
          const char *bar = strchr(p, '|');
          size_t len = bar ? (size_t)(bar - p) : strlen(p);
          if (n >= PCFG_MAX_OPTS || len == 0 || len >= PCFG_OPT_LEN) return -1;
          memcpy(o->opts[n], p, len);
          o->opts[n][len] = '\0';
          n++;
          if (!bar) break;
          p = bar + 1;
      }
      o->n_opts = (uint8_t)n;
      return 0;
  }

  int pcfg_spec_for(pcfg_table_t t, const hg_field_t *f, pcfg_spec_t *out) {
      if (!out) return -1;
      memset(out, 0, sizeof *out);
      out->step = 1;
      out->scale_div = 1;
      out->none_value = -1;
      out->unit = "";
      out->label = (f && f->key) ? f->key : "?";
      if (!f || !f->key) { set_unknown(out); return -1; }
      int ngroups = (t == PCFG_TABLE_MASTER) ? HG_MG_COUNT : HG_G_COUNT;
      if ((t != PCFG_TABLE_ZONE && t != PCFG_TABLE_MASTER) || f->group >= ngroups) { set_unknown(out); return -1; }
      out->ftype = f->type;
      out->min = f->min;
      out->max = f->max;
      switch (f->type) {
      case HG_T_BOOL:
          out->edit_kind = PCFG_K_SWITCH; out->min = 0; out->max = 1; out->fmt = PCFG_FMT_BOOL;
          break;
      case HG_T_ENUM:
          if (split_enums(f->enums, out) != 0) { set_unknown(out); return -1; }
          out->min = 0; out->max = out->n_opts - 1; out->fmt = PCFG_FMT_ENUM;
          if (out->n_opts <= 4) out->edit_kind = PCFG_K_SEGMENTED;
          else { out->edit_kind = PCFG_K_ROLLER; out->roller = PCFG_ROLL_ENUM; }
          break;
      case HG_T_HHMM:
          out->edit_kind = PCFG_K_ROLLER; out->roller = PCFG_ROLL_HHMM;
          out->min = 0; out->max = 1439; out->fmt = PCFG_FMT_HHMM;
          break;
      case HG_T_U8: case HG_T_U16:
          out->edit_kind = PCFG_K_STEPPER; out->keyboard = PCFG_KB_NUMERIC;
          out->min_len = 1; out->max_len = digits(f->max);
          out->big_step = (f->max - f->min > 100) ? 10 : 0;
          out->fmt = PCFG_FMT_DEC;
          break;
      case HG_T_PIN:
          out->edit_kind = PCFG_K_ROLLER; out->roller = PCFG_ROLL_PIN;
          out->none_value = HG_NONE; out->fmt = PCFG_FMT_PIN;
          break;
      case HG_T_STR16:
          out->edit_kind = PCFG_K_TEXT; out->keyboard = PCFG_KB_TEXT_NOSPACE;
          out->min = 0; out->max = 15; out->max_len = 15; out->fmt = PCFG_FMT_TEXT;
          break;
      case HG_T_STR:
          out->edit_kind = (t == PCFG_TABLE_MASTER && hg_mcfg_is_secret(f)) ? PCFG_K_SECRET : PCFG_K_TEXT;
          out->keyboard = PCFG_KB_TEXT;
          out->max_len = (uint8_t)(f->max > 255 ? 255 : f->max);
          out->fmt = PCFG_FMT_TEXT;
          break;
      default:
          set_unknown(out);
          return -1;
      }
      const pcfg_pres_t *p = pcfg_pres_find(t, f->group, f->key);
      if (p) {
          if (p->label) out->label = p->label;
          if (p->unit) out->unit = p->unit;
          if (p->big_step) out->big_step = p->big_step;
          if (p->scale_div > 1) {
              out->scale_div = p->scale_div;
              if (out->fmt == PCFG_FMT_DEC) out->fmt = PCFG_FMT_SCALED;
          }
          if (p->fmt_hex && out->fmt == PCFG_FMT_DEC) out->fmt = PCFG_FMT_HEX;
          if (p->zero_text) out->zero_text = p->zero_text;
          if (out->edit_kind == PCFG_K_TEXT || out->edit_kind == PCFG_K_SECRET) {
              out->min_len = p->min_len;
              if (p->max_len && p->max_len < out->max_len) out->max_len = p->max_len;
              if (p->keyboard && f->type == HG_T_STR) out->keyboard = (pcfg_kb_t)p->keyboard;   /* STR16 stays NOSPACE */
          }
          if (p->readonly) out->readonly = 1;
      }
      if (t == PCFG_TABLE_ZONE && hg_group_is_hw(f->group)) out->readonly = 1;   /* rule 1 */
      out->kind = out->readonly ? PCFG_K_READONLY : out->edit_kind;
      return 0;
  }

  void pcfg_tighten(pcfg_spec_t *s, const hg_field_t *f, int shelf, const hg_zone_hw_t *hw_or_null) {
      if (!s || !f || !hw_or_null || shelf < 0 || shelf >= HG_MAX_SHELVES) return;
      if (f->group != HG_G_WATER || strcmp(f->key, "DOSE_S") != 0) return;
      int32_t m = hw_or_null->shelf[shelf].pump_max_run_s;
      if (m < s->min) m = s->min;
      if (m < s->max) s->max = m;
  }

  int pcfg_format(const pcfg_spec_t *s, const char *raw, char *out, size_t cap) {
      long v;
      if (!out || cap == 0) return -1;
      out[0] = '\0';
      if (!s || !raw) return -1;
      switch (s->fmt) {
      case PCFG_FMT_HHMM: {
          int m = hg_hhmm_parse(raw);
          if (m < 0) break;
          char t[6];
          hg_hhmm_format(m, t);
          snprintf(out, cap, "%s", t);
          return 0;
      }
      case PCFG_FMT_PIN:
          if (ci_eq(raw, "NONE")) { snprintf(out, cap, "none"); return 0; }
          if (parse_long(raw, &v) != 0) break;
          if (v == s->none_value) { snprintf(out, cap, "none"); return 0; }
          snprintf(out, cap, "%ld", v);
          return 0;
      case PCFG_FMT_HEX:
          if (parse_long(raw, &v) != 0 || v < 0) break;
          if (s->ftype == HG_T_U16) snprintf(out, cap, "0x%04lX", (unsigned long)v);
          else                      snprintf(out, cap, "0x%02lX", (unsigned long)v);
          return 0;
      case PCFG_FMT_BOOL:
          if (strcmp(raw, "1") == 0 || ci_eq(raw, "ON"))  { snprintf(out, cap, "on");  return 0; }
          if (strcmp(raw, "0") == 0 || ci_eq(raw, "OFF")) { snprintf(out, cap, "off"); return 0; }
          break;
      case PCFG_FMT_SCALED:
          if (parse_long(raw, &v) != 0) break;
          if (v == 0 && s->zero_text) { snprintf(out, cap, "%s", s->zero_text); return 0; }
          if (s->scale_div == 10) { snprintf(out, cap, "%ld.%ld", v / 10, labs(v % 10)); return 0; }
          snprintf(out, cap, "%ld", v);
          return 0;
      case PCFG_FMT_ENUM:
          if (parse_long(raw, &v) == 0 && v >= 0 && v < s->n_opts) { snprintf(out, cap, "%s", s->opts[v]); return 0; }
          snprintf(out, cap, "%s", raw);
          return 0;
      case PCFG_FMT_DEC:
      case PCFG_FMT_TEXT:
      default:
          snprintf(out, cap, "%s", raw);
          return 0;
      }
      snprintf(out, cap, "%s", raw);   /* unparseable: show it as it is */
      return -1;
  }

  int pcfg_parse_raw(const pcfg_spec_t *s, const char *raw, int32_t *v) {
      long x;
      if (!s || !raw || !v) return -1;
      switch (s->ftype) {
      case HG_T_HHMM: {
          int m = hg_hhmm_parse(raw);
          if (m < 0) return -1;
          *v = m;
          return 0;
      }
      case HG_T_ENUM:
          for (int i = 0; i < s->n_opts; i++) if (ci_eq(raw, s->opts[i])) { *v = i; return 0; }
          if (parse_long(raw, &x) == 0 && x >= 0 && x < s->n_opts) { *v = (int32_t)x; return 0; }
          return -1;
      case HG_T_PIN:
          if (ci_eq(raw, "NONE")) { *v = s->none_value; return 0; }
          if (parse_long(raw, &x) != 0) return -1;
          *v = (int32_t)x;
          return 0;
      case HG_T_BOOL:
          if (strcmp(raw, "1") == 0 || ci_eq(raw, "ON"))  { *v = 1; return 0; }
          if (strcmp(raw, "0") == 0 || ci_eq(raw, "OFF")) { *v = 0; return 0; }
          return -1;
      case HG_T_U8: case HG_T_U16:
          if (parse_long(raw, &x) != 0) return -1;
          *v = (int32_t)x;
          return 0;
      default:
          return -1;   /* STR16 / STR carry text, not a number */
      }
  }

  int pcfg_raw_text(const pcfg_spec_t *s, int32_t v, char *out, size_t cap) {
      if (!s || !out || cap == 0) return -1;
      switch (s->ftype) {
      case HG_T_HHMM: {
          if (v < 0 || v > 1439) return -1;
          char t[6];
          hg_hhmm_format((int)v, t);
          snprintf(out, cap, "%s", t);
          return 0;
      }
      case HG_T_ENUM:
          if (v < 0 || v >= s->n_opts) return -1;
          snprintf(out, cap, "%s", s->opts[v]);
          return 0;
      case HG_T_PIN:
          if (v == s->none_value) { snprintf(out, cap, "NONE"); return 0; }
          snprintf(out, cap, "%ld", (long)v);
          return 0;
      case HG_T_BOOL:
          snprintf(out, cap, "%s", v ? "1" : "0");
          return 0;
      case HG_T_U8: case HG_T_U16:
          snprintf(out, cap, "%ld", (long)v);
          return 0;
      default:
          return -1;
      }
  }

  int32_t pcfg_step(const pcfg_spec_t *s, int32_t cur, int dir, int big) {
      if (!s) return cur;
      int32_t d = (big && s->big_step > 0) ? s->big_step : (s->step > 0 ? s->step : 1);
      int64_t n = (int64_t)cur + (dir > 0 ? d : (dir < 0 ? -d : 0));
      if (n < s->min) n = s->min;
      if (n > s->max) n = s->max;
      return (int32_t)n;
  }

  static const hg_field_t *find_row(pcfg_table_t t, int g, const char *key) {
      const hg_field_t *tab = (t == PCFG_TABLE_MASTER) ? HG_MFIELDS : HG_FIELDS;
      int n = (t == PCFG_TABLE_MASTER) ? HG_MFIELD_COUNT : HG_FIELD_COUNT;
      for (int i = 0; i < n; i++) if (tab[i].group == g && ci_eq(tab[i].key, key)) return &tab[i];
      return NULL;
  }

  int pcfg_locate(pcfg_table_t t, const char *path, uint8_t *group, int *idx, const hg_field_t **row) {
      char buf[96];
      if (!path || !group || !idx || !row) return -1;
      size_t n = strlen(path);
      if (n == 0 || n >= sizeof buf) return -1;
      memcpy(buf, path, n + 1);
      char *p = buf;
      int ix = -1, pfx = 0;                        /* pfx: 0 none, 1 "shelf[N].", 2 "aux[N]." */
      if (t == PCFG_TABLE_ZONE) {
          if (prefix_ci(p, "cfg.")) p += 4;        /* the web's cfgParseBadPath strips the same two roots */
          else if (prefix_ci(p, "hw.")) p += 3;
          if (prefix_ci(p, "shelf[")) { pfx = 1; p += 6; }
          else if (prefix_ci(p, "aux[")) { pfx = 2; p += 4; }
          if (pfx) {
              char *end;
              long v = strtol(p, &end, 10);
              if (end == p || end[0] != ']' || end[1] != '.') return -1;
              ix = (int)v;
              p = end + 2;
          }
      }
      char *dot = strchr(p, '.');
      const char *gname = NULL, *key = p;
      if (dot) {
          *dot = '\0';
          gname = p;
          key = dot + 1;
          if (strchr(key, '.') || !*key) return -1;
      }
      int g = -1;
      if (t == PCFG_TABLE_MASTER) {
          if (!gname) return -1;
          for (int i = 0; i < HG_MG_COUNT; i++) if (ci_eq(gname, HG_MGROUP_NAMES[i])) g = i;
          if (g < 0) return -1;
          ix = -1;
      } else {
          if (gname) { g = hg_group_find(gname); if (g < 0) return -1; }
          else if (pfx == 1) g = HG_G_SHELF;        /* hg_cfg_validate: "shelf[0].enabled" -- SHELF keys carry no group */
          else return -1;
          int sc = hg_group_scope((uint8_t)g);
          if (sc == 0) { if (pfx) return -1; ix = -1; }
          else if (sc == 1) { if (pfx == 2 || (pfx == 1 && (ix < 0 || ix >= HG_MAX_SHELVES))) return -1; }
          else { if (pfx == 1 || (pfx == 2 && (ix < 0 || ix >= HG_MAX_AUX))) return -1; }
      }
      const hg_field_t *r = find_row(t, g, key);
      if (!r) return -1;
      *group = (uint8_t)g;
      *idx = ix;
      *row = r;
      return 0;
  }
  ```

- [ ] **Step 10: Run the generator test and watch it pass**

  Run the single host test with `<name>` = `test_pcfg_gen`.
  Expected: `22 Tests 0 Failures 0 Ignored` and `100% tests passed, 0 tests failed out of 1`.

- [ ] **Step 11: Compile the generator on the P4 too**

  In `components/panel_ui/CMakeLists.txt`, inside the gating `if()` block (after the source lines earlier tasks
  added), add:
  ```cmake
      list(APPEND PANEL_SRCS "pcfg_gen.c" "pcfg_pres.c")      # Task 17: the config-widget generator (pure)
  ```
  No `REQUIRES` change: `hg_cfg` and `hg_mcfg` are already in `panel_ui`'s `REQUIRES` (Task 6).

- [ ] **Step 12: GATE-HOST**

  Run GATE-HOST. Expected: `100% tests passed, 0 tests failed out of 44` (+1).

- [ ] **Step 13: GATE-P4**

  Run GATE-P4. Expected: `Project build complete`, no `warning:` lines, the target line matches, and the lock is
  restored.

- [ ] **Step 14: GATE-ESP32**

  `hg_cfg` is shared with the zone, so GATE-ESP32 is mandatory. Run it. Expected: three `Project build complete`
  lines and an empty lock diff.

- [ ] **Step 15: Commit and push**

  ```powershell
  git -C C:\Projects\HillGrov add components/hg_cfg/hg_cfg.h components/hg_cfg/hg_cfg_fields.c components/panel_ui/pcfg_gen.h components/panel_ui/pcfg_gen.c components/panel_ui/pcfg_pres.h components/panel_ui/pcfg_pres.c components/panel_ui/CMakeLists.txt tests/host/test_hg_cfg_fields.c tests/host/test_pcfg_gen.c tests/host/CMakeLists.txt
  git -C C:\Projects\HillGrov status --short
  ```
  Expected: every path above shows as staged (`M ` or `A `), and ` M docs/hillgrow-features.drawio` stays unstaged.
  ```powershell
  git -C C:\Projects\HillGrov commit -m "feat(panel_ui): type-driven config-widget generator from the field tables" -m "Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>"
  git -C C:\Projects\HillGrov push
  ```

---

### Task 18: Edit model and refusal texts (Stage 2)

**Files:**
- Create: `components/panel_ui/pcfg_edit.h`, `components/panel_ui/pcfg_edit.c` (P)
- Create: `components/panel_ui/pnl_msg.h`, `components/panel_ui/pnl_msg.c` (P)
- Create: `tests/host/test_pcfg_edit.c`, `tests/host/test_pnl_msg.c`
- Modify: `tests/host/CMakeLists.txt` (append two `hg_test` rows)
- Modify: `components/panel_ui/CMakeLists.txt` (append two sources)

**Interfaces:**
- Consumes: from Task 3, `psvc_fedit_t`, `PSVC_FEDIT_TEXT_MAX` (`psvc_edit.h`), `psvc_rc_t`, `PSVC_RC_COUNT` and
  `psvc_rc_token()` (`psvc_rc.h`). From Task 17, `pcfg_table_t` and `hg_group_is_hw()`. Also `hg_group_scope()` and
  `hg_mcfg_is_secret()`.
- Produces:
  ```c
  /* pcfg_edit.h (pure) */
  #define PCFG_EDIT_MAX 128
  typedef struct { pcfg_table_t table; uint8_t zone; int n; psvc_fedit_t d[PCFG_EDIT_MAX]; } pcfg_edits_t;  /* ~10 KB: PSRAM */
  void pcfg_edits_reset(pcfg_edits_t *e, pcfg_table_t t, uint8_t zone);
  int  pcfg_edits_set(pcfg_edits_t *e, uint8_t group, int idx, const hg_field_t *f, const char *text);
       /* replace-or-append keyed (group, idx, f); 0 / -1 full or text too long / -2 hardware-plane row (never editable) */
  int  pcfg_edits_drop(pcfg_edits_t *e, uint8_t group, int idx, const hg_field_t *f);   /* 0 / -1 absent */
  const psvc_fedit_t *pcfg_edits_get(const pcfg_edits_t *e, uint8_t group, int idx, const hg_field_t *f);  /* NULL = clean */
  int  pcfg_edits_export(const pcfg_edits_t *e, psvc_fedit_t *out, int cap);
       /* n copied; master table: a blank secret is dropped (blank = unchanged); 0 -> "No changes to save" */
  void pcfg_edits_wipe(pcfg_edits_t *e);   /* memset 0 (secrets included) + reset */

  /* pnl_msg.h (pure; ASCII) */
  typedef enum { PNL_CTX_ZONE_LOAD = 0, PNL_CTX_ZONE_SAVE, PNL_CTX_MCFG_SAVE, PNL_CTX_WIFI_JOIN, PNL_CTX_WIFI_AP, PNL_CTX_SCAN,
                 PNL_CTX_TZ, PNL_CTX_PASSWORD, PNL_CTX_FLEET_ZONE, PNL_CTX_FLEET_ALL, PNL_CTX_FLEET_ABORT, PNL_CTX_FW_MASTER,
                 PNL_CTX_FW_ZONE, PNL_CTX_COUNT } pnl_msg_ctx_t;
  typedef struct { int zone; const char *version, *slot; uint32_t len; } pnl_msg_arg_t;
  int pnl_msg(pnl_msg_ctx_t ctx, psvc_rc_t rc, const pnl_msg_arg_t *arg /* NULL ok */, char *out, size_t cap);
  ```
  Semantics this task fixes (no new names):
  - `idx` is normalised on the way in: `-1` for every master row and every scope-0 zone group (the `psvc_fedit_t`
    convention). Lookups normalise the same way, so `get(ZONECFG, 0, NAME)` and `get(ZONECFG, -1, NAME)` find the
    same entry. `idx` outside -1..127 (the `int8_t` range) is refused with -1.
  - `pcfg_edits_export` returns -1 when `cap` is smaller than the number to export. Nothing is partially exported,
    so a save can never apply half an edit set.
  - `pnl_msg` returns `strlen(out)` (always > 0), or -1 when `out` is NULL or `cap` is 0. Output is clipped to `cap`
    and NUL-terminated. Every byte outside 0x20..0x7E, including any in a caller-supplied `version` or `slot`,
    becomes `?`. That keeps the Global Constraints' ASCII rule true for every string `pnl_msg` builds.
  - An out-of-range `ctx` answers the token. `rc >= PSVC_RC_COUNT` answers `psvc_rc_token()`'s `"INTERNAL"`.

  The texts (the web's wording where it has one, with ASCII in place of "—" and "…"):

  | Context | Result → text |
  |---|---|
  | ZONE_LOAD | OK → "Loaded." ; NO_CACHE → "Zone config not adopted yet -- the zone must come online and sync at least once before it can be configured." ; ZONE_UNKNOWN → "Unknown zone." ; ZONE_NOT_ONLINE → "Zone is offline." ; LOW_HEAP → "Master is low on memory -- try again shortly." ; else → token, or "Failed to load" |
  | ZONE_SAVE | OK → "Queued, pushing to zone" ; BUSY → "Zone busy, retry" ; ZONE_NOT_ONLINE → "Zone is offline -- nothing was saved" ; else → token, or "Save failed" |
  | MCFG_SAVE | OK → "Saved." ; BUSY → "Master config busy (another change is being applied), retry" ; else → token |
  | WIFI_JOIN | OK → "Saved -- joining..." ; BUSY → the MCFG_SAVE busy text ; else → token |
  | WIFI_AP | OK → "Saved." ; BUSY → the MCFG_SAVE busy text ; else → token |
  | SCAN | OK → "Scan done." ; BUSY → the MCFG_SAVE busy text ; else → token, or "Scan failed" |
  | TZ | OK → "Time zone saved." ; BUSY → the MCFG_SAVE busy text ; else → token |
  | PASSWORD | OK → "Password changed. Every phone and browser was logged out -- log in again with the new password." ; INVALID → "New password invalid (8 to 63 characters)" ; else → "Failed (<TOKEN>)" |
  | FLEET_ZONE | OK → "Update queued for zone N." ; else → token, or "Failed" |
  | FLEET_ALL | OK → "Fleet update queued." ; else → token, or "Failed" |
  | FLEET_ABORT | OK → "Fleet update aborted." ; else → token, or "Failed" |
  | FW_MASTER | OK → "Uploaded v<version> to <slot>." |
  | FW_ZONE | OK → "Uploaded (<len> bytes)." |
  | FW, either | IMAGE_MISMATCH → "Not a <master\|zone> image -- nothing was erased" ; TRIAL_PENDING → "An OTA trial is running -- wait for it to pass or SET OTA CONFIRM" ; FLEET_ACTIVE → "A fleet update is running" ; UPLOAD_ACTIVE → "Another upload is in progress" ; ZONE_FW_BUSY → "A zone is downloading the image -- retry in a minute" ; RECV_FAILED → "microSD read failed" ; else → token |

  The OK texts for ZONE_LOAD, SCAN and TZ, and the BUSY rows for WIFI_JOIN, WIFI_AP, SCAN and TZ, go beyond the
  outline's table. They are added so that every context answers every result with a sentence. The panel-facing
  `psvc_*` setters report lock contention as BUSY (D10), and the bare token "BUSY" would say less than the
  master-config sentence does.

**What each check proves:** both modules are pure and fully host-tested. GATE-P4 proves they compile under GCC.

- [ ] **Step 1: Write the failing edit-model test**

  Append to `tests/host/CMakeLists.txt`:
  ```cmake
  # panel_ui's config edit model and refusal texts (Task 18): pure.
  hg_test(test_pcfg_edit ${COMP}/panel_ui/pcfg_edit.c ${HG_CFG_SRC} ${COMP}/hg_mcfg/hg_mcfg.c ${COMP}/hg_blob/hg_blob.c)
  hg_test(test_pnl_msg ${COMP}/panel_ui/pnl_msg.c ${COMP}/panel_svc/psvc_rc.c)
  ```
  Create `tests/host/test_pcfg_edit.c`:
  ```c
  /* test_pcfg_edit.c -- the dirty set: per-field, replace-or-append, hw plane refused, blank secrets never exported. */
  #include <string.h>
  #include "unity.h"
  #include "hg_cfg.h"
  #include "hg_mcfg.h"
  #include "pcfg_edit.h"

  static pcfg_edits_t E;   /* ~10 KB: static, never on the stack */

  static const hg_field_t *zrow(uint8_t g, const char *k) {
      for (int i = 0; i < HG_FIELD_COUNT; i++)
          if (HG_FIELDS[i].group == g && strcmp(HG_FIELDS[i].key, k) == 0) return &HG_FIELDS[i];
      TEST_FAIL_MESSAGE(k);
      return NULL;
  }
  static const hg_field_t *mrow(uint8_t g, const char *k) {
      for (int i = 0; i < HG_MFIELD_COUNT; i++)
          if (HG_MFIELDS[i].group == g && strcmp(HG_MFIELDS[i].key, k) == 0) return &HG_MFIELDS[i];
      TEST_FAIL_MESSAGE(k);
      return NULL;
  }
  void setUp(void) { pcfg_edits_reset(&E, PCFG_TABLE_ZONE, 2); }
  void tearDown(void) {}

  static void test_reset(void) {
      TEST_ASSERT_EQUAL_INT(0, E.n);
      TEST_ASSERT_EQUAL_INT(PCFG_TABLE_ZONE, E.table);
      TEST_ASSERT_EQUAL_UINT8(2, E.zone);
  }
  static void test_append_then_replace(void) {
      const hg_field_t *t = zrow(HG_G_WATER, "TARGET");
      TEST_ASSERT_EQUAL_INT(0, pcfg_edits_set(&E, HG_G_WATER, 1, t, "55"));
      TEST_ASSERT_EQUAL_INT(1, E.n);
      TEST_ASSERT_EQUAL_INT(0, pcfg_edits_set(&E, HG_G_WATER, 1, t, "56"));   /* same key: replaced */
      TEST_ASSERT_EQUAL_INT(1, E.n);
      TEST_ASSERT_EQUAL_INT(0, pcfg_edits_set(&E, HG_G_WATER, 2, t, "40"));   /* other shelf: appended */
      TEST_ASSERT_EQUAL_INT(2, E.n);
      const psvc_fedit_t *d = pcfg_edits_get(&E, HG_G_WATER, 1, t);
      TEST_ASSERT_NOT_NULL(d);
      TEST_ASSERT_EQUAL_STRING("56", d->text);
      TEST_ASSERT_EQUAL_INT8(1, d->idx);
      TEST_ASSERT_EQUAL_PTR(t, d->f);
      TEST_ASSERT_NULL(pcfg_edits_get(&E, HG_G_WATER, 3, t));
  }
  static void test_scope0_idx_normalised(void) {
      const hg_field_t *name = zrow(HG_G_ZONECFG, "NAME");
      TEST_ASSERT_EQUAL_INT(0, pcfg_edits_set(&E, HG_G_ZONECFG, 3, name, "north"));
      TEST_ASSERT_EQUAL_INT8(-1, E.d[0].idx);
      TEST_ASSERT_NOT_NULL(pcfg_edits_get(&E, HG_G_ZONECFG, -1, name));
      TEST_ASSERT_NOT_NULL(pcfg_edits_get(&E, HG_G_ZONECFG, 0, name));
      TEST_ASSERT_EQUAL_INT(0, pcfg_edits_set(&E, HG_G_ZONECFG, -1, name, "south"));
      TEST_ASSERT_EQUAL_INT(1, E.n);
  }
  static void test_hw_row_refused(void) {
      TEST_ASSERT_EQUAL_INT(-2, pcfg_edits_set(&E, HG_G_HWSHELF, 0, zrow(HG_G_HWSHELF, "PUMP"), "3"));
      TEST_ASSERT_EQUAL_INT(-2, pcfg_edits_set(&E, HG_G_HW, -1, zrow(HG_G_HW, "SHELVES"), "2"));
      TEST_ASSERT_EQUAL_INT(-2, pcfg_edits_set(&E, HG_G_CAL, 1, zrow(HG_G_CAL, "DRY_A"), "2800"));
      TEST_ASSERT_EQUAL_INT(0, E.n);
  }
  static void test_bad_args_and_long_text(void) {
      char big[PSVC_FEDIT_TEXT_MAX + 1];
      memset(big, 'a', sizeof big - 1);
      big[sizeof big - 1] = '\0';                                    /* 65 characters: one too many */
      const hg_field_t *crop = zrow(HG_G_SHELF, "CROP");
      TEST_ASSERT_EQUAL_INT(-1, pcfg_edits_set(&E, HG_G_SHELF, 0, crop, big));
      big[PSVC_FEDIT_TEXT_MAX - 1] = '\0';                           /* 64 characters: fits */
      TEST_ASSERT_EQUAL_INT(0, pcfg_edits_set(&E, HG_G_SHELF, 0, crop, big));
      TEST_ASSERT_EQUAL_INT(-1, pcfg_edits_set(&E, HG_G_SHELF, 0, crop, NULL));
      TEST_ASSERT_EQUAL_INT(-1, pcfg_edits_set(&E, HG_G_SHELF, 0, NULL, "x"));
      TEST_ASSERT_EQUAL_INT(-1, pcfg_edits_set(&E, HG_G_SHELF, 128, crop, "x"));
      TEST_ASSERT_EQUAL_INT(-1, pcfg_edits_set(NULL, HG_G_SHELF, 0, crop, "x"));
  }
  static void test_full(void) {
      const hg_field_t *t = zrow(HG_G_WATER, "TARGET");
      for (int i = 0; i < PCFG_EDIT_MAX; i++)                         /* distinct keys by idx 0..127 */
          TEST_ASSERT_EQUAL_INT(0, pcfg_edits_set(&E, HG_G_WATER, i, t, "50"));
      TEST_ASSERT_EQUAL_INT(PCFG_EDIT_MAX, E.n);
      TEST_ASSERT_EQUAL_INT(-1, pcfg_edits_set(&E, HG_G_LIGHT, 0, zrow(HG_G_LIGHT, "WHITE"), "10"));
      TEST_ASSERT_EQUAL_INT(0, pcfg_edits_set(&E, HG_G_WATER, 5, t, "51"));   /* a replace still works when full */
  }
  static void test_drop_keeps_order(void) {
      const hg_field_t *a = zrow(HG_G_WATER, "TARGET"), *b = zrow(HG_G_WATER, "HYST"), *c = zrow(HG_G_WATER, "DOSE_S");
      pcfg_edits_set(&E, HG_G_WATER, 0, a, "50");
      pcfg_edits_set(&E, HG_G_WATER, 0, b, "5");
      pcfg_edits_set(&E, HG_G_WATER, 0, c, "20");
      TEST_ASSERT_EQUAL_INT(0, pcfg_edits_drop(&E, HG_G_WATER, 0, b));
      TEST_ASSERT_EQUAL_INT(2, E.n);
      TEST_ASSERT_EQUAL_PTR(a, E.d[0].f);
      TEST_ASSERT_EQUAL_PTR(c, E.d[1].f);
      TEST_ASSERT_EQUAL_INT(0, E.d[2].text[0]);                       /* the vacated slot is zeroed */
      TEST_ASSERT_EQUAL_INT(-1, pcfg_edits_drop(&E, HG_G_WATER, 0, b));
  }
  static void test_export_drops_blank_secret_only(void) {
      pcfg_edits_reset(&E, PCFG_TABLE_MASTER, 0);
      TEST_ASSERT_EQUAL_INT(0, pcfg_edits_set(&E, HG_MG_WIFI, -1, mrow(HG_MG_WIFI, "STA_PASS"), ""));
      TEST_ASSERT_EQUAL_INT(0, pcfg_edits_set(&E, HG_MG_WIFI, -1, mrow(HG_MG_WIFI, "STA_SSID"), ""));
      TEST_ASSERT_EQUAL_INT(0, pcfg_edits_set(&E, HG_MG_SYS, -1, mrow(HG_MG_SYS, "HOSTNAME"), "gh1"));
      psvc_fedit_t out[4];
      TEST_ASSERT_EQUAL_INT(2, pcfg_edits_export(&E, out, 4));
      TEST_ASSERT_EQUAL_STRING("STA_SSID", out[0].f->key);            /* a blank SSID is a real edit: open STA off */
      TEST_ASSERT_EQUAL_STRING("", out[0].text);
      TEST_ASSERT_EQUAL_STRING("HOSTNAME", out[1].f->key);
      TEST_ASSERT_EQUAL_INT(-1, pcfg_edits_export(&E, out, 1));       /* never a partial set */
  }
  static void test_export_zone_keeps_blank_text(void) {
      TEST_ASSERT_EQUAL_INT(0, pcfg_edits_set(&E, HG_G_SHELF, 0, zrow(HG_G_SHELF, "CROP"), ""));
      psvc_fedit_t out[2];
      TEST_ASSERT_EQUAL_INT(1, pcfg_edits_export(&E, out, 2));
  }
  static void test_export_none(void) {
      psvc_fedit_t out[1];
      TEST_ASSERT_EQUAL_INT(0, pcfg_edits_export(&E, out, 1));
      TEST_ASSERT_EQUAL_INT(0, pcfg_edits_export(&E, NULL, 0));
  }
  static void test_wipe(void) {
      pcfg_edits_reset(&E, PCFG_TABLE_MASTER, 0);
      pcfg_edits_set(&E, HG_MG_WIFI, -1, mrow(HG_MG_WIFI, "STA_PASS"), "hunter22hunter22");
      pcfg_edits_wipe(&E);
      TEST_ASSERT_EQUAL_INT(0, E.n);
      TEST_ASSERT_EQUAL_INT(PCFG_TABLE_MASTER, E.table);
      TEST_ASSERT_EQUAL_UINT8(0, E.zone);
      for (size_t i = 0; i < sizeof E.d[0].text; i++) TEST_ASSERT_EQUAL_INT(0, E.d[0].text[i]);
      TEST_ASSERT_NULL(E.d[0].f);
  }

  int main(void) {
      UNITY_BEGIN();
      RUN_TEST(test_reset);
      RUN_TEST(test_append_then_replace);
      RUN_TEST(test_scope0_idx_normalised);
      RUN_TEST(test_hw_row_refused);
      RUN_TEST(test_bad_args_and_long_text);
      RUN_TEST(test_full);
      RUN_TEST(test_drop_keeps_order);
      RUN_TEST(test_export_drops_blank_secret_only);
      RUN_TEST(test_export_zone_keeps_blank_text);
      RUN_TEST(test_export_none);
      RUN_TEST(test_wipe);
      return UNITY_END();
  }
  ```

- [ ] **Step 2: Write the failing refusal-text test**

  Create `tests/host/test_pnl_msg.c`:
  ```c
  /* test_pnl_msg.c -- one refusal vocabulary, one set of sentences: every context x every result is a
   * non-empty printable-ASCII sentence; the web-worded ones match the web exactly. */
  #include <string.h>
  #include "unity.h"
  #include "pnl_msg.h"

  void setUp(void) {}
  void tearDown(void) {}

  static const char *M(pnl_msg_ctx_t c, psvc_rc_t rc, const pnl_msg_arg_t *a) {
      static char b[200];
      int n = pnl_msg(c, rc, a, b, sizeof b);
      TEST_ASSERT_GREATER_THAN_INT(0, n);
      TEST_ASSERT_EQUAL_INT(n, (int)strlen(b));
      return b;
  }
  static void assert_ascii(const char *s) {
      TEST_ASSERT_TRUE(s[0] != '\0');
      for (const char *p = s; *p; p++)
          TEST_ASSERT_TRUE_MESSAGE((unsigned char)*p >= 0x20 && (unsigned char)*p <= 0x7E, s);
  }

  static void test_every_context_every_result(void) {
      pnl_msg_arg_t a = { .zone = 2, .version = "1.4.2", .slot = "ota_1", .len = 1234 };
      for (int c = 0; c <= PNL_CTX_COUNT; c++)              /* includes one out-of-range context */
          for (int rc = 0; rc <= PSVC_RC_COUNT; rc++) {      /* includes one out-of-range result */
              assert_ascii(M((pnl_msg_ctx_t)c, (psvc_rc_t)rc, &a));
              assert_ascii(M((pnl_msg_ctx_t)c, (psvc_rc_t)rc, NULL));
          }
  }
  static void test_zone_texts(void) {
      TEST_ASSERT_EQUAL_STRING("Zone config not adopted yet -- the zone must come online and sync at least once before it can be configured.",
                               M(PNL_CTX_ZONE_LOAD, PSVC_E_NO_CACHE, NULL));
      TEST_ASSERT_EQUAL_STRING("Unknown zone.", M(PNL_CTX_ZONE_LOAD, PSVC_E_ZONE_UNKNOWN, NULL));
      TEST_ASSERT_EQUAL_STRING("Zone is offline.", M(PNL_CTX_ZONE_LOAD, PSVC_E_ZONE_NOT_ONLINE, NULL));
      TEST_ASSERT_EQUAL_STRING("STORAGE", M(PNL_CTX_ZONE_LOAD, PSVC_E_STORAGE, NULL));
      TEST_ASSERT_EQUAL_STRING("Queued, pushing to zone", M(PNL_CTX_ZONE_SAVE, PSVC_OK, NULL));
      TEST_ASSERT_EQUAL_STRING("Zone busy, retry", M(PNL_CTX_ZONE_SAVE, PSVC_E_BUSY, NULL));
      TEST_ASSERT_EQUAL_STRING("Zone is offline -- nothing was saved", M(PNL_CTX_ZONE_SAVE, PSVC_E_ZONE_NOT_ONLINE, NULL));
      TEST_ASSERT_EQUAL_STRING("VALIDATION", M(PNL_CTX_ZONE_SAVE, PSVC_E_VALIDATION, NULL));
      TEST_ASSERT_EQUAL_STRING("NO_CACHE", M(PNL_CTX_ZONE_SAVE, PSVC_E_NO_CACHE, NULL));
  }
  static void test_master_and_wifi_texts(void) {
      TEST_ASSERT_EQUAL_STRING("Saved.", M(PNL_CTX_MCFG_SAVE, PSVC_OK, NULL));
      TEST_ASSERT_EQUAL_STRING("Master config busy (another change is being applied), retry", M(PNL_CTX_MCFG_SAVE, PSVC_E_BUSY, NULL));
      TEST_ASSERT_EQUAL_STRING("INVALID_FIELD", M(PNL_CTX_MCFG_SAVE, PSVC_E_INVALID_FIELD, NULL));
      TEST_ASSERT_EQUAL_STRING("Saved -- joining...", M(PNL_CTX_WIFI_JOIN, PSVC_OK, NULL));
      TEST_ASSERT_EQUAL_STRING("Saved.", M(PNL_CTX_WIFI_AP, PSVC_OK, NULL));
      TEST_ASSERT_EQUAL_STRING("Master config busy (another change is being applied), retry", M(PNL_CTX_TZ, PSVC_E_BUSY, NULL));
      TEST_ASSERT_EQUAL_STRING("INTERNAL", M(PNL_CTX_SCAN, PSVC_E_INTERNAL, NULL));
  }
  static void test_password_texts(void) {
      TEST_ASSERT_EQUAL_STRING("Password changed. Every phone and browser was logged out -- log in again with the new password.",
                               M(PNL_CTX_PASSWORD, PSVC_OK, NULL));
      TEST_ASSERT_EQUAL_STRING("New password invalid (8 to 63 characters)", M(PNL_CTX_PASSWORD, PSVC_E_INVALID, NULL));
      TEST_ASSERT_EQUAL_STRING("Failed (STORAGE)", M(PNL_CTX_PASSWORD, PSVC_E_STORAGE, NULL));
  }
  static void test_fleet_texts(void) {
      pnl_msg_arg_t a = { .zone = 3 };
      TEST_ASSERT_EQUAL_STRING("Update queued for zone 3.", M(PNL_CTX_FLEET_ZONE, PSVC_OK, &a));
      TEST_ASSERT_EQUAL_STRING("Fleet update queued.", M(PNL_CTX_FLEET_ALL, PSVC_OK, NULL));
      TEST_ASSERT_EQUAL_STRING("Fleet update aborted.", M(PNL_CTX_FLEET_ABORT, PSVC_OK, NULL));
      TEST_ASSERT_EQUAL_STRING("FLEET_BUSY", M(PNL_CTX_FLEET_ZONE, PSVC_E_FLEET_BUSY, &a));
  }
  static void test_fw_texts(void) {
      pnl_msg_arg_t a = { .version = "1.4.2", .slot = "ota_1", .len = 123456 };
      TEST_ASSERT_EQUAL_STRING("Uploaded v1.4.2 to ota_1.", M(PNL_CTX_FW_MASTER, PSVC_OK, &a));
      TEST_ASSERT_EQUAL_STRING("Uploaded (123456 bytes).", M(PNL_CTX_FW_ZONE, PSVC_OK, &a));
      TEST_ASSERT_EQUAL_STRING("Not a master image -- nothing was erased", M(PNL_CTX_FW_MASTER, PSVC_E_IMAGE_MISMATCH, &a));
      TEST_ASSERT_EQUAL_STRING("Not a zone image -- nothing was erased", M(PNL_CTX_FW_ZONE, PSVC_E_IMAGE_MISMATCH, &a));
      TEST_ASSERT_EQUAL_STRING("An OTA trial is running -- wait for it to pass or SET OTA CONFIRM", M(PNL_CTX_FW_MASTER, PSVC_E_TRIAL_PENDING, &a));
      TEST_ASSERT_EQUAL_STRING("A fleet update is running", M(PNL_CTX_FW_ZONE, PSVC_E_FLEET_ACTIVE, &a));
      TEST_ASSERT_EQUAL_STRING("Another upload is in progress", M(PNL_CTX_FW_ZONE, PSVC_E_UPLOAD_ACTIVE, &a));
      TEST_ASSERT_EQUAL_STRING("A zone is downloading the image -- retry in a minute", M(PNL_CTX_FW_ZONE, PSVC_E_ZONE_FW_BUSY, &a));
      TEST_ASSERT_EQUAL_STRING("microSD read failed", M(PNL_CTX_FW_ZONE, PSVC_E_RECV_FAILED, &a));
      TEST_ASSERT_EQUAL_STRING("TOO_LARGE", M(PNL_CTX_FW_MASTER, PSVC_E_TOO_LARGE, &a));
      TEST_ASSERT_EQUAL_STRING("Uploaded v? to ?.", M(PNL_CTX_FW_MASTER, PSVC_OK, NULL));
  }
  static void test_foreign_bytes_sanitised(void) {
      pnl_msg_arg_t a = { .version = "1.\xC3\xA9", .slot = "ota_0" };
      TEST_ASSERT_EQUAL_STRING("Uploaded v1.?? to ota_0.", M(PNL_CTX_FW_MASTER, PSVC_OK, &a));
  }
  static void test_clipping_and_bad_args(void) {
      char b[8];
      TEST_ASSERT_EQUAL_INT(7, pnl_msg(PNL_CTX_ZONE_SAVE, PSVC_OK, NULL, b, sizeof b));
      TEST_ASSERT_EQUAL_STRING("Queued,", b);
      TEST_ASSERT_EQUAL_INT(-1, pnl_msg(PNL_CTX_ZONE_SAVE, PSVC_OK, NULL, NULL, 8));
      TEST_ASSERT_EQUAL_INT(-1, pnl_msg(PNL_CTX_ZONE_SAVE, PSVC_OK, NULL, b, 0));
  }

  int main(void) {
      UNITY_BEGIN();
      RUN_TEST(test_every_context_every_result);
      RUN_TEST(test_zone_texts);
      RUN_TEST(test_master_and_wifi_texts);
      RUN_TEST(test_password_texts);
      RUN_TEST(test_fleet_texts);
      RUN_TEST(test_fw_texts);
      RUN_TEST(test_foreign_bytes_sanitised);
      RUN_TEST(test_clipping_and_bad_args);
      return UNITY_END();
  }
  ```

- [ ] **Step 3: Run both and watch them fail**

  Run the single host test with `<name>` = `test_pcfg_edit`, then with `<name>` = `test_pnl_msg`.
  Expected, each time: configure fails with `Cannot find source file:` naming `.../components/panel_ui/pcfg_edit.c`
  (the first missing source in the file).

- [ ] **Step 4: Create `components/panel_ui/pcfg_edit.h` and `components/panel_ui/pcfg_edit.c`**

  `pcfg_edit.h`:
  ```c
  #pragma once
  /* pcfg_edit.h -- the panel's dirty set (pure). A save applies ONLY these entries, on the worker, to a
   * fresh copy taken at save time: the web PUT has no generation check, so both faces are last-writer-wins
   * per field (map-cfg §6). Kept per zone in PSRAM; survives tab and destination changes. */
  #include <stdint.h>
  #include "hg_cfg.h"
  #include "psvc_edit.h"
  #include "pcfg_gen.h"
  #ifdef __cplusplus
  extern "C" {
  #endif

  #define PCFG_EDIT_MAX 128
  typedef struct { pcfg_table_t table; uint8_t zone; int n; psvc_fedit_t d[PCFG_EDIT_MAX]; } pcfg_edits_t;  /* ~10 KB: PSRAM */
  void pcfg_edits_reset(pcfg_edits_t *e, pcfg_table_t t, uint8_t zone);
  /* replace-or-append keyed (group, idx, f); idx normalised (-1: master rows and scope-0 groups);
   * 0 / -1 full, text too long (>= PSVC_FEDIT_TEXT_MAX), idx outside -1..127 or NULL args / -2 hardware-plane row */
  int  pcfg_edits_set(pcfg_edits_t *e, uint8_t group, int idx, const hg_field_t *f, const char *text);
  int  pcfg_edits_drop(pcfg_edits_t *e, uint8_t group, int idx, const hg_field_t *f);   /* 0 / -1 absent; order kept */
  const psvc_fedit_t *pcfg_edits_get(const pcfg_edits_t *e, uint8_t group, int idx, const hg_field_t *f);  /* NULL = clean */
  /* n copied (insertion order); master table: a blank secret is dropped (blank = unchanged); 0 -> "No changes to
   * save"; -1 when cap is smaller than the set (nothing is ever partially exported) */
  int  pcfg_edits_export(const pcfg_edits_t *e, psvc_fedit_t *out, int cap);
  void pcfg_edits_wipe(pcfg_edits_t *e);   /* memset 0 (secrets included) + reset, keeping table and zone */

  #ifdef __cplusplus
  }
  #endif
  ```
  `pcfg_edit.c`:
  ```c
  #include <string.h>
  #include "pcfg_edit.h"
  #include "hg_mcfg.h"

  static int norm_idx(const pcfg_edits_t *e, uint8_t group, int idx) {
      if (e->table == PCFG_TABLE_MASTER) return -1;
      return hg_group_scope(group) == 0 ? -1 : idx;
  }
  static int find(const pcfg_edits_t *e, uint8_t group, int idx, const hg_field_t *f) {
      for (int i = 0; i < e->n; i++)
          if (e->d[i].f == f && e->d[i].group == group && e->d[i].idx == idx) return i;
      return -1;
  }
  static int blank_secret(const pcfg_edits_t *e, const psvc_fedit_t *d) {
      return e->table == PCFG_TABLE_MASTER && hg_mcfg_is_secret(d->f) && d->text[0] == '\0';
  }

  void pcfg_edits_reset(pcfg_edits_t *e, pcfg_table_t t, uint8_t zone) {
      if (!e) return;
      memset(e, 0, sizeof *e);
      e->table = t;
      e->zone = zone;
  }

  int pcfg_edits_set(pcfg_edits_t *e, uint8_t group, int idx, const hg_field_t *f, const char *text) {
      if (!e || !f || !text) return -1;
      if (e->table == PCFG_TABLE_ZONE && hg_group_is_hw(group)) return -2;
      size_t len = strlen(text);
      if (len >= PSVC_FEDIT_TEXT_MAX) return -1;
      idx = norm_idx(e, group, idx);
      if (idx < -1 || idx > 127) return -1;
      int i = find(e, group, idx, f);
      if (i < 0) {
          if (e->n >= PCFG_EDIT_MAX) return -1;
          i = e->n++;
          e->d[i].group = group;
          e->d[i].idx = (int8_t)idx;
          e->d[i].f = f;
      }
      memset(e->d[i].text, 0, sizeof e->d[i].text);
      memcpy(e->d[i].text, text, len);
      return 0;
  }

  int pcfg_edits_drop(pcfg_edits_t *e, uint8_t group, int idx, const hg_field_t *f) {
      if (!e || !f) return -1;
      int i = find(e, group, norm_idx(e, group, idx), f);
      if (i < 0) return -1;
      memmove(&e->d[i], &e->d[i + 1], (size_t)(e->n - i - 1) * sizeof e->d[0]);
      e->n--;
      memset(&e->d[e->n], 0, sizeof e->d[0]);
      return 0;
  }

  const psvc_fedit_t *pcfg_edits_get(const pcfg_edits_t *e, uint8_t group, int idx, const hg_field_t *f) {
      if (!e || !f) return NULL;
      int i = find(e, group, norm_idx(e, group, idx), f);
      return i < 0 ? NULL : &e->d[i];
  }

  int pcfg_edits_export(const pcfg_edits_t *e, psvc_fedit_t *out, int cap) {
      if (!e) return 0;
      int need = 0;
      for (int i = 0; i < e->n; i++) if (!blank_secret(e, &e->d[i])) need++;
      if (need == 0) return 0;
      if (!out || cap < need) return -1;
      int n = 0;
      for (int i = 0; i < e->n; i++) if (!blank_secret(e, &e->d[i])) out[n++] = e->d[i];
      return n;
  }

  void pcfg_edits_wipe(pcfg_edits_t *e) {
      if (!e) return;
      pcfg_table_t t = e->table;
      uint8_t z = e->zone;
      memset(e, 0, sizeof *e);
      e->table = t;
      e->zone = z;
  }
  ```

- [ ] **Step 5: Create `components/panel_ui/pnl_msg.h` and `components/panel_ui/pnl_msg.c`**

  `pnl_msg.h`:
  ```c
  #pragma once
  /* pnl_msg.h -- refusal and outcome sentences (pure; printable ASCII only). One refusal vocabulary
   * (psvc_rc_t) -> one sentence per context, so a refusal means the same thing in both faces. */
  #include <stddef.h>
  #include <stdint.h>
  #include "psvc_rc.h"
  #ifdef __cplusplus
  extern "C" {
  #endif

  typedef enum { PNL_CTX_ZONE_LOAD = 0, PNL_CTX_ZONE_SAVE, PNL_CTX_MCFG_SAVE, PNL_CTX_WIFI_JOIN, PNL_CTX_WIFI_AP, PNL_CTX_SCAN,
                 PNL_CTX_TZ, PNL_CTX_PASSWORD, PNL_CTX_FLEET_ZONE, PNL_CTX_FLEET_ALL, PNL_CTX_FLEET_ABORT, PNL_CTX_FW_MASTER,
                 PNL_CTX_FW_ZONE, PNL_CTX_COUNT } pnl_msg_ctx_t;
  typedef struct { int zone; const char *version, *slot; uint32_t len; } pnl_msg_arg_t;
  /* strlen(out) (> 0) / -1 (out NULL or cap 0); clipped to cap; bytes outside 0x20..0x7E become '?' */
  int pnl_msg(pnl_msg_ctx_t ctx, psvc_rc_t rc, const pnl_msg_arg_t *arg /* NULL ok */, char *out, size_t cap);

  #ifdef __cplusplus
  }
  #endif
  ```
  `pnl_msg.c`:
  ```c
  #include <stdio.h>
  #include <string.h>
  #include "pnl_msg.h"

  #define MCFG_BUSY "Master config busy (another change is being applied), retry"

  static const char *tok_or(psvc_rc_t rc, const char *fallback) {
      const char *t = psvc_rc_token(rc);
      return (t && t[0]) ? t : fallback;
  }

  static void fw_fail(psvc_rc_t rc, const char *kind, char *out, size_t cap) {
      switch (rc) {
      case PSVC_E_IMAGE_MISMATCH: snprintf(out, cap, "Not a %s image -- nothing was erased", kind); break;
      case PSVC_E_TRIAL_PENDING:  snprintf(out, cap, "An OTA trial is running -- wait for it to pass or SET OTA CONFIRM"); break;
      case PSVC_E_FLEET_ACTIVE:   snprintf(out, cap, "A fleet update is running"); break;
      case PSVC_E_UPLOAD_ACTIVE:  snprintf(out, cap, "Another upload is in progress"); break;
      case PSVC_E_ZONE_FW_BUSY:   snprintf(out, cap, "A zone is downloading the image -- retry in a minute"); break;
      case PSVC_E_RECV_FAILED:    snprintf(out, cap, "microSD read failed"); break;
      default:                    snprintf(out, cap, "%s", tok_or(rc, "Failed")); break;
      }
  }

  int pnl_msg(pnl_msg_ctx_t ctx, psvc_rc_t rc, const pnl_msg_arg_t *arg, char *out, size_t cap) {
      static const pnl_msg_arg_t none = { 0, NULL, NULL, 0 };
      if (!out || cap == 0) return -1;
      const pnl_msg_arg_t *a = arg ? arg : &none;
      const char *ver  = (a->version && a->version[0]) ? a->version : "?";
      const char *slot = (a->slot && a->slot[0]) ? a->slot : "?";
      out[0] = '\0';
      switch (ctx) {
      case PNL_CTX_ZONE_LOAD:
          if (rc == PSVC_OK) snprintf(out, cap, "Loaded.");
          else if (rc == PSVC_E_NO_CACHE) snprintf(out, cap, "Zone config not adopted yet -- the zone must come online and sync at least once before it can be configured.");
          else if (rc == PSVC_E_ZONE_UNKNOWN) snprintf(out, cap, "Unknown zone.");
          else if (rc == PSVC_E_ZONE_NOT_ONLINE) snprintf(out, cap, "Zone is offline.");
          else if (rc == PSVC_E_LOW_HEAP) snprintf(out, cap, "Master is low on memory -- try again shortly.");
          else snprintf(out, cap, "%s", tok_or(rc, "Failed to load"));
          break;
      case PNL_CTX_ZONE_SAVE:
          if (rc == PSVC_OK) snprintf(out, cap, "Queued, pushing to zone");
          else if (rc == PSVC_E_BUSY) snprintf(out, cap, "Zone busy, retry");
          else if (rc == PSVC_E_ZONE_NOT_ONLINE) snprintf(out, cap, "Zone is offline -- nothing was saved");
          else snprintf(out, cap, "%s", tok_or(rc, "Save failed"));
          break;
      case PNL_CTX_MCFG_SAVE:
          if (rc == PSVC_OK) snprintf(out, cap, "Saved.");
          else if (rc == PSVC_E_BUSY) snprintf(out, cap, MCFG_BUSY);
          else snprintf(out, cap, "%s", tok_or(rc, "Save failed"));
          break;
      case PNL_CTX_WIFI_JOIN:
      case PNL_CTX_WIFI_AP:
      case PNL_CTX_SCAN:
      case PNL_CTX_TZ:
          if (rc == PSVC_OK) {
              snprintf(out, cap, "%s", ctx == PNL_CTX_WIFI_JOIN ? "Saved -- joining..." :
                                       ctx == PNL_CTX_WIFI_AP   ? "Saved." :
                                       ctx == PNL_CTX_SCAN      ? "Scan done." : "Time zone saved.");
          } else if (rc == PSVC_E_BUSY) {
              snprintf(out, cap, MCFG_BUSY);
          } else {
              snprintf(out, cap, "%s", tok_or(rc, ctx == PNL_CTX_SCAN ? "Scan failed" : "Failed"));
          }
          break;
      case PNL_CTX_PASSWORD:
          if (rc == PSVC_OK) snprintf(out, cap, "Password changed. Every phone and browser was logged out -- log in again with the new password.");
          else if (rc == PSVC_E_INVALID) snprintf(out, cap, "New password invalid (8 to 63 characters)");
          else snprintf(out, cap, "Failed (%s)", tok_or(rc, "INTERNAL"));
          break;
      case PNL_CTX_FLEET_ZONE:
          if (rc == PSVC_OK) snprintf(out, cap, "Update queued for zone %d.", a->zone);
          else snprintf(out, cap, "%s", tok_or(rc, "Failed"));
          break;
      case PNL_CTX_FLEET_ALL:
          if (rc == PSVC_OK) snprintf(out, cap, "Fleet update queued.");
          else snprintf(out, cap, "%s", tok_or(rc, "Failed"));
          break;
      case PNL_CTX_FLEET_ABORT:
          if (rc == PSVC_OK) snprintf(out, cap, "Fleet update aborted.");
          else snprintf(out, cap, "%s", tok_or(rc, "Failed"));
          break;
      case PNL_CTX_FW_MASTER:
          if (rc == PSVC_OK) snprintf(out, cap, "Uploaded v%s to %s.", ver, slot);
          else fw_fail(rc, "master", out, cap);
          break;
      case PNL_CTX_FW_ZONE:
          if (rc == PSVC_OK) snprintf(out, cap, "Uploaded (%lu bytes).", (unsigned long)a->len);
          else fw_fail(rc, "zone", out, cap);
          break;
      default:
          snprintf(out, cap, "%s", tok_or(rc, "Failed"));
          break;
      }
      if (!out[0]) snprintf(out, cap, "Failed");
      for (char *p = out; *p; p++)                      /* the ASCII rule holds even for foreign version strings */
          if ((unsigned char)*p < 0x20 || (unsigned char)*p > 0x7E) *p = '?';
      return (int)strlen(out);
  }
  ```

- [ ] **Step 6: Run both and watch them pass**

  Run the single host test with `<name>` = `test_pcfg_edit`. Expected: `11 Tests 0 Failures 0 Ignored`.
  Run it with `<name>` = `test_pnl_msg`. Expected: `8 Tests 0 Failures 0 Ignored`.

- [ ] **Step 7: Compile both on the P4**

  In `components/panel_ui/CMakeLists.txt`, inside the gating `if()` block, add:
  ```cmake
      list(APPEND PANEL_SRCS "pcfg_edit.c" "pnl_msg.c")       # Task 18: edit model, refusal texts (pure)
  ```

- [ ] **Step 8: GATE-HOST, GATE-P4, GATE-ESP32**

  Run GATE-HOST. Expected: `100% tests passed, 0 tests failed out of 46` (+2).
  Run GATE-P4. Expected: `Project build complete`, no `warning:` lines, target matched, lock restored.
  Run GATE-ESP32 (the `panel_ui` CMake is evaluated for every app). Expected: three `Project build complete` and
  an empty lock diff.

- [ ] **Step 9: Commit and push**

  ```powershell
  git -C C:\Projects\HillGrov add components/panel_ui/pcfg_edit.h components/panel_ui/pcfg_edit.c components/panel_ui/pnl_msg.h components/panel_ui/pnl_msg.c components/panel_ui/CMakeLists.txt tests/host/test_pcfg_edit.c tests/host/test_pnl_msg.c tests/host/CMakeLists.txt
  git -C C:\Projects\HillGrov status --short
  ```
  Expected: those paths staged; ` M docs/hillgrow-features.drawio` unstaged.
  ```powershell
  git -C C:\Projects\HillGrov commit -m "feat(panel_ui): config edit model and shared refusal texts" -m "Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>"
  git -C C:\Projects\HillGrov push
  ```

---

### Task 19: Field widgets, keyboard overlay, input filters (Stage 2)

**Files:**
- Create: `components/panel_ui/pnl_input.h`, `components/panel_ui/pnl_input.c` (P)
- Create: `components/panel_ui/wdg_field.h`, `components/panel_ui/wdg_field.c` (G)
- Create: `components/panel_ui/wdg_keyboard.h`, `components/panel_ui/wdg_keyboard.c` (G)
- Create: `tests/host/test_pnl_input.c`
- Modify: `components/panel_ui/CMakeLists.txt` (append three sources)
- Modify: `tests/host/CMakeLists.txt` (append one `hg_test` row)

**Interfaces:**
- Consumes: from Task 17, `pcfg_spec_t`, `pcfg_kind_t`, `pcfg_kb_t`, `pcfg_format`, `pcfg_parse_raw`,
  `pcfg_raw_text` and `pcfg_step`. From Task 3, `PSVC_FEDIT_TEXT_MAX`. From Task 7, the `PNL_C_*` palette
  (`pnl_palette.h`). LVGL 9.5.0 widgets: `lv_button`, `lv_label`, `lv_switch`, `lv_buttonmatrix`, `lv_roller`,
  `lv_textarea`, `lv_keyboard` and `lv_timer`. Every LVGL name used below was checked against an LVGL 9.5.0 source
  tree, the managed copy in `C:\Projects\HillAug\managed_components\lvgl__lvgl`. The 9.x renames are already
  applied: `LV_LABEL_LONG_MODE_WRAP`, `lv_obj_remove_flag` and `lv_obj_remove_state`.
- Produces (three names marked ADDITION: pnl_zero, wdg_reveal_fn, wdg_field_set_reveal):
  ```c
  /* pnl_input.h (pure) */
  static inline void pnl_zero(void *p, size_t n);   /* ADDITION: the panel's one secret wipe (volatile stores, never
                                                       removed as dead stores at -Os); every panel_ui file uses it */
  int pnl_kb_accepts(pcfg_kb_t kb, char c);   /* NUMERIC 0-9; TEXT 0x20..0x7E; TEXT_NOSPACE 0x21..0x7E; HOSTNAME a-z 0-9 '-';
                                                 HEX 0-9 a-f A-F ':' */
  int pnl_text_ok(pcfg_kb_t kb, const char *s, uint8_t min_len, uint8_t max_len);   /* 1 / 0 */
  int pnl_mac_parse(const char *s, uint8_t mac[6]);   /* app.js:1688 ^([0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}$ -- 0 / -1 */

  /* wdg_keyboard.h (glue) */
  typedef void (*wdg_kb_done_fn)(void *ctx, int accepted, const char *text);
  void wdg_keyboard_open(const char *title, pcfg_kb_t kb, const char *initial, uint8_t min_len, uint8_t max_len,
                         int masked, wdg_kb_done_fn done, void *ctx);
  void wdg_keyboard_close(void);
  int  wdg_keyboard_is_open(void);

  /* wdg_field.h (glue) */
  typedef void (*wdg_changed_fn)(void *ctx, uint8_t group, int idx, const hg_field_t *f, const char *raw_text);
  lv_obj_t *wdg_field_create(lv_obj_t *parent, const pcfg_spec_t *s, uint8_t group, int idx, const hg_field_t *f,
                             const char *raw_text, wdg_changed_fn cb, void *ctx);
  void wdg_field_set_error(lv_obj_t *row, const char *code);
  void wdg_field_set_dirty(lv_obj_t *row, int dirty);
  void wdg_field_set_secret_text(lv_obj_t *row, const char *plain);
  /* ADDITION: the Reveal button's value source (a SECRET row never holds the stored secret itself) */
  typedef int (*wdg_reveal_fn)(void *ctx, const hg_field_t *f, char *out, size_t cap);   /* 0 filled / -1 */
  void wdg_field_set_reveal(lv_obj_t *row, wdg_reveal_fn fn, void *ctx);                  /* shows [Reveal] when fn != NULL */
  ```
  Contracts:
  - `wdg_keyboard_close()` never calls `done`. It closes, wipes the textarea (the plain password buffer in password
    mode, and the label), and deletes the overlay asynchronously. Every owner that deletes field rows or tears a
    screen down closes the keyboard first. A row's own `LV_EVENT_DELETE` does it too, as a backstop.
  - `done(ctx, 1, text)`: `text` is valid only during the call and is zeroed after it. `done(ctx, 0, "")` means
    cancel.
  - A SECRET row ignores `raw_text`: the stored secret never enters the row. [Change] opens a masked keyboard
    that starts empty; an empty accept means "unchanged" and calls nothing. A non-empty accept calls `cb` with the
    new text, then zeroes the row's copy. [Reveal] asks `wdg_reveal_fn` for the value, shows it for 10 s
    (`wdg_field_set_secret_text`), then re-masks. It also re-masks when the row is deleted, which covers teardown
    and the idle wipe; the label's buffer is zeroed in place before re-masking.
  - Every row callback runs on the LVGL task and stays well under the 200 ms budget: a row is a handful of objects.

**What each check proves:** `pnl_input` is pure and host-tested. `wdg_field` and `wdg_keyboard` are LVGL widget
trees that the host cannot reach. GATE-P4 proves they compile warning-free against LVGL 9.5.0 and link into the
image. The first check on glass is Task 20's editor, and the Stage 2 gate (steps 1-4 and 7) proves behaviour under
a real finger: stepper bounds, a keyboard that refuses space, masked secrets and the 10 s re-mask.

- [ ] **Step 1: Write the failing input-filter test**

  Append to `tests/host/CMakeLists.txt`:
  ```cmake
  # panel_ui's keyboard filters and MAC parser (Task 19): pure.
  hg_test(test_pnl_input ${COMP}/panel_ui/pnl_input.c)
  ```
  Create `tests/host/test_pnl_input.c`:
  ```c
  /* test_pnl_input.c -- what each keyboard class lets through, text bounds, and the web's MAC rule. */
  #include <string.h>
  #include "unity.h"
  #include "pnl_input.h"

  void setUp(void) {}
  void tearDown(void) {}

  static void test_class_boundaries(void) {
      TEST_ASSERT_EQUAL_INT(1, pnl_kb_accepts(PCFG_KB_TEXT, ' '));
      TEST_ASSERT_EQUAL_INT(0, pnl_kb_accepts(PCFG_KB_TEXT_NOSPACE, ' '));
      TEST_ASSERT_EQUAL_INT(1, pnl_kb_accepts(PCFG_KB_TEXT, '~'));
      TEST_ASSERT_EQUAL_INT(1, pnl_kb_accepts(PCFG_KB_TEXT_NOSPACE, '~'));
      TEST_ASSERT_EQUAL_INT(1, pnl_kb_accepts(PCFG_KB_TEXT_NOSPACE, '!'));
      TEST_ASSERT_EQUAL_INT(0, pnl_kb_accepts(PCFG_KB_TEXT, 0x7F));
      TEST_ASSERT_EQUAL_INT(0, pnl_kb_accepts(PCFG_KB_TEXT, 0x1F));
      TEST_ASSERT_EQUAL_INT(0, pnl_kb_accepts(PCFG_KB_TEXT, '\n'));
      TEST_ASSERT_EQUAL_INT(0, pnl_kb_accepts(PCFG_KB_TEXT, (char)0xC3));   /* UTF-8 lead byte */
      TEST_ASSERT_EQUAL_INT(1, pnl_kb_accepts(PCFG_KB_NUMERIC, '0'));
      TEST_ASSERT_EQUAL_INT(1, pnl_kb_accepts(PCFG_KB_NUMERIC, '9'));
      TEST_ASSERT_EQUAL_INT(0, pnl_kb_accepts(PCFG_KB_NUMERIC, '-'));
      TEST_ASSERT_EQUAL_INT(0, pnl_kb_accepts(PCFG_KB_NUMERIC, '.'));
      TEST_ASSERT_EQUAL_INT(0, pnl_kb_accepts(PCFG_KB_NUMERIC, '+'));
  }
  static void test_hostname_class(void) {
      TEST_ASSERT_EQUAL_INT(1, pnl_kb_accepts(PCFG_KB_HOSTNAME, 'a'));
      TEST_ASSERT_EQUAL_INT(1, pnl_kb_accepts(PCFG_KB_HOSTNAME, 'z'));
      TEST_ASSERT_EQUAL_INT(1, pnl_kb_accepts(PCFG_KB_HOSTNAME, '0'));
      TEST_ASSERT_EQUAL_INT(1, pnl_kb_accepts(PCFG_KB_HOSTNAME, '-'));
      TEST_ASSERT_EQUAL_INT(0, pnl_kb_accepts(PCFG_KB_HOSTNAME, 'A'));   /* upper case refused (hg_mcfg.c:74-78) */
      TEST_ASSERT_EQUAL_INT(0, pnl_kb_accepts(PCFG_KB_HOSTNAME, '_'));
      TEST_ASSERT_EQUAL_INT(0, pnl_kb_accepts(PCFG_KB_HOSTNAME, '.'));
  }
  static void test_hex_class_and_none(void) {
      TEST_ASSERT_EQUAL_INT(1, pnl_kb_accepts(PCFG_KB_HEX, 'f'));
      TEST_ASSERT_EQUAL_INT(1, pnl_kb_accepts(PCFG_KB_HEX, 'F'));
      TEST_ASSERT_EQUAL_INT(1, pnl_kb_accepts(PCFG_KB_HEX, ':'));
      TEST_ASSERT_EQUAL_INT(0, pnl_kb_accepts(PCFG_KB_HEX, 'g'));
      TEST_ASSERT_EQUAL_INT(0, pnl_kb_accepts(PCFG_KB_HEX, '-'));
      TEST_ASSERT_EQUAL_INT(0, pnl_kb_accepts(PCFG_KB_NONE, 'a'));
  }
  static void test_text_bounds(void) {
      TEST_ASSERT_EQUAL_INT(0, pnl_text_ok(PCFG_KB_TEXT_NOSPACE, "", 1, 15));
      TEST_ASSERT_EQUAL_INT(1, pnl_text_ok(PCFG_KB_TEXT_NOSPACE, "", 0, 15));
      TEST_ASSERT_EQUAL_INT(1, pnl_text_ok(PCFG_KB_TEXT_NOSPACE, "123456789012345", 1, 15));
      TEST_ASSERT_EQUAL_INT(0, pnl_text_ok(PCFG_KB_TEXT_NOSPACE, "1234567890123456", 1, 15));
      TEST_ASSERT_EQUAL_INT(0, pnl_text_ok(PCFG_KB_TEXT_NOSPACE, "a b", 1, 15));
      TEST_ASSERT_EQUAL_INT(1, pnl_text_ok(PCFG_KB_TEXT, "a b", 1, 15));
      TEST_ASSERT_EQUAL_INT(0, pnl_text_ok(PCFG_KB_HOSTNAME, "Green", 1, 23));
      TEST_ASSERT_EQUAL_INT(1, pnl_text_ok(PCFG_KB_HOSTNAME, "green-1", 1, 23));
      TEST_ASSERT_EQUAL_INT(0, pnl_text_ok(PCFG_KB_TEXT, NULL, 0, 15));
  }
  static void test_mac(void) {
      uint8_t m[6] = { 0 };
      static const uint8_t want[6] = { 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x0F };
      TEST_ASSERT_EQUAL_INT(0, pnl_mac_parse("aa:bb:cc:dd:ee:0f", m));
      TEST_ASSERT_EQUAL_UINT8_ARRAY(want, m, 6);
      TEST_ASSERT_EQUAL_INT(0, pnl_mac_parse("AA:BB:CC:DD:EE:0F", m));
      TEST_ASSERT_EQUAL_UINT8_ARRAY(want, m, 6);
      memset(m, 0x11, sizeof m);
      TEST_ASSERT_EQUAL_INT(-1, pnl_mac_parse("aa:bb:cc:dd:ee", m));          /* short */
      TEST_ASSERT_EQUAL_INT(-1, pnl_mac_parse("aa:bb:cc:dd:ee:ff:", m));      /* trailing colon */
      TEST_ASSERT_EQUAL_INT(-1, pnl_mac_parse("aa-bb-cc-dd-ee-ff", m));
      TEST_ASSERT_EQUAL_INT(-1, pnl_mac_parse("aa:bb:cc:dd:ee:fg", m));
      TEST_ASSERT_EQUAL_INT(-1, pnl_mac_parse("a:bb:cc:dd:ee:ff0", m));
      TEST_ASSERT_EQUAL_INT(-1, pnl_mac_parse(NULL, m));
      TEST_ASSERT_EQUAL_UINT8(0x11, m[0]);                                     /* untouched on failure */
  }

  int main(void) {
      UNITY_BEGIN();
      RUN_TEST(test_class_boundaries);
      RUN_TEST(test_hostname_class);
      RUN_TEST(test_hex_class_and_none);
      RUN_TEST(test_text_bounds);
      RUN_TEST(test_mac);
      return UNITY_END();
  }
  ```

- [ ] **Step 2: Run it and watch it fail**

  Run the single host test with `<name>` = `test_pnl_input`.
  Expected: configure fails with `Cannot find source file:` naming `.../components/panel_ui/pnl_input.c`.

- [ ] **Step 3: Create `components/panel_ui/pnl_input.h` and `components/panel_ui/pnl_input.c`**

  `pnl_input.h`:
  ```c
  #pragma once
  /* pnl_input.h -- keyboard character classes, text bounds and the MAC rule (pure). */
  #include <stddef.h>
  #include <stdint.h>
  #include "pcfg_gen.h"
  #ifdef __cplusplus
  extern "C" {
  #endif

  /* The secret wipe for every panel_ui file (Global Constraints, "Secrets"). The master builds with -Os
   * (CONFIG_COMPILER_OPTIMIZATION_SIZE=y): GCC drops a plain memset on a local that is never read again, even one whose
   * address was passed out earlier. Volatile stores cannot be dropped. */
  static inline void pnl_zero(void *p, size_t n) { volatile uint8_t *v = (volatile uint8_t *)p; while (n--) *v++ = 0; }

  int pnl_kb_accepts(pcfg_kb_t kb, char c);   /* NUMERIC 0-9; TEXT 0x20..0x7E; TEXT_NOSPACE 0x21..0x7E; HOSTNAME a-z 0-9 '-';
                                                 HEX 0-9 a-f A-F ':'; NONE nothing */
  int pnl_text_ok(pcfg_kb_t kb, const char *s, uint8_t min_len, uint8_t max_len);   /* 1 / 0 */
  int pnl_mac_parse(const char *s, uint8_t mac[6]);   /* app.js:1688 ^([0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}$ -- 0 / -1 (mac untouched) */

  #ifdef __cplusplus
  }
  #endif
  ```
  `pnl_input.c`:
  ```c
  #include <string.h>
  #include "pnl_input.h"

  int pnl_kb_accepts(pcfg_kb_t kb, char c) {
      unsigned char u = (unsigned char)c;
      switch (kb) {
      case PCFG_KB_NUMERIC:      return u >= '0' && u <= '9';
      case PCFG_KB_TEXT:         return u >= 0x20 && u <= 0x7E;
      case PCFG_KB_TEXT_NOSPACE: return u >= 0x21 && u <= 0x7E;
      case PCFG_KB_HOSTNAME:     return (u >= 'a' && u <= 'z') || (u >= '0' && u <= '9') || u == '-';
      case PCFG_KB_HEX:          return (u >= '0' && u <= '9') || (u >= 'a' && u <= 'f') || (u >= 'A' && u <= 'F') || u == ':';
      default:                   return 0;
      }
  }

  int pnl_text_ok(pcfg_kb_t kb, const char *s, uint8_t min_len, uint8_t max_len) {
      if (!s) return 0;
      size_t n = strlen(s);
      if (n < (size_t)min_len || n > (size_t)max_len) return 0;
      for (size_t i = 0; i < n; i++) if (!pnl_kb_accepts(kb, s[i])) return 0;
      return 1;
  }

  static int hexv(char c) {
      if (c >= '0' && c <= '9') return c - '0';
      if (c >= 'a' && c <= 'f') return c - 'a' + 10;
      if (c >= 'A' && c <= 'F') return c - 'A' + 10;
      return -1;
  }

  int pnl_mac_parse(const char *s, uint8_t mac[6]) {
      uint8_t out[6];
      if (!s || !mac || strlen(s) != 17) return -1;
      for (int i = 0; i < 6; i++) {
          int hi = hexv(s[3 * i]), lo = hexv(s[3 * i + 1]);
          if (hi < 0 || lo < 0) return -1;
          if (i < 5 && s[3 * i + 2] != ':') return -1;
          out[i] = (uint8_t)((hi << 4) | lo);
      }
      memcpy(mac, out, 6);
      return 0;
  }
  ```

- [ ] **Step 4: Run it and watch it pass**

  Run the single host test with `<name>` = `test_pnl_input`. Expected: `5 Tests 0 Failures 0 Ignored`.

- [ ] **Step 5: Create `components/panel_ui/wdg_keyboard.h` and `components/panel_ui/wdg_keyboard.c`**

  `wdg_keyboard.h`:
  ```c
  #pragma once
  /* wdg_keyboard.h -- the one modal keyboard / keypad (glue; LVGL task only). Filters every key with
   * pnl_kb_accepts; OK stays disabled until pnl_text_ok; masked = password mode with an eye toggle. */
  #include <stdint.h>
  #include "pcfg_gen.h"
  #ifdef __cplusplus
  extern "C" {
  #endif

  typedef void (*wdg_kb_done_fn)(void *ctx, int accepted, const char *text);   /* text valid during the call only */
  void wdg_keyboard_open(const char *title, pcfg_kb_t kb, const char *initial, uint8_t min_len, uint8_t max_len,
                         int masked, wdg_kb_done_fn done, void *ctx);  /* modal on lv_layer_top(); an open keyboard is
                                                                          closed (without done) first */
  void wdg_keyboard_close(void);   /* wipe + close WITHOUT calling done (teardown, idle wipe, row deletion) */
  int  wdg_keyboard_is_open(void);

  #ifdef __cplusplus
  }
  #endif
  ```
  `wdg_keyboard.c`:
  ```c
  /* wdg_keyboard.c -- modal keyboard overlay (glue; LVGL task only). */
  #include <stdio.h>
  #include <string.h>
  #include "lvgl.h"
  #include "wdg_keyboard.h"
  #include "pnl_input.h"
  #include "pnl_palette.h"

  #define KB_TEXT_CAP 194  /* >= CMD_LINE_MAX + 2: the zone console opens with max_len CMD_LINE_MAX (192) so a 192nd
                              character is accepted here and refused VISIBLY by the console ("Line too long"); every other
                              caller asks for far less (63-character passwords at most) and keeps its own max_len */

  static struct {
      lv_obj_t      *overlay, *ta, *kb, *ok, *hint, *eye_lbl;
      pcfg_kb_t      kind;
      uint8_t        min_len, max_len, masked;
      wdg_kb_done_fn done;
      void          *ctx;
  } s_kb;

  static const char *class_hint(pcfg_kb_t kb) {
      switch (kb) {
      case PCFG_KB_NUMERIC:      return "digits only";
      case PCFG_KB_TEXT_NOSPACE: return "no spaces";
      case PCFG_KB_HOSTNAME:     return "a-z, 0-9 and - only";
      case PCFG_KB_HEX:          return "hex digits and : only";
      default:                   return "printable characters";
      }
  }

  /* LVGL frees without zeroing: overwrite the textarea's own buffers in place first. In password mode
   * lv_textarea_get_text() returns the plain-text buffer (pwd_tmp), not the bullets (LVGL 9.5.0). */
  static void ta_wipe(lv_obj_t *ta) {
      char *t = (char *)lv_textarea_get_text(ta);
      if (t) pnl_zero(t, strlen(t));
      char *l = lv_label_get_text(lv_textarea_get_label(ta));
      if (l) pnl_zero(l, strlen(l));
      lv_textarea_set_text(ta, "");
  }

  static int text_valid(void) {
      return pnl_text_ok(s_kb.kind, lv_textarea_get_text(s_kb.ta), s_kb.min_len, s_kb.max_len);
  }
  static void refresh_ok(void) {
      int ok = text_valid();
      lv_obj_set_state(s_kb.ok, LV_STATE_DISABLED, !ok);
      lv_obj_set_style_text_color(s_kb.hint, lv_color_hex(PNL_C_MUTED), 0);
  }

  static void finish(int accepted) {
      if (!s_kb.overlay) return;
      wdg_kb_done_fn done = s_kb.done;
      void *ctx = s_kb.ctx;
      char text[KB_TEXT_CAP];
      snprintf(text, sizeof text, "%s", accepted ? lv_textarea_get_text(s_kb.ta) : "");
      wdg_keyboard_close();
      if (done) done(ctx, accepted, text);
      pnl_zero(text, sizeof text);
  }

  static void ev_ta(lv_event_t *e) {
      lv_event_code_t c = lv_event_get_code(e);
      if (c == LV_EVENT_INSERT) {
          const char *ins = lv_event_get_param(e);
          for (const char *p = ins; p && *p; p++)
              if (!pnl_kb_accepts(s_kb.kind, *p)) { lv_textarea_set_insert_replace(s_kb.ta, ""); return; }
      } else if (c == LV_EVENT_VALUE_CHANGED) {
          refresh_ok();
      } else if (c == LV_EVENT_READY) {            /* the keyboard's OK key, or Enter in one-line mode */
          if (text_valid()) finish(1);
          else lv_obj_set_style_text_color(s_kb.hint, lv_color_hex(PNL_C_OFFLINE_TEXT), 0);
      } else if (c == LV_EVENT_CANCEL) {           /* the keyboard's close key */
          finish(0);
      }
  }
  static void ev_ok(lv_event_t *e)     { (void)e; if (text_valid()) finish(1); }
  static void ev_cancel(lv_event_t *e) { (void)e; finish(0); }
  static void ev_eye(lv_event_t *e) {
      (void)e;
      int show = lv_textarea_get_password_mode(s_kb.ta);
      lv_textarea_set_password_mode(s_kb.ta, !show);
      lv_label_set_text(s_kb.eye_lbl, show ? LV_SYMBOL_EYE_CLOSE : LV_SYMBOL_EYE_OPEN);
  }

  static lv_obj_t *kb_button(lv_obj_t *parent, const char *text, lv_event_cb_t cb, lv_obj_t **label_out) {
      lv_obj_t *b = lv_button_create(parent);
      lv_obj_set_height(b, 56);
      lv_obj_set_style_min_width(b, 96, 0);
      lv_obj_t *l = lv_label_create(b);
      lv_label_set_text(l, text);
      lv_obj_center(l);
      lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
      if (label_out) *label_out = l;
      return b;
  }

  void wdg_keyboard_open(const char *title, pcfg_kb_t kb, const char *initial, uint8_t min_len, uint8_t max_len,
                         int masked, wdg_kb_done_fn done, void *ctx) {
      if (s_kb.overlay) wdg_keyboard_close();
      if (max_len == 0 || max_len >= KB_TEXT_CAP) max_len = KB_TEXT_CAP - 1;
      s_kb.kind = kb; s_kb.min_len = min_len; s_kb.max_len = max_len;
      s_kb.masked = masked ? 1 : 0; s_kb.done = done; s_kb.ctx = ctx;

      s_kb.overlay = lv_obj_create(lv_layer_top());
      lv_obj_remove_style_all(s_kb.overlay);
      lv_obj_set_size(s_kb.overlay, lv_pct(100), lv_pct(100));
      lv_obj_set_style_bg_color(s_kb.overlay, lv_color_hex(PNL_C_BG), 0);
      lv_obj_set_style_bg_opa(s_kb.overlay, LV_OPA_90, 0);
      lv_obj_add_flag(s_kb.overlay, LV_OBJ_FLAG_CLICKABLE);      /* modal: nothing reaches the screen below */

      lv_obj_t *card = lv_obj_create(s_kb.overlay);
      lv_obj_set_size(card, lv_pct(100), LV_SIZE_CONTENT);
      lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 0);
      lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
      lv_obj_set_style_pad_row(card, 8, 0);
      lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

      lv_obj_t *t = lv_label_create(card);
      lv_obj_set_style_text_font(t, &lv_font_montserrat_28, 0);
      lv_label_set_text(t, title ? title : "");

      lv_obj_t *row = lv_obj_create(card);
      lv_obj_remove_style_all(row);
      lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
      lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
      lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
      lv_obj_set_style_pad_column(row, 8, 0);

      s_kb.ta = lv_textarea_create(row);
      lv_obj_set_flex_grow(s_kb.ta, 1);
      lv_textarea_set_one_line(s_kb.ta, true);
      lv_textarea_set_max_length(s_kb.ta, max_len);
      lv_textarea_set_password_mode(s_kb.ta, s_kb.masked);
      lv_textarea_set_text(s_kb.ta, initial ? initial : "");
      lv_obj_add_event_cb(s_kb.ta, ev_ta, LV_EVENT_ALL, NULL);
      if (s_kb.masked) kb_button(row, LV_SYMBOL_EYE_OPEN, ev_eye, &s_kb.eye_lbl);
      kb_button(row, "Cancel", ev_cancel, NULL);
      s_kb.ok = kb_button(row, "OK", ev_ok, NULL);

      s_kb.hint = lv_label_create(card);
      char h[96];
      snprintf(h, sizeof h, "%u to %u characters, %s", (unsigned)min_len, (unsigned)max_len, class_hint(kb));
      lv_label_set_text(s_kb.hint, h);

      s_kb.kb = lv_keyboard_create(s_kb.overlay);
      lv_obj_set_size(s_kb.kb, lv_pct(100), lv_pct(52));
      lv_obj_align(s_kb.kb, LV_ALIGN_BOTTOM_MID, 0, 0);
      lv_keyboard_set_mode(s_kb.kb, kb == PCFG_KB_NUMERIC ? LV_KEYBOARD_MODE_NUMBER : LV_KEYBOARD_MODE_TEXT_LOWER);
      lv_keyboard_set_textarea(s_kb.kb, s_kb.ta);
      refresh_ok();
  }

  void wdg_keyboard_close(void) {
      if (!s_kb.overlay) return;
      ta_wipe(s_kb.ta);
      lv_obj_add_flag(s_kb.overlay, LV_OBJ_FLAG_HIDDEN);
      lv_obj_delete_async(s_kb.overlay);           /* safe from inside the keyboard's own event */
      memset(&s_kb, 0, sizeof s_kb);
  }

  int wdg_keyboard_is_open(void) { return s_kb.overlay != NULL; }
  ```
  The hint buffer is sized for `-Wformat-truncation`: "255 to 255 characters, " is at most 24 characters, and the
  class text is a pointer, so GCC assumes it is short.

- [ ] **Step 6: Create `components/panel_ui/wdg_field.h`**

  ```c
  #pragma once
  /* wdg_field.h -- one config row per pcfg_kind_t (glue; LVGL task only). The row owns a copy of its spec and
   * the current raw text (hg_field_write form); it reports every change through wdg_changed_fn. */
  #include <stddef.h>
  #include <stdint.h>
  #include "lvgl.h"
  #include "hg_cfg.h"
  #include "pcfg_gen.h"
  #ifdef __cplusplus
  extern "C" {
  #endif

  typedef void (*wdg_changed_fn)(void *ctx, uint8_t group, int idx, const hg_field_t *f, const char *raw_text);
  lv_obj_t *wdg_field_create(lv_obj_t *parent, const pcfg_spec_t *s, uint8_t group, int idx, const hg_field_t *f,
                             const char *raw_text, wdg_changed_fn cb, void *ctx);
      /* one row: label + unit + editor by kind:
         STEPPER [-] value [+] with [--]/[++] when big_step, long-press repeat, tap value -> NUMERIC keypad (not for scaled rows);
         SWITCH lv_switch; SEGMENTED lv_buttonmatrix one-checked; ROLLER two-column HH/MM, or enum / pin roller;
         TEXT value -> keyboard; SECRET "********" (fixed 8) + [Reveal] (10 s) + [Change] (keyboard starts empty; blank = unchanged);
         READONLY formatted value, muted (the owner shows "hardware plane -- set at the zone console" once per group).
         NULL on bad args or no memory. */
  void wdg_field_set_error(lv_obj_t *row, const char *code);   /* outline PNL_C_OFFLINE + code text; NULL clears */
  void wdg_field_set_dirty(lv_obj_t *row, int dirty);          /* "* " marker (U+2022) before the label */
  void wdg_field_set_secret_text(lv_obj_t *row, const char *plain);   /* the revealed value (NULL re-masks and wipes) */
  typedef int (*wdg_reveal_fn)(void *ctx, const hg_field_t *f, char *out, size_t cap);   /* 0 filled / -1 */
  void wdg_field_set_reveal(lv_obj_t *row, wdg_reveal_fn fn, void *ctx);                  /* SECRET rows: shows [Reveal] */

  #ifdef __cplusplus
  }
  #endif
  ```

- [ ] **Step 7: Create `components/panel_ui/wdg_field.c`**

  ```c
  /* wdg_field.c -- config rows by kind (glue; LVGL task only). Row state lives in PSRAM and is freed on LV_EVENT_DELETE. */
  #include <stdio.h>
  #include <stdlib.h>
  #include <string.h>
  #include "esp_heap_caps.h"
  #include "lvgl.h"
  #include "wdg_field.h"
  #include "wdg_keyboard.h"
  #include "pnl_input.h"      /* pnl_zero */
  #include "pnl_palette.h"
  #include "psvc_edit.h"

  #define REVEAL_MS 10000u
  #define MASK_TEXT "********"
  #define DOT       "\xE2\x80\xA2 "     /* U+2022, carried by the built-in Montserrat */

  typedef struct {
      pcfg_spec_t       spec;
      uint8_t           group;
      int               idx;
      const hg_field_t *f;
      wdg_changed_fn    cb;
      void             *ctx;
      wdg_reveal_fn     reveal;
      void             *reveal_ctx;
      char              raw[PSVC_FEDIT_TEXT_MAX];      /* hg_field_write text -- never a secret */
      lv_obj_t         *row, *name, *value, *err, *editor, *roll_b, *reveal_btn;
      lv_timer_t       *remask;
      uint8_t           revealed;
      const char       *map[PCFG_MAX_OPTS + 1];        /* SEGMENTED map: points into spec.opts */
  } wrow_t;

  static void wipe_label(lv_obj_t *l) { char *t = l ? lv_label_get_text(l) : NULL; if (t) pnl_zero(t, strlen(t)); }

  static lv_obj_t *box(lv_obj_t *parent, lv_flex_flow_t flow) {
      lv_obj_t *o = lv_obj_create(parent);
      lv_obj_remove_style_all(o);
      lv_obj_set_size(o, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
      lv_obj_set_flex_flow(o, flow);
      lv_obj_set_flex_align(o, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
      lv_obj_set_style_pad_column(o, 8, 0);
      lv_obj_set_style_pad_row(o, 2, 0);
      return o;
  }
  static lv_obj_t *button(lv_obj_t *parent, const char *text, lv_event_cb_t cb, void *ud) {
      lv_obj_t *b = lv_button_create(parent);
      lv_obj_set_height(b, 56);                        /* a finger, not a stylus */
      lv_obj_set_style_min_width(b, 64, 0);
      lv_obj_t *l = lv_label_create(b);
      lv_label_set_text(l, text);
      lv_obj_center(l);
      lv_obj_add_event_cb(b, cb, LV_EVENT_ALL, ud);
      return b;
  }
  static void button_text(lv_obj_t *b, const char *text) {
      if (b) lv_label_set_text(lv_obj_get_child(b, 0), text);
  }

  static void show_value(wrow_t *w) {
      char v[PSVC_FEDIT_TEXT_MAX + 8], t[PSVC_FEDIT_TEXT_MAX + 32];
      pcfg_format(&w->spec, w->raw, v, sizeof v);
      if (w->spec.kind == PCFG_K_TEXT && v[0] == '\0') snprintf(v, sizeof v, "(empty)");
      if (w->spec.unit && w->spec.unit[0]) snprintf(t, sizeof t, "%s %s", v, w->spec.unit);
      else snprintf(t, sizeof t, "%s", v);
      lv_label_set_text(w->value, t);
  }

  static void emit(wrow_t *w, const char *text) {
      wdg_field_set_error(w->row, NULL);
      wdg_field_set_dirty(w->row, 1);
      if (w->cb) w->cb(w->ctx, w->group, w->idx, w->f, text);
  }
  static void set_num(wrow_t *w, int32_t v) {
      if (pcfg_raw_text(&w->spec, v, w->raw, sizeof w->raw) != 0) return;
      if (w->value) show_value(w);
      emit(w, w->raw);
  }

  /* ---- STEPPER ---- */
  static void step(lv_event_t *e, int dir, int big) {
      lv_event_code_t c = lv_event_get_code(e);
      if (c != LV_EVENT_SHORT_CLICKED && c != LV_EVENT_LONG_PRESSED_REPEAT) return;
      wrow_t *w = lv_event_get_user_data(e);
      int32_t cur;
      if (pcfg_parse_raw(&w->spec, w->raw, &cur) != 0) cur = w->spec.min;
      int32_t nv = pcfg_step(&w->spec, cur, dir, big);
      if (nv != cur) set_num(w, nv);
  }
  static void ev_dec(lv_event_t *e)     { step(e, -1, 0); }
  static void ev_inc(lv_event_t *e)     { step(e, +1, 0); }
  static void ev_dec_big(lv_event_t *e) { step(e, -1, 1); }
  static void ev_inc_big(lv_event_t *e) { step(e, +1, 1); }
  static void keypad_done(void *ctx, int accepted, const char *text) {
      wrow_t *w = ctx;
      if (!accepted) return;
      char *end;
      long v = strtol(text, &end, 10);
      if (end == text || *end || v < w->spec.min || v > w->spec.max) {
          char m[48];
          snprintf(m, sizeof m, "Out of range (%ld..%ld)", (long)w->spec.min, (long)w->spec.max);
          wdg_field_set_error(w->row, m);
          return;
      }
      set_num(w, (int32_t)v);
  }
  static void ev_value_tap(lv_event_t *e) {
      if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
      wrow_t *w = lv_event_get_user_data(e);
      wdg_keyboard_open(w->spec.label, PCFG_KB_NUMERIC, w->raw, 1, w->spec.max_len, 0, keypad_done, w);
  }

  /* ---- SWITCH / SEGMENTED / ROLLERS ---- */
  static void ev_switch(lv_event_t *e) {
      if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
      wrow_t *w = lv_event_get_user_data(e);
      set_num(w, lv_obj_has_state(w->editor, LV_STATE_CHECKED) ? 1 : 0);
  }
  static void ev_seg(lv_event_t *e) {
      if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
      wrow_t *w = lv_event_get_user_data(e);
      uint32_t sel = lv_buttonmatrix_get_selected_button(w->editor);
      if (sel >= w->spec.n_opts) return;                     /* LV_BUTTONMATRIX_BUTTON_NONE */
      lv_buttonmatrix_set_button_ctrl(w->editor, sel, LV_BUTTONMATRIX_CTRL_CHECKED);   /* never leave none checked */
      set_num(w, (int32_t)sel);
  }
  static void ev_hhmm(lv_event_t *e) {
      if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
      wrow_t *w = lv_event_get_user_data(e);
      set_num(w, (int32_t)lv_roller_get_selected(w->editor) * 60 + (int32_t)lv_roller_get_selected(w->roll_b));
  }
  static void ev_roller(lv_event_t *e) {
      if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
      wrow_t *w = lv_event_get_user_data(e);
      uint32_t sel = lv_roller_get_selected(w->editor);
      if (w->spec.roller == PCFG_ROLL_PIN) set_num(w, sel == 0 ? w->spec.none_value : (int32_t)sel - 1 + w->spec.min);
      else set_num(w, (int32_t)sel);
  }
  static void put2(char **p, int v) { *(*p)++ = (char)('0' + v / 10); *(*p)++ = (char)('0' + v % 10); }
  static const char *two_digit_opts(int n) {         /* "00\n01\n..": 24 hours or 60 minutes */
      static char hh[24 * 3], mm[60 * 3];
      char *buf = (n == 24) ? hh : mm;
      if (!buf[0]) {
          char *p = buf;
          for (int i = 0; i < n; i++) { if (i) *p++ = '\n'; put2(&p, i); }
          *p = '\0';
      }
      return buf;
  }
  static lv_obj_t *roller(lv_obj_t *parent, const char *opts, uint32_t sel, int width, wrow_t *w, lv_event_cb_t cb) {
      lv_obj_t *r = lv_roller_create(parent);
      lv_roller_set_options(r, opts, LV_ROLLER_MODE_NORMAL);    /* copies opts */
      lv_roller_set_visible_row_count(r, 3);
      lv_obj_set_width(r, width);
      lv_roller_set_selected(r, sel, LV_ANIM_OFF);
      lv_obj_add_event_cb(r, cb, LV_EVENT_VALUE_CHANGED, w);
      return r;
  }

  /* ---- TEXT / SECRET ---- */
  static void text_done(void *ctx, int accepted, const char *text) {
      wrow_t *w = ctx;
      if (!accepted) return;
      snprintf(w->raw, sizeof w->raw, "%s", text);
      show_value(w);
      emit(w, w->raw);
  }
  static void ev_text(lv_event_t *e) {
      if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
      wrow_t *w = lv_event_get_user_data(e);
      wdg_keyboard_open(w->spec.label, w->spec.keyboard, w->raw, w->spec.min_len, w->spec.max_len, 0, text_done, w);
  }
  static void secret_done(void *ctx, int accepted, const char *text) {
      wrow_t *w = ctx;
      if (!accepted || !text[0]) return;                 /* blank = unchanged: nothing to record */
      char tmp[PSVC_FEDIT_TEXT_MAX];
      snprintf(tmp, sizeof tmp, "%s", text);
      if (w->revealed) wdg_field_set_secret_text(w->row, NULL);
      emit(w, tmp);
      pnl_zero(tmp, sizeof tmp);
  }
  static void ev_change(lv_event_t *e) {
      if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
      wrow_t *w = lv_event_get_user_data(e);
      wdg_keyboard_open(w->spec.label, w->spec.keyboard, "", w->spec.min_len, w->spec.max_len, 1, secret_done, w);
  }
  static void ev_reveal(lv_event_t *e) {
      if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
      wrow_t *w = lv_event_get_user_data(e);
      if (w->revealed) { wdg_field_set_secret_text(w->row, NULL); return; }
      if (!w->reveal) return;
      char plain[PSVC_FEDIT_TEXT_MAX];
      if (w->reveal(w->reveal_ctx, w->f, plain, sizeof plain) == 0) wdg_field_set_secret_text(w->row, plain);
      pnl_zero(plain, sizeof plain);
  }
  static void remask_cb(lv_timer_t *t) {
      wrow_t *w = lv_timer_get_user_data(t);
      w->remask = NULL;                                  /* repeat count 1: LVGL deletes the timer after this */
      wdg_field_set_secret_text(w->row, NULL);
  }

  static void ev_row_delete(lv_event_t *e) {
      wrow_t *w = lv_event_get_user_data(e);
      if (wdg_keyboard_is_open()) wdg_keyboard_close();  /* its done() would get a dead ctx */
      if (w->remask) lv_timer_delete(w->remask);
      if (w->revealed) wipe_label(w->value);             /* children are still alive during LV_EVENT_DELETE */
      pnl_zero(w, sizeof *w);
      heap_caps_free(w);
  }

  lv_obj_t *wdg_field_create(lv_obj_t *parent, const pcfg_spec_t *s, uint8_t group, int idx, const hg_field_t *f,
                             const char *raw_text, wdg_changed_fn cb, void *ctx) {
      if (!parent || !s || !f) return NULL;
      wrow_t *w = heap_caps_calloc(1, sizeof *w, MALLOC_CAP_SPIRAM);
      if (!w) return NULL;
      w->spec = *s; w->group = group; w->idx = idx; w->f = f; w->cb = cb; w->ctx = ctx;
      if (s->kind != PCFG_K_SECRET && raw_text) snprintf(w->raw, sizeof w->raw, "%s", raw_text);

      lv_obj_t *row = lv_obj_create(parent);
      w->row = row;
      lv_obj_set_user_data(row, w);
      lv_obj_add_event_cb(row, ev_row_delete, LV_EVENT_DELETE, w);
      lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
      lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
      lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
      lv_obj_set_style_pad_all(row, 8, 0);
      lv_obj_set_style_radius(row, 0, 0);
      lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
      lv_obj_set_style_border_width(row, 1, 0);
      lv_obj_set_style_border_color(row, lv_color_hex(PNL_C_BORDER), 0);
      lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

      lv_obj_t *left = box(row, LV_FLEX_FLOW_COLUMN);
      lv_obj_set_flex_grow(left, 1);
      w->name = lv_label_create(left);
      lv_obj_set_width(w->name, lv_pct(100));
      lv_label_set_long_mode(w->name, LV_LABEL_LONG_MODE_WRAP);
      lv_label_set_text(w->name, s->label);
      w->err = lv_label_create(left);
      lv_obj_set_width(w->err, lv_pct(100));
      lv_label_set_long_mode(w->err, LV_LABEL_LONG_MODE_WRAP);
      lv_obj_set_style_text_color(w->err, lv_color_hex(PNL_C_OFFLINE_TEXT), 0);
      lv_obj_add_flag(w->err, LV_OBJ_FLAG_HIDDEN);

      lv_obj_t *right = box(row, LV_FLEX_FLOW_ROW);
      int32_t v = 0;
      int have = pcfg_parse_raw(s, w->raw, &v) == 0;
      switch (s->kind) {
      case PCFG_K_STEPPER:
          if (s->big_step > 0) button(right, LV_SYMBOL_MINUS LV_SYMBOL_MINUS, ev_dec_big, w);
          button(right, LV_SYMBOL_MINUS, ev_dec, w);
          w->value = lv_label_create(right);
          lv_obj_set_style_min_width(w->value, 120, 0);
          lv_obj_set_style_text_align(w->value, LV_TEXT_ALIGN_CENTER, 0);
          if (s->scale_div <= 1) {                       /* a keypad of raw x10 units would mislead */
              lv_obj_add_flag(w->value, LV_OBJ_FLAG_CLICKABLE);
              lv_obj_add_event_cb(w->value, ev_value_tap, LV_EVENT_CLICKED, w);
          }
          button(right, LV_SYMBOL_PLUS, ev_inc, w);
          if (s->big_step > 0) button(right, LV_SYMBOL_PLUS LV_SYMBOL_PLUS, ev_inc_big, w);
          show_value(w);
          break;
      case PCFG_K_SWITCH:
          w->editor = lv_switch_create(right);
          if (have && v) lv_obj_add_state(w->editor, LV_STATE_CHECKED);
          lv_obj_add_event_cb(w->editor, ev_switch, LV_EVENT_VALUE_CHANGED, w);
          break;
      case PCFG_K_SEGMENTED:
          for (int i = 0; i < s->n_opts; i++) w->map[i] = w->spec.opts[i];
          w->map[s->n_opts] = "";
          w->editor = lv_buttonmatrix_create(right);
          lv_buttonmatrix_set_map(w->editor, w->map);
          lv_buttonmatrix_set_button_ctrl_all(w->editor, LV_BUTTONMATRIX_CTRL_CHECKABLE);
          lv_buttonmatrix_set_one_checked(w->editor, true);
          lv_obj_set_size(w->editor, s->n_opts * 130, 56);
          if (have && v >= 0 && v < s->n_opts) lv_buttonmatrix_set_button_ctrl(w->editor, (uint32_t)v, LV_BUTTONMATRIX_CTRL_CHECKED);
          lv_obj_add_event_cb(w->editor, ev_seg, LV_EVENT_VALUE_CHANGED, w);
          break;
      case PCFG_K_ROLLER:
          if (s->roller == PCFG_ROLL_HHMM) {
              int m = have ? (int)v : 0;
              w->editor = roller(right, two_digit_opts(24), (uint32_t)(m / 60), 90, w, ev_hhmm);
              lv_label_set_text(lv_label_create(right), ":");
              w->roll_b = roller(right, two_digit_opts(60), (uint32_t)(m % 60), 90, w, ev_hhmm);
          } else {
              char opts[PCFG_MAX_OPTS * PCFG_OPT_LEN + 64];
              char *p = opts;
              uint32_t sel = 0;
              if (s->roller == PCFG_ROLL_PIN) {
                  p += snprintf(p, 8, "none");
                  for (int32_t pin = s->min; pin <= s->max && pin < 100; pin++) {
                      *p++ = '\n';
                      if (pin >= 10) put2(&p, (int)pin); else *p++ = (char)('0' + pin);
                  }
                  *p = '\0';
                  sel = (!have || v == s->none_value) ? 0 : (uint32_t)(v - s->min + 1);
              } else {
                  for (int i = 0; i < s->n_opts; i++) {
                      size_t l = strlen(s->opts[i]);
                      if (i) *p++ = '\n';
                      memcpy(p, s->opts[i], l);
                      p += l;
                  }
                  *p = '\0';
                  sel = have ? (uint32_t)v : 0;
              }
              w->editor = roller(right, opts, sel, 180, w, ev_roller);
          }
          break;
      case PCFG_K_TEXT:
          w->value = lv_label_create(right);
          lv_obj_add_flag(w->value, LV_OBJ_FLAG_CLICKABLE);
          lv_obj_add_event_cb(w->value, ev_text, LV_EVENT_CLICKED, w);
          show_value(w);
          button(right, LV_SYMBOL_EDIT, ev_text, w);
          break;
      case PCFG_K_SECRET:
          w->value = lv_label_create(right);
          lv_label_set_text(w->value, MASK_TEXT);
          w->reveal_btn = button(right, "Reveal", ev_reveal, w);
          lv_obj_add_flag(w->reveal_btn, LV_OBJ_FLAG_HIDDEN);   /* until wdg_field_set_reveal() */
          button(right, "Change", ev_change, w);
          break;
      case PCFG_K_READONLY:
      default:
          w->value = lv_label_create(right);
          lv_obj_set_style_text_color(w->value, lv_color_hex(PNL_C_MUTED), 0);
          show_value(w);
          break;
      }
      return row;
  }

  void wdg_field_set_error(lv_obj_t *row, const char *code) {
      wrow_t *w = row ? lv_obj_get_user_data(row) : NULL;
      if (!w) return;
      if (code && code[0]) {
          lv_obj_set_style_border_side(row, LV_BORDER_SIDE_FULL, 0);
          lv_obj_set_style_border_width(row, 2, 0);
          lv_obj_set_style_border_color(row, lv_color_hex(PNL_C_OFFLINE), 0);
          lv_label_set_text(w->err, code);
          lv_obj_remove_flag(w->err, LV_OBJ_FLAG_HIDDEN);
          lv_obj_scroll_to_view_recursive(row, LV_ANIM_ON);
      } else {
          lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
          lv_obj_set_style_border_width(row, 1, 0);
          lv_obj_set_style_border_color(row, lv_color_hex(PNL_C_BORDER), 0);
          lv_obj_add_flag(w->err, LV_OBJ_FLAG_HIDDEN);
      }
  }

  void wdg_field_set_dirty(lv_obj_t *row, int dirty) {
      wrow_t *w = row ? lv_obj_get_user_data(row) : NULL;
      if (!w) return;
      if (dirty) lv_label_set_text_fmt(w->name, DOT "%s", w->spec.label);
      else lv_label_set_text(w->name, w->spec.label);
      lv_obj_set_style_text_color(w->name, lv_color_hex(dirty ? PNL_C_OK_TEXT : PNL_C_TEXT), 0);
  }

  void wdg_field_set_secret_text(lv_obj_t *row, const char *plain) {
      wrow_t *w = row ? lv_obj_get_user_data(row) : NULL;
      if (!w || w->spec.kind != PCFG_K_SECRET) return;
      if (w->remask) { lv_timer_delete(w->remask); w->remask = NULL; }
      wipe_label(w->value);
      if (plain) {
          lv_label_set_text(w->value, plain[0] ? plain : "(empty)");
          w->revealed = 1;
          w->remask = lv_timer_create(remask_cb, REVEAL_MS, w);
          lv_timer_set_repeat_count(w->remask, 1);
          button_text(w->reveal_btn, "Hide");
      } else {
          lv_label_set_text(w->value, MASK_TEXT);
          w->revealed = 0;
          button_text(w->reveal_btn, "Reveal");
      }
  }

  void wdg_field_set_reveal(lv_obj_t *row, wdg_reveal_fn fn, void *ctx) {
      wrow_t *w = row ? lv_obj_get_user_data(row) : NULL;
      if (!w || w->spec.kind != PCFG_K_SECRET) return;
      w->reveal = fn;
      w->reveal_ctx = ctx;
      if (fn) lv_obj_remove_flag(w->reveal_btn, LV_OBJ_FLAG_HIDDEN);
      else lv_obj_add_flag(w->reveal_btn, LV_OBJ_FLAG_HIDDEN);
  }
  ```
  Notes for the implementer:
  - The buffers in `show_value` and the PIN roller are sized so that GCC's `-Wformat-truncation` can prove the
    maximum fits. `%s` of a pointer argument is assumed short by GCC; `%s` of an array uses its size.
  - `lv_label_set_text_fmt` is LVGL's own printf: its buffer is sized by LVGL, so there is no truncation concern.
  - The row state (`wrow_t`, about 400 B) is in PSRAM, not the 64 KB LVGL pool (Global Constraints, memory).

- [ ] **Step 8: Add the sources to `panel_ui`**

  In `components/panel_ui/CMakeLists.txt`, inside the gating `if()` block, add:
  ```cmake
      list(APPEND PANEL_SRCS "pnl_input.c" "wdg_field.c" "wdg_keyboard.c")   # Task 19: field widgets, keyboard
  ```
  `esp_heap_caps.h` comes from `heap`, which every IDF component can reach. `lvgl.h` comes from the
  `optional_requires` Task 6 added.

- [ ] **Step 9: GATE-HOST, GATE-P4, GATE-ESP32**

  Run GATE-HOST. Expected: `100% tests passed, 0 tests failed out of 47` (+1).
  Run GATE-P4. Expected: `Project build complete`, no `warning:` lines, target matched, lock restored. This is the
  proof that both widget files compile against LVGL 9.5.0. Nothing calls them yet, so there is no on-glass check
  until Task 20, and flashing this build is optional.
  Run GATE-ESP32. Expected: three `Project build complete` and an empty lock diff.

- [ ] **Step 10: Commit and push**

  ```powershell
  git -C C:\Projects\HillGrov add components/panel_ui/pnl_input.h components/panel_ui/pnl_input.c components/panel_ui/wdg_field.h components/panel_ui/wdg_field.c components/panel_ui/wdg_keyboard.h components/panel_ui/wdg_keyboard.c components/panel_ui/CMakeLists.txt tests/host/test_pnl_input.c tests/host/CMakeLists.txt
  git -C C:\Projects\HillGrov status --short
  ```
  Expected: those paths staged; ` M docs/hillgrow-features.drawio` unstaged.
  ```powershell
  git -C C:\Projects\HillGrov commit -m "feat(panel_ui): field widgets and filtered keyboard" -m "Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>"
  git -C C:\Projects\HillGrov push
  ```

---

### Task 20: Config editor — zones 1..8 (Stage 2)

**Files:**
- Create: `components/panel_ui/scr_config.h`, `components/panel_ui/scr_config.c` (G)
- Create: `components/panel_ui/cfg_zone.c` (G)
- Modify: `components/panel_ui/scr_shell.c` (the registry line for `PNL_DEST_CONFIG`)
- Modify: `components/panel_ui/CMakeLists.txt` (append two sources)

**Interfaces:**
- Consumes:
  - Task 5: `psvc_zone_cfg_get`, `psvc_zone_cfg_edit`, `psvc_zone_fields_fn`. `psvc_zone_fields_fn` is pure: it only
    calls `hg_field_base` and `hg_field_write`. So the LVGL task also calls it for the local pre-validation on a
    scratch copy, which gives exactly the same path shapes the worker's refusal would.
  - Tasks 17-19: `pcfg_spec_for`, `pcfg_tighten`, `pcfg_locate`, the `pcfg_edits_*` functions,
    `pnl_msg(PNL_CTX_ZONE_LOAD | PNL_CTX_ZONE_SAVE)`, the `wdg_field_*` functions and the `wdg_keyboard_*` functions.
  - Task 3: `psvc_rc_token`, `psvc_fedit_t`, `psvc_fedits_t`.
  - Task 8: `pnl_worker_submit`, `pnl_job_t`, `PNL_JOB_OUT_MAX`, `pnl_screen_gen`.
  - Task 10: `pnl_zone_name`. Task 11: `pnl_poll_latest`, `pnl_poll_kick`, `pnl_poll_seq`, `pnl_snap_t`
    (`st.node[]`, `st.cfg_sync_failed[]`, `cfg_busy[]`, `seq`).
  - Task 12: `pnl_screen_ops_t`, `PNL_SCR_CONFIG` (declared in `scr_shell.h`) and `pnl_label_set_if_changed`.
  - `hg_field_read`, `hg_field_base`, `hg_cfg_validate` (pure, `[ANY]`); `node_health_t` `NODE_H_ONLINE` and
    `hg_node_t` (`ring_proto.h:208-228`).
- Produces (ADDITION marks the names this task adds):
  ```c
  /* scr_config.h (glue) */
  typedef const char *(*cfg_value_fn)(void *ctx, uint8_t group, int idx, const hg_field_t *f, char *buf, size_t cap);
  typedef struct { pcfg_table_t table; uint8_t zone; cfg_value_fn value; void *ctx;
                   const hg_zone_hw_t *hw_or_null; pcfg_edits_t *edits;
                   wdg_reveal_fn reveal; } cfg_view_t;                  /* ADDITION: reveal (SECRET rows; NULL for zones) */
  int  cfg_render_group(lv_obj_t *list, const cfg_view_t *v, uint8_t group, int idx);  /* clears list, one wdg_field per row */
  void cfg_show_error(const cfg_view_t *v, const char *path, const char *code);
       /* pcfg_locate -> switch tab/index -> wdg_field_set_error; no row -> banner "<code>: <path>" */
  void cfg_set_status(const char *text, int is_error);
  void cfg_zone_open(lv_obj_t *body, uint8_t zone);
  void cfg_zone_close(void);
  void cfg_zone_update(const pnl_snap_t *s);
  void cfg_zone_wipe_all(void);   /* every zone's edit set + loaded doc (Task 27 calls it) */
  void cfg_master_open(lv_obj_t *body);
  void cfg_master_close(void);
  void cfg_master_wipe(void);     /* produced by Task 21 */
  /* ADDITION: the editor frame both editors share, and the title */
  typedef void (*cfg_save_fn)(void);
  lv_obj_t *cfg_frame_build(lv_obj_t *body, const cfg_view_t *v, cfg_save_fn on_save);
       /* tabs (the table's groups that have rows, in table order; ten zone tabs in two rows), shelf/aux index selector,
          Save, row list; renders the current tab; keeps tab + index across rebuilds of the same (table, zone) */
  void cfg_frame_rerender(void);           /* re-render the current tab/index from the view (after a reload or a save) */
  void cfg_frame_set_saving(int saving);   /* Save -> "Saving..." + disabled / back to "Save" */
  void cfg_set_title(const char *text);
  void cfg_master_update(const pnl_snap_t *s);   /* ADDITION, produced by Task 21 (save reconciliation) */
  ```
  `PNL_SCR_CONFIG` (`title "Config"`, `in_rail 1`). `arg` is the zone id: `1..8` opens that zone, `0` means master
  (Task 21; in this task it falls through to the next rule), `-1` means the last used zone, else the first used zone.
  An asked-for zone that is not enrolled opens the fallback and says "Zone N is not enrolled.".

  Behaviour:
  - **Picker:** "Master" plus one button per used zone, named with `pnl_zone_name` (A11). "Master" is disabled in
    this task; Task 21 enables it. The picker is rebuilt when the set of used zones changes.
  - **Tabs** follow `HG_GROUP_NAMES` order. The **index selector** shows shelves 0..3 for scope 1 and aux 0..1 for
    scope 2, labelled 0..3 like the web (`app.js:1022-1029`). A tab change clamps the index and clears the
    highlight, because it re-renders.
  - **Load:** a job runs `psvc_zone_cfg_get` into a PSRAM buffer owned by the module. On failure the body shows
    `pnl_msg(PNL_CTX_ZONE_LOAD)` and [Retry]. One self-heal per visit happens when the zone turns ONLINE with
    `cfg_sync` OK. The title reads "Config -- Zone N (gen G)".
  - **Edits:** kept per zone in a `pcfg_edits_t` allocated lazily in PSRAM. They survive tab and destination
    changes.
  - **Save:**
    1. Export the edit set. An empty set shows "No changes to save".
    2. Pre-validate locally: `psvc_zone_fields_fn` then `hg_cfg_validate(scratch, hw_present ? &hw : NULL)` on a
       scratch copy of the document on screen. On failure, highlight the field and send nothing.
    3. Submit a job running `psvc_zone_cfg_edit(zone, psvc_zone_fields_fn, &fedits, ...)`. The `fedits` array is
       in PSRAM, frozen and worker-owned while pending, and the button shows "Saving...".
    4. On done:
       - show `pnl_msg`, plus " (<warnings>)" when there are warnings, as the web does;
       - on a refusal with a path, highlight the field through `cfg_show_error`;
       - on OK, drop exactly the saved entries (unless re-edited meanwhile), apply them to the document on screen
         (the web merges the body into its cached doc, `app.js:1546-1574`), call `pnl_poll_kick()`, reload after
         3 s, and follow `cfg_busy` and `cfg_sync` in the status line until the save lands.
  - `pcfg_tighten` is applied with the loaded hw when `hw_present`. The hardware groups show the note "hardware
    plane -- set at the zone console" once, above their rows.
  - **THE RULE:** every `done()` checks `j->screen_gen == pnl_screen_gen()` before it touches a widget. A stale load
    re-submits itself if the rebuilt editor is waiting. A stale save leaves its message for the next build.
    `cfg_zone_update()` reconciles the Save button. No widget is touched from `pnl_work`.
  - No re-render happens while the keyboard is open: a reload that lands then is deferred to the next update.
  - **The "refused" status is not sticky.** `node_mgr_cfg_sync_failed()` is 1 while either plane's §4.4 latch is set, and
    a later successful sync of that plane clears it (`nmgr_cfg_note_synced()`, `components/node_mgr/node_mgr_cfg_latch.c`).
    So a zone that failed once reads "refused" after a save only while a plane is still latched. A latched HW plane
    also counts, which is what `GET NODE`'s CfgSync shows too.

**What each check proves:** the whole task is LVGL glue plus job wiring over host-tested pieces (generator,
edit model, messages, `psvc_zcfg`). GATE-P4 proves it compiles and links. The Stage 2 gate steps 1-6 and 8, run
on glass at the end of this task, prove the behaviour: tab order, read-only hardware, stepper save and push,
validation highlight, the input bounds, the offline refusal, the web collision and the never-synced zone.

- [ ] **Step 1: Create `components/panel_ui/scr_config.h`**

  ```c
  #pragma once
  /* scr_config.h -- the Config destination and the generated-editor frame both editors share (glue; LVGL task). */
  #include <stddef.h>
  #include <stdint.h>
  #include "lvgl.h"
  #include "hg_cfg.h"
  #include "pcfg_gen.h"
  #include "pcfg_edit.h"
  #include "pnl_poll.h"
  #include "wdg_field.h"
  #ifdef __cplusplus
  extern "C" {
  #endif

  typedef const char *(*cfg_value_fn)(void *ctx, uint8_t group, int idx, const hg_field_t *f, char *buf, size_t cap);
  typedef struct { pcfg_table_t table; uint8_t zone; cfg_value_fn value; void *ctx;
                   const hg_zone_hw_t *hw_or_null; pcfg_edits_t *edits;
                   wdg_reveal_fn reveal; } cfg_view_t;          /* reveal: SECRET rows only; NULL for zones */
  int  cfg_render_group(lv_obj_t *list, const cfg_view_t *v, uint8_t group, int idx);  /* clears list, one wdg_field per row */
  void cfg_show_error(const cfg_view_t *v, const char *path, const char *code);
       /* pcfg_locate -> switch tab/index -> wdg_field_set_error; no row -> banner "<code>: <path>" */
  void cfg_set_status(const char *text, int is_error);
  void cfg_set_title(const char *text);
  typedef void (*cfg_save_fn)(void);
  lv_obj_t *cfg_frame_build(lv_obj_t *body, const cfg_view_t *v, cfg_save_fn on_save);
  void cfg_frame_rerender(void);
  void cfg_frame_set_saving(int saving);

  void cfg_zone_open(lv_obj_t *body, uint8_t zone);
  void cfg_zone_close(void);
  void cfg_zone_update(const pnl_snap_t *s);
  void cfg_zone_wipe_all(void);   /* every zone's edit set + loaded doc (Task 27 calls it) */
  void cfg_master_open(lv_obj_t *body);          /* Task 21 */
  void cfg_master_close(void);                   /* Task 21 */
  void cfg_master_update(const pnl_snap_t *s);   /* Task 21 */
  void cfg_master_wipe(void);                    /* Task 21 */

  #ifdef __cplusplus
  }
  #endif
  ```

- [ ] **Step 2: Create `components/panel_ui/scr_config.c`**

  ```c
  /* scr_config.c -- the Config destination (glue; LVGL task only).
   * Owns the picker (Master + enrolled zones), the generated-editor frame both editors share (tabs in table order,
   * shelf/aux index selector, Save, row list), the title and the status line. The editors (cfg_zone.c,
   * cfg_master.c) own their documents, edit sets and worker jobs. */
  #include <stdio.h>
  #include <string.h>
  #include "esp_log.h"
  #include "esp_timer.h"
  #include "lvgl.h"
  #include "scr_config.h"
  #include "scr_shell.h"
  #include "pnl_fmt.h"
  #include "pnl_palette.h"
  #include "hg_mcfg.h"
  #include "wdg_keyboard.h"

  static const char *TAG = "scr_config";

  #define CFG_ROWS_MAX 16         /* the largest group (HWSHELF) has 11 rows */
  #define CFG_SLOW_US  200000     /* the <= 200 ms callback budget */
  #define PICK_TXT     24

  typedef struct { lv_obj_t *row; uint8_t group; int idx; const hg_field_t *f; } cfg_row_t;

  static struct {
      lv_obj_t *title, *picker, *body, *status;
      int       open_zone;                 /* -1 none, 0 master (Task 21), 1..8 a zone */
      uint16_t  used_mask;
      int       n_pick;
      uint8_t   zone_of[1 + HG_MAX_ZONES];
  } s_ui = { .open_zone = -1 };

  /* button-matrix maps must outlive their widgets: file scope, never cleared */
  static char        s_pick_txt[1 + HG_MAX_ZONES][PICK_TXT];
  static const char *s_pick_map[1 + HG_MAX_ZONES + 1];
  static const char *s_tab_map[HG_G_COUNT + 2];
  static const char *const IDX4[] = { "0", "1", "2", "3", "" };
  static const char *const IDX2[] = { "0", "1", "" };

  static struct {
      const cfg_view_t *v;
      cfg_save_fn       on_save;
      lv_obj_t         *tabs, *idxsel, *list, *save, *save_lbl;
      uint8_t           groups[HG_G_COUNT];
      int               n_groups, tab, idx, idx_scope;
      cfg_row_t         rows[CFG_ROWS_MAX];
      int               n_rows;
  } s_fr;

  static int        s_last_zone = -1;                                        /* "-1 = last used" */
  static int        s_keep_table = -1, s_keep_zone = -1, s_keep_tab, s_keep_idx;
  static pnl_snap_t s_snap;                                                  /* ~2 KB: never on the LVGL stack */

  /* ---------- table helpers ---------- */
  static const hg_field_t *rows_of(pcfg_table_t t, int *n) {
      if (t == PCFG_TABLE_MASTER) { *n = HG_MFIELD_COUNT; return HG_MFIELDS; }
      *n = HG_FIELD_COUNT;
      return HG_FIELDS;
  }
  static const char *gname(pcfg_table_t t, uint8_t g) { return t == PCFG_TABLE_MASTER ? HG_MGROUP_NAMES[g] : HG_GROUP_NAMES[g]; }
  static int gscope(pcfg_table_t t, uint8_t g) { return t == PCFG_TABLE_MASTER ? 0 : hg_group_scope(g); }

  /* ---------- title / status ---------- */
  void cfg_set_title(const char *text) {
      if (s_ui.title) pnl_label_set_if_changed(s_ui.title, text ? text : "");
  }
  void cfg_set_status(const char *text, int is_error) {
      if (!s_ui.status) return;
      pnl_label_set_if_changed(s_ui.status, text ? text : "");
      lv_obj_set_style_text_color(s_ui.status, lv_color_hex(is_error ? PNL_C_OFFLINE_TEXT : PNL_C_MUTED), 0);
  }

  /* ---------- rows ---------- */
  static cfg_row_t *row_find(const hg_field_t *f) {
      for (int i = 0; i < s_fr.n_rows; i++) if (s_fr.rows[i].f == f) return &s_fr.rows[i];
      return NULL;       /* one group/index is on screen at a time, so the row pointer is unique */
  }
  static void on_changed(void *ctx, uint8_t group, int idx, const hg_field_t *f, const char *raw) {
      const cfg_view_t *v = ctx;
      int rc = v->edits ? pcfg_edits_set(v->edits, group, idx, f, raw) : -3;
      if (rc == 0) return;
      cfg_row_t *r = row_find(f);
      if (r) wdg_field_set_dirty(r->row, 0);
      cfg_set_status(rc == -1 ? "Too many unsaved changes -- save first" :
                     rc == -2 ? "Hardware plane is read-only" : "Out of memory -- the change was not kept", 1);
  }

  int cfg_render_group(lv_obj_t *list, const cfg_view_t *v, uint8_t group, int idx) {
      int64_t t0 = esp_timer_get_time();
      if (wdg_keyboard_is_open()) wdg_keyboard_close();
      lv_obj_clean(list);
      s_fr.n_rows = 0;
      if (v->table == PCFG_TABLE_ZONE && hg_group_is_hw(group)) {
          lv_obj_t *n = lv_label_create(list);
          lv_obj_set_width(n, lv_pct(100));
          lv_label_set_long_mode(n, LV_LABEL_LONG_MODE_WRAP);
          lv_obj_set_style_text_color(n, lv_color_hex(PNL_C_MUTED), 0);
          lv_label_set_text(n, v->hw_or_null ? "hardware plane -- set at the zone console"
                                             : "hardware plane -- set at the zone console (not received from the zone yet: shown as 0)");
      }
      int nrows;
      const hg_field_t *tab = rows_of(v->table, &nrows);
      for (int i = 0; i < nrows && s_fr.n_rows < CFG_ROWS_MAX; i++) {
          const hg_field_t *f = &tab[i];
          if (f->group != group) continue;
          pcfg_spec_t sp;
          (void)pcfg_spec_for(v->table, f, &sp);       /* -1 still yields a READONLY/TEXT spec that renders */
          if (v->table == PCFG_TABLE_ZONE) pcfg_tighten(&sp, f, idx, v->hw_or_null);
          const psvc_fedit_t *e = v->edits ? pcfg_edits_get(v->edits, group, idx, f) : NULL;
          char buf[PSVC_FEDIT_TEXT_MAX];
          const char *raw;
          if (sp.kind == PCFG_K_SECRET) raw = "";      /* a secret never enters a row */
          else if (e) raw = e->text;
          else raw = v->value(v->ctx, group, idx, f, buf, sizeof buf);
          lv_obj_t *row = wdg_field_create(list, &sp, group, idx, f, raw, on_changed, (void *)v);
          if (!row) continue;
          if (e) wdg_field_set_dirty(row, 1);
          if (sp.kind == PCFG_K_SECRET && v->reveal) wdg_field_set_reveal(row, v->reveal, v->ctx);
          s_fr.rows[s_fr.n_rows++] = (cfg_row_t){ row, group, idx, f };
      }
      int64_t dt = esp_timer_get_time() - t0;
      if (dt > CFG_SLOW_US) ESP_LOGW(TAG, "render %s took %lld ms", gname(v->table, group), (long long)(dt / 1000));
      return s_fr.n_rows;
  }

  /* ---------- frame ---------- */
  static int frame_idx(void) {
      int sc = gscope(s_fr.v->table, s_fr.groups[s_fr.tab]);
      if (sc == 0) return -1;
      int max = (sc == 1 ? HG_MAX_SHELVES : HG_MAX_AUX) - 1;
      if (s_fr.idx < 0) s_fr.idx = 0;
      if (s_fr.idx > max) s_fr.idx = max;              /* a tab change clamps the index (app.js:1703-1715) */
      return s_fr.idx;
  }
  static void frame_render(void) {
      if (!s_fr.list || !s_fr.v) return;
      int ix = frame_idx();
      int sc = gscope(s_fr.v->table, s_fr.groups[s_fr.tab]);
      if (sc == 0) {
          lv_obj_add_flag(s_fr.idxsel, LV_OBJ_FLAG_HIDDEN);
      } else {
          if (sc != s_fr.idx_scope) {
              lv_buttonmatrix_set_map(s_fr.idxsel, sc == 1 ? IDX4 : IDX2);
              lv_buttonmatrix_set_button_ctrl_all(s_fr.idxsel, LV_BUTTONMATRIX_CTRL_CHECKABLE);
              lv_obj_set_width(s_fr.idxsel, sc == 1 ? 4 * 72 : 2 * 72);
          }
          lv_buttonmatrix_set_button_ctrl(s_fr.idxsel, (uint32_t)ix, LV_BUTTONMATRIX_CTRL_CHECKED);
          lv_obj_remove_flag(s_fr.idxsel, LV_OBJ_FLAG_HIDDEN);
      }
      s_fr.idx_scope = sc;
      lv_buttonmatrix_set_button_ctrl(s_fr.tabs, (uint32_t)s_fr.tab, LV_BUTTONMATRIX_CTRL_CHECKED);
      s_keep_table = (int)s_fr.v->table; s_keep_zone = s_fr.v->zone; s_keep_tab = s_fr.tab; s_keep_idx = s_fr.idx;
      cfg_render_group(s_fr.list, s_fr.v, s_fr.groups[s_fr.tab], ix);
  }
  static void ev_tab(lv_event_t *e) {
      (void)e;
      uint32_t sel = lv_buttonmatrix_get_selected_button(s_fr.tabs);
      if (sel >= (uint32_t)s_fr.n_groups) return;
      s_fr.tab = (int)sel;
      frame_render();
  }
  static void ev_idx(lv_event_t *e) {
      (void)e;
      uint32_t sel = lv_buttonmatrix_get_selected_button(s_fr.idxsel);
      if (sel == LV_BUTTONMATRIX_BUTTON_NONE) return;
      s_fr.idx = (int)sel;
      frame_render();
  }
  static void ev_save(lv_event_t *e) {
      (void)e;
      if (s_fr.on_save) s_fr.on_save();
  }

  lv_obj_t *cfg_frame_build(lv_obj_t *body, const cfg_view_t *v, cfg_save_fn on_save) {
      memset(&s_fr, 0, sizeof s_fr);
      s_fr.v = v;
      s_fr.on_save = on_save;
      s_fr.idx_scope = -1;
      int nrows;
      const hg_field_t *rows = rows_of(v->table, &nrows);
      int ng = v->table == PCFG_TABLE_MASTER ? HG_MG_COUNT : HG_G_COUNT;
      for (int g = 0; g < ng; g++) {
          int has = 0;
          for (int i = 0; i < nrows && !has; i++) has = rows[i].group == g;
          if (has) s_fr.groups[s_fr.n_groups++] = (uint8_t)g;    /* WEB has no rows: it never appears */
      }
      int m = 0;
      for (int t = 0; t < s_fr.n_groups; t++) {
          if (t == 5 && s_fr.n_groups > 6) s_tab_map[m++] = "\n";  /* ten zone tabs: two rows of five */
          s_tab_map[m++] = gname(v->table, s_fr.groups[t]);
      }
      s_tab_map[m] = "";
      if (s_keep_table == (int)v->table && s_keep_zone == v->zone && s_keep_tab < s_fr.n_groups) {
          s_fr.tab = s_keep_tab;
          s_fr.idx = s_keep_idx;
      }

      s_fr.tabs = lv_buttonmatrix_create(body);
      lv_buttonmatrix_set_map(s_fr.tabs, s_tab_map);
      lv_buttonmatrix_set_button_ctrl_all(s_fr.tabs, LV_BUTTONMATRIX_CTRL_CHECKABLE);
      lv_buttonmatrix_set_one_checked(s_fr.tabs, true);
      lv_obj_set_size(s_fr.tabs, lv_pct(100), s_fr.n_groups > 6 ? 112 : 56);
      lv_obj_add_event_cb(s_fr.tabs, ev_tab, LV_EVENT_VALUE_CHANGED, NULL);

      lv_obj_t *bar = lv_obj_create(body);
      lv_obj_remove_style_all(bar);
      lv_obj_set_size(bar, lv_pct(100), LV_SIZE_CONTENT);
      lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
      lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
      s_fr.idxsel = lv_buttonmatrix_create(bar);
      lv_buttonmatrix_set_one_checked(s_fr.idxsel, true);
      lv_obj_set_height(s_fr.idxsel, 56);
      lv_obj_add_flag(s_fr.idxsel, LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_event_cb(s_fr.idxsel, ev_idx, LV_EVENT_VALUE_CHANGED, NULL);
      s_fr.save = lv_button_create(bar);
      lv_obj_set_size(s_fr.save, 180, 56);
      lv_obj_add_flag(s_fr.save, LV_OBJ_FLAG_FLOATING);           /* stays right-aligned when idxsel is hidden */
      lv_obj_align(s_fr.save, LV_ALIGN_RIGHT_MID, 0, 0);
      s_fr.save_lbl = lv_label_create(s_fr.save);
      lv_label_set_text(s_fr.save_lbl, "Save");
      lv_obj_center(s_fr.save_lbl);
      lv_obj_add_event_cb(s_fr.save, ev_save, LV_EVENT_CLICKED, NULL);
      lv_obj_set_style_min_height(bar, 56, 0);

      s_fr.list = lv_obj_create(body);
      lv_obj_set_width(s_fr.list, lv_pct(100));
      lv_obj_set_flex_grow(s_fr.list, 1);
      lv_obj_set_flex_flow(s_fr.list, LV_FLEX_FLOW_COLUMN);
      lv_obj_set_style_pad_all(s_fr.list, 4, 0);
      lv_obj_set_style_pad_row(s_fr.list, 0, 0);
      frame_render();
      return s_fr.list;
  }
  void cfg_frame_rerender(void) { frame_render(); }
  void cfg_frame_set_saving(int saving) {
      if (!s_fr.save) return;
      lv_label_set_text(s_fr.save_lbl, saving ? "Saving..." : "Save");
      lv_obj_set_state(s_fr.save, LV_STATE_DISABLED, saving != 0);
  }

  void cfg_show_error(const cfg_view_t *v, const char *path, const char *code) {
      char msg[160];
      uint8_t g;
      int ix;
      const hg_field_t *row;
      const char *c = (code && code[0]) ? code : "ERROR";
      if (!s_fr.list || v != s_fr.v || !path || pcfg_locate(v->table, path, &g, &ix, &row) != 0) {
          snprintf(msg, sizeof msg, "%s%s%s", c, (path && path[0]) ? ": " : "", path ? path : "");
          cfg_set_status(msg, 1);
          return;
      }
      for (int t = 0; t < s_fr.n_groups; t++) if (s_fr.groups[t] == g) s_fr.tab = t;
      if (ix >= 0) s_fr.idx = ix;
      frame_render();
      cfg_row_t *r = row_find(row);
      if (r) wdg_field_set_error(r->row, c);
      pcfg_spec_t sp;
      (void)pcfg_spec_for(v->table, row, &sp);
      int sc = gscope(v->table, g);
      if (sc == 0) snprintf(msg, sizeof msg, "%s: %s", c, sp.label);
      else snprintf(msg, sizeof msg, "%s: %s (%s %d%s)", c, sp.label, sc == 1 ? "shelf" : "aux", s_fr.idx,
                    ix < 0 ? ", index not reported" : "");
      cfg_set_status(msg, 1);
  }

  /* ---------- picker ---------- */
  static int zone_used(const pnl_snap_t *s, int z) { return z >= 1 && z <= HG_MAX_ZONES && s->st.node[z - 1].used; }
  static uint16_t used_mask(const pnl_snap_t *s) {
      uint16_t m = 0;
      for (int z = 1; z <= HG_MAX_ZONES; z++) if (zone_used(s, z)) m |= (uint16_t)(1u << z);
      return m;
  }
  static void picker_check(int zone) {
      if (!s_ui.picker) return;
      lv_buttonmatrix_clear_button_ctrl_all(s_ui.picker, LV_BUTTONMATRIX_CTRL_CHECKED);
      for (int i = 0; i < s_ui.n_pick; i++)
          if (zone >= 0 && s_ui.zone_of[i] == zone) lv_buttonmatrix_set_button_ctrl(s_ui.picker, (uint32_t)i, LV_BUTTONMATRIX_CTRL_CHECKED);
  }
  static int master_enabled(void);
  static void picker_rebuild(const pnl_snap_t *s) {
      int n = 0;
      snprintf(s_pick_txt[0], PICK_TXT, "Master");
      s_pick_map[n] = s_pick_txt[0];
      s_ui.zone_of[n++] = 0;
      for (int z = 1; z <= HG_MAX_ZONES; z++) {
          if (!zone_used(s, z)) continue;
          char nm[17];
          pnl_zone_name(&s->st.node[z - 1], nm);
          snprintf(s_pick_txt[n], PICK_TXT, "%s", nm);
          s_pick_map[n] = s_pick_txt[n];
          s_ui.zone_of[n++] = (uint8_t)z;
      }
      s_pick_map[n] = "";
      s_ui.n_pick = n;
      s_ui.used_mask = used_mask(s);
      lv_buttonmatrix_set_map(s_ui.picker, s_pick_map);
      lv_buttonmatrix_set_button_ctrl_all(s_ui.picker, LV_BUTTONMATRIX_CTRL_CHECKABLE);
      if (!master_enabled()) lv_buttonmatrix_set_button_ctrl(s_ui.picker, 0, LV_BUTTONMATRIX_CTRL_DISABLED);
      picker_check(s_ui.open_zone);
  }

  /* ---------- editor routing (Task 21 replaces this whole block to add the master) ---------- */
  static int master_enabled(void) { return 0; }
  static void close_editor(void) {
      if (s_ui.open_zone >= 1) cfg_zone_close();
      s_ui.open_zone = -1;
      s_fr.v = NULL; s_fr.list = NULL; s_fr.tabs = NULL; s_fr.idxsel = NULL; s_fr.save = NULL; s_fr.save_lbl = NULL;
      s_fr.n_rows = 0;
  }
  static void open_editor(int zone) {
      close_editor();
      if (wdg_keyboard_is_open()) wdg_keyboard_close();
      lv_obj_clean(s_ui.body);
      cfg_set_status("", 0);
      s_ui.open_zone = zone;
      picker_check(zone);
      if (zone >= 1) { s_last_zone = zone; cfg_zone_open(s_ui.body, (uint8_t)zone); return; }
      cfg_set_title("Config");
      lv_obj_t *l = lv_label_create(s_ui.body);
      lv_label_set_text(l, "No zones enrolled.");
  }
  static int pick_target(const pnl_snap_t *s, int arg) {
      if (arg >= 1 && zone_used(s, arg)) return arg;
      if (s_last_zone >= 1 && zone_used(s, s_last_zone)) return s_last_zone;
      for (int z = 1; z <= HG_MAX_ZONES; z++) if (zone_used(s, z)) return z;
      return -1;
  }
  static void ev_pick(lv_event_t *e) {
      (void)e;
      uint32_t sel = lv_buttonmatrix_get_selected_button(s_ui.picker);
      if (sel >= (uint32_t)s_ui.n_pick) return;
      int z = s_ui.zone_of[sel];
      if (z == 0 || z == s_ui.open_zone) { picker_check(s_ui.open_zone); return; }
      open_editor(z);
  }
  static void route_update(const pnl_snap_t *snap) {
      if (s_ui.open_zone >= 1) { cfg_zone_update(snap); return; }
      int z = pick_target(snap, -1);
      if (z >= 1) open_editor(z);
  }
  /* ---------- end of editor routing ---------- */

  /* ---------- the destination ---------- */
  static void cfg_build(lv_obj_t *content, int arg) {
      int64_t t0 = esp_timer_get_time();
      s_ui.open_zone = -1; s_ui.n_pick = 0; s_ui.used_mask = 0;
      lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
      lv_obj_set_style_pad_row(content, 6, 0);
      s_ui.title = lv_label_create(content);
      lv_obj_set_style_text_font(s_ui.title, &lv_font_montserrat_28, 0);
      lv_label_set_text(s_ui.title, "Config");
      s_ui.picker = lv_buttonmatrix_create(content);
      lv_buttonmatrix_set_one_checked(s_ui.picker, true);
      lv_obj_set_size(s_ui.picker, lv_pct(100), 56);
      lv_obj_add_event_cb(s_ui.picker, ev_pick, LV_EVENT_VALUE_CHANGED, NULL);
      s_ui.body = lv_obj_create(content);
      lv_obj_remove_style_all(s_ui.body);
      lv_obj_set_width(s_ui.body, lv_pct(100));
      lv_obj_set_flex_grow(s_ui.body, 1);
      lv_obj_set_flex_flow(s_ui.body, LV_FLEX_FLOW_COLUMN);
      lv_obj_set_style_pad_row(s_ui.body, 6, 0);
      s_ui.status = lv_label_create(content);
      lv_obj_set_width(s_ui.status, lv_pct(100));
      lv_label_set_long_mode(s_ui.status, LV_LABEL_LONG_MODE_WRAP);
      lv_label_set_text(s_ui.status, "");
      pnl_poll_latest(&s_snap);
      picker_rebuild(&s_snap);
      int z = pick_target(&s_snap, arg);
      open_editor(z);
      if (arg >= 1 && z != arg) {
          char m[48];
          snprintf(m, sizeof m, "Zone %d is not enrolled.", arg);
          cfg_set_status(m, 1);
      }
      int64_t dt = esp_timer_get_time() - t0;
      if (dt > CFG_SLOW_US) ESP_LOGW(TAG, "build took %lld ms", (long long)(dt / 1000));
  }
  static void cfg_update(const pnl_snap_t *snap) {
      if (!s_ui.picker) return;
      if (used_mask(snap) != s_ui.used_mask) picker_rebuild(snap);
      route_update(snap);
  }
  static void cfg_teardown(void) {
      if (wdg_keyboard_is_open()) wdg_keyboard_close();
      close_editor();
      s_ui.title = NULL; s_ui.picker = NULL; s_ui.body = NULL; s_ui.status = NULL;
  }

  const pnl_screen_ops_t PNL_SCR_CONFIG = {
      .title = "Config", .build = cfg_build, .update = cfg_update, .teardown = cfg_teardown, .in_rail = 1,
  };
  ```

- [ ] **Step 3: Create `components/panel_ui/cfg_zone.c`**

  ```c
  /* cfg_zone.c -- the generated editor for zones 1..8 (glue; LVGL task only).
   * Document: psvc_zone_cfg_get on the worker into a PSRAM buffer. Edits: one pcfg_edits_t per zone, kept across
   * tabs and destinations. Save: the dirty set only, pre-validated on a scratch copy, then psvc_zone_cfg_edit on
   * the worker, which applies it to a FRESH copy (last writer wins per field, like the web PUT). */
  #include <stdio.h>
  #include <string.h>
  #include "esp_heap_caps.h"
  #include "esp_log.h"
  #include "lvgl.h"
  #include "scr_config.h"
  #include "pnl_worker.h"
  #include "pnl_poll.h"
  #include "pnl_msg.h"
  #include "psvc_zcfg.h"
  #include "psvc_rc.h"
  #include "wdg_keyboard.h"

  static const char *TAG = "cfg_zone";

  #define RELOAD_MS 3000u    /* the web refetches 3 s after a 202 (app.js:1594) */

  typedef struct { hg_zone_cfg_t cfg; hg_zone_hw_t hw; uint32_t gen; int hw_present; } zdoc_t;
  typedef struct { uint8_t zone; zdoc_t *dst; } load_arg_t;
  typedef struct { uint8_t zone; const psvc_fedit_t *e; int n; } save_arg_t;
  typedef enum { ZS_CLOSED = 0, ZS_LOADING, ZS_LOADED, ZS_FAILED } zstate_t;

  static zdoc_t        *s_doc;                   /* PSRAM: the document on screen */
  static zdoc_t        *s_ldoc;                  /* PSRAM: the load target -- worker-owned while s_load_pending */
  static psvc_fedit_t  *s_fedits;                /* PSRAM: the frozen save set -- worker-owned while s_saving */
  static pcfg_edits_t  *s_edits[HG_MAX_ZONES];   /* PSRAM, lazily per zone */
  static hg_zone_cfg_t  s_scratch;               /* local pre-validation copy */
  static cfg_view_t     s_view;
  static lv_obj_t      *s_body;
  static lv_timer_t    *s_reload;
  static zstate_t       s_state;
  static uint8_t        s_zone, s_load_pending, s_load_again, s_saving, s_ui_saving, s_selfheal_used,
                        s_watch, s_rerender_due, s_gone, s_last_err;
  static uint32_t       s_watch_seq;
  static char           s_last_msg[160];

  static void submit_load(void);
  static void zone_save(void);

  static int buffers_ok(void) {
      if (!s_doc)    s_doc    = heap_caps_calloc(1, sizeof *s_doc, MALLOC_CAP_SPIRAM);
      if (!s_ldoc)   s_ldoc   = heap_caps_calloc(1, sizeof *s_ldoc, MALLOC_CAP_SPIRAM);
      if (!s_fedits) s_fedits = heap_caps_calloc(PCFG_EDIT_MAX, sizeof *s_fedits, MALLOC_CAP_SPIRAM);
      return s_doc && s_ldoc && s_fedits;
  }
  static pcfg_edits_t *edits_for(uint8_t zone) {
      pcfg_edits_t **pe = &s_edits[zone - 1];
      if (!*pe) {
          *pe = heap_caps_calloc(1, sizeof **pe, MALLOC_CAP_SPIRAM);
          if (*pe) pcfg_edits_reset(*pe, PCFG_TABLE_ZONE, zone);
          else ESP_LOGW(TAG, "no PSRAM for the zone %u edit set", (unsigned)zone);
      }
      return *pe;
  }

  static const char *zone_value(void *ctx, uint8_t group, int idx, const hg_field_t *f, char *buf, size_t cap) {
      (void)ctx;
      const void *base = hg_field_base(group, idx, &s_doc->hw, &s_doc->cfg);
      if (!base || hg_field_read(f, base, buf, cap) != 0) snprintf(buf, cap, "?");
      return buf;
  }

  static void set_title(void) {
      char t[64];
      if (s_state == ZS_LOADED) snprintf(t, sizeof t, "Config -- Zone %u (gen %lu)", (unsigned)s_zone, (unsigned long)s_doc->gen);
      else snprintf(t, sizeof t, "Config -- Zone %u", (unsigned)s_zone);
      cfg_set_title(t);
  }

  static void retry_async(void *unused) {
      (void)unused;
      if (s_state != ZS_FAILED || !s_body) return;
      s_state = ZS_LOADING;
      set_title();
      lv_obj_clean(s_body);
      lv_label_set_text(lv_label_create(s_body), "Loading...");
      submit_load();
  }
  static void ev_retry(lv_event_t *e) { (void)e; lv_async_call(retry_async, NULL); }   /* never delete the button inside its own event */

  static void body_message(const char *text, int retry) {
      if (!s_body) return;
      lv_obj_clean(s_body);
      lv_obj_t *l = lv_label_create(s_body);
      lv_obj_set_width(l, lv_pct(100));
      lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
      lv_label_set_text(l, text);
      if (retry) {
          lv_obj_t *b = lv_button_create(s_body);
          lv_obj_set_size(b, 180, 56);
          lv_obj_t *bl = lv_label_create(b);
          lv_label_set_text(bl, "Retry");
          lv_obj_center(bl);
          lv_obj_add_event_cb(b, ev_retry, LV_EVENT_CLICKED, NULL);
      }
  }

  static void show_loaded(void) {
      lv_obj_clean(s_body);
      cfg_frame_build(s_body, &s_view, zone_save);
      s_ui_saving = s_saving;
      cfg_frame_set_saving(s_saving);
      if (!s_saving && s_last_msg[0]) { cfg_set_status(s_last_msg, s_last_err); s_last_msg[0] = '\0'; }
  }

  /* ---------- load ---------- */
  static void load_run(pnl_job_t *j) {             /* pnl_work: may block; never lv_* */
      load_arg_t a;
      memcpy(&a, j->arg, sizeof a);
      j->rc = psvc_zone_cfg_get(a.zone, &a.dst->cfg, &a.dst->hw, &a.dst->gen, &a.dst->hw_present);
  }
  static void load_done(pnl_job_t *j) {            /* LVGL task */
      load_arg_t a;
      memcpy(&a, j->arg, sizeof a);
      s_load_pending = 0;
      int live = j->screen_gen == pnl_screen_gen() && s_state != ZS_CLOSED && a.zone == s_zone;
      if (!live) {                                  /* THE RULE: stale -- touch no widget */
          if (s_load_again) { s_load_again = 0; if (s_state == ZS_LOADING) submit_load(); }
          return;
      }
      s_load_again = 0;
      if (j->rc == PSVC_OK) {
          int was_loaded = s_state == ZS_LOADED;
          memcpy(s_doc, s_ldoc, sizeof *s_doc);
          s_state = ZS_LOADED;
          s_view.hw_or_null = s_doc->hw_present ? &s_doc->hw : NULL;   /* the hw_present rule (http_api_cfg.c:132-149) */
          set_title();
          if (!was_loaded) show_loaded();
          else if (wdg_keyboard_is_open()) s_rerender_due = 1;         /* never yank a keyboard from under a finger */
          else cfg_frame_rerender();
          return;
      }
      char m[200];
      pnl_msg_arg_t ma = { .zone = a.zone };
      pnl_msg(PNL_CTX_ZONE_LOAD, j->rc, &ma, m, sizeof m);
      if (s_state == ZS_LOADED) {                   /* a reload failed: keep the document on screen */
          char r[220];
          snprintf(r, sizeof r, "Reload failed: %s", m);
          cfg_set_status(r, 1);
          return;
      }
      s_state = ZS_FAILED;
      set_title();
      body_message(m, 1);
  }
  static void submit_load(void) {
      if (s_load_pending) { s_load_again = 1; return; }   /* s_ldoc belongs to the worker until done() */
      load_arg_t a = { .zone = s_zone, .dst = s_ldoc };
      if (pnl_worker_submit(load_run, load_done, &a, sizeof a) != 0) {
          if (s_state != ZS_LOADED) { s_state = ZS_FAILED; body_message("Panel busy -- tap Retry.", 1); }
          return;
      }
      s_load_pending = 1;
  }
  static void reload_cb(lv_timer_t *t) {
      (void)t;
      s_reload = NULL;                              /* repeat count 1: LVGL deletes it after this */
      if (s_state == ZS_LOADED) submit_load();
  }

  /* ---------- save ---------- */
  static void save_run(pnl_job_t *j) {             /* pnl_work */
      save_arg_t a;
      memcpy(&a, j->arg, sizeof a);
      psvc_fedits_t fe = { a.e, a.n };
      j->rc = psvc_zone_cfg_edit(a.zone, psvc_zone_fields_fn, &fe, j->err, sizeof j->err, (char *)j->out, sizeof j->out);
  }
  static void save_done(pnl_job_t *j) {            /* LVGL task */
      save_arg_t a;
      memcpy(&a, j->arg, sizeof a);
      s_saving = 0;
      j->err[sizeof j->err - 1] = '\0';
      j->out[PNL_JOB_OUT_MAX - 1] = 0;
      char msg[160];
      pnl_msg_arg_t ma = { .zone = a.zone };
      pnl_msg(PNL_CTX_ZONE_SAVE, j->rc, &ma, msg, sizeof msg);
      int live = j->screen_gen == pnl_screen_gen() && s_state == ZS_LOADED && a.zone == s_zone;
      if (j->rc == PSVC_OK) {
          pcfg_edits_t *e = s_edits[a.zone - 1];
          for (int i = 0; e && i < a.n; i++) {      /* drop what was saved -- unless re-edited meanwhile */
              const psvc_fedit_t *cur = pcfg_edits_get(e, a.e[i].group, a.e[i].idx, a.e[i].f);
              if (cur && strcmp(cur->text, a.e[i].text) == 0) pcfg_edits_drop(e, a.e[i].group, a.e[i].idx, a.e[i].f);
          }
          if (live) {                               /* merge into the doc on screen, as the web does (app.js:1546-1574) */
              psvc_fedits_t fe = { a.e, a.n };
              char e2[8], w2[8];
              (void)psvc_zone_fields_fn(&s_doc->cfg, NULL, &fe, e2, sizeof e2, w2, sizeof w2);
          }
          const char *w = (const char *)j->out;
          if (w[0]) { size_t l = strlen(msg); snprintf(msg + l, sizeof msg - l, " (%s)", w); }
          pnl_poll_kick();
      }
      memset(s_fedits, 0, PCFG_EDIT_MAX * sizeof *s_fedits);
      if (!live) {                                  /* stale: leave the outcome for the next build */
          snprintf(s_last_msg, sizeof s_last_msg, "%s", msg);
          s_last_err = j->rc != PSVC_OK;
          return;
      }
      s_ui_saving = 0;
      cfg_frame_set_saving(0);
      cfg_set_status(msg, j->rc != PSVC_OK);
      if (j->rc == PSVC_OK) {
          s_watch = 1;
          s_watch_seq = pnl_poll_seq();
          if (wdg_keyboard_is_open()) s_rerender_due = 1; else cfg_frame_rerender();
          if (s_reload) lv_timer_delete(s_reload);
          s_reload = lv_timer_create(reload_cb, RELOAD_MS, NULL);
          lv_timer_set_repeat_count(s_reload, 1);
      } else if (j->err[0]) {
          cfg_show_error(&s_view, j->err, psvc_rc_token(j->rc));
      }
  }
  static void zone_save(void) {
      if (s_saving || s_state != ZS_LOADED) return;
      int n = s_view.edits ? pcfg_edits_export(s_view.edits, s_fedits, PCFG_EDIT_MAX) : 0;
      if (n <= 0) { cfg_set_status("No changes to save", 0); return; }
      const hg_zone_hw_t *hw = s_doc->hw_present ? &s_doc->hw : NULL;     /* the hw_present rule, both ways */
      psvc_fedits_t fe = { s_fedits, n };
      char err[96], warn[8];
      err[0] = '\0';
      s_scratch = s_doc->cfg;
      if (psvc_zone_fields_fn(&s_scratch, hw, &fe, err, sizeof err, warn, sizeof warn) != 0) {
          cfg_show_error(&s_view, err, psvc_rc_token(PSVC_E_INVALID_FIELD));
          return;
      }
      if (hg_cfg_validate(&s_scratch, hw, err, sizeof err) != 0) {
          cfg_show_error(&s_view, err, psvc_rc_token(PSVC_E_VALIDATION));
          return;
      }
      save_arg_t a = { .zone = s_zone, .e = s_fedits, .n = n };
      if (pnl_worker_submit(save_run, save_done, &a, sizeof a) != 0) { cfg_set_status("Panel busy, retry", 1); return; }
      s_saving = 1;
      s_ui_saving = 1;
      cfg_frame_set_saving(1);
      cfg_set_status("Saving...", 0);
  }

  /* ---------- the editor's life ---------- */
  void cfg_zone_open(lv_obj_t *body, uint8_t zone) {
      s_body = body;
      s_zone = zone;
      s_selfheal_used = 0; s_watch = 0; s_rerender_due = 0; s_gone = 0;
      s_state = ZS_LOADING;
      set_title();
      if (!buffers_ok()) { s_state = ZS_FAILED; body_message("Out of memory (PSRAM) -- the editor cannot open.", 0); return; }
      s_view = (cfg_view_t){ .table = PCFG_TABLE_ZONE, .zone = zone, .value = zone_value, .ctx = NULL,
                             .hw_or_null = NULL, .edits = edits_for(zone), .reveal = NULL };
      body_message("Loading...", 0);
      submit_load();
  }

  void cfg_zone_close(void) {
      if (s_reload) { lv_timer_delete(s_reload); s_reload = NULL; }
      s_state = ZS_CLOSED;
      s_body = NULL;
      s_zone = 0;
      s_watch = 0;
      s_rerender_due = 0;
  }

  void cfg_zone_update(const pnl_snap_t *s) {
      if (s_state == ZS_CLOSED || s_zone < 1) return;
      const hg_node_t *n = &s->st.node[s_zone - 1];
      if (!n->used) {
          if (!s_gone) { s_gone = 1; cfg_set_status("This zone is no longer enrolled.", 1); }
          return;
      }
      s_gone = 0;
      if (s_state == ZS_FAILED && !s_selfheal_used && !s_load_pending &&
          n->health == NODE_H_ONLINE && !s->st.cfg_sync_failed[s_zone - 1]) {   /* one self-heal per visit */
          s_selfheal_used = 1;
          s_state = ZS_LOADING;
          set_title();
          body_message("Loading...", 0);
          submit_load();
          return;
      }
      if (s_state != ZS_LOADED) return;
      if (s_ui_saving != s_saving) {                /* a save that finished while this screen was rebuilt */
          s_ui_saving = s_saving;
          cfg_frame_set_saving(s_saving);
          if (!s_saving && s_last_msg[0]) { cfg_set_status(s_last_msg, s_last_err); s_last_msg[0] = '\0'; }
      }
      if (s_watch && s->seq > s_watch_seq) {        /* only snapshots taken after the save */
          if (s->cfg_busy[s_zone - 1]) cfg_set_status("Queued, pushing to zone...", 0);
          else if (s->st.cfg_sync_failed[s_zone - 1]) { s_watch = 0; cfg_set_status("The zone refused the new config (CFG_SYNC_FAILED) -- see Alarms.", 1); }
          else { s_watch = 0; cfg_set_status("Landed on the zone.", 0); }
      }
      if (s_rerender_due && !wdg_keyboard_is_open()) { s_rerender_due = 0; cfg_frame_rerender(); }
  }

  void cfg_zone_wipe_all(void) {
      for (int z = 0; z < HG_MAX_ZONES; z++) if (s_edits[z]) pcfg_edits_wipe(s_edits[z]);
      if (s_doc && s_state == ZS_CLOSED) memset(s_doc, 0, sizeof *s_doc);
      s_last_msg[0] = '\0';
  }
  ```

- [ ] **Step 4: Register the destination and add the sources**

  In `components/panel_ui/scr_shell.c`, change the registry entry for `PNL_DEST_CONFIG` from `&PNL_SCR_PLACEHOLDER`
  to `&PNL_SCR_CONFIG`. It is the one-line-per-destination `REG[]` array Task 12 wrote. Replace only this line:
  ```c
      [PNL_DEST_CONFIG]    = { "Config",     &PNL_SCR_PLACEHOLDER, 1 },   /* Task 20 */
  ```
  with:
  ```c
      [PNL_DEST_CONFIG]    = { "Config",     &PNL_SCR_CONFIG,      1 },
  ```

  In `components/panel_ui/CMakeLists.txt`, inside the gating `if()` block, add:
  ```cmake
      list(APPEND PANEL_SRCS "scr_config.c" "cfg_zone.c")     # Task 20: the generated config editor (zones)
  ```

- [ ] **Step 5: GATE-HOST, GATE-P4, GATE-ESP32**

  Run GATE-HOST. Expected: `100% tests passed, 0 tests failed out of 47` (+0).
  Run GATE-P4. Expected: `Project build complete`, no `warning:` lines, target matched, lock restored.
  Run GATE-ESP32. Expected: three `Project build complete` and an empty lock diff.

- [ ] **Step 6: Flash and check on glass**

  The owner releases the bench first. Then:
  ```powershell
  python C:\Projects\HillGrov\tools\flash_app.py --app master --target esp32p4 --build-dir C:\Projects\HillGrov\master\build_p4 --port COM28 --dry-run
  ```
  Every printed path must say `build_p4`. Then:
  ```powershell
  python C:\Projects\HillGrov\tools\flash_app.py --app master --target esp32p4 --build-dir C:\Projects\HillGrov\master\build_p4 --port COM28
  ```
  Run Stage 2 gate steps 1-6 and 8 (below). Record the results in the task report.

- [ ] **Step 7: Commit and push**

  ```powershell
  git -C C:\Projects\HillGrov add components/panel_ui/scr_config.h components/panel_ui/scr_config.c components/panel_ui/cfg_zone.c components/panel_ui/scr_shell.c components/panel_ui/CMakeLists.txt
  git -C C:\Projects\HillGrov status --short
  ```
  Expected: those paths staged; ` M docs/hillgrow-features.drawio` unstaged.
  ```powershell
  git -C C:\Projects\HillGrov commit -m "feat(panel_ui): generated config editor for zones" -m "Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>"
  git -C C:\Projects\HillGrov push
  ```

---

### Task 21: Config editor — the master (zone 0), secrets masked with reveal (Stage 2)

**Files:**
- Create: `components/panel_ui/cfg_master.c` (G)
- Modify: `components/panel_ui/scr_config.c` (replace the "editor routing" block written in Task 20)
- Modify: `components/panel_ui/CMakeLists.txt` (append one source; `PRIV_REQUIRES` gains `time_core`)

**Interfaces:**
- Consumes:
  - Task 3: `psvc_mcfg_get` (`[ANY]`), `psvc_mcfg_edit`, `psvc_mcfg_fields_fn` and `PSVC_LOCK_PANEL_MS`
    (`psvc_mcfg.h`), plus `psvc_fedits_t`. `psvc_mcfg_fields_fn` is pure (it calls only `hg_mcfg_is_secret` and
    `hg_field_write`), so the LVGL task also uses it for local pre-validation.
  - Task 18: `pcfg_edits_export` (it drops blank secrets) and `pnl_msg(PNL_CTX_MCFG_SAVE)`.
  - Task 19: `wdg_reveal_fn` and `wdg_keyboard_*`.
  - Task 20: `cfg_frame_build`, `cfg_frame_rerender`, `cfg_frame_set_saving`, `cfg_show_error` (master paths
    `GROUP.KEY`), `cfg_set_status` and `cfg_set_title`.
  - Task 8: `pnl_worker_submit` and `pnl_screen_gen`.
  - `hg_mcfg_validate` (`hg_mcfg.h:42`) and `tz_check` (`time_core.h:43`), both pure.
- Produces: `cfg_master_open`, `cfg_master_close`, `cfg_master_update` and `cfg_master_wipe` (declared in Task 20).
  - **Tabs:** WIFI, TIME and SYS. WEB has no rows and never appears.
  - **Display copy:** `psvc_mcfg_get` into PSRAM when the editor opens. `sta_pass`, `ap_pass`, `web_salt` and
    `web_hash` are zeroed in the copy at once: no secret is ever in the copy the rows read from.
  - **Reveal:** a job-free read. A pending (unsaved) new secret is shown if there is one; otherwise the stored
    value, read with `psvc_mcfg_get` into a PSRAM scratch copy and zeroed straight after. It goes to the row through
    the 65-byte local in `wdg_field` and re-masks after 10 s or on teardown.
  - **AP warning:** a save whose set touches AP_SSID or AP_PASS asks first: "Changing the AP drops every phone
    connected to it." The buttons are [Save anyway] and [Cancel].
  - **Save:**
    1. Export the edit set (blank secrets dropped). Pre-validate with `psvc_mcfg_fields_fn` and then
       `hg_mcfg_validate(scratch, tz_check)` on a fresh scratch copy, then zero it.
    2. Run the AP confirm when the set touches the AP.
    3. Submit a job running `psvc_mcfg_edit(psvc_mcfg_fields_fn, &fedits, PSVC_LOCK_PANEL_MS, "PANEL MCFG", err, cap)`.
    4. On done: `pnl_msg`. On OK, drop the saved entries and refresh the display copy. Always zero the frozen set,
       because it may hold secrets.
  - Routing: `pnl_nav_go(PNL_DEST_CONFIG, 0)` opens the master. With no zone enrolled, Config opens the master.
    "Master" in the picker is enabled.

**What each check proves:** glue over the host-tested `psvc_mcfg` (Task 3) and the Task 17-18 pieces. GATE-P4
proves it compiles and links. Stage 2 gate step 7 on glass proves the NTP save, the HOSTNAME save keeping
STA_PASS (the blank-secret rule, end to end), and Reveal with its 10 s re-mask.

- [ ] **Step 1: Create `components/panel_ui/cfg_master.c`**

  ```c
  /* cfg_master.c -- the generated editor for the master (zone 0): WIFI, TIME, SYS (glue; LVGL task only).
   * No panel auth (Decision 3), but secrets are masked with a deliberate reveal: the display copy never holds
   * them, a blank secret means "unchanged", and every buffer that held one is zeroed after use. */
  #include <stdint.h>
  #include <stdio.h>
  #include <string.h>
  #include "esp_heap_caps.h"
  #include "esp_log.h"
  #include "lvgl.h"
  #include "scr_config.h"
  #include "pnl_worker.h"
  #include "pnl_msg.h"
  #include "psvc_mcfg.h"
  #include "psvc_rc.h"
  #include "hg_mcfg.h"
  #include "time_core.h"
  #include "pnl_input.h"      /* pnl_zero */
  #include "wdg_keyboard.h"

  static const char *TAG = "cfg_master";

  typedef struct { const psvc_fedit_t *e; int n; } msave_arg_t;

  static hg_mcfg_t    *s_m;          /* PSRAM display copy -- secrets zeroed the moment it is taken */
  static hg_mcfg_t    *s_scratch;    /* PSRAM pre-validation / reveal copy -- zeroed after each use */
  static pcfg_edits_t *s_medits;     /* PSRAM; survives destination changes (cfg_master_wipe clears it) */
  static psvc_fedit_t *s_mfedits;    /* PSRAM frozen save set -- worker-owned while s_saving */
  static cfg_view_t    s_mview;
  static lv_obj_t     *s_confirm;
  static int           s_pending_n;
  static uint8_t       s_open, s_saving, s_ui_saving, s_rerender_due, s_last_err;
  static char          s_mlast[160];

  static int buffers_ok(void) {
      if (!s_m)        s_m        = heap_caps_calloc(1, sizeof *s_m, MALLOC_CAP_SPIRAM);
      if (!s_scratch)  s_scratch  = heap_caps_calloc(1, sizeof *s_scratch, MALLOC_CAP_SPIRAM);
      if (!s_mfedits)  s_mfedits  = heap_caps_calloc(PCFG_EDIT_MAX, sizeof *s_mfedits, MALLOC_CAP_SPIRAM);
      if (!s_medits) {
          s_medits = heap_caps_calloc(1, sizeof *s_medits, MALLOC_CAP_SPIRAM);
          if (s_medits) pcfg_edits_reset(s_medits, PCFG_TABLE_MASTER, 0);
      }
      return s_m && s_scratch && s_mfedits && s_medits;
  }
  static void take_copy(void) {
      psvc_mcfg_get(s_m);                                 /* [ANY]: a copy under no lock of ours */
      pnl_zero(s_m->sta_pass, sizeof s_m->sta_pass);
      pnl_zero(s_m->ap_pass, sizeof s_m->ap_pass);
      pnl_zero(s_m->web_salt, sizeof s_m->web_salt);
      pnl_zero(s_m->web_hash, sizeof s_m->web_hash);
  }

  static const char *master_value(void *ctx, uint8_t group, int idx, const hg_field_t *f, char *buf, size_t cap) {
      (void)ctx; (void)group; (void)idx;
      if (hg_mcfg_is_secret(f) || hg_field_read(f, s_m, buf, cap) != 0) snprintf(buf, cap, "%s", "");
      return buf;
  }

  static int master_reveal(void *ctx, const hg_field_t *f, char *out, size_t cap) {
      (void)ctx;
      if (!hg_mcfg_is_secret(f)) return -1;
      const psvc_fedit_t *e = s_medits ? pcfg_edits_get(s_medits, f->group, -1, f) : NULL;
      if (e && e->text[0]) { snprintf(out, cap, "%s", e->text); return 0; }   /* the new, unsaved value */
      psvc_mcfg_get(s_scratch);
      int rc = hg_field_read(f, s_scratch, out, cap);
      pnl_zero(s_scratch, sizeof *s_scratch);
      return rc == 0 ? 0 : -1;
  }

  /* ---------- save ---------- */
  static void msave_run(pnl_job_t *j) {             /* pnl_work: takes the master-config lock, NVS, radio apply */
      msave_arg_t a;
      memcpy(&a, j->arg, sizeof a);
      psvc_fedits_t fe = { a.e, a.n };
      j->rc = psvc_mcfg_edit(psvc_mcfg_fields_fn, &fe, PSVC_LOCK_PANEL_MS, "PANEL MCFG", j->err, sizeof j->err);
  }
  static void msave_done(pnl_job_t *j) {            /* LVGL task */
      msave_arg_t a;
      memcpy(&a, j->arg, sizeof a);
      s_saving = 0;
      j->err[sizeof j->err - 1] = '\0';
      char msg[160];
      pnl_msg(PNL_CTX_MCFG_SAVE, j->rc, NULL, msg, sizeof msg);
      if (j->rc == PSVC_OK) {
          for (int i = 0; i < a.n; i++) {           /* drop what was saved -- unless re-edited meanwhile */
              const psvc_fedit_t *cur = pcfg_edits_get(s_medits, a.e[i].group, -1, a.e[i].f);
              if (cur && strcmp(cur->text, a.e[i].text) == 0) pcfg_edits_drop(s_medits, a.e[i].group, -1, a.e[i].f);
          }
          take_copy();                              /* the web refetches immediately (app.js:1589-1591) */
      }
      pnl_zero(s_mfedits, PCFG_EDIT_MAX * sizeof *s_mfedits);   /* the frozen set may hold passwords */
      int live = j->screen_gen == pnl_screen_gen() && s_open;
      if (!live) {
          snprintf(s_mlast, sizeof s_mlast, "%s", msg);
          s_last_err = j->rc != PSVC_OK;
          return;
      }
      s_ui_saving = 0;
      cfg_frame_set_saving(0);
      cfg_set_status(msg, j->rc != PSVC_OK);
      if (j->rc == PSVC_OK) {
          if (wdg_keyboard_is_open()) s_rerender_due = 1; else cfg_frame_rerender();
      } else {
          cfg_show_error(&s_mview, j->err, psvc_rc_token(j->rc));   /* an empty path becomes the banner */
      }
  }
  static void submit_save(void) {
      msave_arg_t a = { .e = s_mfedits, .n = s_pending_n };
      if (pnl_worker_submit(msave_run, msave_done, &a, sizeof a) != 0) {
          pnl_zero(s_mfedits, PCFG_EDIT_MAX * sizeof *s_mfedits);
          cfg_set_status("Panel busy, retry", 1);
          return;
      }
      s_saving = 1;
      s_ui_saving = 1;
      cfg_frame_set_saving(1);
      cfg_set_status("Saving...", 0);
  }
  static int touches_ap(const psvc_fedit_t *e, int n) {
      for (int i = 0; i < n; i++)
          if (e[i].group == HG_MG_WIFI && (strcmp(e[i].f->key, "AP_SSID") == 0 || strcmp(e[i].f->key, "AP_PASS") == 0))
              return 1;
      return 0;
  }
  static void ev_confirm(lv_event_t *e) {
      int go = (int)(intptr_t)lv_event_get_user_data(e);
      if (s_confirm) { lv_msgbox_close_async(s_confirm); s_confirm = NULL; }
      if (go && s_open && !s_saving) { submit_save(); return; }
      pnl_zero(s_mfedits, PCFG_EDIT_MAX * sizeof *s_mfedits);
      cfg_set_status("Not saved.", 0);
  }
  static void ask_ap_confirm(void) {
      s_confirm = lv_msgbox_create(NULL);                 /* modal, on lv_layer_top() */
      lv_msgbox_add_title(s_confirm, "Change the access point?");
      lv_msgbox_add_text(s_confirm, "Changing the AP drops every phone connected to it.");
      lv_obj_t *go = lv_msgbox_add_footer_button(s_confirm, "Save anyway");
      lv_obj_add_event_cb(go, ev_confirm, LV_EVENT_CLICKED, (void *)(intptr_t)1);
      lv_obj_t *no = lv_msgbox_add_footer_button(s_confirm, "Cancel");
      lv_obj_add_event_cb(no, ev_confirm, LV_EVENT_CLICKED, (void *)(intptr_t)0);
  }
  static void master_save(void) {
      if (s_saving || !s_open || s_confirm) return;
      int n = pcfg_edits_export(s_medits, s_mfedits, PCFG_EDIT_MAX);
      if (n <= 0) { cfg_set_status("No changes to save", 0); return; }
      psvc_fedits_t fe = { s_mfedits, n };
      char err[96];
      err[0] = '\0';
      psvc_mcfg_get(s_scratch);                     /* fresh: the display copy has no secrets */
      int frc = psvc_mcfg_fields_fn(s_scratch, &fe, err, sizeof err);
      int vrc = frc == 0 ? hg_mcfg_validate(s_scratch, tz_check, err, sizeof err) : 0;
      pnl_zero(s_scratch, sizeof *s_scratch);
      if (frc != 0 || vrc != 0) {
          pnl_zero(s_mfedits, PCFG_EDIT_MAX * sizeof *s_mfedits);
          cfg_show_error(&s_mview, err, psvc_rc_token(frc != 0 ? PSVC_E_INVALID_FIELD : PSVC_E_VALIDATION));
          return;
      }
      s_pending_n = n;
      if (touches_ap(s_mfedits, n)) { ask_ap_confirm(); return; }
      submit_save();
  }

  /* ---------- the editor's life ---------- */
  void cfg_master_open(lv_obj_t *body) {
      cfg_set_title("Config -- Master");
      if (!buffers_ok()) {
          ESP_LOGW(TAG, "no PSRAM for the master editor");
          lv_label_set_text(lv_label_create(body), "Out of memory (PSRAM) -- the editor cannot open.");
          return;
      }
      s_open = 1;
      s_rerender_due = 0;
      take_copy();
      s_mview = (cfg_view_t){ .table = PCFG_TABLE_MASTER, .zone = 0, .value = master_value, .ctx = NULL,
                              .hw_or_null = NULL, .edits = s_medits, .reveal = master_reveal };
      cfg_frame_build(body, &s_mview, master_save);
      s_ui_saving = s_saving;
      cfg_frame_set_saving(s_saving);
      if (!s_saving && s_mlast[0]) { cfg_set_status(s_mlast, s_last_err); s_mlast[0] = '\0'; }
  }

  void cfg_master_close(void) {
      if (s_confirm) {
          lv_msgbox_close(s_confirm);
          s_confirm = NULL;
          if (!s_saving && s_mfedits) pnl_zero(s_mfedits, PCFG_EDIT_MAX * sizeof *s_mfedits);
      }
      if (wdg_keyboard_is_open()) wdg_keyboard_close();
      s_open = 0;
      s_rerender_due = 0;
  }

  void cfg_master_update(const pnl_snap_t *s) {
      (void)s;
      if (!s_open) return;
      if (s_ui_saving != s_saving) {                /* a save that finished while this screen was rebuilt */
          s_ui_saving = s_saving;
          cfg_frame_set_saving(s_saving);
          if (!s_saving && s_mlast[0]) { cfg_set_status(s_mlast, s_last_err); s_mlast[0] = '\0'; }
      }
      if (s_rerender_due && !wdg_keyboard_is_open()) { s_rerender_due = 0; cfg_frame_rerender(); }
  }

  void cfg_master_wipe(void) {
      if (s_medits) pcfg_edits_wipe(s_medits);
      if (s_m) take_copy();                          /* keeps the no-secret invariant; drops nothing else sensitive */
      if (!s_saving && s_mfedits) pnl_zero(s_mfedits, PCFG_EDIT_MAX * sizeof *s_mfedits);
      if (s_scratch) pnl_zero(s_scratch, sizeof *s_scratch);
      s_mlast[0] = '\0';
  }
  ```

- [ ] **Step 2: Route zone 0 in `components/panel_ui/scr_config.c`**

  Replace everything from the line `/* ---------- editor routing (Task 21 replaces this whole block to add the master) ---------- */`
  through the line `/* ---------- end of editor routing ---------- */`, both included, with:
  ```c
  /* ---------- editor routing: the master (zone 0) and zones 1..8 ---------- */
  static int master_enabled(void) { return 1; }
  static void close_editor(void) {
      if (s_ui.open_zone >= 1) cfg_zone_close();
      else if (s_ui.open_zone == 0) cfg_master_close();
      s_ui.open_zone = -1;
      s_fr.v = NULL; s_fr.list = NULL; s_fr.tabs = NULL; s_fr.idxsel = NULL; s_fr.save = NULL; s_fr.save_lbl = NULL;
      s_fr.n_rows = 0;
  }
  static void open_editor(int zone) {
      close_editor();
      if (wdg_keyboard_is_open()) wdg_keyboard_close();
      lv_obj_clean(s_ui.body);
      cfg_set_status("", 0);
      s_ui.open_zone = zone;
      picker_check(zone);
      s_last_zone = zone;
      if (zone >= 1) cfg_zone_open(s_ui.body, (uint8_t)zone);
      else cfg_master_open(s_ui.body);
  }
  static int pick_target(const pnl_snap_t *s, int arg) {
      if (arg == 0) return 0;
      if (arg >= 1 && zone_used(s, arg)) return arg;
      if (s_last_zone == 0) return 0;
      if (s_last_zone >= 1 && zone_used(s, s_last_zone)) return s_last_zone;
      for (int z = 1; z <= HG_MAX_ZONES; z++) if (zone_used(s, z)) return z;
      return 0;                                   /* nothing enrolled: the master is always there */
  }
  static void ev_pick(lv_event_t *e) {
      (void)e;
      uint32_t sel = lv_buttonmatrix_get_selected_button(s_ui.picker);
      if (sel >= (uint32_t)s_ui.n_pick) return;
      int z = s_ui.zone_of[sel];
      if (z == s_ui.open_zone) { picker_check(z); return; }
      open_editor(z);
  }
  static void route_update(const pnl_snap_t *snap) {
      if (s_ui.open_zone >= 1) cfg_zone_update(snap);
      else if (s_ui.open_zone == 0) cfg_master_update(snap);
      else open_editor(pick_target(snap, -1));
  }
  /* ---------- end of editor routing ---------- */
  ```

- [ ] **Step 3: Add the source and the requirement**

  In `components/panel_ui/CMakeLists.txt`, inside the gating `if()` block, add:
  ```cmake
      list(APPEND PANEL_SRCS "cfg_master.c")                  # Task 21: the master config editor
  ```
  and add `time_core` to the component's `PRIV_REQUIRES` list (for `tz_check`). `time_core` is a plain component
  that exists in every app build, so the early requirements pass stays valid for zone and rescue.

- [ ] **Step 4: GATE-HOST, GATE-P4, GATE-ESP32**

  Run GATE-HOST. Expected: `100% tests passed, 0 tests failed out of 47` (+0).
  Run GATE-P4. Expected: `Project build complete`, no `warning:` lines, target matched, lock restored.
  Run GATE-ESP32. Expected: three `Project build complete` and an empty lock diff.

- [ ] **Step 5: Flash and check on glass**

  ```powershell
  python C:\Projects\HillGrov\tools\flash_app.py --app master --target esp32p4 --build-dir C:\Projects\HillGrov\master\build_p4 --port COM28 --dry-run
  python C:\Projects\HillGrov\tools\flash_app.py --app master --target esp32p4 --build-dir C:\Projects\HillGrov\master\build_p4 --port COM28
  ```
  (After the dry run, confirm every printed path says `build_p4` before running the second line.)
  Run Stage 2 gate step 7. Then run the whole Stage 2 gate (below).

- [ ] **Step 6: Commit and push**

  ```powershell
  git -C C:\Projects\HillGrov add components/panel_ui/cfg_master.c components/panel_ui/scr_config.c components/panel_ui/CMakeLists.txt
  git -C C:\Projects\HillGrov status --short
  ```
  Expected: those paths staged; ` M docs/hillgrow-features.drawio` unstaged.
  ```powershell
  git -C C:\Projects\HillGrov commit -m "feat(panel_ui): master config editor with masked secrets" -m "Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>"
  git -C C:\Projects\HillGrov push
  ```

**Stage 2 gate: run it now.**

---

### Stage 2 bench gate (owner, about 45 min): the type-driven config editor

**Setup:**
- The P4 master is on COM28, running a GATE-P4 build of HEAD (after Task 21), flashed with:
  ```powershell
  python C:\Projects\HillGrov\tools\flash_app.py --app master --target esp32p4 --build-dir C:\Projects\HillGrov\master\build_p4 --port COM28 --dry-run
  python C:\Projects\HillGrov\tools\flash_app.py --app master --target esp32p4 --build-dir C:\Projects\HillGrov\master\build_p4 --port COM28
  ```
  Every dry-run path must say `build_p4`.
- Zones 1 and 2 are on the ring and ONLINE. Zone 2 has synced at least once.
- A phone is on the `HillGrow` AP and logged in to `http://192.168.7.7` (the bench's current web password).
- The master console is open in one foreground terminal:
  `C:\Python311\python -m serial.tools.miniterm --dtr 0 --rts 0 COM28 115200`. Exit with Ctrl+]. Close it before
  any `uart_test.py` or flash.
- Before starting, note the internal heap free/min from the Diag screen (reached through the Panel icon until
  Task 26).

Every step is done with a finger on the glass. A step fails on any mismatch; record the actual text.

1. **Tabs and the read-only hardware plane.** Home → HillGrow → Config on the rail → pick zone 2.
   - Pass:
     - The title reads `Config -- Zone 2 (gen G)`, with G equal to the web's Config heading for zone 2.
     - The tabs read ZONECFG SHELF LIGHT WATER FAN, then VIB AUX HW HWSHELF CAL.
     - HW, HWSHELF and CAL show "hardware plane -- set at the zone console" above rows that have no editor, in
       muted text.
     - HW shows `PCA9685 I2C address 0x40`, `PCF8575 I2C address 0x20` and `PCF8575 active-low mask 0xFFFF`
       (or the zone's real values, in hex).
     - HWSHELF, each shelf 0..3: any pin that `GET ZONE 2 HWSHELF <n>` (n = index + 1) on the console prints as
       `NONE` shows `none`.
   - Fail: a tab out of order, any editor on a hardware row, a decimal address, or `255` for a pin.
2. **Stepper save and push.** WATER tab, shelf index 0 (the console's shelf 1). Note the current "Target moisture"
   value V. Use [+]/[-] to reach 55 and tap Save.
   - Pass:
     - The button reads "Saving..." and then the status reads `Queued, pushing to zone`.
     - Within about 10 s the status reads `Landed on the zone.`.
     - The console `GET ZONE 2 WATER 1` shows `Target : 55`.
     - After about 3 s the title's gen has gone up by 1.
   - Restore V the same way and confirm `Target : V` on the console.
3. **Validation, highlighted.** LIGHT tab, shelf index 1. Set "Lights off" equal to "Lights on" with the rollers
   and tap Save.
   - Pass:
     - Nothing is sent: the console shows no `CFG` traffic and the title's gen is unchanged.
     - The status reads `VALIDATION: Lights off (shelf 1)`.
     - The "Lights off" row on the LIGHT tab, shelf 1, is outlined red with `VALIDATION` under its label.
   - Move "Lights off" back to its old value. The row's dot stays (it is still an edit), and Save then succeeds.
4. **Input bounds.**
   - ZONECFG → "Zone name" → the keyboard opens. Tap the space bar: nothing is inserted. Clear the text: OK is
     disabled. Type a 16th character: it is not accepted. Cancel.
   - "Link-loss timeout": hold [++] until the value stops at 600; hold [--] until it stops at 10. Tap the value,
     type 700, OK → `Out of range (10..600)` under the label and the value is unchanged.
   - Pass: all four behaviours hold and no stepper passes its min or max.
5. **Offline is not busy.** Hold zone 2 in reset (EN low) until its Dashboard badge reads OFFLINE (about 10 s).
   On Config zone 2, change any field and tap Save.
   - Pass: the status reads `Zone is offline -- nothing was saved`. It does not read `Zone busy, retry`.
   - Release zone 2 and wait for ONLINE. Save again: `Queued, pushing to zone`, then `Landed on the zone.`.
6. **The two faces collide the way the web does.** On the phone, open Config zone 2 and type a new WATER Target
   without saving. On the panel, change a field of zone 2 and tap Save; within 2 s tap Save on the phone.
   - Pass: the phone shows `Zone busy, retry` (409 BUSY), and the panel's save lands.
   - Restore both values.
7. **The master, secrets masked** (run after Task 21). Picker → Master.
   - Tabs: WIFI, TIME, SYS; there is no WEB tab. Both password rows show `********` with [Reveal] and [Change].
   - TIME → "NTP server" → `time.google.com` → Save → `Saved.`. On the phone, the web Config for the master shows
     `time.google.com`. Restore the old value → `Saved.`.
   - SYS → "Hostname" → change it (for example to `hillgrow2`) → Save → `Saved.`.
     - Pass: within about 30 s `netsh wlan show interfaces` on the bench PC, and the master's STA line on the
       Dashboard, show STA re-joined to the house network. That proves the blank STA_PASS kept the stored password.
     - Restore the hostname.
   - WIFI → "Wi-Fi password" → [Reveal]. Pass: the stored password shows in clear, the button reads "Hide", and
     after 10 s it re-masks by itself. [Reveal] again, then tap HillGrow → Home and come back: it is masked.
   - WIFI → "Access point name" → change one character → Save. Pass: the dialog reads "Changing the AP drops every
     phone connected to it." → [Cancel] → `Not saved.`, and the phone stays connected. Undo the edit.
8. **A zone that never synced.** This needs a zone with no cached config, for example a spare zone board freshly
   enrolled for the first time.
   - Pass: Config for it shows `Zone config not adopted yet -- the zone must come online and sync at least once
     before it can be configured.` with [Retry]. When the zone comes ONLINE and syncs, the editor loads by itself,
     once.
   - If no such board is available, record "not exercised". The NO_CACHE text is host-pinned by `test_pnl_msg` and
     the refusal by `test_psvc_zcfg`, so the gap is only the on-glass rendering.
9. **The web is unchanged.** Close the miniterm. With the panel on Config, run:
   ```powershell
   python C:\Projects\HillGrov\tools\web_test.py 192.168.7.7 --password hillgrow1 --only config,mcfg,state
   ```
   (Use the bench's current password if it differs.)
   - Pass: every line PASS, and the last line reads `N/N passed`.
10. **Memory.** Diag screen again.
    - Pass: internal heap min ≥ 64 KB.
    - Record the internal free/min before and after, and `lv_mem` max as the Stage 1 gate did. If `lv_mem` max is
      above 75 % of the internal pool, apply the Global Constraints' 128 KB rule in the next task.

**Stage 2 passes** when steps 1-7, 9 and 10 pass, and step 8 either passes or is recorded as not exercised with
the reason. Record the results in the Task 21 report.

---

## Stage 3 — System functions, Panel settings and night dimming

### Task 22: Panel command session; zone console; Replace board (Stage 3)

The web's console and Replace board both run CLI lines through `POST /api/cmd`, which is `http_cmd.c`: two
`cmd_session_t` slots with `source = CMD_SRC_HTTP`, a 4000 ms wait, and permanent quarantine of a slot whose
`cmd_task_execute()` returned -2 (`components/http_srv/http_cmd.c:21-99`). The panel gets its own copy of that shape
(`pnl_cmd`), a pure console model (`pnl_console`, host-tested), and two zone-view sections. Two small glue files are
added so later tasks do not each grow their own card, button and confirm-dialog code (`pnl_ui_kit`), and so the zone
sections have an LVGL-typed header (`zone_sections.h`).

**Files:**
- Create: `components/panel_ui/pnl_console.h`, `components/panel_ui/pnl_console.c` (P)
- Create: `components/panel_ui/pnl_cmd.h`, `components/panel_ui/pnl_cmd.c` (G)
- Create: `components/panel_ui/pnl_ui_kit.h`, `components/panel_ui/pnl_ui_kit.c` (G, addition)
- Create: `components/panel_ui/zone_sections.h` (G, addition), `components/panel_ui/zone_console.c`, `components/panel_ui/zone_replace.c` (G)
- Create: `tests/host/test_pnl_console.c`
- Modify: `components/panel_ui/scr_zone.c` (build both sections into `scr_zone_extra_area()`; tear them down)
- Modify: `components/panel_ui/pnl_poll.c` (`panel_cmd_quarantined = pnl_cmd_quarantined()`)
- Modify: `components/panel_ui/scr_dashboard.c` ("N panel console slot(s) degraded")
- Modify: `components/panel_ui/panel_ui.c` (`pnl_cmd_init()` in `panel_services_start()`)
- Modify: `components/panel_ui/CMakeLists.txt`, `tests/host/CMakeLists.txt`

**Interfaces:**
- Consumes: `cmd_task_execute` (`components/cmd_task/cmd_task.h:24`); `cmd_session_t`, `CMD_SRC_HTTP`, `CMD_LINE_MAX` (192)
  and `CMD_RESP_MAX` (4096) (`components/cmd_core/cmd_core.h:8-11,14,34`); `pnl_mac_parse` and `wdg_keyboard_open` (Task 19);
  `PCFG_KB_TEXT`, `PCFG_KB_HEX` (Task 17); `pnl_worker_submit`, `pnl_job_t`, `pnl_screen_gen`, `pnl_on_lvgl_task`,
  `PNL_JOB_OUT_MAX` (Task 8); `scr_zone_extra_area`, `scr_zone_current` (Task 14); `pnl_snap_t.panel_cmd_quarantined`
  (Task 11); `pnl_label_set_if_changed` (Task 12); `PNL_C_*` (Task 7).
- Produces:
  ```c
  /* pnl_cmd.h */
  #define PNL_CMD_SLOTS      2
  #define PNL_CMD_TIMEOUT_MS 4000u   /* > the 3500 ms forward budget (http_cmd.c:23-30) */
  void    pnl_cmd_init(void);        /* slots: CMD_SRC_HTTP semantics, own CMD_RESP_MAX buffers, 100 ms claim mutex */
  int     pnl_cmd_run(const char *line, char *reply, size_t cap);
          /* [WORKER] 0 OK / -1 ERR (reply = the ERR line) / -2 orphaned (slot quarantined forever, reply "ERR INTERNAL") /
             -3 no free slot (reply "ERR BUSY") / -4 line empty or > CMD_LINE_MAX-1 (reply "ERR TOO_LONG"); the slot's own
             buffer is what cmd_task writes -- reply is a copy, so an orphan can never scribble on caller memory */
  uint8_t pnl_cmd_quarantined(void); /* [ANY] */

  /* pnl_console.h (pure) */
  #define PNL_CON_LOG  20
  #define PNL_CON_HIST 20
  typedef struct { char sent[CMD_LINE_MAX]; char *reply; uint8_t pending, used; } pnl_con_entry_t;  /* reply -> CMD_RESP_MAX */
  typedef struct { pnl_con_entry_t log[PNL_CON_LOG]; int head, n_log; char hist[PNL_CON_HIST][CMD_LINE_MAX]; int n_hist, hist_pos;
                   uint8_t forward; char draft[CMD_LINE_MAX]; } pnl_console_t;
  void pnl_con_init(pnl_console_t *c, char (*reply_store)[CMD_RESP_MAX]);   /* PNL_CON_LOG buffers (PSRAM, caller-owned) */
  int  pnl_con_forward(const char *line, uint8_t zone, char *out, size_t cap);
  int  pnl_con_line_ok(const char *line);
  pnl_con_entry_t *pnl_con_push(pnl_console_t *c, const char *sent);
  void pnl_con_reply(pnl_con_entry_t *e, const char *reply);
  const char *pnl_con_hist_prev(pnl_console_t *c);
  const char *pnl_con_hist_next(pnl_console_t *c);   /* "" past the newest */
  void pnl_con_wipe(pnl_console_t *c);

  /* zone_console.c / zone_replace.c (glue) -- declared in zone_sections.h */
  void zone_console_build(lv_obj_t *parent, uint8_t zone);
  void zone_console_teardown(void);
  void zone_console_wipe_all(void);                           /* Task 27 */
  void zone_replace_build(lv_obj_t *parent, uint8_t zone);
  void zone_replace_wipe(void);                               /* Task 27 */
  ```
- Produces **(additions)**:
  ```c
  /* pnl_console.h */
  void pnl_con_hist_add(pnl_console_t *c, const char *line);            /* the ORIGINAL line (app.js:1655); pnl_con_push
                                                                           stores the line actually sent */
  const pnl_con_entry_t *pnl_con_log_at(const pnl_console_t *c, int i); /* 0 = oldest .. n_log-1 = newest; NULL outside */
  /* zone_sections.h */
  void zone_replace_teardown(void);
  /* pnl_ui_kit.h (glue): the widgets every System/Panel section is made of */
  typedef enum { PNL_KIT_OK = 0, PNL_KIT_ERR, PNL_KIT_INFO } pnl_kit_tone_t;
  lv_obj_t *pnl_kit_card(lv_obj_t *parent, const char *title);            /* column card, title in Montserrat 28 */
  lv_obj_t *pnl_kit_row(lv_obj_t *parent);                                /* transparent wrapping row */
  lv_obj_t *pnl_kit_button(lv_obj_t *parent, const char *text, lv_event_cb_t cb, void *ud);   /* 56 px tall */
  void      pnl_kit_enable(lv_obj_t *obj, int on);                        /* LV_STATE_DISABLED off/on; NULL-safe */
  lv_obj_t *pnl_kit_field(lv_obj_t *parent, const char *caption, lv_event_cb_t cb, void *ud);  /* returns the value label */
  lv_obj_t *pnl_kit_msg(lv_obj_t *parent);                                /* wrapping status label */
  void      pnl_kit_msg_set(lv_obj_t *lbl, const char *text, pnl_kit_tone_t tone);           /* NULL-safe */
  typedef void (*pnl_confirm_fn)(void *ctx);
  void      pnl_confirm(const char *title, const char *text, const char *ok_label, pnl_confirm_fn on_ok, void *ctx);
            /* [LVGL] modal msgbox on the top layer, Cancel + ok_label; on_ok runs after the box is closed */
  void      pnl_confirm_close(void);                                      /* [LVGL] Task 27's idle wipe */
  ```

**What the host test proves, and what only glass can:** `test_pnl_console` pins the console model (forward rewrite,
length rule, log ring, history, wipe). `pnl_cmd.c`, the two sections and the kit are target glue: GATE-P4 proves they
compile against LVGL 9.5.0 and the real `cmd_task`; the Stage 3 gate steps 1-2 prove the behaviour on glass.

- [ ] **Step 1: Write the failing test**

Create `tests/host/test_pnl_console.c`:
```c
#include <stdio.h>
#include <string.h>
#include "unity.h"
#include "pnl_console.h"

/* pnl_console is pure: the zone console's model. The web's console (app.js:582-590, 1647-1682) is the reference:
   forward rewrite "VERB rest" -> "VERB ZONE <z> rest" unless token 2 is already ZONE, a 20-entry sent/reply log,
   history of the ORIGINAL lines walked with prev/next, and a wipe for the idle operator-state rule (D18). */

static char g_store[PNL_CON_LOG][CMD_RESP_MAX];
static pnl_console_t g_c;

void setUp(void) { pnl_con_init(&g_c, g_store); }
void tearDown(void) {}

static void fill(char *s, char ch, int n) { memset(s, ch, (size_t)n); s[n] = '\0'; }

static void test_forward_plain(void) {
    char out[CMD_LINE_MAX];
    TEST_ASSERT_EQUAL_INT(0, pnl_con_forward("GET WATER 1", 2, out, sizeof out));
    TEST_ASSERT_EQUAL_STRING("GET ZONE 2 WATER 1", out);
}

static void test_forward_already_addressed_is_unchanged(void) {
    char out[CMD_LINE_MAX];
    TEST_ASSERT_EQUAL_INT(0, pnl_con_forward("SET ZONE 2 WATER 1 TARGET 55", 3, out, sizeof out));
    TEST_ASSERT_EQUAL_STRING("SET ZONE 2 WATER 1 TARGET 55", out);
    TEST_ASSERT_EQUAL_INT(0, pnl_con_forward("get zone 3 id", 2, out, sizeof out));   /* case-insensitive, like app.js */
    TEST_ASSERT_EQUAL_STRING("get zone 3 id", out);
}

static void test_forward_single_token(void) {
    char out[CMD_LINE_MAX];
    TEST_ASSERT_EQUAL_INT(0, pnl_con_forward("GET", 5, out, sizeof out));
    TEST_ASSERT_EQUAL_STRING("GET ZONE 5", out);
}

static void test_forward_collapses_whitespace(void) {
    char out[CMD_LINE_MAX];
    TEST_ASSERT_EQUAL_INT(0, pnl_con_forward("GET   WATER\t1", 2, out, sizeof out));
    TEST_ASSERT_EQUAL_STRING("GET ZONE 2 WATER 1", out);
}

static void test_forward_overflow_limit(void) {
    char line[256], out[CMD_LINE_MAX];
    memcpy(line, "GET ", 4); fill(line + 4, 'A', 176);          /* 180 chars -> "GET ZONE 2 " + 176 = 187: fits */
    TEST_ASSERT_EQUAL_INT(0, pnl_con_forward(line, 2, out, sizeof out));
    TEST_ASSERT_EQUAL_size_t(187, strlen(out));
    memcpy(line, "GET ", 4); fill(line + 4, 'A', 184);          /* 188 chars -> 195: over CMD_LINE_MAX-1 */
    TEST_ASSERT_EQUAL_INT(-1, pnl_con_forward(line, 2, out, sizeof out));
}

static void test_line_ok_bounds(void) {
    char s[256];
    TEST_ASSERT_EQUAL_INT(0, pnl_con_line_ok(""));
    TEST_ASSERT_EQUAL_INT(0, pnl_con_line_ok("   \r\n"));
    fill(s, 'x', 191); TEST_ASSERT_EQUAL_INT(1, pnl_con_line_ok(s));
    fill(s, 'x', 192); TEST_ASSERT_EQUAL_INT(0, pnl_con_line_ok(s));
    fill(s, 'x', 191); strcat(s, "  \r\n"); TEST_ASSERT_EQUAL_INT(1, pnl_con_line_ok(s));  /* trailing trimmed first */
}

static void test_log_ring_wraps_at_21(void) {
    char sent[16];
    for (int i = 0; i < 21; i++) {
        snprintf(sent, sizeof sent, "L%d", i);
        pnl_con_entry_t *e = pnl_con_push(&g_c, sent);
        TEST_ASSERT_NOT_NULL(e);
        pnl_con_reply(e, "OK\n");
    }
    TEST_ASSERT_EQUAL_INT(PNL_CON_LOG, g_c.n_log);
    TEST_ASSERT_EQUAL_STRING("L1", pnl_con_log_at(&g_c, 0)->sent);      /* L0 fell off */
    TEST_ASSERT_EQUAL_STRING("L20", pnl_con_log_at(&g_c, 19)->sent);
    TEST_ASSERT_NULL(pnl_con_log_at(&g_c, 20));
    TEST_ASSERT_NULL(pnl_con_log_at(&g_c, -1));
}

static void test_push_never_overwrites_a_pending_entry(void) {
    pnl_con_entry_t *first = pnl_con_push(&g_c, "FIRST");               /* left pending: the worker owns it */
    for (int i = 1; i < PNL_CON_LOG; i++) pnl_con_reply(pnl_con_push(&g_c, "X"), "OK\n");
    TEST_ASSERT_NULL(pnl_con_push(&g_c, "WOULD-WRAP-ONTO-FIRST"));
    TEST_ASSERT_EQUAL_STRING("FIRST", first->sent);
    TEST_ASSERT_EQUAL_UINT8(1, first->pending);
}

static void test_reply_is_verbatim_and_clipped(void) {
    static char big[5000];
    pnl_con_entry_t *e = pnl_con_push(&g_c, "GET ID");
    TEST_ASSERT_EQUAL_UINT8(1, e->pending);
    pnl_con_reply(e, "OK ID MASTER 0\n");
    TEST_ASSERT_EQUAL_STRING("OK ID MASTER 0\n", e->reply);
    TEST_ASSERT_EQUAL_UINT8(0, e->pending);
    fill(big, 'r', 4999);
    pnl_con_reply(e, big);
    TEST_ASSERT_EQUAL_size_t(CMD_RESP_MAX - 1, strlen(e->reply));
}

static void test_history_prev_next_ends(void) {
    TEST_ASSERT_NULL(pnl_con_hist_prev(&g_c));                           /* empty: nothing to recall */
    pnl_con_hist_add(&g_c, "GET ID");
    pnl_con_hist_add(&g_c, "GET WATER 1");
    TEST_ASSERT_EQUAL_STRING("GET WATER 1", pnl_con_hist_prev(&g_c));
    TEST_ASSERT_EQUAL_STRING("GET ID", pnl_con_hist_prev(&g_c));
    TEST_ASSERT_EQUAL_STRING("GET ID", pnl_con_hist_prev(&g_c));         /* stops at the oldest */
    TEST_ASSERT_EQUAL_STRING("GET WATER 1", pnl_con_hist_next(&g_c));
    TEST_ASSERT_EQUAL_STRING("", pnl_con_hist_next(&g_c));               /* past the newest */
    TEST_ASSERT_EQUAL_STRING("", pnl_con_hist_next(&g_c));
}

static void test_history_keeps_the_newest_20(void) {
    char s[16];
    for (int i = 0; i < 21; i++) { snprintf(s, sizeof s, "H%d", i); pnl_con_hist_add(&g_c, s); }
    TEST_ASSERT_EQUAL_INT(PNL_CON_HIST, g_c.n_hist);
    TEST_ASSERT_EQUAL_STRING("H1", g_c.hist[0]);
    TEST_ASSERT_EQUAL_STRING("H20", g_c.hist[PNL_CON_HIST - 1]);
}

static void test_wipe_clears_everything_idle(void) {
    pnl_con_reply(pnl_con_push(&g_c, "SET WIFI STA house secretpass"), "OK WIFI STA\n");
    pnl_con_hist_add(&g_c, "SET WIFI STA house secretpass");
    strcpy(g_c.draft, "SET WIFI STA other pw");
    g_c.forward = 1;
    pnl_con_wipe(&g_c);
    TEST_ASSERT_EQUAL_INT(0, g_c.n_log);
    TEST_ASSERT_EQUAL_INT(0, g_c.n_hist);
    TEST_ASSERT_EQUAL_UINT8(0, g_c.forward);
    TEST_ASSERT_EQUAL_STRING("", g_c.draft);
    for (int i = 0; i < PNL_CON_LOG; i++) {
        TEST_ASSERT_EQUAL_STRING("", g_c.log[i].sent);
        TEST_ASSERT_EQUAL_STRING("", g_c.log[i].reply);
    }
    TEST_ASSERT_NULL(strstr(g_store[0], "secretpass"));
}

static void test_wipe_leaves_a_pending_entry_to_the_worker(void) {
    pnl_con_entry_t *e = pnl_con_push(&g_c, "GET ZONE 2 WATER 1");
    pnl_con_wipe(&g_c);
    TEST_ASSERT_EQUAL_STRING("GET ZONE 2 WATER 1", e->sent);
    TEST_ASSERT_EQUAL_UINT8(1, e->pending);
    pnl_con_reply(e, "OK\n");
    TEST_ASSERT_EQUAL_STRING("OK\n", e->reply);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_forward_plain);
    RUN_TEST(test_forward_already_addressed_is_unchanged);
    RUN_TEST(test_forward_single_token);
    RUN_TEST(test_forward_collapses_whitespace);
    RUN_TEST(test_forward_overflow_limit);
    RUN_TEST(test_line_ok_bounds);
    RUN_TEST(test_log_ring_wraps_at_21);
    RUN_TEST(test_push_never_overwrites_a_pending_entry);
    RUN_TEST(test_reply_is_verbatim_and_clipped);
    RUN_TEST(test_history_prev_next_ends);
    RUN_TEST(test_history_keeps_the_newest_20);
    RUN_TEST(test_wipe_clears_everything_idle);
    RUN_TEST(test_wipe_leaves_a_pending_entry_to_the_worker);
    return UNITY_END();
}
```
In `tests/host/CMakeLists.txt`, after the last `hg_test(...)` row that Task 21 left, add:
```cmake
hg_test(test_pnl_console ${COMP}/panel_ui/pnl_console.c)
```
(`${COMP}/panel_ui` and `${COMP}/cmd_core` are already in `include_directories`: Task 7 added the first, the second is
original.)

- [ ] **Step 2: Run the test to verify it fails**

Run GATE-HOST with ` -R test_pnl_console`.
Expected: the CMake configure fails with `Cannot find source file: .../components/panel_ui/pnl_console.c`.

- [ ] **Step 3: Write the pure console model**

Create `components/panel_ui/pnl_console.h`:
```c
#pragma once
/* Pure (host-tested, tests/host/test_pnl_console.c): the zone console's model -- the forward rewrite (web/app.js:582-590),
 * the 20-entry sent/reply log, the history of ORIGINAL lines and the idle operator-state wipe (D18). No LVGL, no IDF.
 * Ownership rule: an entry with pending != 0 belongs to the panel worker (its sent line and reply buffer are being used
 * by a job) until the job's done() calls pnl_con_reply(); nothing here writes into a pending entry. */
#include <stddef.h>
#include <stdint.h>
#include "cmd_core.h"   /* CMD_LINE_MAX, CMD_RESP_MAX -- a pure header */

#ifdef __cplusplus
extern "C" {
#endif

#define PNL_CON_LOG  20
#define PNL_CON_HIST 20
typedef struct { char sent[CMD_LINE_MAX]; char *reply; uint8_t pending, used; } pnl_con_entry_t;  /* reply -> CMD_RESP_MAX */
typedef struct { pnl_con_entry_t log[PNL_CON_LOG]; int head, n_log; char hist[PNL_CON_HIST][CMD_LINE_MAX]; int n_hist, hist_pos;
                 uint8_t forward; char draft[CMD_LINE_MAX]; } pnl_console_t;

void pnl_con_init(pnl_console_t *c, char (*reply_store)[CMD_RESP_MAX]);   /* PNL_CON_LOG buffers (PSRAM, caller-owned) */
int  pnl_con_forward(const char *line, uint8_t zone, char *out, size_t cap);
     /* app.js:582-590: split on whitespace; tok2 == "ZONE" (case-insensitive) -> unchanged; else "VERB ZONE <z> rest"
        joined with single spaces; 0 / -1 result would exceed CMD_LINE_MAX-1 (or cap-1) */
int  pnl_con_line_ok(const char *line);          /* after trimming trailing CR/LF/space: 1..CMD_LINE_MAX-1 bytes */
pnl_con_entry_t *pnl_con_push(pnl_console_t *c, const char *sent);
     /* newest entry, pending; NULL when the slot it would reuse is still pending (never overwrites the worker's buffer) */
void pnl_con_hist_add(pnl_console_t *c, const char *line);            /* the ORIGINAL line; keeps the newest PNL_CON_HIST */
const pnl_con_entry_t *pnl_con_log_at(const pnl_console_t *c, int i); /* 0 = oldest .. n_log-1 = newest; NULL outside */
void pnl_con_reply(pnl_con_entry_t *e, const char *reply);           /* verbatim, clipped to CMD_RESP_MAX-1; clears pending */
const char *pnl_con_hist_prev(pnl_console_t *c);   /* NULL when there is no history */
const char *pnl_con_hist_next(pnl_console_t *c);   /* "" past the newest */
void pnl_con_wipe(pnl_console_t *c);              /* memset log/history/draft; pending entries are left to the worker */

#ifdef __cplusplus
}
#endif
```
Create `components/panel_ui/pnl_console.c`:
```c
#include <string.h>
#include "pnl_console.h"

static void copy_clip(char *dst, size_t cap, const char *src) {
    size_t n = src ? strlen(src) : 0;
    if (n > cap - 1) n = cap - 1;
    if (n) memcpy(dst, src, n);
    dst[n] = '\0';
}

static int is_ws(char ch) {
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' || ch == '\f' || ch == '\v';
}

static char up(char ch) { return (ch >= 'a' && ch <= 'z') ? (char)(ch - 'a' + 'A') : ch; }

void pnl_con_init(pnl_console_t *c, char (*reply_store)[CMD_RESP_MAX]) {
    memset(c, 0, sizeof *c);
    for (int i = 0; i < PNL_CON_LOG; i++) {
        c->log[i].reply = reply_store[i];
        reply_store[i][0] = '\0';
    }
}

int pnl_con_forward(const char *line, uint8_t zone, char *out, size_t cap) {
    if (!line || !out || cap == 0) return -1;
    const size_t line_max = (size_t)(CMD_LINE_MAX - 1);
    size_t limit = cap - 1 < line_max ? cap - 1 : line_max;
    const char *p = line;
    while (*p && is_ws(*p)) p++;
    const char *t0 = p;
    while (*p && !is_ws(*p)) p++;
    size_t l0 = (size_t)(p - t0);
    const char *q = p;
    while (*q && is_ws(*q)) q++;
    const char *t1 = q;
    while (*q && !is_ws(*q)) q++;
    size_t l1 = (size_t)(q - t1);
    int addressed = l1 == 4 && up(t1[0]) == 'Z' && up(t1[1]) == 'O' && up(t1[2]) == 'N' && up(t1[3]) == 'E';
    if (l0 == 0 || addressed) {                 /* no tokens, or already "VERB ZONE n ...": unchanged (app.js) */
        size_t n = strlen(line);
        if (n > limit) return -1;
        memcpy(out, line, n + 1);
        return 0;
    }
    char buf[2 * CMD_LINE_MAX];
    int o = snprintf(buf, sizeof buf, "%.*s ZONE %u", (int)l0, t0, (unsigned)zone);
    if (o < 0 || (size_t)o >= sizeof buf) return -1;
    for (;;) {
        while (*p && is_ws(*p)) p++;
        if (!*p) break;
        const char *s = p;
        while (*p && !is_ws(*p)) p++;
        size_t k = (size_t)(p - s);
        if ((size_t)o + 1 + k >= sizeof buf) return -1;
        buf[o++] = ' ';
        memcpy(buf + o, s, k);
        o += (int)k;
        buf[o] = '\0';
    }
    if ((size_t)o > limit) return -1;
    memcpy(out, buf, (size_t)o + 1);
    return 0;
}

int pnl_con_line_ok(const char *line) {
    if (!line) return 0;
    size_t n = strlen(line);
    while (n > 0 && (line[n - 1] == '\r' || line[n - 1] == '\n' || line[n - 1] == ' ')) n--;
    return n >= 1 && n <= (size_t)(CMD_LINE_MAX - 1);
}

pnl_con_entry_t *pnl_con_push(pnl_console_t *c, const char *sent) {
    pnl_con_entry_t *e = &c->log[c->head];
    if (e->pending) return NULL;
    copy_clip(e->sent, sizeof e->sent, sent);
    e->reply[0] = '\0';
    e->pending = 1;
    e->used = 1;
    c->head = (c->head + 1) % PNL_CON_LOG;
    if (c->n_log < PNL_CON_LOG) c->n_log++;
    return e;
}

void pnl_con_hist_add(pnl_console_t *c, const char *line) {
    if (c->n_hist == PNL_CON_HIST) {
        memmove(c->hist[0], c->hist[1], (size_t)(PNL_CON_HIST - 1) * CMD_LINE_MAX);
        c->n_hist--;
    }
    copy_clip(c->hist[c->n_hist], CMD_LINE_MAX, line);
    c->n_hist++;
    c->hist_pos = c->n_hist;
}

const pnl_con_entry_t *pnl_con_log_at(const pnl_console_t *c, int i) {
    if (i < 0 || i >= c->n_log) return NULL;
    return &c->log[(c->head - c->n_log + i + 2 * PNL_CON_LOG) % PNL_CON_LOG];
}

void pnl_con_reply(pnl_con_entry_t *e, const char *reply) {
    if (!e) return;
    copy_clip(e->reply, CMD_RESP_MAX, reply);
    e->pending = 0;
}

const char *pnl_con_hist_prev(pnl_console_t *c) {
    if (c->n_hist == 0) return NULL;
    if (c->hist_pos > 0) c->hist_pos--;
    return c->hist[c->hist_pos];
}

const char *pnl_con_hist_next(pnl_console_t *c) {
    if (c->hist_pos < c->n_hist) c->hist_pos++;
    return c->hist_pos < c->n_hist ? c->hist[c->hist_pos] : "";
}

void pnl_con_wipe(pnl_console_t *c) {
    int pending = 0;
    for (int i = 0; i < PNL_CON_LOG; i++) {
        pnl_con_entry_t *e = &c->log[i];
        if (e->pending) { pending = 1; continue; }   /* the worker owns it until done() */
        memset(e->sent, 0, sizeof e->sent);
        memset(e->reply, 0, CMD_RESP_MAX);
        e->used = 0;
    }
    if (!pending) { c->head = 0; c->n_log = 0; }
    memset(c->hist, 0, sizeof c->hist);
    c->n_hist = 0;
    c->hist_pos = 0;
    memset(c->draft, 0, sizeof c->draft);
    c->forward = 0;
}
```

- [ ] **Step 4: Run the test to verify it passes**

Run GATE-HOST with ` -R test_pnl_console`. Expected: `13 Tests 0 Failures 0 Ignored`, CTest `1/1 ... Passed`.

- [ ] **Step 5: Write the panel command session**

Create `components/panel_ui/pnl_cmd.h`:
```c
#pragma once
/* The panel's own command sessions: http_cmd.c's shape (components/http_srv/http_cmd.c:21-99) with web semantics (D1):
 * source CMD_SRC_HTTP, echo 0, notify_mask 0, unlock_until_ms 0 -- NOT_LOCAL for CMDF_SESSION rows, no DEBUG ENABLE
 * from the glass. SSIDs and passwords never travel as lines (a line cannot carry spaces): the panel calls psvc_* for those. */
#include <stddef.h>
#include <stdint.h>

#define PNL_CMD_SLOTS      2
#define PNL_CMD_TIMEOUT_MS 4000u   /* > the 3500 ms forward budget (http_cmd.c:23-30) */
void    pnl_cmd_init(void);        /* slots: CMD_SRC_HTTP semantics, own CMD_RESP_MAX buffers, 100 ms claim mutex */
int     pnl_cmd_run(const char *line, char *reply, size_t cap);
        /* [WORKER] 0 OK / -1 ERR (reply = the ERR line) / -2 orphaned (slot quarantined forever, reply "ERR INTERNAL") /
           -3 no free slot (reply "ERR BUSY") / -4 line empty or > CMD_LINE_MAX-1 (reply "ERR TOO_LONG"); the slot's own
           buffer is what cmd_task writes -- reply is a copy, so an orphan can never scribble on caller memory */
uint8_t pnl_cmd_quarantined(void); /* [ANY] */
```
Create `components/panel_ui/pnl_cmd.c`:
```c
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "cmd_core.h"
#include "cmd_task.h"
#include "pnl_worker.h"
#include "pnl_cmd.h"

static const char *TAG = "pnl_cmd";

typedef struct {
    cmd_session_t ses;
    char          resp[CMD_RESP_MAX];   /* what cmd_task writes; never handed to a caller */
    uint8_t       busy;
    uint8_t       dead;                 /* quarantined: an orphaned cmd_task worker still owns resp */
} pnl_cmd_slot_t;

static pnl_cmd_slot_t   *s_slot;        /* PNL_CMD_SLOTS in PSRAM, never freed */
static SemaphoreHandle_t s_lock;
static volatile uint8_t  s_quarantined;

void pnl_cmd_init(void) {
    if (s_slot && s_lock) return;
    if (!s_slot) s_slot = heap_caps_calloc(PNL_CMD_SLOTS, sizeof *s_slot, MALLOC_CAP_SPIRAM);
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    if (!s_slot || !s_lock) {
        ESP_LOGE(TAG, "no memory for the panel command sessions -- console, Replace board, Set clock and Reboot unavailable");
        return;
    }
    for (int i = 0; i < PNL_CMD_SLOTS; i++) {
        s_slot[i].ses.source          = CMD_SRC_HTTP;   /* D1: the web's semantics exactly */
        s_slot[i].ses.echo            = 0;
        s_slot[i].ses.notify_mask     = 0;
        s_slot[i].ses.unlock_until_ms = 0;
    }
}

uint8_t pnl_cmd_quarantined(void) { return s_quarantined; }

static void put(char *reply, size_t cap, const char *s) {
    if (reply && cap) snprintf(reply, cap, "%s", s);
}

static void release(pnl_cmd_slot_t *slot) {
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        slot->busy = 0;
        xSemaphoreGive(s_lock);
        return;
    }
    slot->busy = 0;   /* a byte store either way; losing the slot would be worse (http_cmd.c's release) */
}

int pnl_cmd_run(const char *line, char *reply, size_t cap) {
    if (pnl_on_lvgl_task()) {
        ESP_LOGE(TAG, "pnl_cmd_run called on the LVGL task -- refused (the LVGL task must never block)");
        put(reply, cap, "ERR INTERNAL\n");
        return -1;
    }
    size_t n = line ? strlen(line) : 0;
    if (n == 0 || n > (size_t)(CMD_LINE_MAX - 1)) { put(reply, cap, "ERR TOO_LONG\n"); return -4; }
    if (!s_slot || !s_lock) { put(reply, cap, "ERR INTERNAL\n"); return -1; }

    pnl_cmd_slot_t *slot = NULL;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        for (int i = 0; i < PNL_CMD_SLOTS && !slot; i++)
            if (!s_slot[i].busy && !s_slot[i].dead) { s_slot[i].busy = 1; slot = &s_slot[i]; }
        xSemaphoreGive(s_lock);
    }
    if (!slot) { put(reply, cap, "ERR BUSY\n"); return -3; }

    int rc = cmd_task_execute(&slot->ses, line, slot->resp, CMD_RESP_MAX, PNL_CMD_TIMEOUT_MS);
    if (rc == -2) {
        /* cmd_task.h: the orphaned worker still owns slot->resp and will write into it later. Never read it, never
         * release the slot. The line is not logged: a console line can carry a SET WIFI STA credential. */
        slot->dead = 1;
        s_quarantined++;
        ESP_LOGE(TAG, "cmd dispatch did not return in %u ms -- panel session slot %u/%d withdrawn",
                 (unsigned)PNL_CMD_TIMEOUT_MS, (unsigned)s_quarantined, PNL_CMD_SLOTS);
        put(reply, cap, "ERR INTERNAL\n");
        return -2;
    }
    put(reply, cap, slot->resp);
    memset(slot->resp, 0, CMD_RESP_MAX);   /* a reply can echo a credential; do not leave it in a pooled buffer */
    release(slot);
    return rc == 0 ? 0 : -1;
}
```

- [ ] **Step 6: Write the UI kit (shared by every later section)**

Create `components/panel_ui/pnl_ui_kit.h`:
```c
#pragma once
/* Glue: the handful of widgets every System and Panel section is built from, so each section file holds behaviour, not
 * styling. [LVGL] only: call from the LVGL task (event/timer callbacks) or under panel_lock(). */
#include "lvgl.h"

typedef enum { PNL_KIT_OK = 0, PNL_KIT_ERR, PNL_KIT_INFO } pnl_kit_tone_t;
lv_obj_t *pnl_kit_card(lv_obj_t *parent, const char *title);
lv_obj_t *pnl_kit_row(lv_obj_t *parent);
lv_obj_t *pnl_kit_button(lv_obj_t *parent, const char *text, lv_event_cb_t cb, void *ud);
void      pnl_kit_enable(lv_obj_t *obj, int on);
lv_obj_t *pnl_kit_field(lv_obj_t *parent, const char *caption, lv_event_cb_t cb, void *ud);
lv_obj_t *pnl_kit_msg(lv_obj_t *parent);
void      pnl_kit_msg_set(lv_obj_t *lbl, const char *text, pnl_kit_tone_t tone);
typedef void (*pnl_confirm_fn)(void *ctx);
void      pnl_confirm(const char *title, const char *text, const char *ok_label, pnl_confirm_fn on_ok, void *ctx);
void      pnl_confirm_close(void);
```
Create `components/panel_ui/pnl_ui_kit.c`:
```c
#include <string.h>
#include "lvgl.h"
#include "pnl_palette.h"
#include "pnl_ui_kit.h"

lv_obj_t *pnl_kit_card(lv_obj_t *parent, const char *title) {
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_set_width(c, LV_PCT(100));
    lv_obj_set_height(c, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(c, 8, 0);
    lv_obj_set_style_bg_color(c, lv_color_hex(PNL_C_CARD), 0);
    lv_obj_set_style_border_color(c, lv_color_hex(PNL_C_BORDER), 0);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    if (title) {
        lv_obj_t *t = lv_label_create(c);
        lv_label_set_text(t, title);
        lv_obj_set_style_text_font(t, &lv_font_montserrat_28, 0);
    }
    return c;
}

lv_obj_t *pnl_kit_row(lv_obj_t *parent) {
    lv_obj_t *r = lv_obj_create(parent);
    lv_obj_set_width(r, LV_PCT(100));
    lv_obj_set_height(r, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_all(r, 0, 0);
    lv_obj_set_style_pad_column(r, 12, 0);
    lv_obj_set_style_pad_row(r, 8, 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(r, 0, 0);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    return r;
}

lv_obj_t *pnl_kit_button(lv_obj_t *parent, const char *text, lv_event_cb_t cb, void *ud) {
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_height(b, 56);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_center(l);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    return b;
}

void pnl_kit_enable(lv_obj_t *obj, int on) {
    if (!obj) return;
    if (on) lv_obj_remove_state(obj, LV_STATE_DISABLED);
    else    lv_obj_add_state(obj, LV_STATE_DISABLED);
}

lv_obj_t *pnl_kit_field(lv_obj_t *parent, const char *caption, lv_event_cb_t cb, void *ud) {
    lv_obj_t *r = pnl_kit_row(parent);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *cap = lv_label_create(r);
    lv_label_set_text(cap, caption);
    lv_obj_set_width(cap, 220);
    lv_obj_set_style_text_color(cap, lv_color_hex(PNL_C_MUTED), 0);
    lv_obj_t *b = lv_button_create(r);
    lv_obj_set_height(b, 56);
    lv_obj_set_flex_grow(b, 1);
    lv_obj_set_style_bg_color(b, lv_color_hex(PNL_C_BG), 0);
    lv_obj_set_style_border_color(b, lv_color_hex(PNL_C_BORDER), 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_t *v = lv_label_create(b);
    lv_label_set_text(v, "");
    lv_obj_align(v, LV_ALIGN_LEFT_MID, 0, 0);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    return v;
}

lv_obj_t *pnl_kit_msg(lv_obj_t *parent) {
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_width(l, LV_PCT(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    lv_label_set_text(l, "");
    return l;
}

void pnl_kit_msg_set(lv_obj_t *lbl, const char *text, pnl_kit_tone_t tone) {
    if (!lbl) return;
    lv_label_set_text(lbl, text ? text : "");
    uint32_t c = tone == PNL_KIT_ERR ? PNL_C_OFFLINE_TEXT : tone == PNL_KIT_OK ? PNL_C_OK_TEXT : PNL_C_MUTED;
    lv_obj_set_style_text_color(lbl, lv_color_hex(c), 0);
}

typedef struct { pnl_confirm_fn fn; void *ctx; lv_obj_t *mb; } confirm_t;
static confirm_t s_cf;

static void cf_ok(lv_event_t *e) {
    (void)e;
    confirm_t c = s_cf;
    s_cf.mb = NULL;
    if (c.mb) lv_msgbox_close_async(c.mb);   /* the event target lives inside the box: never delete synchronously */
    if (c.fn) c.fn(c.ctx);
}

static void cf_cancel(lv_event_t *e) {
    (void)e;
    if (s_cf.mb) lv_msgbox_close_async(s_cf.mb);
    s_cf.mb = NULL;
}

void pnl_confirm(const char *title, const char *text, const char *ok_label, pnl_confirm_fn on_ok, void *ctx) {
    pnl_confirm_close();
    lv_obj_t *mb = lv_msgbox_create(NULL);   /* NULL parent: modal on the top layer */
    lv_msgbox_add_title(mb, title);
    lv_msgbox_add_text(mb, text);
    lv_obj_t *b = lv_msgbox_add_footer_button(mb, "Cancel");
    lv_obj_add_event_cb(b, cf_cancel, LV_EVENT_CLICKED, NULL);
    b = lv_msgbox_add_footer_button(mb, ok_label);
    lv_obj_add_event_cb(b, cf_ok, LV_EVENT_CLICKED, NULL);
    s_cf.fn = on_ok;
    s_cf.ctx = ctx;
    s_cf.mb = mb;
}

void pnl_confirm_close(void) {
    if (s_cf.mb) lv_msgbox_close(s_cf.mb);
    s_cf.mb = NULL;
}
```

- [ ] **Step 7: Write the zone console section**

Create `components/panel_ui/zone_sections.h`:
```c
#pragma once
/* Glue: the two sections Task 22 adds below the zone view's shelf table (scr_zone_extra_area(), Task 14). */
#include <stdint.h>
#include "lvgl.h"

void zone_console_build(lv_obj_t *parent, uint8_t zone);   /* per-zone console allocated lazily (PSRAM); the send job's arg
                                                               carries pointers to the entry's sent line and reply buffer,
                                                               which belong to the worker until done() */
void zone_console_teardown(void);
void zone_console_wipe_all(void);                           /* Task 27 */
void zone_replace_build(lv_obj_t *parent, uint8_t zone);   /* MAC via HEX keyboard; bad -> "Enter a MAC like aa:bb:cc:dd:ee:ff";
                                                               job pnl_cmd_run("SET NODE <z> MAC <mac>") -> reply verbatim */
void zone_replace_teardown(void);
void zone_replace_wipe(void);                               /* Task 27 */
```
Create `components/panel_ui/zone_console.c`:
```c
#include <stdio.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "lvgl.h"
#include "ring_proto.h"     /* HG_MAX_ZONES */
#include "pcfg_gen.h"       /* PCFG_KB_TEXT */
#include "pnl_console.h"
#include "pnl_cmd.h"
#include "pnl_worker.h"
#include "pnl_ui_kit.h"
#include "wdg_keyboard.h"
#include "zone_sections.h"

static const char *TAG = "zone_console";

#define CON_VIEW_MAX   8192u   /* transcript text shown on glass (PSRAM, lv_label_set_text_static: never copied into LVGL's pool) */
#define CON_REPLY_VIEW 600     /* characters of each reply shown; the stored reply stays whole */

typedef struct { pnl_console_t con; char store[PNL_CON_LOG][CMD_RESP_MAX]; } zcon_t;   /* ~88 KB each, PSRAM */
typedef struct { uint8_t zone; pnl_con_entry_t *e; } con_arg_t;

static zcon_t  *s_con[HG_MAX_ZONES];   /* lazily allocated; a zone's console survives navigation, like the web's */
static char    *s_view;                /* CON_VIEW_MAX */
static char    *s_scratch;             /* CMD_RESP_MAX: the worker writes the reply here, done() copies it */
static uint8_t  s_zone;                /* zone shown, 0 = not built */
static uint8_t  s_busy_zone;           /* zone whose line is in flight, 0 = none (one line at a time, panel-wide) */
static lv_obj_t *s_log, *s_log_lbl, *s_draft_lbl, *s_fwd_cb, *s_msg;

static zcon_t *con_for(uint8_t zone) {
    if (zone < 1 || zone > HG_MAX_ZONES) return NULL;
    zcon_t **pp = &s_con[zone - 1];
    if (!*pp) {
        *pp = heap_caps_calloc(1, sizeof **pp, MALLOC_CAP_SPIRAM);
        if (*pp) pnl_con_init(&(*pp)->con, (*pp)->store);
        else ESP_LOGE(TAG, "no PSRAM for zone %u's console", (unsigned)zone);
    }
    return *pp;
}

static void render(void) {
    zcon_t *z = con_for(s_zone);
    if (!z || !s_log_lbl || !s_view) return;
    size_t o = 0;
    for (int first = 0; first < z->con.n_log; first++) {   /* drop the oldest until the rest fits */
        int fits = 1;
        o = 0;
        for (int i = first; i < z->con.n_log && fits; i++) {
            const pnl_con_entry_t *e = pnl_con_log_at(&z->con, i);
            if (!e || !e->used) continue;
            int w = snprintf(s_view + o, CON_VIEW_MAX - o, "> %s\n%.*s\n", e->sent, CON_REPLY_VIEW,
                             e->pending ? "..." : e->reply);
            if (w < 0 || (size_t)w >= CON_VIEW_MAX - o) fits = 0;
            else o += (size_t)w;
        }
        if (fits) break;
    }
    if (o == 0) snprintf(s_view, CON_VIEW_MAX, "No commands sent yet.");
    lv_label_set_text_static(s_log_lbl, s_view);
    lv_obj_scroll_to_y(s_log, LV_COORD_MAX, LV_ANIM_OFF);
    lv_label_set_text(s_draft_lbl, z->con.draft[0] ? z->con.draft : "Tap to type a command");
    if (s_fwd_cb) {
        if (z->con.forward) lv_obj_add_state(s_fwd_cb, LV_STATE_CHECKED);
        else lv_obj_remove_state(s_fwd_cb, LV_STATE_CHECKED);
    }
}

static void con_run(pnl_job_t *j) {
    const con_arg_t *a = (const con_arg_t *)j->arg;
    j->irc = pnl_cmd_run(a->e->sent, s_scratch, CMD_RESP_MAX);
}

static void con_done(pnl_job_t *j) {
    const con_arg_t *a = (const con_arg_t *)j->arg;
    pnl_con_reply(a->e, s_scratch);
    memset(s_scratch, 0, CMD_RESP_MAX);
    s_busy_zone = 0;
    if (j->screen_gen == pnl_screen_gen() && s_zone == a->zone) {
        pnl_kit_msg_set(s_msg, "", PNL_KIT_INFO);
        render();
    }
}

static void send_line(const char *text) {
    zcon_t *z = con_for(s_zone);
    if (!z || !s_scratch) { pnl_kit_msg_set(s_msg, "Console unavailable (no memory)", PNL_KIT_ERR); return; }
    while (*text == ' ' || *text == '\t') text++;
    if (!*text) return;
    if (!pnl_con_line_ok(text)) { pnl_kit_msg_set(s_msg, "Line too long -- 191 characters at most", PNL_KIT_ERR); return; }
    if (s_busy_zone) { pnl_kit_msg_set(s_msg, "A command is still running -- wait for its reply", PNL_KIT_ERR); return; }
    char line[CMD_LINE_MAX];
    snprintf(line, sizeof line, "%s", text);
    size_t n = strlen(line);
    while (n && (line[n - 1] == ' ' || line[n - 1] == '\r' || line[n - 1] == '\n')) line[--n] = '\0';
    char sent[CMD_LINE_MAX];
    if (z->con.forward) {
        if (pnl_con_forward(line, s_zone, sent, sizeof sent) != 0) {
            pnl_kit_msg_set(s_msg, "Line too long once addressed to the zone -- 191 characters at most", PNL_KIT_ERR);
            return;
        }
    } else {
        snprintf(sent, sizeof sent, "%s", line);
    }
    pnl_con_entry_t *e = pnl_con_push(&z->con, sent);
    if (!e) { pnl_kit_msg_set(s_msg, "A command is still running -- wait for its reply", PNL_KIT_ERR); return; }
    pnl_con_hist_add(&z->con, line);
    memset(z->con.draft, 0, sizeof z->con.draft);
    con_arg_t a = { .zone = s_zone, .e = e };
    if (pnl_worker_submit(con_run, con_done, &a, sizeof a) != 0) {
        pnl_con_reply(e, "ERR BUSY (panel worker queue full)\n");
        pnl_kit_msg_set(s_msg, "Panel busy -- try again", PNL_KIT_ERR);
    } else {
        s_busy_zone = s_zone;
        pnl_kit_msg_set(s_msg, "", PNL_KIT_INFO);
    }
    render();
}

static void kb_done(void *ctx, int accepted, const char *text) {
    (void)ctx;
    zcon_t *z = con_for(s_zone);
    if (!z || !s_draft_lbl) return;             /* the section was torn down while the keyboard was open */
    if (!accepted) return;                      /* Cancel: wdg_keyboard passes "" then, so con.draft is left as it was */
    send_line(text ? text : "");
}

static void edit_click(lv_event_t *e) {
    (void)e;
    zcon_t *z = con_for(s_zone);
    if (!z) return;
    char title[40];
    snprintf(title, sizeof title, "Command (zone %u)", (unsigned)s_zone);
    /* max_len CMD_LINE_MAX, one over the limit, so an over-long line is refused here, visibly, not silently cut */
    wdg_keyboard_open(title, PCFG_KB_TEXT, z->con.draft, 0, CMD_LINE_MAX, 0, kb_done, NULL);
}

static void send_click(lv_event_t *e) {
    (void)e;
    zcon_t *z = con_for(s_zone);
    if (z && z->con.draft[0]) { char d[CMD_LINE_MAX]; snprintf(d, sizeof d, "%s", z->con.draft); send_line(d); }
}

static void prev_click(lv_event_t *e) {
    (void)e;
    zcon_t *z = con_for(s_zone);
    if (!z) return;
    const char *h = pnl_con_hist_prev(&z->con);
    if (h) snprintf(z->con.draft, sizeof z->con.draft, "%s", h);
    render();
}

static void next_click(lv_event_t *e) {
    (void)e;
    zcon_t *z = con_for(s_zone);
    if (!z) return;
    snprintf(z->con.draft, sizeof z->con.draft, "%s", pnl_con_hist_next(&z->con));
    render();
}

static void fwd_changed(lv_event_t *e) {
    zcon_t *z = con_for(s_zone);
    if (z) z->con.forward = lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED) ? 1 : 0;
}

void zone_console_build(lv_obj_t *parent, uint8_t zone) {
    s_zone = zone;
    if (!s_view) s_view = heap_caps_calloc(1, CON_VIEW_MAX, MALLOC_CAP_SPIRAM);
    if (!s_scratch) s_scratch = heap_caps_calloc(1, CMD_RESP_MAX, MALLOC_CAP_SPIRAM);
    char title[32];
    snprintf(title, sizeof title, "Console -- zone %u", (unsigned)zone);
    lv_obj_t *card = pnl_kit_card(parent, title);
    s_log = lv_obj_create(card);
    lv_obj_set_size(s_log, LV_PCT(100), 220);
    s_log_lbl = lv_label_create(s_log);
    lv_obj_set_width(s_log_lbl, LV_PCT(100));
    lv_label_set_long_mode(s_log_lbl, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_t *dr = pnl_kit_row(card);
    lv_obj_t *db = lv_button_create(dr);
    lv_obj_set_height(db, 56);
    lv_obj_set_flex_grow(db, 1);
    lv_obj_add_event_cb(db, edit_click, LV_EVENT_CLICKED, NULL);
    s_draft_lbl = lv_label_create(db);
    lv_obj_align(s_draft_lbl, LV_ALIGN_LEFT_MID, 0, 0);
    pnl_kit_button(dr, "Send", send_click, NULL);
    lv_obj_t *r = pnl_kit_row(card);
    pnl_kit_button(r, LV_SYMBOL_UP " Prev", prev_click, NULL);
    pnl_kit_button(r, LV_SYMBOL_DOWN " Next", next_click, NULL);
    s_fwd_cb = lv_checkbox_create(r);
    char fw[32];
    snprintf(fw, sizeof fw, "Forward to zone %u", (unsigned)zone);
    lv_checkbox_set_text(s_fwd_cb, fw);
    lv_obj_add_event_cb(s_fwd_cb, fwd_changed, LV_EVENT_VALUE_CHANGED, NULL);
    s_msg = pnl_kit_msg(card);
    render();
}

void zone_console_teardown(void) {
    s_log = s_log_lbl = s_draft_lbl = s_fwd_cb = s_msg = NULL;
    s_zone = 0;
}

void zone_console_wipe_all(void) {
    for (int i = 0; i < HG_MAX_ZONES; i++)
        if (s_con[i]) pnl_con_wipe(&s_con[i]->con);
    if (s_view) memset(s_view, 0, CON_VIEW_MAX);
    if (s_log_lbl) render();
}
```

- [ ] **Step 8: Write the Replace board section**

Create `components/panel_ui/zone_replace.c`:
```c
#include <stdio.h>
#include <string.h>
#include "lvgl.h"
#include "ring_proto.h"     /* HG_MAX_ZONES */
#include "pcfg_gen.h"       /* PCFG_KB_HEX */
#include "pnl_input.h"      /* pnl_mac_parse */
#include "pnl_cmd.h"
#include "pnl_worker.h"
#include "pnl_ui_kit.h"
#include "wdg_keyboard.h"
#include "zone_sections.h"

static char     s_mac[HG_MAX_ZONES][18];   /* per-zone draft, like the web's macDraftKey (app.js:62-81) */
static uint8_t  s_zone, s_busy;
static lv_obj_t *s_mac_lbl, *s_out, *s_btn;

typedef struct { uint8_t zone; char line[48]; } rep_arg_t;

static void show_draft(void) {
    if (!s_mac_lbl || !s_zone) return;
    lv_label_set_text(s_mac_lbl, s_mac[s_zone - 1][0] ? s_mac[s_zone - 1] : "aa:bb:cc:dd:ee:ff");
}

static void rep_run(pnl_job_t *j) {
    const rep_arg_t *a = (const rep_arg_t *)j->arg;
    j->irc = pnl_cmd_run(a->line, (char *)j->out, PNL_JOB_OUT_MAX);
}

static void rep_done(pnl_job_t *j) {
    const rep_arg_t *a = (const rep_arg_t *)j->arg;
    s_busy = 0;
    if (j->irc == 0) memset(s_mac[a->zone - 1], 0, sizeof s_mac[0]);   /* the web clears the draft on success only */
    if (j->screen_gen != pnl_screen_gen() || s_zone != a->zone) return;
    pnl_kit_msg_set(s_out, (const char *)j->out, j->irc == 0 ? PNL_KIT_OK : PNL_KIT_ERR);   /* the reply, verbatim */
    pnl_kit_enable(s_btn, 1);
    show_draft();
}

static void mac_kb_done(void *ctx, int accepted, const char *text) {
    (void)ctx;
    if (!accepted || !s_zone) return;
    snprintf(s_mac[s_zone - 1], sizeof s_mac[0], "%s", text ? text : "");
    show_draft();
}

static void mac_click(lv_event_t *e) {
    (void)e;
    char title[40];
    snprintf(title, sizeof title, "New board MAC for zone %u", (unsigned)s_zone);
    wdg_keyboard_open(title, PCFG_KB_HEX, s_mac[s_zone - 1], 1, 17, 0, mac_kb_done, NULL);
}

static void replace_click(lv_event_t *e) {
    (void)e;
    if (s_busy || !s_zone) return;
    uint8_t mac[6];
    const char *d = s_mac[s_zone - 1];
    if (pnl_mac_parse(d, mac) != 0) { pnl_kit_msg_set(s_out, "Enter a MAC like aa:bb:cc:dd:ee:ff", PNL_KIT_ERR); return; }
    rep_arg_t a;
    memset(&a, 0, sizeof a);
    a.zone = s_zone;
    snprintf(a.line, sizeof a.line, "SET NODE %u MAC %s", (unsigned)s_zone, d);
    if (pnl_worker_submit(rep_run, rep_done, &a, sizeof a) != 0) {
        pnl_kit_msg_set(s_out, "Panel busy -- try again", PNL_KIT_ERR);
        return;
    }
    s_busy = 1;
    pnl_kit_enable(s_btn, 0);
    pnl_kit_msg_set(s_out, "...", PNL_KIT_INFO);
}

void zone_replace_build(lv_obj_t *parent, uint8_t zone) {
    s_zone = zone;
    lv_obj_t *card = pnl_kit_card(parent, "Replace board");
    lv_obj_t *note = pnl_kit_msg(card);
    pnl_kit_msg_set(note, "Binds this zone slot to a new board's MAC (SET NODE <zone> MAC <mac>).", PNL_KIT_INFO);
    s_mac_lbl = pnl_kit_field(card, "New MAC", mac_click, NULL);
    s_btn = pnl_kit_button(card, "Replace", replace_click, NULL);
    pnl_kit_enable(s_btn, !s_busy);
    s_out = pnl_kit_msg(card);
    show_draft();
}

void zone_replace_teardown(void) {
    s_mac_lbl = s_out = s_btn = NULL;
    s_zone = 0;
}

void zone_replace_wipe(void) {
    memset(s_mac, 0, sizeof s_mac);
    pnl_kit_msg_set(s_out, "", PNL_KIT_INFO);
    show_draft();
}
```

- [ ] **Step 9: Wire the sections, the dashboard line, the poller field and the session init**

In `components/panel_ui/scr_zone.c` (Task 14):
- add `#include "zone_sections.h"` with the other includes;
- in `zone_build()`, after the `s_extra` container is created and styled (the block ending
  `lv_obj_set_style_pad_row(s_extra, 10, 0);`) and before the closing `zone_update(sn);`, add:
  ```c
      /* Task 22: console and Replace board, below the shelf table */
      lv_obj_t *extra = scr_zone_extra_area();   /* NULL when no zone is shown */
      if (extra) {
          zone_console_build(extra, scr_zone_current());
          zone_replace_build(extra, scr_zone_current());
      }
  ```
- in `zone_teardown()`, as its first two lines: `zone_console_teardown();` and `zone_replace_teardown();`.

Task 14's zone selector changes zone through `pnl_nav_go(PNL_DEST_ZONE, id)`, a full teardown and rebuild, so no
in-place refill path needs its own teardown call.

In `components/panel_ui/pnl_poll.c` (Task 11): add `#include "pnl_cmd.h"`, and in `poll_task()`, directly after the line
`psvc_state_fill(&s_stage->st, PSVC_FILL_SKIP_WIFI);`, add:
```c
        s_stage->panel_cmd_quarantined = pnl_cmd_quarantined();
```

In `components/panel_ui/scr_dashboard.c` (Task 12): next to the "N web console slot(s) degraded" label (`s_quar`) add a
second label, created hidden in `dash_build()` in the same parent, updated in `dash_update()` and dropped in
`dash_teardown()`:
```c
/* with the other statics, after  static lv_obj_t   *s_quar, *s_grid;  */
static lv_obj_t *s_panel_q;   /* Task 22: the panel's own quarantined command slots (A7) */

/* dash_build(), directly after the line  s_quar = pnl_label(page, ...);  (same parent, so it sits right below it): */
    s_panel_q = lv_label_create(lv_obj_get_parent(s_quar));
    lv_obj_set_style_text_color(s_panel_q, lv_color_hex(PNL_C_WARN_TEXT), 0);
    lv_obj_add_flag(s_panel_q, LV_OBJ_FLAG_HIDDEN);

/* dash_update(const pnl_snap_t *sn), directly after the web-quarantine if/else: */
    if (s_panel_q) {
        if (sn->panel_cmd_quarantined) {
            char b[48];
            snprintf(b, sizeof b, "%u panel console slot(s) degraded", (unsigned)sn->panel_cmd_quarantined);
            pnl_label_set_if_changed(s_panel_q, b);
            lv_obj_remove_flag(s_panel_q, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_panel_q, LV_OBJ_FLAG_HIDDEN);
        }
    }

/* dash_teardown(): */
    s_panel_q = NULL;
```
Also add `pnl_obj_show(s_panel_q, 0);` to the `if (!up) { ... }` early-return line in `dash_update()`, next to
`pnl_obj_show(s_quar, 0);`, so the line never shows before the first snapshot.

In `components/panel_ui/panel_ui.c`: add `#include "pnl_cmd.h"`, and in `panel_services_start()` (Task 11's version) add
`pnl_cmd_init();` directly before `pnl_worker_start();`, after the `if (!s_lit) return -1;` guard, so a dark panel
allocates no command slots.

In `components/panel_ui/CMakeLists.txt`, inside the `if("${CMAKE_PROJECT_NAME}" STREQUAL "hillgrow_master" AND
"${panel_target}" STREQUAL "esp32p4")` block, after the sources earlier tasks appended, add:
```cmake
    list(APPEND PANEL_SRCS "pnl_console.c" "pnl_cmd.c" "pnl_ui_kit.c" "zone_console.c" "zone_replace.c")
```
`cmd_task` and `cmd_core` are already in the component's `REQUIRES`/`PRIV_REQUIRES` (Task 6).

- [ ] **Step 10: Run the gates**

1. GATE-HOST. Expected: `100% tests passed, 0 tests failed out of 48`.
2. GATE-ESP32. Expected: three `Project build complete`, empty lock diff. (panel_ui compiles no sources there; this
   proves the new files did not leak into the gating.)
3. GATE-P4. Expected: `Project build complete`, 0 warnings. This proves the glue compiles against LVGL 9.5.0, the kit's
   msgbox API and the real `cmd_task_execute()` signature.
4. FLASH-P4 (dry run, then real).
5. Bench spot check (agent, then again in the Stage 3 gate with the owner): open Zone 2, type `GET ID`, Send → the
   reply `OK ID ...` appears verbatim; tick Forward, send `GET WATER 1` → a zone-2 reply; Prev/Next walk the history;
   a 192-character line is refused with "Line too long". Replace board with `aa:bb` → "Enter a MAC like
   aa:bb:cc:dd:ee:ff". The Dashboard shows no "panel console slot(s) degraded" line.

- [ ] **Step 11: Commit**

```powershell
git -C C:\Projects\HillGrov add components/panel_ui/pnl_console.h components/panel_ui/pnl_console.c components/panel_ui/pnl_cmd.h components/panel_ui/pnl_cmd.c components/panel_ui/pnl_ui_kit.h components/panel_ui/pnl_ui_kit.c components/panel_ui/zone_sections.h components/panel_ui/zone_console.c components/panel_ui/zone_replace.c components/panel_ui/scr_zone.c components/panel_ui/pnl_poll.c components/panel_ui/scr_dashboard.c components/panel_ui/panel_ui.c components/panel_ui/CMakeLists.txt tests/host/test_pnl_console.c tests/host/CMakeLists.txt
git -C C:\Projects\HillGrov commit -m "feat(panel_ui): zone console and replace-board on the panel's own command session" -m "Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>"
git -C C:\Projects\HillGrov push
```

---

### Task 23: System — Wi-Fi and Time (Stage 3)

The System destination becomes a section host with one row of section buttons. Wi-Fi and Time are real in this task;
Password, Fleet and Firmware show the placeholder until Tasks 24, 25 and 32 swap their registry line. The web reference
is `HG.views.system` (`web/app.js:913-958`) and its handlers `wifiScan`/`wifiPick`/`wifiJoin`/`apSet`/`tzSet`
(`web/app.js:1787-1843`). SSIDs and passwords never travel as CLI lines: Wi-Fi calls `psvc_wifi_set_sta/ap`, TZ calls
`psvc_tz_set`. Only "Set clock" (D14) is a CLI line (`SET TIME`, `components/cmd_common/cmd_common.c:117-135`), because
`SET TIME` is the only way to set the clock and it carries no secret.

**Files:**
- Create: `components/panel_ui/scr_system.h`, `components/panel_ui/scr_system.c`, `components/panel_ui/sys_wifi.c`, `components/panel_ui/sys_time.c`
- Modify: `components/panel_ui/scr_shell.c` (registry SYSTEM), `components/panel_ui/CMakeLists.txt`

**Interfaces:**
- Consumes: `psvc_wifi_scan`, `psvc_wifi_set_sta`, `psvc_wifi_set_ap`, `psvc_tz_set` (Task 4, `psvc_net.h`);
  `PSVC_LOCK_PANEL_MS`, `psvc_mcfg_get` (Task 3); `wifi_scan_t` (`components/wifi_mgr/wifi_mgr.h:43`);
  `pnl_fmt_sta`, `pnl_fmt_ap`, `pnl_fmt_master_time`, `pnl_set_time_line`, `pnl_local_time` (Task 10);
  `pnl_cmd_run` (Task 22); `pnl_msg` with `PNL_CTX_SCAN`, `PNL_CTX_WIFI_JOIN`, `PNL_CTX_WIFI_AP`, `PNL_CTX_TZ` (Task 18);
  `wdg_keyboard_open` (Task 19); `pnl_kit_*`, `pnl_confirm` (Task 22); `pnl_worker_submit` (Task 8);
  `pnl_screen_ops_t`, `pnl_label_set_if_changed` (Task 12); `pnl_poll_latest` (Task 11).
- Produces:
  ```c
  /* scr_system.h (glue) */
  typedef struct { const char *title; void (*build)(lv_obj_t *parent); void (*update)(const pnl_snap_t *s);
                   void (*teardown)(void); } pnl_sys_section_t;
  extern const pnl_sys_section_t PNL_SYS_WIFI, PNL_SYS_TIME, PNL_SYS_PASSWORD, PNL_SYS_FLEET, PNL_SYS_FIRMWARE,
                                 PNL_SYS_PLACEHOLDER;   /* registry in scr_system.c; PASSWORD/FLEET/FIRMWARE -> PLACEHOLDER until
                                                          Tasks 24/25/32 swap their line */
  void sys_wifi_wipe(void);   /* Wi-Fi form text (Task 27) */
  ```
  and `PNL_SCR_SYSTEM` (declared by Task 12, defined here).
- Produces **(addition)**: the section index for `pnl_nav_go(PNL_DEST_SYSTEM, arg)`:
  ```c
  enum { PNL_SYS_SEC_WIFI = 0, PNL_SYS_SEC_TIME, PNL_SYS_SEC_PASSWORD, PNL_SYS_SEC_FLEET, PNL_SYS_SEC_FIRMWARE,
         PNL_SYS_SEC_COUNT };   /* arg outside 0..COUNT-1 = the section last shown (Wi-Fi the first time) */
  ```

**What proves what:** all four files are LVGL glue. GATE-P4 proves they compile and link against the Task 4 and Task 10
signatures. The behaviour (scan list, join, AP warning, TZ, Set clock) is proven only by the Stage 3 gate steps 3-4.

- [ ] **Step 1: Write the System section host**

Create `components/panel_ui/scr_system.h`:
```c
#pragma once
/* Glue: the System destination -- the web's System page (web/app.js:913-1020) as sections on one screen. */
#include "lvgl.h"
#include "pnl_poll.h"
#include "scr_shell.h"

typedef struct { const char *title; void (*build)(lv_obj_t *parent); void (*update)(const pnl_snap_t *s);
                 void (*teardown)(void); } pnl_sys_section_t;
extern const pnl_sys_section_t PNL_SYS_WIFI, PNL_SYS_TIME, PNL_SYS_PASSWORD, PNL_SYS_FLEET, PNL_SYS_FIRMWARE,
                               PNL_SYS_PLACEHOLDER;   /* registry in scr_system.c; PASSWORD/FLEET/FIRMWARE -> PLACEHOLDER until
                                                        Tasks 24/25/32 swap their line */
enum { PNL_SYS_SEC_WIFI = 0, PNL_SYS_SEC_TIME, PNL_SYS_SEC_PASSWORD, PNL_SYS_SEC_FLEET, PNL_SYS_SEC_FIRMWARE,
       PNL_SYS_SEC_COUNT };   /* pnl_nav_go(PNL_DEST_SYSTEM, sec); outside the range = last shown */
void sys_wifi_wipe(void);   /* Wi-Fi form text (Task 27) */
```
Create `components/panel_ui/scr_system.c`:
```c
#include <stdint.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "lvgl.h"
#include "pnl_ui_kit.h"
#include "scr_system.h"

static const char *TAG = "scr_system";

typedef struct { const char *title; const pnl_sys_section_t *sec; } sys_row_t;
static const sys_row_t SYS_ROWS[PNL_SYS_SEC_COUNT] = {
    [PNL_SYS_SEC_WIFI]     = { "Wi-Fi",    &PNL_SYS_WIFI },
    [PNL_SYS_SEC_TIME]     = { "Time",     &PNL_SYS_TIME },
    [PNL_SYS_SEC_PASSWORD] = { "Password", &PNL_SYS_PLACEHOLDER },
    [PNL_SYS_SEC_FLEET]    = { "Fleet",    &PNL_SYS_PLACEHOLDER },
    [PNL_SYS_SEC_FIRMWARE] = { "Firmware", &PNL_SYS_PLACEHOLDER },
};

static lv_obj_t   *s_body, *s_tab[PNL_SYS_SEC_COUNT];
static int         s_cur = -1, s_last = PNL_SYS_SEC_WIFI;
static const char *s_cur_title = "";
static pnl_snap_t *s_snap;   /* PSRAM: a section's first update() is fed from here, not from the 8 KB LVGL stack */

/* ---- the placeholder section (Tasks 24/25/32 replace it row by row) ---- */
static void ph_build(lv_obj_t *parent) {
    lv_obj_t *c = pnl_kit_card(parent, s_cur_title);
    lv_obj_t *m = pnl_kit_msg(c);
    char b[64];
    snprintf(b, sizeof b, "%s: not available yet", s_cur_title);
    pnl_kit_msg_set(m, b, PNL_KIT_INFO);
}
const pnl_sys_section_t PNL_SYS_PLACEHOLDER = { .title = "", .build = ph_build, .update = NULL, .teardown = NULL };

static void open_section(int i) {
    if (s_cur >= 0 && SYS_ROWS[s_cur].sec->teardown) SYS_ROWS[s_cur].sec->teardown();
    lv_obj_clean(s_body);
    s_cur = i;
    s_last = i;
    s_cur_title = SYS_ROWS[i].title;
    for (int k = 0; k < PNL_SYS_SEC_COUNT; k++) {
        if (k == i) lv_obj_add_state(s_tab[k], LV_STATE_CHECKED);
        else lv_obj_remove_state(s_tab[k], LV_STATE_CHECKED);
    }
    int64_t t0 = esp_timer_get_time();
    SYS_ROWS[i].sec->build(s_body);
    if (!s_snap) s_snap = heap_caps_malloc(sizeof *s_snap, MALLOC_CAP_SPIRAM);
    if (s_snap && SYS_ROWS[i].sec->update) {
        pnl_poll_latest(s_snap);
        if (s_snap->started) SYS_ROWS[i].sec->update(s_snap);
    }
    int64_t ms = (esp_timer_get_time() - t0) / 1000;
    if (ms > 200) ESP_LOGW(TAG, "System section %s built in %lld ms (budget 200 ms)", s_cur_title, (long long)ms);
}

static void tab_click(lv_event_t *e) {
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i != s_cur) open_section(i);
}

static void sys_build(lv_obj_t *content, int arg) {
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_t *tabs = pnl_kit_row(content);
    for (int k = 0; k < PNL_SYS_SEC_COUNT; k++) {
        s_tab[k] = pnl_kit_button(tabs, SYS_ROWS[k].title, tab_click, (void *)(intptr_t)k);
        lv_obj_add_flag(s_tab[k], LV_OBJ_FLAG_CHECKABLE);
    }
    s_body = lv_obj_create(content);
    lv_obj_set_width(s_body, LV_PCT(100));
    lv_obj_set_flex_grow(s_body, 1);
    lv_obj_set_flex_flow(s_body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_bg_opa(s_body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_body, 0, 0);
    s_cur = -1;
    open_section((arg >= 0 && arg < PNL_SYS_SEC_COUNT) ? arg : s_last);
}

static void sys_update(const pnl_snap_t *snap) {
    if (s_cur >= 0 && SYS_ROWS[s_cur].sec->update) SYS_ROWS[s_cur].sec->update(snap);
}

static void sys_teardown(void) {
    if (s_cur >= 0 && SYS_ROWS[s_cur].sec->teardown) SYS_ROWS[s_cur].sec->teardown();
    s_cur = -1;
    s_body = NULL;
    for (int k = 0; k < PNL_SYS_SEC_COUNT; k++) s_tab[k] = NULL;
}

const pnl_screen_ops_t PNL_SCR_SYSTEM = { .title = "System", .build = sys_build, .update = sys_update,
                                          .teardown = sys_teardown, .in_rail = 1 };
```

- [ ] **Step 2: Write the Wi-Fi section**

Create `components/panel_ui/sys_wifi.c`:
```c
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "esp_heap_caps.h"
#include "lvgl.h"
#include "psvc_net.h"
#include "psvc_mcfg.h"      /* PSVC_LOCK_PANEL_MS */
#include "pcfg_gen.h"       /* PCFG_KB_TEXT */
#include "pnl_input.h"      /* pnl_zero */
#include "pnl_fmt.h"
#include "pnl_msg.h"
#include "pnl_worker.h"
#include "pnl_ui_kit.h"
#include "wdg_keyboard.h"
#include "scr_system.h"

#define SCAN_CAP 20

typedef struct { char *buf; size_t cap; uint8_t min_len, max_len, secret; const char *title; lv_obj_t **lbl; } wfield_t;
typedef struct { wifi_scan_t *out; int cap; } scan_arg_t;
typedef struct { char ssid[33]; char pass[65]; } wifi_arg_t;   /* 98 B by value; the pool memsets arg after done() */

static wifi_scan_t *s_scan;                 /* PSRAM; the worker owns it from submit until scan_done() */
static int      s_scan_n = -1;              /* -1 = no list to show */
static char     s_scan_err[64];
static uint8_t  s_scanning, s_joining, s_apping;
static char     s_join_last[96], s_ap_last[96]; /* the last outcomes, kept for a rebuilt section */
static uint8_t  s_join_err, s_ap_err;
static char     s_sta_ssid[33], s_sta_pass[65], s_ap_ssid[33], s_ap_pass[65];
static lv_obj_t *s_sta_lbl, *s_ap_lbl, *s_scan_btn, *s_list, *s_join_msg, *s_ap_msg;
static lv_obj_t *s_f_sta_ssid, *s_f_sta_pass, *s_f_ap_ssid, *s_f_ap_pass;

static const wfield_t F_STA_SSID = { s_sta_ssid, sizeof s_sta_ssid, 0, 32, 0, "House Wi-Fi SSID", &s_f_sta_ssid };
static const wfield_t F_STA_PASS = { s_sta_pass, sizeof s_sta_pass, 0, 63, 1, "House Wi-Fi password", &s_f_sta_pass };
static const wfield_t F_AP_SSID  = { s_ap_ssid,  sizeof s_ap_ssid,  1, 32, 0, "AP SSID", &s_f_ap_ssid };
static const wfield_t F_AP_PASS  = { s_ap_pass,  sizeof s_ap_pass,  8, 63, 1, "AP password (8+ chars)", &s_f_ap_pass };

static void show_field(const wfield_t *f) {
    if (!*f->lbl) return;
    if (f->secret) lv_label_set_text(*f->lbl, f->buf[0] ? "********" : "(none)");
    else lv_label_set_text(*f->lbl, f->buf[0] ? f->buf : "(none)");
}

static void show_all(void) {
    show_field(&F_STA_SSID); show_field(&F_STA_PASS); show_field(&F_AP_SSID); show_field(&F_AP_PASS);
}

static void kb_done(void *ctx, int accepted, const char *text) {
    const wfield_t *f = (const wfield_t *)ctx;
    if (accepted) snprintf(f->buf, f->cap, "%s", text ? text : "");
    show_field(f);
}

static void field_click(lv_event_t *e) {
    const wfield_t *f = (const wfield_t *)lv_event_get_user_data(e);
    /* the keyboard's masked mode has the reveal (eye) toggle, Task 19; a secret's keyboard starts from the draft only */
    wdg_keyboard_open(f->title, PCFG_KB_TEXT, f->buf, f->min_len, f->max_len, f->secret, kb_done, (void *)f);
}

/* ---- scan ---- */
static void pick_click(lv_event_t *e) {
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i >= 0 && i < s_scan_n) snprintf(s_sta_ssid, sizeof s_sta_ssid, "%s", s_scan[i].ssid);   /* app.js wifiPick */
    show_field(&F_STA_SSID);
}

static void render_scan(void) {
    if (!s_list) return;
    lv_obj_clean(s_list);
    pnl_kit_enable(s_scan_btn, !s_scanning);
    lv_obj_t *m;
    if (s_scanning) { m = pnl_kit_msg(s_list); pnl_kit_msg_set(m, "Scanning... (the AP pauses briefly)", PNL_KIT_INFO); return; }
    if (s_scan_err[0]) { m = pnl_kit_msg(s_list); pnl_kit_msg_set(m, s_scan_err, PNL_KIT_ERR); return; }
    if (s_scan_n < 0) return;
    if (s_scan_n == 0) { m = pnl_kit_msg(s_list); pnl_kit_msg_set(m, "No networks found.", PNL_KIT_INFO); return; }
    for (int i = 0; i < s_scan_n; i++) {
        char t[80];
        /* auth is a raw wifi_auth_mode_t: 0 == WIFI_AUTH_OPEN, anything else is secured (app.js:928-930) */
        snprintf(t, sizeof t, "%s  |  %s  |  %d dBm", s_scan[i].ssid, s_scan[i].auth == 0 ? "open" : "secured",
                 (int)s_scan[i].rssi);
        lv_obj_t *b = pnl_kit_button(s_list, t, pick_click, (void *)(intptr_t)i);
        lv_obj_set_width(b, LV_PCT(100));
    }
}

static void scan_run(pnl_job_t *j) {
    const scan_arg_t *a = (const scan_arg_t *)j->arg;
    int n = 0;
    j->rc = psvc_wifi_scan(a->out, a->cap, &n, PSVC_LOCK_PANEL_MS);
    j->irc = n;
}

static void scan_done(pnl_job_t *j) {
    s_scanning = 0;
    if (j->rc == PSVC_OK) { s_scan_n = j->irc; s_scan_err[0] = '\0'; }
    else { s_scan_n = -1; pnl_msg(PNL_CTX_SCAN, j->rc, NULL, s_scan_err, sizeof s_scan_err); }
    render_scan();   /* NULL-safe, no screen_gen gate: a scan that ends after the operator came back must redraw */
}

static void scan_click(lv_event_t *e) {
    (void)e;
    if (s_scanning) return;
    if (!s_scan) s_scan = heap_caps_calloc(SCAN_CAP, sizeof *s_scan, MALLOC_CAP_SPIRAM);
    if (!s_scan) { snprintf(s_scan_err, sizeof s_scan_err, "Scan unavailable (no memory)"); render_scan(); return; }
    scan_arg_t a = { .out = s_scan, .cap = SCAN_CAP };
    if (pnl_worker_submit(scan_run, scan_done, &a, sizeof a) != 0) {
        snprintf(s_scan_err, sizeof s_scan_err, "Panel busy -- try again");
    } else {
        s_scanning = 1;
        s_scan_err[0] = '\0';
    }
    render_scan();
}

/* ---- join the house Wi-Fi (STA) ---- */
static void join_run(pnl_job_t *j) {
    const wifi_arg_t *a = (const wifi_arg_t *)j->arg;
    j->rc = psvc_wifi_set_sta(a->ssid, a->pass, PSVC_LOCK_PANEL_MS);
}

static void join_done(pnl_job_t *j) {
    s_joining = 0;
    pnl_msg(PNL_CTX_WIFI_JOIN, j->rc, NULL, s_join_last, sizeof s_join_last);
    s_join_err = j->rc != PSVC_OK;
    pnl_kit_msg_set(s_join_msg, s_join_last, s_join_err ? PNL_KIT_ERR : PNL_KIT_OK);   /* NULL-safe */
}

static void join_click(lv_event_t *e) {
    (void)e;
    if (s_joining) return;
    if (!s_sta_ssid[0]) { pnl_kit_msg_set(s_join_msg, "Enter an SSID", PNL_KIT_ERR); return; }   /* app.js:1809 */
    wifi_arg_t a;
    memset(&a, 0, sizeof a);
    snprintf(a.ssid, sizeof a.ssid, "%s", s_sta_ssid);
    snprintf(a.pass, sizeof a.pass, "%s", s_sta_pass);   /* "" = an open network */
    if (pnl_worker_submit(join_run, join_done, &a, sizeof a) != 0) {
        pnl_kit_msg_set(s_join_msg, "Panel busy -- try again", PNL_KIT_ERR);
    } else {
        s_joining = 1;
        s_join_last[0] = '\0';
        pnl_zero(s_sta_pass, sizeof s_sta_pass);     /* secrets are wiped right after use */
        show_field(&F_STA_PASS);
        pnl_kit_msg_set(s_join_msg, "...", PNL_KIT_INFO);
    }
    pnl_zero(&a, sizeof a);                          /* a dying local: a plain memset is a dead store at -Os */
}

/* ---- set the AP ---- */
static void ap_run(pnl_job_t *j) {
    const wifi_arg_t *a = (const wifi_arg_t *)j->arg;
    j->rc = psvc_wifi_set_ap(a->ssid, a->pass, PSVC_LOCK_PANEL_MS);
}

static void ap_done(pnl_job_t *j) {
    s_apping = 0;
    pnl_msg(PNL_CTX_WIFI_AP, j->rc, NULL, s_ap_last, sizeof s_ap_last);
    s_ap_err = j->rc != PSVC_OK;
    pnl_kit_msg_set(s_ap_msg, s_ap_last, s_ap_err ? PNL_KIT_ERR : PNL_KIT_OK);   /* NULL-safe */
}

static void ap_go(void *ctx) {
    (void)ctx;
    wifi_arg_t a;
    memset(&a, 0, sizeof a);
    snprintf(a.ssid, sizeof a.ssid, "%s", s_ap_ssid);
    snprintf(a.pass, sizeof a.pass, "%s", s_ap_pass);
    if (pnl_worker_submit(ap_run, ap_done, &a, sizeof a) != 0) {
        pnl_kit_msg_set(s_ap_msg, "Panel busy -- try again", PNL_KIT_ERR);
    } else {
        s_apping = 1;
        s_ap_last[0] = '\0';
        pnl_zero(s_ap_pass, sizeof s_ap_pass);
        show_field(&F_AP_PASS);
        pnl_kit_msg_set(s_ap_msg, "...", PNL_KIT_INFO);
    }
    pnl_zero(&a, sizeof a);
}

static void ap_click(lv_event_t *e) {
    (void)e;
    if (s_apping) return;
    if (!s_ap_ssid[0] || strlen(s_ap_pass) < 8) {                       /* app.js:1821 */
        pnl_kit_msg_set(s_ap_msg, "AP SSID required, password 8+ chars", PNL_KIT_ERR);
        return;
    }
    pnl_confirm("Change the AP?", "Changing the AP drops every phone connected to it", "Change AP", ap_go, NULL);
}

/* ---- section ---- */
static void wifi_build(lv_obj_t *parent) {
    lv_obj_t *c = pnl_kit_card(parent, "Wi-Fi");
    s_sta_lbl = pnl_kit_msg(c);
    s_ap_lbl = pnl_kit_msg(c);
    s_scan_btn = pnl_kit_button(c, "Scan", scan_click, NULL);
    s_list = lv_obj_create(c);
    lv_obj_set_width(s_list, LV_PCT(100));
    lv_obj_set_height(s_list, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_list, 0, 0);

    lv_obj_t *sta = pnl_kit_card(parent, "Join house Wi-Fi");
    s_f_sta_ssid = pnl_kit_field(sta, "SSID", field_click, (void *)&F_STA_SSID);
    s_f_sta_pass = pnl_kit_field(sta, "Password", field_click, (void *)&F_STA_PASS);
    pnl_kit_button(sta, "Join", join_click, NULL);
    s_join_msg = pnl_kit_msg(sta);

    lv_obj_t *ap = pnl_kit_card(parent, "Master AP");
    s_f_ap_ssid = pnl_kit_field(ap, "AP SSID", field_click, (void *)&F_AP_SSID);
    s_f_ap_pass = pnl_kit_field(ap, "AP password", field_click, (void *)&F_AP_PASS);
    pnl_kit_button(ap, "Set AP", ap_click, NULL);
    s_ap_msg = pnl_kit_msg(ap);
    /* a join or AP change still running, or its kept outcome (it may have landed while the operator was elsewhere) */
    if (s_joining) pnl_kit_msg_set(s_join_msg, "...", PNL_KIT_INFO);
    else if (s_join_last[0]) pnl_kit_msg_set(s_join_msg, s_join_last, s_join_err ? PNL_KIT_ERR : PNL_KIT_OK);
    if (s_apping) pnl_kit_msg_set(s_ap_msg, "...", PNL_KIT_INFO);
    else if (s_ap_last[0]) pnl_kit_msg_set(s_ap_msg, s_ap_last, s_ap_err ? PNL_KIT_ERR : PNL_KIT_OK);
    show_all();
    render_scan();
}

static void wifi_update(const pnl_snap_t *s) {
    char b[96], line[112];
    pnl_fmt_sta(&s->st.wifi, b, sizeof b);
    snprintf(line, sizeof line, "STA: %s", b);
    pnl_label_set_if_changed(s_sta_lbl, line);
    pnl_fmt_ap(&s->st.wifi, b, sizeof b);
    snprintf(line, sizeof line, "AP: %s", b);
    pnl_label_set_if_changed(s_ap_lbl, line);
}

static void wifi_teardown(void) {
    s_sta_lbl = s_ap_lbl = s_scan_btn = s_list = s_join_msg = s_ap_msg = NULL;
    s_f_sta_ssid = s_f_sta_pass = s_f_ap_ssid = s_f_ap_pass = NULL;
}

void sys_wifi_wipe(void) {
    memset(s_sta_ssid, 0, sizeof s_sta_ssid);
    pnl_zero(s_sta_pass, sizeof s_sta_pass);
    memset(s_ap_ssid, 0, sizeof s_ap_ssid);
    pnl_zero(s_ap_pass, sizeof s_ap_pass);
    s_join_last[0] = s_ap_last[0] = '\0';
    if (!s_scanning) {                    /* a scan in flight owns s_scan until scan_done() */
        s_scan_n = -1;
        s_scan_err[0] = '\0';
        if (s_scan) memset(s_scan, 0, SCAN_CAP * sizeof *s_scan);
    }
    show_all();
    render_scan();
}

const pnl_sys_section_t PNL_SYS_WIFI = { .title = "Wi-Fi", .build = wifi_build, .update = wifi_update,
                                         .teardown = wifi_teardown };
```
`render_scan()`, `show_field()` and `pnl_kit_msg_set()` are NULL-safe, so every `done()` above is safe after the
section was torn down or another section opened: its widget pointers are NULL then. That is why these `done()`s draw
without a `screen_gen` check (Task 8's alternative form of THE RULE): each stores its outcome in module state first, so a
scan, join or AP change that finishes after the operator left System by the rail and came back still reaches the rebuilt
widgets instead of leaving "Scanning..." (and a disabled Scan) on screen. `pnl_label_set_if_changed()` is
NULL-safe too (Task 12 returns early on a NULL label), so `wifi_update` needs no guard of its own.

- [ ] **Step 3: Write the Time section**

Create `components/panel_ui/sys_time.c`:
```c
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "lvgl.h"
#include "hg_mcfg.h"
#include "psvc_net.h"
#include "psvc_mcfg.h"
#include "pcfg_gen.h"       /* PCFG_KB_TEXT_NOSPACE */
#include "pnl_input.h"      /* pnl_zero */
#include "pnl_time.h"
#include "pnl_fmt.h"
#include "pnl_msg.h"
#include "pnl_cmd.h"
#include "pnl_worker.h"
#include "pnl_ui_kit.h"
#include "wdg_keyboard.h"
#include "scr_system.h"

#define YEAR0 2020   /* the master's own SET TIME range, 2020..2099 (components/app_common/app_if_common.c:138) */
#define YEARS 80

static char     s_tz[48];
static char     s_tz_last[96], s_clk_last[PNL_JOB_OUT_MAX];   /* kept outcomes for a rebuilt section (sys_wifi.c note) */
static uint8_t  s_tz_err, s_clk_err;
static uint8_t  s_tz_dirty, s_tz_busy, s_clk_busy, s_have_snap, s_prefilled;
static int32_t  s_offset;
static lv_obj_t *s_now_lbl, *s_tz_lbl, *s_tz_msg, *s_clk_msg, *s_clk_btn, *s_r_y, *s_r_mo, *s_r_d, *s_r_h, *s_r_mi;
static char     s_opt_y[YEARS * 5 + 1], s_opt_mo[12 * 3 + 1], s_opt_d[31 * 3 + 1], s_opt_h[24 * 3 + 1], s_opt_mi[60 * 3 + 1];

static void build_opts(char *out, size_t cap, int from, int n, int width) {
    size_t o = 0;
    out[0] = '\0';
    for (int i = 0; i < n; i++) {
        int w = snprintf(out + o, cap - o, "%s%0*d", i ? "\n" : "", width, from + i);
        if (w < 0 || (size_t)w >= cap - o) break;
        o += (size_t)w;
    }
}

static lv_obj_t *roller(lv_obj_t *parent, const char *opts) {
    lv_obj_t *r = lv_roller_create(parent);
    lv_roller_set_options(r, opts, LV_ROLLER_MODE_NORMAL);
    lv_roller_set_visible_row_count(r, 3);
    return r;
}

/* ---- TZ ---- */
static void tz_show(void) { if (s_tz_lbl) lv_label_set_text(s_tz_lbl, s_tz[0] ? s_tz : "(none)"); }

static void tz_kb_done(void *ctx, int accepted, const char *text) {
    (void)ctx;
    if (!accepted) return;
    snprintf(s_tz, sizeof s_tz, "%s", text ? text : "");
    s_tz_dirty = 1;
    tz_show();
}

static void tz_click(lv_event_t *e) {
    (void)e;
    wdg_keyboard_open("POSIX TZ (e.g. CET-1CEST,M3.5.0,M10.5.0/3)", PCFG_KB_TEXT_NOSPACE, s_tz, 1, 47, 0, tz_kb_done, NULL);
}

static void tz_run(pnl_job_t *j) { j->rc = psvc_tz_set((const char *)j->arg, PSVC_LOCK_PANEL_MS); }

static void tz_done(pnl_job_t *j) {
    s_tz_busy = 0;
    if (j->rc == PSVC_OK) s_tz_dirty = 0;
    pnl_msg(PNL_CTX_TZ, j->rc, NULL, s_tz_last, sizeof s_tz_last);
    s_tz_err = j->rc != PSVC_OK;
    pnl_kit_msg_set(s_tz_msg, s_tz_last, s_tz_err ? PNL_KIT_ERR : PNL_KIT_OK);   /* NULL-safe, no screen_gen gate */
}

static void tz_set_click(lv_event_t *e) {
    (void)e;
    if (s_tz_busy) return;
    if (!s_tz[0] || strchr(s_tz, ' ')) {                                /* app.js:1834 */
        pnl_kit_msg_set(s_tz_msg, "Enter a POSIX TZ with no spaces", PNL_KIT_ERR);
        return;
    }
    char tz[48];
    snprintf(tz, sizeof tz, "%s", s_tz);
    if (pnl_worker_submit(tz_run, tz_done, tz, sizeof tz) != 0) {
        pnl_kit_msg_set(s_tz_msg, "Panel busy -- try again", PNL_KIT_ERR);
        return;
    }
    s_tz_busy = 1;
    s_tz_last[0] = '\0';
    pnl_kit_msg_set(s_tz_msg, "...", PNL_KIT_INFO);
}

/* ---- Set clock (D14): local time in, SET TIME <UTC> out ---- */
static void clk_run(pnl_job_t *j) { j->irc = pnl_cmd_run((const char *)j->arg, (char *)j->out, PNL_JOB_OUT_MAX); }

static void clk_done(pnl_job_t *j) {
    s_clk_busy = 0;
    memcpy(s_clk_last, j->out, sizeof s_clk_last);   /* the reply, verbatim; pnl_cmd_run NUL-terminates within out */
    s_clk_last[sizeof s_clk_last - 1] = '\0';
    s_clk_err = j->irc != 0;
    pnl_kit_msg_set(s_clk_msg, s_clk_last, s_clk_err ? PNL_KIT_ERR : PNL_KIT_OK);   /* NULL-safe, no screen_gen gate */
    pnl_kit_enable(s_clk_btn, s_have_snap);
}

static void clk_click(lv_event_t *e) {
    (void)e;
    if (s_clk_busy || !s_have_snap || !s_r_y) return;
    int y  = YEAR0 + (int)lv_roller_get_selected(s_r_y);
    int mo = 1 + (int)lv_roller_get_selected(s_r_mo);
    int d  = 1 + (int)lv_roller_get_selected(s_r_d);
    int h  = (int)lv_roller_get_selected(s_r_h);
    int mi = (int)lv_roller_get_selected(s_r_mi);
    char line[48];
    if (pnl_set_time_line(y, mo, d, h, mi, s_offset, line, sizeof line) != 0) {
        pnl_kit_msg_set(s_clk_msg, "Pick a real date between 2020 and 2099", PNL_KIT_ERR);
        return;
    }
    if (pnl_worker_submit(clk_run, clk_done, line, sizeof line) != 0) {
        pnl_kit_msg_set(s_clk_msg, "Panel busy -- try again", PNL_KIT_ERR);
        return;
    }
    s_clk_busy = 1;
    s_clk_last[0] = '\0';
    pnl_kit_enable(s_clk_btn, 0);
    pnl_kit_msg_set(s_clk_msg, "...", PNL_KIT_INFO);
}

/* ---- section ---- */
static void time_build(lv_obj_t *parent) {
    if (!s_tz_dirty) {                          /* prefill from the master config, like app.js's cfgDoc[0].TIME.TZ */
        hg_mcfg_t m;
        psvc_mcfg_get(&m);
        snprintf(s_tz, sizeof s_tz, "%s", m.tz);
        pnl_zero(&m, sizeof m);                 /* the copy holds both Wi-Fi passwords; a memset here is a dead store */
    }
    lv_obj_t *c = pnl_kit_card(parent, "Time");
    s_now_lbl = pnl_kit_msg(c);
    s_tz_lbl = pnl_kit_field(c, "Time zone", tz_click, NULL);
    pnl_kit_button(c, "Set TZ", tz_set_click, NULL);
    s_tz_msg = pnl_kit_msg(c);
    if (s_tz_busy) pnl_kit_msg_set(s_tz_msg, "...", PNL_KIT_INFO);
    else if (s_tz_last[0]) pnl_kit_msg_set(s_tz_msg, s_tz_last, s_tz_err ? PNL_KIT_ERR : PNL_KIT_OK);
    tz_show();

    lv_obj_t *k = pnl_kit_card(parent, "Set clock (local time)");
    lv_obj_t *note = pnl_kit_msg(k);
    pnl_kit_msg_set(note, "For when NTP is unavailable. The master keeps UTC; this converts with the current UTC offset.",
                    PNL_KIT_INFO);
    if (!s_opt_y[0]) {
        build_opts(s_opt_y, sizeof s_opt_y, YEAR0, YEARS, 4);
        build_opts(s_opt_mo, sizeof s_opt_mo, 1, 12, 2);
        build_opts(s_opt_d, sizeof s_opt_d, 1, 31, 2);
        build_opts(s_opt_h, sizeof s_opt_h, 0, 24, 2);
        build_opts(s_opt_mi, sizeof s_opt_mi, 0, 60, 2);
    }
    lv_obj_t *r = pnl_kit_row(k);
    s_r_y = roller(r, s_opt_y);
    s_r_mo = roller(r, s_opt_mo);
    s_r_d = roller(r, s_opt_d);
    lv_obj_t *sep = lv_label_create(r);
    lv_label_set_text(sep, "   ");
    s_r_h = roller(r, s_opt_h);
    s_r_mi = roller(r, s_opt_mi);
    lv_roller_set_selected(s_r_y, 2026 - YEAR0, LV_ANIM_OFF);
    lv_roller_set_selected(s_r_h, 12, LV_ANIM_OFF);
    s_clk_btn = pnl_kit_button(k, "Set clock", clk_click, NULL);
    pnl_kit_enable(s_clk_btn, 0);               /* until the first snapshot delivers the UTC offset */
    s_clk_msg = pnl_kit_msg(k);
    if (s_clk_busy) pnl_kit_msg_set(s_clk_msg, "...", PNL_KIT_INFO);
    else if (s_clk_last[0]) pnl_kit_msg_set(s_clk_msg, s_clk_last, s_clk_err ? PNL_KIT_ERR : PNL_KIT_OK);
    s_prefilled = 0;
}

static void time_update(const pnl_snap_t *s) {
    if (!s_now_lbl) return;
    char b[80];
    pnl_fmt_master_time(s->st.time, s->st.time_src, s->st.utc_offset_s, s->st.time_is_set, b, sizeof b);
    pnl_label_set_if_changed(s_now_lbl, b);
    s_offset = s->st.utc_offset_s;
    s_have_snap = 1;
    if (!s_clk_busy) pnl_kit_enable(s_clk_btn, 1);
    if (!s_prefilled && s_r_y) {
        pnl_local_t t;
        pnl_local_time((int64_t)time(NULL), s->st.utc_offset_s, s->st.time_is_set, &t);
        if (t.valid && t.year >= YEAR0 && t.year < YEAR0 + YEARS) {
            lv_roller_set_selected(s_r_y, (uint32_t)(t.year - YEAR0), LV_ANIM_OFF);
            lv_roller_set_selected(s_r_mo, (uint32_t)(t.mon - 1), LV_ANIM_OFF);
            lv_roller_set_selected(s_r_d, (uint32_t)(t.mday - 1), LV_ANIM_OFF);
            lv_roller_set_selected(s_r_h, (uint32_t)t.hour, LV_ANIM_OFF);
            lv_roller_set_selected(s_r_mi, (uint32_t)t.min, LV_ANIM_OFF);
        }
        s_prefilled = 1;
    }
}

static void time_teardown(void) {
    s_now_lbl = s_tz_lbl = s_tz_msg = s_clk_msg = s_clk_btn = NULL;
    s_r_y = s_r_mo = s_r_d = s_r_h = s_r_mi = NULL;
}

const pnl_sys_section_t PNL_SYS_TIME = { .title = "Time", .build = time_build, .update = time_update,
                                         .teardown = time_teardown };
```
`lv_roller_set_visible_row_count` is LVGL 9.5 API (`src/widgets/roller/lv_roller.h`).

- [ ] **Step 4: Register the destination and the sources**

In `components/panel_ui/scr_shell.c`'s destination registry, the `PNL_DEST_SYSTEM` row changes from
`&PNL_SCR_PLACEHOLDER` to `&PNL_SCR_SYSTEM` (the extern is already in `scr_shell.h`, Task 12). Replace only this line:
```c
    [PNL_DEST_SYSTEM]    = { "System",     &PNL_SCR_PLACEHOLDER, 1 },   /* Task 23 */
```
with:
```c
    [PNL_DEST_SYSTEM]    = { "System",     &PNL_SCR_SYSTEM,      1 },
```

In `components/panel_ui/CMakeLists.txt`, inside the gated block:
```cmake
    list(APPEND PANEL_SRCS "scr_system.c" "sys_wifi.c" "sys_time.c")
```

- [ ] **Step 5: Run the gates**

1. GATE-HOST. Expected: `out of 48` (no new test file; the pure helpers used here were host-tested in Tasks 10 and 18).
2. GATE-ESP32. Expected: three `Project build complete`, empty lock diff.
3. GATE-P4. Expected: `Project build complete`, 0 warnings.
4. FLASH-P4 (dry run, then real).
5. Bench spot check: System → Wi-Fi shows the STA and AP lines matching the web's System page; Scan shows "Scanning...
   (the AP pauses briefly)" and then a list; tapping an entry fills SSID; Join with an empty SSID says "Enter an SSID";
   Set AP with a 5-character password says "AP SSID required, password 8+ chars". Time shows the local time with its
   "(UTC+hh:mm)" suffix and the source. The full checks are Stage 3 gate steps 3-4.

- [ ] **Step 6: Commit**

```powershell
git -C C:\Projects\HillGrov add components/panel_ui/scr_system.h components/panel_ui/scr_system.c components/panel_ui/sys_wifi.c components/panel_ui/sys_time.c components/panel_ui/scr_shell.c components/panel_ui/CMakeLists.txt
git -C C:\Projects\HillGrov commit -m "feat(panel_ui): System -- Wi-Fi and time" -m "Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>"
git -C C:\Projects\HillGrov push
```

---

### Task 24: System — web password reset without the old password (Stage 3)

Spec Decision 3 and "Consequences of no panel auth": the panel is the deliberate password-recovery path, so it sets the
web password **without** the old one. The web's own form verifies the old password first
(`components/http_srv/http_login.c:132-176`); the panel calls `psvc_web_password_set()` (Task 4), which hashes into a
copy, commits, and drops every web session inside the master-config lock. `docs/what_we_learned.md` records that a
password change must say loudly that every phone is logged out.

**Files:**
- Create: `components/panel_ui/sys_password.c`
- Modify: `components/panel_ui/scr_system.h` (declare `sys_password_wipe`), `components/panel_ui/scr_system.c` (registry PASSWORD), `components/panel_ui/CMakeLists.txt`

**Interfaces:**
- Consumes: `psvc_web_password_set(const char *pw, uint32_t lock_ms)` and `PSVC_LOCK_PANEL_MS` (Tasks 3-4);
  `pnl_msg(PNL_CTX_PASSWORD, ...)` (Task 18); `wdg_keyboard_open` masked, `pnl_text_ok` (Task 19);
  `pnl_kit_*`, `pnl_confirm` (Task 22); `pnl_worker_submit` (Task 8).
- Produces: `PNL_SYS_PASSWORD` and `void sys_password_wipe(void)` (Task 27).

**What proves what:** glue only. GATE-P4 proves it links; the Stage 3 gate step 5 (a phone logged out, then logging in
with the new password) proves the behaviour. The service call itself is covered by Task 4's refactor gates and by
`web_test --only mcfg` (Task 1) on the bench.

- [ ] **Step 1: Write the section**

Create `components/panel_ui/sys_password.c`:
```c
#include <stdio.h>
#include <string.h>
#include "lvgl.h"
#include "psvc_net.h"
#include "psvc_mcfg.h"      /* PSVC_LOCK_PANEL_MS */
#include "pcfg_gen.h"       /* PCFG_KB_TEXT */
#include "pnl_input.h"      /* pnl_text_ok */
#include "pnl_msg.h"
#include "pnl_worker.h"
#include "pnl_ui_kit.h"
#include "wdg_keyboard.h"
#include "scr_system.h"

#define PW_MIN 8    /* WA_PW_MIN / WA_PW_MAX, components/web_auth/web_auth.h:22-23 */
#define PW_MAX 63

static char     s_pw[PW_MAX + 1];
static uint8_t  s_busy;
static lv_obj_t *s_pw_lbl, *s_set_btn, *s_msg;

static void refresh(void) {
    if (s_pw_lbl) lv_label_set_text(s_pw_lbl, s_pw[0] ? "********" : "(not entered)");
    pnl_kit_enable(s_set_btn, !s_busy && pnl_text_ok(PCFG_KB_TEXT, s_pw, PW_MIN, PW_MAX));
}

static void kb_done(void *ctx, int accepted, const char *text) {
    (void)ctx;
    if (accepted) snprintf(s_pw, sizeof s_pw, "%s", text ? text : "");
    refresh();
}

static void pw_click(lv_event_t *e) {
    (void)e;
    wdg_keyboard_open("New web password (8 to 63 characters)", PCFG_KB_TEXT, "", PW_MIN, PW_MAX, 1, kb_done, NULL);
}

static void pw_run(pnl_job_t *j) { j->rc = psvc_web_password_set((const char *)j->arg, PSVC_LOCK_PANEL_MS); }

static void close_click(lv_event_t *e) { lv_msgbox_close_async((lv_obj_t *)lv_event_get_user_data(e)); }

static void pw_done(pnl_job_t *j) {
    s_busy = 0;
    char m[128];
    pnl_msg(PNL_CTX_PASSWORD, j->rc, NULL, m, sizeof m);
    if (j->rc == PSVC_OK) {
        /* large and unmistakable (docs/what_we_learned.md): every phone just lost its session */
        lv_obj_t *mb = lv_msgbox_create(NULL);
        lv_msgbox_add_title(mb, "Web password changed");
        lv_obj_t *t = lv_msgbox_add_text(mb, m);
        lv_obj_set_style_text_font(t, &lv_font_montserrat_28, 0);
        lv_obj_t *b = lv_msgbox_add_footer_button(mb, "OK");
        lv_obj_add_event_cb(b, close_click, LV_EVENT_CLICKED, mb);
    }
    if (j->screen_gen == pnl_screen_gen()) {
        pnl_kit_msg_set(s_msg, m, j->rc == PSVC_OK ? PNL_KIT_OK : PNL_KIT_ERR);
        refresh();
    }
}

static void pw_go(void *ctx) {
    (void)ctx;
    char pw[PW_MAX + 1];
    snprintf(pw, sizeof pw, "%s", s_pw);
    pnl_zero(s_pw, sizeof s_pw);                /* wiped right after submit */
    if (pnl_worker_submit(pw_run, pw_done, pw, sizeof pw) != 0) pnl_kit_msg_set(s_msg, "Panel busy -- try again", PNL_KIT_ERR);
    else { s_busy = 1; pnl_kit_msg_set(s_msg, "...", PNL_KIT_INFO); }
    pnl_zero(pw, sizeof pw);                    /* a dying local: a plain memset is a dead store at -Os */
    refresh();
}

static void set_click(lv_event_t *e) {
    (void)e;
    if (s_busy || !pnl_text_ok(PCFG_KB_TEXT, s_pw, PW_MIN, PW_MAX)) return;
    pnl_confirm("Change the web password?", "This logs out every phone and browser using the web UI",
                "Change password", pw_go, NULL);
}

static void pw_build(lv_obj_t *parent) {
    lv_obj_t *c = pnl_kit_card(parent, "Web password");
    lv_obj_t *note = pnl_kit_msg(c);
    pnl_kit_msg_set(note, "No old password is needed here: standing at the panel is the gate.", PNL_KIT_INFO);
    s_pw_lbl = pnl_kit_field(c, "New password", pw_click, NULL);
    s_set_btn = pnl_kit_button(c, "Set password", set_click, NULL);
    s_msg = pnl_kit_msg(c);
    lv_obj_set_style_text_font(s_msg, &lv_font_montserrat_28, 0);
    refresh();
}

static void pw_teardown(void) {
    memset(s_pw, 0, sizeof s_pw);               /* an unsubmitted password never outlives its screen */
    s_pw_lbl = s_set_btn = s_msg = NULL;
}

void sys_password_wipe(void) {
    memset(s_pw, 0, sizeof s_pw);
    pnl_kit_msg_set(s_msg, "", PNL_KIT_INFO);
    refresh();
}

const pnl_sys_section_t PNL_SYS_PASSWORD = { .title = "Password", .build = pw_build, .update = NULL,
                                             .teardown = pw_teardown };
```

- [ ] **Step 2: Register it**

In `components/panel_ui/scr_system.h`, after `void sys_wifi_wipe(void);`, add:
```c
void sys_password_wipe(void);   /* the unsubmitted new password (Task 27) */
```
In `components/panel_ui/scr_system.c`, change
`[PNL_SYS_SEC_PASSWORD] = { "Password", &PNL_SYS_PLACEHOLDER },` to
`[PNL_SYS_SEC_PASSWORD] = { "Password", &PNL_SYS_PASSWORD },`.

In `components/panel_ui/CMakeLists.txt`, inside the gated block:
```cmake
    list(APPEND PANEL_SRCS "sys_password.c")
```

- [ ] **Step 3: Run the gates**

1. GATE-HOST: `out of 48`.
2. GATE-ESP32: three `Project build complete`, empty lock diff.
3. GATE-P4: `Project build complete`, 0 warnings.
4. FLASH-P4 (dry run, then real).
5. Bench spot check: System → Password; "Set password" stays disabled until 8 characters are entered; the confirm text is
   "This logs out every phone and browser using the web UI". Cancel it: the bench password is changed only in the
   Stage 3 gate.

- [ ] **Step 4: Commit**

```powershell
git -C C:\Projects\HillGrov add components/panel_ui/sys_password.c components/panel_ui/scr_system.h components/panel_ui/scr_system.c components/panel_ui/CMakeLists.txt
git -C C:\Projects\HillGrov commit -m "feat(panel_ui): web password reset from the panel" -m "Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>"
git -C C:\Projects\HillGrov push
```

---

### Task 25: System — fleet update and reboot; the web's fleet codes from the shared mapping (Stage 3)

`http_fleet.c` maps `node_mgr_fw_*` return codes to tokens inline (`components/http_srv/http_fleet.c:47-63`:
start `-2` → `FLEET_BUSY`, any other non-zero → `FLEET_REJECTED`; abort non-zero → `NOT_ACTIVE`). That mapping moves
into `psvc_rc` so both faces say the same thing, and a small `psvc_fleet` wraps the three calls. The web's HTTP status
codes are unchanged (400 INVALID, 409 with the token, 202/200). The Fleet section follows `web/app.js:989-1010` (buttons
enabled only while the fleet line is `IDLE`, Abort only while it is not) and `web/app.js:1928-1954` (messages). The one
reboot flow for the whole panel lives here (D16): a confirm, a second confirm while the running slot is on OTA trial,
then an optimistic full-screen "Rebooting..." exactly as the web shows it before the reply (`web/app.js:1909-1925`).

**Files:**
- Create: `components/panel_svc/psvc_fleet.h`, `components/panel_svc/psvc_fleet.c`, `components/panel_ui/sys_fleet.c`
- Modify: `components/panel_svc/psvc_rc.h`, `components/panel_svc/psvc_rc.c`, `tests/host/test_psvc_rc.c`
- Modify: `components/panel_svc/CMakeLists.txt`, `tests/host/CMakeLists.txt` (the `test_psvc_rc` row gains `psvc_fleet.c`)
- Modify: `components/http_srv/http_fleet.c`
- Modify: `components/panel_ui/scr_system.h` (declare `sys_reboot_confirm`), `components/panel_ui/scr_system.c` (registry FLEET), `components/panel_ui/CMakeLists.txt`

**Interfaces:**
- Consumes: `node_mgr_fw_zone`, `node_mgr_fw_all`, `node_mgr_fw_abort` (`components/node_mgr/node_mgr.h:113-115`);
  `psvc_rc_t`, `psvc_rc_token` (Task 3); `pnl_cmd_run` (Task 22); `pnl_msg(PNL_CTX_FLEET_ZONE|FLEET_ALL|FLEET_ABORT)`
  (Task 18); `pnl_zone_name` (Task 10); `pnl_kit_*`, `pnl_confirm` (Task 22); `pnl_poll_latest`, `pnl_poll_kick` (Task 11).
- Produces:
  ```c
  /* psvc_rc.h addition (pure) */
  psvc_rc_t psvc_rc_from_fleet(int rc, int is_abort);   /* 0 OK; abort: -1 NOT_ACTIVE; start: -2 FLEET_BUSY, -1 FLEET_REJECTED */
  /* psvc_fleet.h */
  psvc_rc_t psvc_fleet_zone(uint8_t zone);   /* [WORKER] zone outside 1..HG_MAX_ZONES -> PSVC_E_INVALID */
  psvc_rc_t psvc_fleet_all(void);            /* [WORKER] */
  psvc_rc_t psvc_fleet_abort(void);          /* [WORKER] */
  int       psvc_fleet_idle(const char *fleet_line);   /* pure: 1 when "IDLE" -- the web's enable rule (app.js:995-1008) */
  /* sys_fleet.c (glue) -- the ONE reboot flow; Tasks 26 (orientation restart) and 32 (Reboot now) call it */
  void sys_reboot_confirm(const char *why);
  ```
  and `PNL_SYS_FLEET`. Mapping detail: every non-zero abort rc is `NOT_ACTIVE`, and every non-zero start rc other than
  `-2` is `FLEET_REJECTED`, which is what `http_fleet.c` does today.

**What proves what:** `test_psvc_rc` pins the mapping and `psvc_fleet_*`'s range check and wiring (with `node_mgr`
stubs in the test). `web_test.py --only uploads --fleet 2` proves the web's codes did not move. The Fleet section and
the reboot flow are glue: GATE-P4 proves they link, the Stage 3 gate steps 6-7 prove them on glass.

- [ ] **Step 1: Write the failing tests**

In `tests/host/test_psvc_rc.c` (Task 3's file), add `#include "psvc_fleet.h"` after its existing includes, then add
these stubs and tests above `main()`:
```c
/* node_mgr stubs: psvc_fleet.c is linked here so its range check and wiring are pinned, not just the mapping. */
static int     g_zone_rc, g_all_rc, g_abort_rc, g_zone_calls;
static uint8_t g_zone_last;
int node_mgr_fw_zone(uint8_t zone) { g_zone_calls++; g_zone_last = zone; return g_zone_rc; }
int node_mgr_fw_all(void)          { return g_all_rc; }
int node_mgr_fw_abort(void)        { return g_abort_rc; }

static void test_fleet_start_codes(void) {
    TEST_ASSERT_EQUAL_INT(PSVC_OK, psvc_rc_from_fleet(0, 0));
    TEST_ASSERT_EQUAL_INT(PSVC_E_FLEET_BUSY, psvc_rc_from_fleet(-2, 0));
    TEST_ASSERT_EQUAL_INT(PSVC_E_FLEET_REJECTED, psvc_rc_from_fleet(-1, 0));
    TEST_ASSERT_EQUAL_INT(PSVC_E_FLEET_REJECTED, psvc_rc_from_fleet(-7, 0));   /* http_fleet.c: anything but -2 */
}

static void test_fleet_abort_codes(void) {
    TEST_ASSERT_EQUAL_INT(PSVC_OK, psvc_rc_from_fleet(0, 1));
    TEST_ASSERT_EQUAL_INT(PSVC_E_NOT_ACTIVE, psvc_rc_from_fleet(-1, 1));
    TEST_ASSERT_EQUAL_INT(PSVC_E_NOT_ACTIVE, psvc_rc_from_fleet(-2, 1));
}

static void test_fleet_tokens_are_the_webs(void) {
    TEST_ASSERT_EQUAL_STRING("FLEET_BUSY", psvc_rc_token(psvc_rc_from_fleet(-2, 0)));
    TEST_ASSERT_EQUAL_STRING("FLEET_REJECTED", psvc_rc_token(psvc_rc_from_fleet(-1, 0)));
    TEST_ASSERT_EQUAL_STRING("NOT_ACTIVE", psvc_rc_token(psvc_rc_from_fleet(-1, 1)));
}

static void test_fleet_zone_range_and_wiring(void) {
    g_zone_calls = 0; g_zone_rc = 0;
    TEST_ASSERT_EQUAL_INT(PSVC_E_INVALID, psvc_fleet_zone(0));
    TEST_ASSERT_EQUAL_INT(PSVC_E_INVALID, psvc_fleet_zone(9));
    TEST_ASSERT_EQUAL_INT(0, g_zone_calls);                               /* refused before node_mgr is asked */
    TEST_ASSERT_EQUAL_INT(PSVC_OK, psvc_fleet_zone(2));
    TEST_ASSERT_EQUAL_UINT8(2, g_zone_last);
    g_zone_rc = -2;
    TEST_ASSERT_EQUAL_INT(PSVC_E_FLEET_BUSY, psvc_fleet_zone(8));
    g_all_rc = -1;
    TEST_ASSERT_EQUAL_INT(PSVC_E_FLEET_REJECTED, psvc_fleet_all());
    g_abort_rc = -1;
    TEST_ASSERT_EQUAL_INT(PSVC_E_NOT_ACTIVE, psvc_fleet_abort());
    g_abort_rc = 0;
    TEST_ASSERT_EQUAL_INT(PSVC_OK, psvc_fleet_abort());
}

static void test_fleet_idle(void) {
    TEST_ASSERT_EQUAL_INT(1, psvc_fleet_idle("IDLE"));
    TEST_ASSERT_EQUAL_INT(0, psvc_fleet_idle("2 UPDATING"));
    TEST_ASSERT_EQUAL_INT(0, psvc_fleet_idle("idle"));      /* the token is exact (fleet_seq.c) */
    TEST_ASSERT_EQUAL_INT(0, psvc_fleet_idle(""));
    TEST_ASSERT_EQUAL_INT(0, psvc_fleet_idle(NULL));
}
```
and in `main()`, before `return UNITY_END();`:
```c
    RUN_TEST(test_fleet_start_codes);
    RUN_TEST(test_fleet_abort_codes);
    RUN_TEST(test_fleet_tokens_are_the_webs);
    RUN_TEST(test_fleet_zone_range_and_wiring);
    RUN_TEST(test_fleet_idle);
```
In `tests/host/CMakeLists.txt`, change the `test_psvc_rc` row (Task 3) to:
```cmake
hg_test(test_psvc_rc ${COMP}/panel_svc/psvc_rc.c ${COMP}/panel_svc/psvc_fleet.c)
```

- [ ] **Step 2: Run the test to verify it fails**

Run GATE-HOST with ` -R test_psvc_rc`.
Expected: the configure fails with `Cannot find source file: .../components/panel_svc/psvc_fleet.c`.

- [ ] **Step 3: Implement the mapping and the wrapper**

In `components/panel_svc/psvc_rc.h`, after `psvc_rc_to_net_legacy`'s declaration, add:
```c
psvc_rc_t psvc_rc_from_fleet(int rc, int is_abort);   /* 0 OK; abort: -1 NOT_ACTIVE; start: -2 FLEET_BUSY, -1 FLEET_REJECTED
                                                          (every other non-zero start rc is FLEET_REJECTED, every other
                                                          non-zero abort rc NOT_ACTIVE -- http_fleet.c's rule) */
```
In `components/panel_svc/psvc_rc.c`, append:
```c
psvc_rc_t psvc_rc_from_fleet(int rc, int is_abort) {
    if (rc == 0) return PSVC_OK;
    if (is_abort) return PSVC_E_NOT_ACTIVE;
    return rc == -2 ? PSVC_E_FLEET_BUSY : PSVC_E_FLEET_REJECTED;
}
```
Create `components/panel_svc/psvc_fleet.h`:
```c
#pragma once
/* Fleet start/abort for both faces: node_mgr's sequencer (SP3) behind the shared refusal vocabulary. The sequencer pulls
 * the zone_fw image over GET /fw/zone.bin; nothing here touches flash. */
#include <stdint.h>
#include "psvc_rc.h"

#ifdef __cplusplus
extern "C" {
#endif

psvc_rc_t psvc_fleet_zone(uint8_t zone);   /* [WORKER] zone outside 1..HG_MAX_ZONES -> PSVC_E_INVALID */
psvc_rc_t psvc_fleet_all(void);            /* [WORKER] */
psvc_rc_t psvc_fleet_abort(void);          /* [WORKER] */
int       psvc_fleet_idle(const char *fleet_line);   /* pure: 1 when "IDLE" -- the web's enable rule (app.js:995-1008) */

#ifdef __cplusplus
}
#endif
```
Create `components/panel_svc/psvc_fleet.c`:
```c
#include <string.h>
#include "node_mgr.h"      /* node_mgr_fw_zone/all/abort, HG_MAX_ZONES */
#include "psvc_fleet.h"

psvc_rc_t psvc_fleet_zone(uint8_t zone) {
    if (zone < 1 || zone > HG_MAX_ZONES) return PSVC_E_INVALID;
    return psvc_rc_from_fleet(node_mgr_fw_zone(zone), 0);
}

psvc_rc_t psvc_fleet_all(void) { return psvc_rc_from_fleet(node_mgr_fw_all(), 0); }

psvc_rc_t psvc_fleet_abort(void) { return psvc_rc_from_fleet(node_mgr_fw_abort(), 1); }

int psvc_fleet_idle(const char *fleet_line) { return fleet_line && strcmp(fleet_line, "IDLE") == 0; }
```
In `components/panel_svc/CMakeLists.txt`, add `"psvc_fleet.c"` to the `hillgrow_master` source list (`PSVC_SRCS`).
`node_mgr` is already in its `PRIV_REQUIRES` (Task 4).

- [ ] **Step 4: Run the test to verify it passes**

Run GATE-HOST with ` -R test_psvc_rc`. Expected: every case passes, including the five new ones.

- [ ] **Step 5: Put the web's fleet handlers on the shared mapping**

In `components/http_srv/http_fleet.c`, add `#include "psvc_fleet.h"` after `#include "node_mgr.h"` (keep that include:
it supplies `HG_MAX_ZONES`). Replace the body of `h_fleet_post` from `int rc;` to the end of the function with:
```c
    psvc_rc_t rc;
    if (cJSON_IsTrue(all)) {
        rc = psvc_fleet_all();
    } else if (zone_ok && zone->valueint >= 1 && zone->valueint <= HG_MAX_ZONES) {
        rc = psvc_fleet_zone((uint8_t)zone->valueint);
    } else {
        cJSON_Delete(root);
        http_srv_error(req, 400, "INVALID", NULL);
        return http_srv_done(req, 1);
    }
    cJSON_Delete(root);

    if (rc != PSVC_OK) {
        /* psvc_rc_from_fleet: -2 = a sequence is already running (or an upload holds the gate) -> FLEET_BUSY,
         * any other refusal -> FLEET_REJECTED -- unchanged tokens, now shared with the panel. */
        http_srv_error(req, 409, psvc_rc_token(rc), NULL);
        return http_srv_done(req, 1);
    }
    http_srv_json(req, 202, "{\"queued\":true}");
    return http_srv_done(req, 1);
}
```
and replace `h_fleet_delete`'s body with:
```c
    psvc_rc_t rc = psvc_fleet_abort();
    if (rc != PSVC_OK) {
        http_srv_error(req, 409, psvc_rc_token(rc), NULL);   /* NOT_ACTIVE */
        return http_srv_done(req, 0);
    }
    http_srv_json(req, 200, "{\"ok\":true}");
    return http_srv_done(req, 0);
```
`http_srv` already REQUIRES `panel_svc` (Task 4).

- [ ] **Step 6: Write the Fleet section and the one reboot flow**

Create `components/panel_ui/sys_fleet.c`:
```c
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "esp_heap_caps.h"
#include "lvgl.h"
#include "ring_proto.h"     /* HG_MAX_ZONES */
#include "psvc_fleet.h"
#include "pnl_fmt.h"        /* pnl_zone_name */
#include "pnl_msg.h"
#include "pnl_cmd.h"
#include "pnl_worker.h"
#include "pnl_palette.h"
#include "pnl_ui_kit.h"
#include "scr_system.h"

enum { OP_ZONE = 0, OP_ALL, OP_ABORT };
typedef struct { uint8_t op, zone; } fleet_arg_t;

static uint8_t  s_busy, s_idle;
static int      s_rows_mask = -1;   /* -1 = rows not built yet (0xFF is a real mask: 8 enrolled zones) */
static lv_obj_t *s_status, *s_rows, *s_all_btn, *s_abort_btn, *s_msg, *s_fw_lbl, *s_trial_lbl;
static lv_obj_t *s_upd[HG_MAX_ZONES];

/* ---- the one reboot flow (D16) ---- */
static pnl_snap_t *s_rsnap;       /* PSRAM */
static lv_obj_t   *s_overlay, *s_overlay_lbl;

static void overlay_close(lv_event_t *e) {
    (void)e;
    if (s_overlay) lv_obj_delete_async(s_overlay);
    s_overlay = s_overlay_lbl = NULL;
}

static void reboot_run(pnl_job_t *j) { j->irc = pnl_cmd_run("REBOOT CONFIRM", (char *)j->out, PNL_JOB_OUT_MAX); }

static void reboot_done(pnl_job_t *j) {
    /* Only reached when the reboot did not happen: a real rejection overrides the optimistic text (app.js:1920-1923). */
    if (!s_overlay_lbl) return;
    lv_label_set_text(s_overlay_lbl, ((const char *)j->out)[0] ? (const char *)j->out : "Reboot failed");
    pnl_kit_button(s_overlay, "Close", overlay_close, NULL);
}

static void reboot_go(void *ctx) {
    (void)ctx;
    s_overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_overlay, lv_color_hex(PNL_C_BG), 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(s_overlay, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_overlay, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    s_overlay_lbl = lv_label_create(s_overlay);
    lv_obj_set_style_text_font(s_overlay_lbl, &lv_font_montserrat_48, 0);
    lv_label_set_text(s_overlay_lbl, "Rebooting...");          /* shown before the reply, like the web */
    if (pnl_worker_submit(reboot_run, reboot_done, NULL, 0) != 0) {
        lv_label_set_text(s_overlay_lbl, "Panel busy -- reboot not sent");
        pnl_kit_button(s_overlay, "Close", overlay_close, NULL);
    }
}

static void reboot_trial_check(void *ctx) {
    (void)ctx;
    if (!s_rsnap) s_rsnap = heap_caps_malloc(sizeof *s_rsnap, MALLOC_CAP_SPIRAM);
    int pending = 0;
    if (s_rsnap) { pnl_poll_latest(s_rsnap); pending = strcmp(s_rsnap->st.fw_state, "PENDING") == 0; }
    if (pending)
        pnl_confirm("OTA trial in progress", "An OTA trial is in progress -- rebooting now retires the new image",
                    "Reboot anyway", reboot_go, NULL);
    else
        reboot_go(NULL);
}

void sys_reboot_confirm(const char *why) {
    char text[128];
    if (why) snprintf(text, sizeof text, "Reboot the master %s?", why);
    else snprintf(text, sizeof text, "Reboot the master?");
    pnl_confirm("Reboot", text, "Reboot", reboot_trial_check, NULL);
}

/* ---- fleet ---- */
static void fleet_run(pnl_job_t *j) {
    const fleet_arg_t *a = (const fleet_arg_t *)j->arg;
    j->rc = a->op == OP_ZONE ? psvc_fleet_zone(a->zone) : a->op == OP_ALL ? psvc_fleet_all() : psvc_fleet_abort();
}

static void enable_all(void) {
    for (int i = 0; i < HG_MAX_ZONES; i++) pnl_kit_enable(s_upd[i], !s_busy && s_idle);
    pnl_kit_enable(s_all_btn, !s_busy && s_idle);
    pnl_kit_enable(s_abort_btn, !s_busy && !s_idle);
}

static void fleet_done(pnl_job_t *j) {
    const fleet_arg_t *a = (const fleet_arg_t *)j->arg;
    s_busy = 0;
    pnl_msg_ctx_t ctx = a->op == OP_ZONE ? PNL_CTX_FLEET_ZONE : a->op == OP_ALL ? PNL_CTX_FLEET_ALL : PNL_CTX_FLEET_ABORT;
    pnl_msg_arg_t ma = { .zone = a->zone, .version = NULL, .slot = NULL, .len = 0 };
    char m[96];
    pnl_msg(ctx, j->rc, &ma, m, sizeof m);
    pnl_poll_kick();
    if (j->screen_gen == pnl_screen_gen()) {
        pnl_kit_msg_set(s_msg, m, j->rc == PSVC_OK ? PNL_KIT_OK : PNL_KIT_ERR);
        enable_all();
    }
}

static void submit(uint8_t op, uint8_t zone) {
    if (s_busy) return;
    fleet_arg_t a = { .op = op, .zone = zone };
    if (pnl_worker_submit(fleet_run, fleet_done, &a, sizeof a) != 0) { pnl_kit_msg_set(s_msg, "Panel busy -- try again", PNL_KIT_ERR); return; }
    s_busy = 1;
    enable_all();
    pnl_kit_msg_set(s_msg, "...", PNL_KIT_INFO);
}

static void upd_click(lv_event_t *e) { submit(OP_ZONE, (uint8_t)(intptr_t)lv_event_get_user_data(e)); }
static void all_click(lv_event_t *e) { (void)e; submit(OP_ALL, 0); }
static void abort_click(lv_event_t *e) { (void)e; submit(OP_ABORT, 0); }
static void reboot_click(lv_event_t *e) { (void)e; sys_reboot_confirm(NULL); }

static void rebuild_rows(const pnl_snap_t *s) {
    lv_obj_clean(s_rows);
    for (int i = 0; i < HG_MAX_ZONES; i++) s_upd[i] = NULL;
    uint8_t mask = 0;
    for (int i = 0; i < HG_MAX_ZONES; i++) {
        const hg_node_t *n = &s->st.node[i];
        if (!n->used) continue;
        mask |= (uint8_t)(1u << i);
        lv_obj_t *r = pnl_kit_row(s_rows);
        lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        char name[17];
        pnl_zone_name(n, name);
        lv_obj_t *l = lv_label_create(r);
        lv_label_set_text(l, name);
        lv_obj_set_width(l, 220);
        s_upd[i] = pnl_kit_button(r, "Update", upd_click, (void *)(intptr_t)n->id);
    }
    s_rows_mask = (int)mask;
}

static void fleet_build(lv_obj_t *parent) {
    lv_obj_t *c = pnl_kit_card(parent, "Fleet");
    s_status = pnl_kit_msg(c);
    s_rows = lv_obj_create(c);
    lv_obj_set_width(s_rows, LV_PCT(100));
    lv_obj_set_height(s_rows, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_rows, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_bg_opa(s_rows, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_rows, 0, 0);
    lv_obj_t *r = pnl_kit_row(c);
    s_all_btn = pnl_kit_button(r, "Update all", all_click, NULL);
    s_abort_btn = pnl_kit_button(r, "Abort", abort_click, NULL);
    s_msg = pnl_kit_msg(c);
    s_rows_mask = -1;     /* force the first update to build the rows */
    s_idle = 0;
    enable_all();

    lv_obj_t *rb = pnl_kit_card(parent, "Reboot");
    s_fw_lbl = pnl_kit_msg(rb);
    s_trial_lbl = pnl_kit_msg(rb);
    pnl_kit_button(rb, "Reboot master", reboot_click, NULL);
}

static void fleet_update(const pnl_snap_t *s) {
    if (!s_status) return;
    char b[64];
    snprintf(b, sizeof b, "Status: %s", s->st.fleet_line);
    pnl_label_set_if_changed(s_status, b);
    uint8_t mask = 0;
    for (int i = 0; i < HG_MAX_ZONES; i++) if (s->st.node[i].used) mask |= (uint8_t)(1u << i);
    if ((int)mask != s_rows_mask) rebuild_rows(s);
    s_idle = (uint8_t)psvc_fleet_idle(s->st.fleet_line);
    enable_all();
    snprintf(b, sizeof b, "Master firmware: %s %s", s->st.fw_slot, s->st.fw_state);
    pnl_label_set_if_changed(s_fw_lbl, b);
    int trial = strcmp(s->st.fw_state, "PENDING") == 0;
    pnl_kit_msg_set(s_trial_lbl, trial ? "OTA trial in progress -- rebooting now retires this image" : "",
                    trial ? PNL_KIT_ERR : PNL_KIT_INFO);
}

static void fleet_teardown(void) {
    s_status = s_rows = s_all_btn = s_abort_btn = s_msg = s_fw_lbl = s_trial_lbl = NULL;
    for (int i = 0; i < HG_MAX_ZONES; i++) s_upd[i] = NULL;
}

const pnl_sys_section_t PNL_SYS_FLEET = { .title = "Fleet", .build = fleet_build, .update = fleet_update,
                                          .teardown = fleet_teardown };
```
- [ ] **Step 7: Register it**

In `components/panel_ui/scr_system.h`, after `sys_password_wipe`, add:
```c
void sys_reboot_confirm(const char *why);   /* [LVGL] the ONE reboot flow (sys_fleet.c): confirm; a second confirm while
                                               st.fw_state is "PENDING" (D16); "Rebooting..."; job REBOOT CONFIRM */
```
In `components/panel_ui/scr_system.c`, change `[PNL_SYS_SEC_FLEET] = { "Fleet", &PNL_SYS_PLACEHOLDER },` to
`[PNL_SYS_SEC_FLEET] = { "Fleet", &PNL_SYS_FLEET },`.

In `components/panel_ui/CMakeLists.txt`, inside the gated block:
```cmake
    list(APPEND PANEL_SRCS "sys_fleet.c")
```

- [ ] **Step 8: Run the gates**

1. GATE-HOST. Expected: `out of 48` (test_psvc_rc gained cases, not a file).
2. GATE-ESP32. Expected: three `Project build complete`, empty lock diff (`http_fleet.c` and `panel_svc` build on the ESP32
   master too).
3. GATE-P4. Expected: `Project build complete`, 0 warnings.
4. FLASH-P4 (dry run, then real).
5. Web regression for the fleet codes (the zone build from GATE-ESP32 is the zone image):
   ```powershell
   python C:\Projects\HillGrov\tools\web_test.py 192.168.7.7 --password hillgrow1 --only uploads --master-bin C:\Projects\HillGrov\master\build_p4\hillgrow_master.bin --zone-bin C:\Projects\HillGrov\zone\build\hillgrow_zone.bin --fleet 2
   ```
   Expected: every check PASS. The suite OTA-updates the master (upload, `REBOOT CONFIRM`, reconnect within 90 s, the new
   slot PENDING then VALID) and reflashes zone 2 through the fleet sequencer, so allow about five minutes and expect the
   panel to restart once.
6. The panel checks are Stage 3 gate steps 6-7, and Stage 4 gate step 3 for the in-trial reboot warning and second
   confirm (D16): Stage 3 has no OTA trial to exercise it in.

- [ ] **Step 9: Commit**

```powershell
git -C C:\Projects\HillGrov add components/panel_svc/psvc_fleet.h components/panel_svc/psvc_fleet.c components/panel_svc/psvc_rc.h components/panel_svc/psvc_rc.c components/panel_svc/CMakeLists.txt components/http_srv/http_fleet.c components/panel_ui/sys_fleet.c components/panel_ui/scr_system.h components/panel_ui/scr_system.c components/panel_ui/CMakeLists.txt tests/host/test_psvc_rc.c tests/host/CMakeLists.txt
git -C C:\Projects\HillGrov commit -m "feat(panel_ui): fleet update and reboot; one fleet-code mapping for both faces" -m "Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>"
git -C C:\Projects\HillGrov push
```

---

### Task 26: Panel screen — preferences, brightness, clock face (analogue), orientation, About (Stage 3)

The spec's third home icon is **Panel**: display-local preferences only (brightness, dim schedule, clock face including an
analogue option, orientation, about). They are stored in their own NVS key, `"panel"/"prefs"`, packed by explicit byte
offset inside an `hg_blob` envelope (`components/hg_blob/hg_blob.h:19-24`), and never in the master config. A write that
fails (NVS full, or the recovery design's §6.5 "writes disabled") leaves the value live in RAM and logs one WARN.

**Files:**
- Create: `components/panel_ui/pnl_prefs.h`, `components/panel_ui/pnl_prefs.c` (P); `components/panel_ui/pnl_prefs_nvs.h`, `components/panel_ui/pnl_prefs_nvs.c` (G); `components/panel_ui/scr_panel.c` (G)
- Create: `tests/host/test_pnl_prefs.c`
- Modify: `components/panel_ui/panel_ui.c` (`pnl_prefs_load()` before `panel_hw_start(prefs.orient)`; initial brightness `prefs.dim.day_pct`)
- Modify: `components/panel_ui/scr_home.c` (the analogue face when `face == 1`)
- Modify: `components/panel_ui/scr_shell.c` (registry PANEL → `PNL_SCR_PANEL`)
- Modify: `components/panel_ui/CMakeLists.txt`, `tests/host/CMakeLists.txt`

**Interfaces:**
- Consumes: `hg_blob_wrap`, `hg_blob_unwrap`, `HG_BLOB_OK`, `HG_BLOB_MIGRATED`, `HG_BLOB_HDR_LEN` (`hg_blob.h`);
  `panel_hw_brightness`, `panel_hw_status`, `panel_hw_status_t`, `PNL_ORIENT_*` (Task 7); `lv_mem_monitor`;
  `psvc_state_t.version`, `.fw_slot`, `.fw_state`, `.wifi`, `.uptime_s`, `.heap_int_free_kb`, `.heap_int_min_kb` (Task 9);
  `pnl_local_t` (Task 10); `pnl_worker_submit`, `pnl_on_lvgl_task`, `pnl_worker_stack_free` (Task 8);
  `pnl_poll_stack_free` (Task 11); `uxTaskGetStackHighWaterMark`; `sys_reboot_confirm` (Task 25);
  `pnl_kit_*` (Task 22); `pnl_nav_go`, `PNL_DEST_DIAG`, `pnl_label_set_if_changed` (Task 12).
- Produces:
  ```c
  /* pnl_prefs.h (pure) */
  #define PNL_MAGIC_PREFS 0x4E504748u   /* 'HGPN' LE */
  #define PNL_PREFS_VER   1
  typedef struct { uint8_t mode;          /* pnl_dim_mode_t (Task 27): 0 OFF, 1 FOLLOW_LIGHTS, 2 FIXED */
                   uint8_t day_pct, night_pct; uint16_t fixed_start_min, fixed_end_min; uint16_t idle_s; } pnl_dim_cfg_t;
  typedef struct { pnl_dim_cfg_t dim; uint16_t wipe_idle_s; uint8_t face /*0 digital 1 analogue*/;
                   uint8_t orient /*PNL_ORIENT_**/; } pnl_prefs_t;
  void   pnl_prefs_defaults(pnl_prefs_t *p);   /* FOLLOW_LIGHTS, day 80, night 10, fixed 22:00-06:00, idle 60 s, wipe 300 s,
                                                   digital, NORMAL */
  size_t pnl_prefs_pack(const pnl_prefs_t *p, uint8_t *out, size_t cap);   /* hg_blob envelope; explicit offsets; 0 = cap too small */
  int    pnl_prefs_unpack(const uint8_t *in, size_t n, pnl_prefs_t *p);    /* 0 / -1 (p gets defaults); clamps night_pct >= 5 */
  /* pnl_prefs_nvs.h (glue) */
  void               pnl_prefs_load(void);                     /* boot: NVS "panel"/"prefs"; defaults on any failure */
  const pnl_prefs_t *pnl_prefs_get(void);                      /* [ANY] live copy (LVGL task writes it) */
  void               pnl_prefs_set(const pnl_prefs_t *p);      /* [LVGL] updates the live copy + submits a save job */
  int                pnl_prefs_save(const pnl_prefs_t *p);     /* [WORKER] 0 / -1 (value stays in RAM; logged) */
  ```
  and `PNL_SCR_PANEL`.
- Produces **(additions)**:
  ```c
  /* pnl_prefs.h */
  void pnl_prefs_clamp(pnl_prefs_t *p);   /* mode > 2 -> 1; day/night_pct -> 5..100; fixed_*_min > 1439 -> the default;
                                             idle_s < 10 -> 10; wipe_idle_s < 60 -> 60; face > 1 -> 0; orient > 1 -> 0 */
  /* pnl_prefs_nvs.h */
  void pnl_prefs_preview(const pnl_prefs_t *p);   /* [LVGL] live copy only, no save: a slider being dragged */
  ```
  Payload layout (16 bytes after the 16-byte envelope header): `+0 mode`, `+1 day_pct`, `+2 night_pct`, `+3 face`,
  `+4 orient`, `+5 0`, `+6 fixed_start_min u16 LE`, `+8 fixed_end_min u16 LE`, `+10 idle_s u16 LE`,
  `+12 wipe_idle_s u16 LE`, `+14..15 0`.

**What proves what:** `test_pnl_prefs` pins the defaults, the byte layout, corruption and version handling, and the
clamps. The NVS glue, the Panel screen and the analogue face are proven by GATE-P4 (they compile) and the Stage 3 gate
step 8 (they work on glass, including the orientation restart).

- [ ] **Step 1: Write the failing test**

Create `tests/host/test_pnl_prefs.c`:
```c
#include <string.h>
#include "unity.h"
#include "hg_blob.h"
#include "pnl_prefs.h"

/* Panel-local preferences live in NVS "panel"/"prefs" as an hg_blob envelope with explicit byte offsets (the project's
   rule for persisted data). Anything unreadable falls back to the defaults, and every stored value is clamped on the
   way in so a bad blob can never set the backlight to 0 %. */

void setUp(void) {}
void tearDown(void) {}

static void assert_defaults(const pnl_prefs_t *p) {
    TEST_ASSERT_EQUAL_UINT8(1, p->dim.mode);
    TEST_ASSERT_EQUAL_UINT8(80, p->dim.day_pct);
    TEST_ASSERT_EQUAL_UINT8(10, p->dim.night_pct);
    TEST_ASSERT_EQUAL_UINT16(22 * 60, p->dim.fixed_start_min);
    TEST_ASSERT_EQUAL_UINT16(6 * 60, p->dim.fixed_end_min);
    TEST_ASSERT_EQUAL_UINT16(60, p->dim.idle_s);
    TEST_ASSERT_EQUAL_UINT16(300, p->wipe_idle_s);
    TEST_ASSERT_EQUAL_UINT8(0, p->face);
    TEST_ASSERT_EQUAL_UINT8(0, p->orient);
}

static pnl_prefs_t sample(void) {
    pnl_prefs_t a;
    pnl_prefs_defaults(&a);
    a.dim.mode = 2; a.dim.day_pct = 65; a.dim.night_pct = 7;
    a.dim.fixed_start_min = 21 * 60 + 30; a.dim.fixed_end_min = 5 * 60 + 15; a.dim.idle_s = 30;
    a.wipe_idle_s = 600; a.face = 1; a.orient = 1;
    return a;
}

static void test_defaults(void) {
    pnl_prefs_t p;
    memset(&p, 0xAA, sizeof p);
    pnl_prefs_defaults(&p);
    assert_defaults(&p);
}

static void test_round_trip(void) {
    pnl_prefs_t a = sample(), b;
    uint8_t buf[64];
    size_t n = pnl_prefs_pack(&a, buf, sizeof buf);
    TEST_ASSERT_EQUAL_size_t(HG_BLOB_HDR_LEN + 16, n);
    memset(&b, 0, sizeof b);
    TEST_ASSERT_EQUAL_INT(0, pnl_prefs_unpack(buf, n, &b));
    TEST_ASSERT_EQUAL_UINT8(2, b.dim.mode);
    TEST_ASSERT_EQUAL_UINT8(65, b.dim.day_pct);
    TEST_ASSERT_EQUAL_UINT8(7, b.dim.night_pct);
    TEST_ASSERT_EQUAL_UINT16(21 * 60 + 30, b.dim.fixed_start_min);
    TEST_ASSERT_EQUAL_UINT16(5 * 60 + 15, b.dim.fixed_end_min);
    TEST_ASSERT_EQUAL_UINT16(30, b.dim.idle_s);
    TEST_ASSERT_EQUAL_UINT16(600, b.wipe_idle_s);
    TEST_ASSERT_EQUAL_UINT8(1, b.face);
    TEST_ASSERT_EQUAL_UINT8(1, b.orient);
}

static void test_explicit_byte_offsets(void) {
    pnl_prefs_t a = sample();
    uint8_t buf[64];
    TEST_ASSERT_EQUAL_size_t(32, pnl_prefs_pack(&a, buf, sizeof buf));
    const uint8_t *p = buf + HG_BLOB_HDR_LEN;
    TEST_ASSERT_EQUAL_HEX8(2, p[0]);
    TEST_ASSERT_EQUAL_HEX8(65, p[1]);
    TEST_ASSERT_EQUAL_HEX8(7, p[2]);
    TEST_ASSERT_EQUAL_HEX8(1, p[3]);
    TEST_ASSERT_EQUAL_HEX8(1, p[4]);
    TEST_ASSERT_EQUAL_HEX8(0, p[5]);
    TEST_ASSERT_EQUAL_HEX8((21 * 60 + 30) & 0xFF, p[6]); TEST_ASSERT_EQUAL_HEX8((21 * 60 + 30) >> 8, p[7]);
    TEST_ASSERT_EQUAL_HEX8((5 * 60 + 15) & 0xFF, p[8]);  TEST_ASSERT_EQUAL_HEX8((5 * 60 + 15) >> 8, p[9]);
    TEST_ASSERT_EQUAL_HEX8(30, p[10]);  TEST_ASSERT_EQUAL_HEX8(0, p[11]);
    TEST_ASSERT_EQUAL_HEX8(600 & 0xFF, p[12]); TEST_ASSERT_EQUAL_HEX8(600 >> 8, p[13]);
    TEST_ASSERT_EQUAL_HEX8(0x48, buf[0]);   /* 'HGPN' little-endian magic */
    TEST_ASSERT_EQUAL_HEX8(0x47, buf[1]);
    TEST_ASSERT_EQUAL_HEX8(0x50, buf[2]);
    TEST_ASSERT_EQUAL_HEX8(0x4E, buf[3]);
}

static void test_corrupt_crc_gives_defaults(void) {
    pnl_prefs_t a = sample(), b;
    uint8_t buf[64];
    size_t n = pnl_prefs_pack(&a, buf, sizeof buf);
    buf[HG_BLOB_HDR_LEN + 1] ^= 0x01;
    TEST_ASSERT_EQUAL_INT(-1, pnl_prefs_unpack(buf, n, &b));
    assert_defaults(&b);
}

static void test_newer_version_gives_defaults(void) {
    pnl_prefs_t a = sample(), b;
    uint8_t buf[64], newer[64];
    pnl_prefs_pack(&a, buf, sizeof buf);
    size_t n = hg_blob_wrap(PNL_MAGIC_PREFS, PNL_PREFS_VER + 1, 0, buf + HG_BLOB_HDR_LEN, 16, newer, sizeof newer);
    TEST_ASSERT_EQUAL_size_t(32, n);
    TEST_ASSERT_EQUAL_INT(-1, pnl_prefs_unpack(newer, n, &b));
    assert_defaults(&b);
}

static void test_short_and_wrong_magic_give_defaults(void) {
    pnl_prefs_t a = sample(), b;
    uint8_t buf[64];
    size_t n = pnl_prefs_pack(&a, buf, sizeof buf);
    TEST_ASSERT_EQUAL_INT(-1, pnl_prefs_unpack(buf, 10, &b));
    assert_defaults(&b);
    buf[0] ^= 0xFF;
    TEST_ASSERT_EQUAL_INT(-1, pnl_prefs_unpack(buf, n, &b));
    assert_defaults(&b);
}

static void test_clamps_on_unpack(void) {
    pnl_prefs_t a, b;
    pnl_prefs_defaults(&a);
    a.dim.mode = 9; a.dim.day_pct = 0; a.dim.night_pct = 0;
    a.dim.fixed_start_min = 2000; a.dim.fixed_end_min = 1440; a.dim.idle_s = 0;
    a.wipe_idle_s = 5; a.face = 7; a.orient = 3;
    uint8_t buf[64];
    size_t n = pnl_prefs_pack(&a, buf, sizeof buf);      /* pack writes what it is given */
    TEST_ASSERT_EQUAL_INT(0, pnl_prefs_unpack(buf, n, &b));
    TEST_ASSERT_EQUAL_UINT8(1, b.dim.mode);
    TEST_ASSERT_EQUAL_UINT8(5, b.dim.day_pct);
    TEST_ASSERT_EQUAL_UINT8(5, b.dim.night_pct);          /* never a dark panel (D4: minimum duty >= 5 %) */
    TEST_ASSERT_EQUAL_UINT16(22 * 60, b.dim.fixed_start_min);
    TEST_ASSERT_EQUAL_UINT16(6 * 60, b.dim.fixed_end_min);
    TEST_ASSERT_EQUAL_UINT16(10, b.dim.idle_s);
    TEST_ASSERT_EQUAL_UINT16(60, b.wipe_idle_s);
    TEST_ASSERT_EQUAL_UINT8(0, b.face);
    TEST_ASSERT_EQUAL_UINT8(0, b.orient);
}

static void test_clamp_upper_bounds(void) {
    pnl_prefs_t p;
    pnl_prefs_defaults(&p);
    p.dim.day_pct = 150; p.dim.night_pct = 101;
    pnl_prefs_clamp(&p);
    TEST_ASSERT_EQUAL_UINT8(100, p.dim.day_pct);
    TEST_ASSERT_EQUAL_UINT8(100, p.dim.night_pct);
}

static void test_pack_cap_too_small(void) {
    pnl_prefs_t a = sample();
    uint8_t buf[20];
    TEST_ASSERT_EQUAL_size_t(0, pnl_prefs_pack(&a, buf, sizeof buf));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_defaults);
    RUN_TEST(test_round_trip);
    RUN_TEST(test_explicit_byte_offsets);
    RUN_TEST(test_corrupt_crc_gives_defaults);
    RUN_TEST(test_newer_version_gives_defaults);
    RUN_TEST(test_short_and_wrong_magic_give_defaults);
    RUN_TEST(test_clamps_on_unpack);
    RUN_TEST(test_clamp_upper_bounds);
    RUN_TEST(test_pack_cap_too_small);
    return UNITY_END();
}
```
In `tests/host/CMakeLists.txt`, after the `test_pnl_console` row:
```cmake
hg_test(test_pnl_prefs ${COMP}/panel_ui/pnl_prefs.c ${COMP}/hg_blob/hg_blob.c)
```

- [ ] **Step 2: Run the test to verify it fails**

Run GATE-HOST with ` -R test_pnl_prefs`.
Expected: `Cannot find source file: .../components/panel_ui/pnl_prefs.c`.

- [ ] **Step 3: Write the pure preferences module**

Create `components/panel_ui/pnl_prefs.h`:
```c
#pragma once
/* Pure (host-tested, tests/host/test_pnl_prefs.c): panel-local preferences and their persisted form -- an hg_blob
 * envelope around a 16-byte payload with explicit offsets:
 *   +0 mode  +1 day_pct  +2 night_pct  +3 face  +4 orient  +5 0
 *   +6 fixed_start_min u16 LE  +8 fixed_end_min u16 LE  +10 idle_s u16 LE  +12 wipe_idle_s u16 LE  +14..15 0 */
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PNL_MAGIC_PREFS 0x4E504748u   /* 'HGPN' LE */
#define PNL_PREFS_VER   1
typedef struct { uint8_t mode;          /* pnl_dim_mode_t (Task 27): 0 OFF, 1 FOLLOW_LIGHTS, 2 FIXED */
                 uint8_t day_pct, night_pct; uint16_t fixed_start_min, fixed_end_min; uint16_t idle_s; } pnl_dim_cfg_t;
typedef struct { pnl_dim_cfg_t dim; uint16_t wipe_idle_s; uint8_t face /*0 digital 1 analogue*/;
                 uint8_t orient /*PNL_ORIENT_**/; } pnl_prefs_t;

void   pnl_prefs_defaults(pnl_prefs_t *p);   /* FOLLOW_LIGHTS, day 80, night 10, fixed 22:00-06:00, idle 60 s, wipe 300 s,
                                                 digital, NORMAL */
void   pnl_prefs_clamp(pnl_prefs_t *p);      /* mode > 2 -> 1; day/night_pct -> 5..100; fixed_*_min > 1439 -> the default;
                                                 idle_s < 10 -> 10; wipe_idle_s < 60 -> 60; face > 1 -> 0; orient > 1 -> 0 */
size_t pnl_prefs_pack(const pnl_prefs_t *p, uint8_t *out, size_t cap);   /* hg_blob envelope; explicit offsets; 0 = cap too small */
int    pnl_prefs_unpack(const uint8_t *in, size_t n, pnl_prefs_t *p);    /* 0 / -1 (p gets defaults); clamps night_pct >= 5 */

#ifdef __cplusplus
}
#endif
```
Create `components/panel_ui/pnl_prefs.c`:
```c
#include <string.h>
#include "hg_blob.h"
#include "pnl_prefs.h"

#define PREFS_PAYLOAD 16u
#define PCT_MIN 5u     /* D4: the minimum duty; bench-tune for flicker, never 0 */

static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }

void pnl_prefs_defaults(pnl_prefs_t *p) {
    memset(p, 0, sizeof *p);
    p->dim.mode = 1;                 /* PNL_DIM_FOLLOW_LIGHTS */
    p->dim.day_pct = 80;
    p->dim.night_pct = 10;
    p->dim.fixed_start_min = 22 * 60;
    p->dim.fixed_end_min = 6 * 60;
    p->dim.idle_s = 60;
    p->wipe_idle_s = 300;
    p->face = 0;
    p->orient = 0;                   /* PNL_ORIENT_NORMAL */
}

static uint8_t clamp_pct(uint8_t v) { return v < PCT_MIN ? PCT_MIN : v > 100 ? 100 : v; }

void pnl_prefs_clamp(pnl_prefs_t *p) {
    if (p->dim.mode > 2) p->dim.mode = 1;
    p->dim.day_pct = clamp_pct(p->dim.day_pct);
    p->dim.night_pct = clamp_pct(p->dim.night_pct);
    if (p->dim.fixed_start_min > 1439) p->dim.fixed_start_min = 22 * 60;
    if (p->dim.fixed_end_min > 1439) p->dim.fixed_end_min = 6 * 60;
    if (p->dim.idle_s < 10) p->dim.idle_s = 10;
    if (p->wipe_idle_s < 60) p->wipe_idle_s = 60;
    if (p->face > 1) p->face = 0;
    if (p->orient > 1) p->orient = 0;
}

size_t pnl_prefs_pack(const pnl_prefs_t *p, uint8_t *out, size_t cap) {
    uint8_t b[PREFS_PAYLOAD];
    memset(b, 0, sizeof b);
    b[0] = p->dim.mode;
    b[1] = p->dim.day_pct;
    b[2] = p->dim.night_pct;
    b[3] = p->face;
    b[4] = p->orient;
    wr16(b + 6, p->dim.fixed_start_min);
    wr16(b + 8, p->dim.fixed_end_min);
    wr16(b + 10, p->dim.idle_s);
    wr16(b + 12, p->wipe_idle_s);
    return hg_blob_wrap(PNL_MAGIC_PREFS, PNL_PREFS_VER, 0, b, PREFS_PAYLOAD, out, cap);
}

int pnl_prefs_unpack(const uint8_t *in, size_t n, pnl_prefs_t *p) {
    uint8_t b[PREFS_PAYLOAD];
    uint32_t gen = 0;
    hg_blob_rc_t rc = in ? hg_blob_unwrap(PNL_MAGIC_PREFS, PNL_PREFS_VER, PNL_PREFS_VER, in, n, b, PREFS_PAYLOAD, &gen)
                         : HG_BLOB_E_SHORT;
    pnl_prefs_defaults(p);
    if (rc != HG_BLOB_OK && rc != HG_BLOB_MIGRATED) return -1;
    p->dim.mode = b[0];
    p->dim.day_pct = b[1];
    p->dim.night_pct = b[2];
    p->face = b[3];
    p->orient = b[4];
    p->dim.fixed_start_min = rd16(b + 6);
    p->dim.fixed_end_min = rd16(b + 8);
    p->dim.idle_s = rd16(b + 10);
    p->wipe_idle_s = rd16(b + 12);
    pnl_prefs_clamp(p);
    return 0;
}
```

- [ ] **Step 4: Run the test to verify it passes**

Run GATE-HOST with ` -R test_pnl_prefs`. Expected: `9 Tests 0 Failures 0 Ignored`.

- [ ] **Step 5: Write the NVS glue**

Create `components/panel_ui/pnl_prefs_nvs.h`:
```c
#pragma once
/* Glue: the live preferences and their NVS home, "panel"/"prefs". */
#include "pnl_prefs.h"

void               pnl_prefs_load(void);                     /* boot: NVS "panel"/"prefs"; defaults on any failure */
const pnl_prefs_t *pnl_prefs_get(void);                      /* [ANY] live copy (LVGL task writes it) */
void               pnl_prefs_set(const pnl_prefs_t *p);      /* [LVGL] updates the live copy + submits a save job */
void               pnl_prefs_preview(const pnl_prefs_t *p);  /* [LVGL] live copy only, no save (a slider being dragged) */
int                pnl_prefs_save(const pnl_prefs_t *p);     /* [WORKER] 0 / -1 (value stays in RAM; logged) */
```
Create `components/panel_ui/pnl_prefs_nvs.c`:
```c
#include "esp_log.h"
#include "nvs.h"
#include "pnl_worker.h"
#include "pnl_prefs_nvs.h"

static const char *TAG = "pnl_prefs";
static pnl_prefs_t s_prefs;

void pnl_prefs_load(void) {
    pnl_prefs_defaults(&s_prefs);
    nvs_handle_t h;
    esp_err_t e = nvs_open("panel", NVS_READONLY, &h);
    if (e != ESP_OK) {
        if (e != ESP_ERR_NVS_NOT_FOUND) ESP_LOGW(TAG, "nvs_open(panel): %s -- defaults", esp_err_to_name(e));
        return;
    }
    uint8_t buf[64];
    size_t n = sizeof buf;
    e = nvs_get_blob(h, "prefs", buf, &n);
    nvs_close(h);
    if (e != ESP_OK) {
        if (e != ESP_ERR_NVS_NOT_FOUND) ESP_LOGW(TAG, "panel/prefs unreadable (%s) -- defaults", esp_err_to_name(e));
        return;
    }
    if (pnl_prefs_unpack(buf, n, &s_prefs) != 0) ESP_LOGW(TAG, "panel/prefs corrupt or from a newer image -- defaults");
}

const pnl_prefs_t *pnl_prefs_get(void) { return &s_prefs; }

void pnl_prefs_preview(const pnl_prefs_t *p) {
    pnl_prefs_t c = *p;
    pnl_prefs_clamp(&c);
    s_prefs = c;
}

static void save_run(pnl_job_t *j) { j->irc = pnl_prefs_save((const pnl_prefs_t *)j->arg); }
static void save_done(pnl_job_t *j) { (void)j; }

void pnl_prefs_set(const pnl_prefs_t *p) {
    pnl_prefs_t c = *p;
    pnl_prefs_clamp(&c);
    s_prefs = c;
    if (pnl_worker_submit(save_run, save_done, &c, sizeof c) != 0)
        ESP_LOGW(TAG, "preferences not queued for saving -- kept in RAM until the next change");
}

int pnl_prefs_save(const pnl_prefs_t *p) {
    if (pnl_on_lvgl_task()) { ESP_LOGE(TAG, "pnl_prefs_save called on the LVGL task -- refused"); return -1; }
    uint8_t buf[64];
    size_t n = pnl_prefs_pack(p, buf, sizeof buf);
    if (n == 0) return -1;
    nvs_handle_t h;
    esp_err_t e = nvs_open("panel", NVS_READWRITE, &h);
    if (e == ESP_OK) {
        e = nvs_set_blob(h, "prefs", buf, n);
        if (e == ESP_OK) e = nvs_commit(h);
        nvs_close(h);
    }
    if (e != ESP_OK) {
        /* recovery design 6.5: NVS may be deliberately read-only after a downgrade -- the value stays live in RAM */
        ESP_LOGW(TAG, "preferences not saved (%s) -- the new value stays in RAM", esp_err_to_name(e));
        return -1;
    }
    return 0;
}
```

- [ ] **Step 6: Write the Panel screen**

Create `components/panel_ui/scr_panel.c`:
```c
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "sdkconfig.h"      /* CONFIG_LV_MEM_SIZE_KILOBYTES */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"  /* uxTaskGetStackHighWaterMark */
#include "lvgl.h"
#include "panel_hw.h"
#include "pnl_poll.h"       /* pnl_poll_stack_free */
#include "pnl_worker.h"     /* pnl_worker_stack_free */
#include "pnl_prefs_nvs.h"
#include "pnl_palette.h"
#include "pnl_ui_kit.h"
#include "scr_shell.h"
#include "scr_system.h"     /* sys_reboot_confirm */

static const uint16_t IDLE_S[] = { 30, 60, 120, 300 };
static const char     IDLE_TXT[] = "30 s\n1 min\n2 min\n5 min";
static const uint16_t WIPE_S[] = { 120, 300, 600 };
static const char     WIPE_TXT[] = "2 min\n5 min\n10 min";
static const char    *MODE_TXT[3] = { "Off", "Follow the lights", "Fixed hours" };

static lv_obj_t *s_day, *s_night, *s_day_lbl, *s_night_lbl, *s_mode[3], *s_face[2], *s_orient[2];
static lv_obj_t *s_start, *s_end, *s_idle, *s_wipe, *s_about[8];
static char      s_hours[24 * 3 + 1];

static void set_checked(lv_obj_t **b, int n, int sel) {
    for (int i = 0; i < n; i++) {
        if (!b[i]) continue;
        if (i == sel) lv_obj_add_state(b[i], LV_STATE_CHECKED);
        else lv_obj_remove_state(b[i], LV_STATE_CHECKED);
    }
}

static int index_of(const uint16_t *v, int n, uint16_t x, int dflt) {
    for (int i = 0; i < n; i++) if (v[i] == x) return i;
    return dflt;
}

static void pct_labels(void) {
    const pnl_prefs_t *p = pnl_prefs_get();
    if (s_day_lbl) lv_label_set_text_fmt(s_day_lbl, "Day brightness %u %%", (unsigned)p->dim.day_pct);
    if (s_night_lbl) lv_label_set_text_fmt(s_night_lbl, "Night brightness %u %%", (unsigned)p->dim.night_pct);
}

static void day_changed(lv_event_t *e) {
    pnl_prefs_t p = *pnl_prefs_get();
    p.dim.day_pct = (uint8_t)lv_slider_get_value(lv_event_get_target_obj(e));
    pnl_prefs_preview(&p);
    panel_hw_brightness(p.dim.day_pct);
    pct_labels();
}

static void night_changed(lv_event_t *e) {
    pnl_prefs_t p = *pnl_prefs_get();
    p.dim.night_pct = (uint8_t)lv_slider_get_value(lv_event_get_target_obj(e));
    pnl_prefs_preview(&p);
    panel_hw_brightness(p.dim.night_pct);        /* show the night level while it is being chosen */
    pct_labels();
}

static void slider_released(lv_event_t *e) {
    (void)e;
    pnl_prefs_set(pnl_prefs_get());               /* save once, on release */
    panel_hw_brightness(pnl_prefs_get()->dim.day_pct);
}

static void mode_click(lv_event_t *e) {
    pnl_prefs_t p = *pnl_prefs_get();
    p.dim.mode = (uint8_t)(intptr_t)lv_event_get_user_data(e);
    pnl_prefs_set(&p);
    set_checked(s_mode, 3, p.dim.mode);
}

static void hours_changed(lv_event_t *e) {
    (void)e;
    pnl_prefs_t p = *pnl_prefs_get();
    p.dim.fixed_start_min = (uint16_t)(lv_roller_get_selected(s_start) * 60u);
    p.dim.fixed_end_min = (uint16_t)(lv_roller_get_selected(s_end) * 60u);
    pnl_prefs_set(&p);
}

static void idle_changed(lv_event_t *e) {
    (void)e;
    pnl_prefs_t p = *pnl_prefs_get();
    p.dim.idle_s = IDLE_S[lv_roller_get_selected(s_idle)];
    p.wipe_idle_s = WIPE_S[lv_roller_get_selected(s_wipe)];
    pnl_prefs_set(&p);
}

static void face_click(lv_event_t *e) {
    pnl_prefs_t p = *pnl_prefs_get();
    p.face = (uint8_t)(intptr_t)lv_event_get_user_data(e);
    pnl_prefs_set(&p);
    set_checked(s_face, 2, p.face);
}

static void orient_click(lv_event_t *e) {
    pnl_prefs_t p = *pnl_prefs_get();
    p.orient = (uint8_t)(intptr_t)lv_event_get_user_data(e);
    pnl_prefs_set(&p);
    set_checked(s_orient, 2, p.orient);
}

static void restart_click(lv_event_t *e) { (void)e; sys_reboot_confirm("to apply the new orientation"); }
static void touch_click(lv_event_t *e) { (void)e; pnl_nav_go(PNL_DEST_DIAG, 0); }

static lv_obj_t *slider(lv_obj_t *parent, uint8_t v, lv_event_cb_t changed) {
    lv_obj_t *s = lv_slider_create(parent);
    lv_obj_set_width(s, LV_PCT(90));
    lv_slider_set_range(s, 5, 100);
    lv_slider_set_value(s, v, LV_ANIM_OFF);
    lv_obj_add_event_cb(s, changed, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s, slider_released, LV_EVENT_RELEASED, NULL);
    return s;
}

static lv_obj_t *roller(lv_obj_t *parent, const char *opts, uint32_t sel, lv_event_cb_t cb) {
    lv_obj_t *r = lv_roller_create(parent);
    lv_roller_set_options(r, opts, LV_ROLLER_MODE_NORMAL);
    lv_roller_set_visible_row_count(r, 3);
    lv_roller_set_selected(r, sel, LV_ANIM_OFF);
    lv_obj_add_event_cb(r, cb, LV_EVENT_VALUE_CHANGED, NULL);
    return r;
}

static void panel_build(lv_obj_t *content, int arg) {
    (void)arg;
    const pnl_prefs_t *p = pnl_prefs_get();
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *b = pnl_kit_card(content, "Brightness");
    s_day_lbl = lv_label_create(b);
    s_day = slider(b, p->dim.day_pct, day_changed);
    s_night_lbl = lv_label_create(b);
    s_night = slider(b, p->dim.night_pct, night_changed);
    pct_labels();

    lv_obj_t *d = pnl_kit_card(content, "Night dimming");
    lv_obj_t *r = pnl_kit_row(d);
    for (int i = 0; i < 3; i++) {
        s_mode[i] = pnl_kit_button(r, MODE_TXT[i], mode_click, (void *)(intptr_t)i);
        lv_obj_add_flag(s_mode[i], LV_OBJ_FLAG_CHECKABLE);
    }
    set_checked(s_mode, 3, p->dim.mode);
    if (!s_hours[0]) {
        size_t o = 0;
        for (int h = 0; h < 24; h++) o += (size_t)snprintf(s_hours + o, sizeof s_hours - o, "%s%02d", h ? "\n" : "", h);
    }
    lv_obj_t *hr = pnl_kit_row(d);
    lv_obj_t *l = lv_label_create(hr); lv_label_set_text(l, "Fixed hours: dim from");
    s_start = roller(hr, s_hours, p->dim.fixed_start_min / 60u, hours_changed);
    l = lv_label_create(hr); lv_label_set_text(l, "to");
    s_end = roller(hr, s_hours, p->dim.fixed_end_min / 60u, hours_changed);
    lv_obj_t *ir = pnl_kit_row(d);
    l = lv_label_create(ir); lv_label_set_text(l, "Dim after idle");
    s_idle = roller(ir, IDLE_TXT, (uint32_t)index_of(IDLE_S, 4, p->dim.idle_s, 1), idle_changed);
    l = lv_label_create(ir); lv_label_set_text(l, "Return to Home and wipe after");
    s_wipe = roller(ir, WIPE_TXT, (uint32_t)index_of(WIPE_S, 3, p->wipe_idle_s, 1), idle_changed);
    lv_obj_t *note = pnl_kit_msg(d);
    pnl_kit_msg_set(note, "Follow the lights: dim while every scheduled shelf light is off. The first touch on a dimmed "
                          "panel only wakes it.", PNL_KIT_INFO);

    lv_obj_t *f = pnl_kit_card(content, "Clock face");
    r = pnl_kit_row(f);
    s_face[0] = pnl_kit_button(r, "Digital", face_click, (void *)(intptr_t)0);
    s_face[1] = pnl_kit_button(r, "Analogue", face_click, (void *)(intptr_t)1);
    for (int i = 0; i < 2; i++) lv_obj_add_flag(s_face[i], LV_OBJ_FLAG_CHECKABLE);
    set_checked(s_face, 2, p->face);

    lv_obj_t *o = pnl_kit_card(content, "Orientation");
    r = pnl_kit_row(o);
    s_orient[0] = pnl_kit_button(r, "Normal", orient_click, (void *)(intptr_t)0);
    s_orient[1] = pnl_kit_button(r, "Flipped", orient_click, (void *)(intptr_t)1);
    for (int i = 0; i < 2; i++) lv_obj_add_flag(s_orient[i], LV_OBJ_FLAG_CHECKABLE);
    set_checked(s_orient, 2, p->orient);
    note = pnl_kit_msg(o);
    pnl_kit_msg_set(note, "Applies after restart.", PNL_KIT_INFO);
    pnl_kit_button(o, "Restart now", restart_click, NULL);

    lv_obj_t *a = pnl_kit_card(content, "About");
    for (int i = 0; i < 8; i++) s_about[i] = pnl_kit_msg(a);
    pnl_kit_button(a, "Touch test", touch_click, NULL);
}

static void panel_update(const pnl_snap_t *s) {
    if (!s_about[0]) return;
    char b[112];
    snprintf(b, sizeof b, "Version %s", s->st.version);
    pnl_label_set_if_changed(s_about[0], b);
    snprintf(b, sizeof b, "Firmware slot %s, %s", s->st.fw_slot, s->st.fw_state);
    pnl_label_set_if_changed(s_about[1], b);
    snprintf(b, sizeof b, "STA IP %s | AP IP %s", s->st.wifi.sta_up ? s->st.wifi.sta_ip : "--",
             s->st.wifi.ap_ip[0] ? s->st.wifi.ap_ip : "--");
    pnl_label_set_if_changed(s_about[2], b);
    snprintf(b, sizeof b, "Uptime %lu s", (unsigned long)s->st.uptime_s);
    pnl_label_set_if_changed(s_about[3], b);
    snprintf(b, sizeof b, "Internal RAM %lu KB free, %lu KB minimum", (unsigned long)s->st.heap_int_free_kb,
             (unsigned long)s->st.heap_int_min_kb);
    pnl_label_set_if_changed(s_about[4], b);
    lv_mem_monitor_t mon;
    lv_mem_monitor(&mon);
    snprintf(b, sizeof b, "LVGL memory peak %u KB of %u KB internal (the 75 %% budget; PSRAM overflow beyond)",
             (unsigned)(mon.max_used / 1024u), (unsigned)CONFIG_LV_MEM_SIZE_KILOBYTES);
    pnl_label_set_if_changed(s_about[5], b);
    panel_hw_status_t hs;
    panel_hw_status(&hs);
    snprintf(b, sizeof b, "Touch: %lu reads, %lu errors, %lu points, last %u,%u%s", (unsigned long)hs.reads,
             (unsigned long)hs.read_errs, (unsigned long)hs.points, (unsigned)hs.last_x, (unsigned)hs.last_y,
             hs.touch_ok ? "" : " (touch unavailable)");
    pnl_label_set_if_changed(s_about[6], b);
    /* Stack margins (the task stacks are chosen, not measured; the Stage 3 and 4 gates fail below 1024 B). This
     * runs on the LVGL task, so NULL is the LVGL task itself. */
    uint32_t sp = 0, sw = 0;
    pnl_poll_stack_free(&sp, &sw);
    snprintf(b, sizeof b, "Stack free (min, bytes): lvgl %lu, pnl_work %lu, pnl_poll %lu, pnl_wifi %lu",
             (unsigned long)uxTaskGetStackHighWaterMark(NULL), (unsigned long)pnl_worker_stack_free(),
             (unsigned long)sp, (unsigned long)sw);
    pnl_label_set_if_changed(s_about[7], b);
}

static void panel_teardown(void) {
    s_day = s_night = s_day_lbl = s_night_lbl = s_start = s_end = s_idle = s_wipe = NULL;
    for (int i = 0; i < 3; i++) s_mode[i] = NULL;
    for (int i = 0; i < 2; i++) { s_face[i] = NULL; s_orient[i] = NULL; }
    for (int i = 0; i < 8; i++) s_about[i] = NULL;
}

const pnl_screen_ops_t PNL_SCR_PANEL = { .title = "Panel", .build = panel_build, .update = panel_update,
                                         .teardown = panel_teardown, .in_rail = 0 };
```

- [ ] **Step 7: Add the analogue face to the home screen**

In `components/panel_ui/scr_home.c`, add `#include "pnl_prefs_nvs.h"`, and add these statics and three functions directly
after Task 13's block of statics (the line `static uint32_t    s_ring_c, s_wifi_c;`). They must come before
`clock_tick()`, which calls `home_analogue_set()` and reads `s_scale` (edit 2 below):
```c
/* Task 26 (D3): the analogue face -- lv_scale round, hour/minute/second needles, updated by the same 1 s clock timer */
static lv_obj_t *s_scale, *s_hand_h, *s_hand_m, *s_hand_s, *s_ana_unset;
static const char *HOUR_TXT[] = { "12", "1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11", NULL };

static lv_obj_t *home_hand(lv_obj_t *scale, int width, uint32_t color) {
    lv_obj_t *l = lv_line_create(scale);
    lv_obj_set_style_line_width(l, width, 0);
    lv_obj_set_style_line_rounded(l, true, 0);
    lv_obj_set_style_line_color(l, lv_color_hex(color), 0);
    return l;
}

static void home_analogue_build(lv_obj_t *parent) {
    s_scale = lv_scale_create(parent);
    lv_obj_set_size(s_scale, 200, 200);
    lv_scale_set_mode(s_scale, LV_SCALE_MODE_ROUND_INNER);
    lv_scale_set_range(s_scale, 0, 60);
    lv_scale_set_total_tick_count(s_scale, 61);
    lv_scale_set_major_tick_every(s_scale, 5);
    lv_scale_set_angle_range(s_scale, 360);
    lv_scale_set_rotation(s_scale, 270);             /* 0 at the top */
    lv_scale_set_label_show(s_scale, true);
    lv_scale_set_text_src(s_scale, HOUR_TXT);
    lv_obj_set_style_radius(s_scale, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_scale, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_scale, lv_color_hex(PNL_C_CARD), 0);
    s_hand_h = home_hand(s_scale, 8, PNL_C_TEXT);
    s_hand_m = home_hand(s_scale, 5, PNL_C_TEXT);
    s_hand_s = home_hand(s_scale, 2, PNL_C_ACCENT);
    s_ana_unset = lv_label_create(s_scale);
    lv_label_set_text(s_ana_unset, "Clock not set");  /* an unset clock is reported honestly, never as a plausible time */
    lv_obj_center(s_ana_unset);
    lv_obj_add_flag(s_ana_unset, LV_OBJ_FLAG_HIDDEN);
}

static void home_analogue_set(const pnl_local_t *t) {
    if (!s_scale) return;
    lv_obj_t *hands[3] = { s_hand_h, s_hand_m, s_hand_s };
    for (int i = 0; i < 3; i++) {
        if (t->valid) lv_obj_remove_flag(hands[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(hands[i], LV_OBJ_FLAG_HIDDEN);
    }
    if (!t->valid) { lv_obj_remove_flag(s_ana_unset, LV_OBJ_FLAG_HIDDEN); return; }
    lv_obj_add_flag(s_ana_unset, LV_OBJ_FLAG_HIDDEN);
    lv_scale_set_line_needle_value(s_scale, s_hand_h, 55, (t->hour % 12) * 5 + t->min / 12);
    lv_scale_set_line_needle_value(s_scale, s_hand_m, 80, t->min);
    lv_scale_set_line_needle_value(s_scale, s_hand_s, 88, t->sec);
}
```
Then make three edits to the Task 13 code in `scr_home.c`:
1. In `home_build()`, replace the two lines that create and place the digital clock:
   ```c
       s_clock = pnl_label(page, "--:--", pnl_font_clock(), PNL_C_TEXT);
       lv_obj_align(s_clock, LV_ALIGN_TOP_MID, 0, 36);
   ```
   with:
   ```c
       if (pnl_prefs_get()->face == 1) {
           s_clock = NULL;                                   /* the analogue face: no digital label */
           home_analogue_build(page);
           lv_obj_align(s_scale, LV_ALIGN_TOP_MID, 0, 20);   /* 200 px face ends at y 220, above the date line (y 222) */
       } else {
           s_clock = pnl_label(page, "--:--", pnl_font_clock(), PNL_C_TEXT);
           lv_obj_align(s_clock, LV_ALIGN_TOP_MID, 0, 36);
       }
   ```
2. In `clock_tick()`, change the first line `if (!s_clock) return;` to `if (!s_clock && !s_scale) return;` (the date and
   context lines must keep ticking on the analogue face), and directly after the
   `pnl_local_time((int64_t)time(NULL), sn->st.utc_offset_s, sn->st.time_is_set, &lt);` line add
   `home_analogue_set(&lt);`. The existing `pnl_label_set_if_changed(s_clock, b);` stays as it is: it is NULL-safe
   (Task 12), so it does nothing on the analogue face.
3. In `home_teardown()`, add `s_scale = s_hand_h = s_hand_m = s_hand_s = s_ana_unset = NULL;`.

- [ ] **Step 8: Wire preferences into boot and the registry**

In `components/panel_ui/panel_ui.c` (Task 7's `panel_start()`): add `#include "pnl_prefs_nvs.h"`; insert
`pnl_prefs_load();` directly before the line `if (panel_hw_start(PNL_ORIENT_NORMAL) != 0) {` and change that call's
argument from `PNL_ORIENT_NORMAL` to `pnl_prefs_get()->orient`; change `(void)panel_hw_brightness(80);` to
`(void)panel_hw_brightness(pnl_prefs_get()->dim.day_pct);`.
`panel_start()` runs after `nvs_flash_init()` in `app_main` (it sits right after `ota_trial_start(1)`), so NVS is up.

In `components/panel_ui/scr_shell.c`'s registry, the `PNL_DEST_PANEL` row changes from `&PNL_SCR_DIAG` to `&PNL_SCR_PANEL`.
Replace only Task 13's line:
```c
    [PNL_DEST_PANEL]     = { "Panel",      &PNL_SCR_DIAG,        0 },   /* the touch test until Task 26's &PNL_SCR_PANEL */
```
with:
```c
    [PNL_DEST_PANEL]     = { "Panel",      &PNL_SCR_PANEL,       0 },
```

In `components/panel_ui/CMakeLists.txt`, inside the gated block:
```cmake
    list(APPEND PANEL_SRCS "pnl_prefs.c" "pnl_prefs_nvs.c" "scr_panel.c")
```
(`nvs_flash` and `hg_blob` are already in `PRIV_REQUIRES`, Task 6.)

- [ ] **Step 9: Run the gates**

1. GATE-HOST. Expected: `out of 49`.
2. GATE-ESP32. Expected: three `Project build complete`, empty lock diff.
3. GATE-P4. Expected: `Project build complete`, 0 warnings.
4. FLASH-P4 (dry run, then real).
5. Bench spot check: Panel shows every card; the day slider changes the backlight as it moves; choose Analogue and go
   Home: the round face shows the time (or "Clock not set"); About shows internal RAM and LVGL peak memory; Touch test
   opens the diagnostics screen. The orientation restart is Stage 3 gate step 8.

- [ ] **Step 10: Commit**

```powershell
git -C C:\Projects\HillGrov add components/panel_ui/pnl_prefs.h components/panel_ui/pnl_prefs.c components/panel_ui/pnl_prefs_nvs.h components/panel_ui/pnl_prefs_nvs.c components/panel_ui/scr_panel.c components/panel_ui/scr_home.c components/panel_ui/panel_ui.c components/panel_ui/scr_shell.c components/panel_ui/CMakeLists.txt tests/host/test_pnl_prefs.c tests/host/CMakeLists.txt
git -C C:\Projects\HillGrov commit -m "feat(panel_ui): Panel preferences, analogue clock face, orientation, About" -m "Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>"
git -C C:\Projects\HillGrov push
```

---

### Task 27: Night dimming, wake-on-touch guard, idle return and operator-state wipe (Stage 3)

"The panel knows the light schedule and dims when the lights go off, waking on touch" (spec). The decision is pure and
host-tested (`pnl_dim`); a 1 s LVGL timer applies it (`pnl_idle`). The adapter's `auto_sleep` stays off: it would pause
the LVGL worker and freeze the clock. While the panel is dimmed a transparent full-screen catcher on `lv_layer_top()`
takes the first press, so a tap on a dark panel wakes it without pressing the button underneath. After `wipe_idle_s`
(default 300 s) with no job pending, the web's logout wipe is applied (D18): console logs, history and drafts, unsaved
config edits, form text and any revealed secret are wiped, dialogs and the keyboard close, and the panel returns Home.

**Files:**
- Create: `components/panel_ui/pnl_dim.h`, `components/panel_ui/pnl_dim.c` (P); `components/panel_ui/pnl_idle.h`, `components/panel_ui/pnl_idle.c` (G)
- Create: `tests/host/test_pnl_dim.c`
- Modify: `components/panel_ui/panel_ui.c` (`pnl_idle_start()`), `components/panel_ui/CMakeLists.txt`, `tests/host/CMakeLists.txt`

**Interfaces:**
- Consumes: `pnl_lights_on_now`, `pnl_sched_t`, `pnl_local_t`, `pnl_local_time` (Task 10); `pnl_dim_cfg_t`,
  `pnl_prefs_get` (Task 26); `panel_hw_brightness` (Task 7); `lv_display_get_inactive_time`,
  `lv_display_trigger_activity`, `lv_indev_active`, `lv_indev_wait_release` (LVGL 9.5); `pnl_worker_pending` (Task 8);
  `pnl_poll_latest` (Task 11); `pnl_nav_go` (Task 12); `wdg_keyboard_is_open`, `wdg_keyboard_close` (Task 19);
  `pnl_confirm_close` (Task 22); the wipes `cfg_zone_wipe_all`, `cfg_master_wipe` (Tasks 20-21), `zone_console_wipe_all`,
  `zone_replace_wipe` (Task 22), `sys_wifi_wipe` (Task 23), `sys_password_wipe` (Task 24).
- Produces:
  ```c
  /* pnl_dim.h (pure) */
  typedef enum { PNL_DIM_OFF = 0, PNL_DIM_FOLLOW_LIGHTS, PNL_DIM_FIXED } pnl_dim_mode_t;
  typedef struct { uint8_t night; uint8_t pct; } pnl_dim_out_t;
  void pnl_dim_eval(const pnl_dim_cfg_t *c, const pnl_sched_t *s, const pnl_local_t *t, uint32_t idle_ms, pnl_dim_out_t *out);
       /* night: OFF never; FIXED: t->valid && now in [start,end) (wraps); FOLLOW_LIGHTS: t->valid && s->n_light > 0 &&
          !pnl_lights_on_now; pct = (night && idle_ms >= c->idle_s*1000) ? c->night_pct : c->day_pct */
  /* pnl_idle.h (glue) */
  void pnl_idle_start(void);
  ```
  One precision the outline leaves open, fixed here: a FIXED window with `start == end` is empty (never night).

**What proves what:** `test_pnl_dim` pins every branch of the decision. The timer, the catcher and the wipe are glue:
GATE-P4 proves they compile; the Stage 3 gate step 9 proves dimming, the wake-only first tap and the wipe on glass.

- [ ] **Step 1: Write the failing test**

Create `tests/host/test_pnl_dim.c`:
```c
#include <string.h>
#include "unity.h"
#include "pnl_dim.h"

/* pnl_dim_eval is the whole night/brightness decision. The clock must be set for any night (an unset clock never dims
   the panel by schedule), FOLLOW_LIGHTS needs at least one scheduled light, and the night level applies only after the
   idle timeout -- a panel being used stays at day brightness. */

static pnl_dim_cfg_t g_c;
static pnl_sched_t   g_s;

void setUp(void) {
    memset(&g_c, 0, sizeof g_c);
    g_c.mode = PNL_DIM_FOLLOW_LIGHTS; g_c.day_pct = 80; g_c.night_pct = 10;
    g_c.fixed_start_min = 22 * 60; g_c.fixed_end_min = 6 * 60; g_c.idle_s = 60;
    pnl_sched_reset(&g_s);
}
void tearDown(void) {}

static pnl_local_t at(int hh, int mm) {
    pnl_local_t t;
    memset(&t, 0, sizeof t);
    t.valid = 1; t.hour = hh; t.min = mm; t.minute_of_day = hh * 60 + mm;
    return t;
}

static void eval(int hh, int mm, uint32_t idle_ms, pnl_dim_out_t *o) {
    pnl_local_t t = at(hh, mm);
    pnl_dim_eval(&g_c, &g_s, &t, idle_ms, o);
}

static void light(uint16_t on, uint16_t off) {
    g_s.light_on[g_s.n_light] = on;
    g_s.light_off[g_s.n_light] = off;
    g_s.n_light++;
}

static void test_off_never_dims(void) {
    pnl_dim_out_t o;
    g_c.mode = PNL_DIM_OFF;
    light(6 * 60, 22 * 60);
    eval(23, 0, 600000, &o);
    TEST_ASSERT_EQUAL_UINT8(0, o.night);
    TEST_ASSERT_EQUAL_UINT8(80, o.pct);
}

static void test_fixed_window_wrapping_midnight(void) {
    pnl_dim_out_t o;
    g_c.mode = PNL_DIM_FIXED;
    eval(23, 0, 120000, &o);  TEST_ASSERT_EQUAL_UINT8(1, o.night); TEST_ASSERT_EQUAL_UINT8(10, o.pct);
    eval(22, 0, 120000, &o);  TEST_ASSERT_EQUAL_UINT8(1, o.night);                 /* start is inside */
    eval(5, 59, 120000, &o);  TEST_ASSERT_EQUAL_UINT8(1, o.night);
    eval(6, 0, 120000, &o);   TEST_ASSERT_EQUAL_UINT8(0, o.night); TEST_ASSERT_EQUAL_UINT8(80, o.pct);   /* end is not */
    eval(12, 0, 120000, &o);  TEST_ASSERT_EQUAL_UINT8(0, o.night);
}

static void test_fixed_window_not_wrapping(void) {
    pnl_dim_out_t o;
    g_c.mode = PNL_DIM_FIXED; g_c.fixed_start_min = 60; g_c.fixed_end_min = 300;
    eval(3, 0, 120000, &o);   TEST_ASSERT_EQUAL_UINT8(1, o.night);
    eval(5, 0, 120000, &o);   TEST_ASSERT_EQUAL_UINT8(0, o.night);
    eval(0, 59, 120000, &o);  TEST_ASSERT_EQUAL_UINT8(0, o.night);
}

static void test_fixed_empty_window(void) {
    pnl_dim_out_t o;
    g_c.mode = PNL_DIM_FIXED; g_c.fixed_start_min = 600; g_c.fixed_end_min = 600;
    eval(10, 0, 120000, &o);  TEST_ASSERT_EQUAL_UINT8(0, o.night);
}

static void test_unset_clock_never_dims(void) {
    pnl_dim_out_t o;
    pnl_local_t t = at(23, 0);
    t.valid = 0;
    g_c.mode = PNL_DIM_FIXED;
    pnl_dim_eval(&g_c, &g_s, &t, 600000, &o);
    TEST_ASSERT_EQUAL_UINT8(0, o.night); TEST_ASSERT_EQUAL_UINT8(80, o.pct);
    g_c.mode = PNL_DIM_FOLLOW_LIGHTS;
    light(6 * 60, 22 * 60);
    pnl_dim_eval(&g_c, &g_s, &t, 600000, &o);
    TEST_ASSERT_EQUAL_UINT8(0, o.night);
}

static void test_follow_lights(void) {
    pnl_dim_out_t o;
    light(6 * 60, 22 * 60);
    eval(23, 0, 120000, &o);  TEST_ASSERT_EQUAL_UINT8(1, o.night); TEST_ASSERT_EQUAL_UINT8(10, o.pct);
    eval(12, 0, 120000, &o);  TEST_ASSERT_EQUAL_UINT8(0, o.night); TEST_ASSERT_EQUAL_UINT8(80, o.pct);
}

static void test_follow_lights_any_light_on_keeps_day(void) {
    pnl_dim_out_t o;
    light(6 * 60, 22 * 60);
    light(20 * 60, 4 * 60);                              /* wraps midnight: on 20:00-04:00 */
    eval(2, 0, 120000, &o);   TEST_ASSERT_EQUAL_UINT8(0, o.night);
    eval(5, 0, 120000, &o);   TEST_ASSERT_EQUAL_UINT8(1, o.night);
}

static void test_follow_lights_without_schedules_is_day(void) {
    pnl_dim_out_t o;
    eval(23, 0, 600000, &o);
    TEST_ASSERT_EQUAL_UINT8(0, o.night); TEST_ASSERT_EQUAL_UINT8(80, o.pct);
}

static void test_idle_threshold(void) {
    pnl_dim_out_t o;
    light(6 * 60, 22 * 60);
    eval(23, 0, 59999, &o);   TEST_ASSERT_EQUAL_UINT8(1, o.night); TEST_ASSERT_EQUAL_UINT8(80, o.pct);   /* in use */
    eval(23, 0, 60000, &o);   TEST_ASSERT_EQUAL_UINT8(1, o.night); TEST_ASSERT_EQUAL_UINT8(10, o.pct);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_off_never_dims);
    RUN_TEST(test_fixed_window_wrapping_midnight);
    RUN_TEST(test_fixed_window_not_wrapping);
    RUN_TEST(test_fixed_empty_window);
    RUN_TEST(test_unset_clock_never_dims);
    RUN_TEST(test_follow_lights);
    RUN_TEST(test_follow_lights_any_light_on_keeps_day);
    RUN_TEST(test_follow_lights_without_schedules_is_day);
    RUN_TEST(test_idle_threshold);
    return UNITY_END();
}
```
In `tests/host/CMakeLists.txt`, after the `test_pnl_prefs` row:
```cmake
hg_test(test_pnl_dim ${COMP}/panel_ui/pnl_dim.c ${COMP}/panel_ui/pnl_home.c ${COMP}/panel_ui/pnl_time.c ${HG_CFG_SRC})
```
(`pnl_home.c` provides `pnl_lights_on_now` and `pnl_sched_reset`. If its link reports an unresolved symbol, add the
source file that Task 10's `test_pnl_home` row links for that symbol.)

- [ ] **Step 2: Run the test to verify it fails**

Run GATE-HOST with ` -R test_pnl_dim`.
Expected: `Cannot find source file: .../components/panel_ui/pnl_dim.c`.

- [ ] **Step 3: Write the decision**

Create `components/panel_ui/pnl_dim.h`:
```c
#pragma once
/* Pure (host-tested, tests/host/test_pnl_dim.c): is it night, and what brightness? */
#include <stdint.h>
#include "pnl_prefs.h"   /* pnl_dim_cfg_t */
#include "pnl_home.h"    /* pnl_sched_t, pnl_lights_on_now */
#include "pnl_time.h"    /* pnl_local_t */

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { PNL_DIM_OFF = 0, PNL_DIM_FOLLOW_LIGHTS, PNL_DIM_FIXED } pnl_dim_mode_t;
typedef struct { uint8_t night; uint8_t pct; } pnl_dim_out_t;
void pnl_dim_eval(const pnl_dim_cfg_t *c, const pnl_sched_t *s, const pnl_local_t *t, uint32_t idle_ms, pnl_dim_out_t *out);
     /* night: OFF never; FIXED: t->valid && now in [start,end) (wraps; start == end is empty); FOLLOW_LIGHTS: t->valid &&
        s->n_light > 0 && !pnl_lights_on_now; pct = (night && idle_ms >= c->idle_s*1000) ? c->night_pct : c->day_pct */

#ifdef __cplusplus
}
#endif
```
Create `components/panel_ui/pnl_dim.c`:
```c
#include "pnl_dim.h"

static int in_window(int now, int start, int end) {
    if (start == end) return 0;
    if (start < end) return now >= start && now < end;
    return now >= start || now < end;          /* wraps midnight */
}

void pnl_dim_eval(const pnl_dim_cfg_t *c, const pnl_sched_t *s, const pnl_local_t *t, uint32_t idle_ms, pnl_dim_out_t *out) {
    if (!c || !out) return;
    uint8_t night = 0;
    if (t && t->valid) {
        if (c->mode == PNL_DIM_FIXED)
            night = (uint8_t)in_window(t->minute_of_day, c->fixed_start_min, c->fixed_end_min);
        else if (c->mode == PNL_DIM_FOLLOW_LIGHTS)
            night = (uint8_t)(s && s->n_light > 0 && !pnl_lights_on_now(s, t->minute_of_day));
    }
    out->night = night;
    out->pct = (night && idle_ms >= (uint32_t)c->idle_s * 1000u) ? c->night_pct : c->day_pct;
}
```

- [ ] **Step 4: Run the test to verify it passes**

Run GATE-HOST with ` -R test_pnl_dim`. Expected: `9 Tests 0 Failures 0 Ignored`.

- [ ] **Step 5: Write the idle timer, the wake guard and the wipe**

Create `components/panel_ui/pnl_idle.h`:
```c
#pragma once
/* Glue: night dimming, the wake-only first touch and the idle operator-state wipe (D4, D18). */
void pnl_idle_start(void);   /* [LVGL] 1 s lv_timer: pnl_dim_eval -> panel_hw_brightness on change only; while dimmed, a
                                transparent full-screen catcher on lv_layer_top() swallows the first press (wake only); when
                                inactive time >= wipe_idle_s and !pnl_worker_pending(): call every *_wipe(), close the
                                keyboard, pnl_nav_go(PNL_DEST_HOME, 0) */
```
Create `components/panel_ui/pnl_idle.c`:
```c
#include <time.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "lvgl.h"
#include "panel_hw.h"
#include "pnl_dim.h"
#include "pnl_poll.h"
#include "pnl_prefs_nvs.h"
#include "pnl_worker.h"
#include "pnl_ui_kit.h"
#include "scr_shell.h"
#include "scr_config.h"      /* cfg_zone_wipe_all, cfg_master_wipe */
#include "scr_system.h"      /* sys_wifi_wipe, sys_password_wipe */
#include "zone_sections.h"   /* zone_console_wipe_all, zone_replace_wipe */
#include "wdg_keyboard.h"
#include "pnl_idle.h"

static const char *TAG = "pnl_idle";

static pnl_snap_t *s_snap;             /* PSRAM: never a 2 KB copy on the LVGL stack */
static uint8_t     s_cur_pct = 0xFF;   /* last duty applied; 0xFF = none yet */
static uint8_t     s_wiped;
static lv_obj_t   *s_catcher;

static void catcher_pressed(lv_event_t *e) {
    (void)e;
    lv_indev_t *in = lv_indev_active();
    if (in) lv_indev_wait_release(in);          /* this press and its release reach nothing else: wake only */
    if (s_catcher) { lv_obj_delete_async(s_catcher); s_catcher = NULL; }
    const pnl_prefs_t *p = pnl_prefs_get();
    panel_hw_brightness(p->dim.day_pct);
    s_cur_pct = p->dim.day_pct;
    lv_display_trigger_activity(NULL);
}

static void catcher_show(int on) {
    if (on && !s_catcher) {
        s_catcher = lv_obj_create(lv_layer_top());
        lv_obj_remove_style_all(s_catcher);     /* fully transparent, no border */
        lv_obj_set_size(s_catcher, LV_PCT(100), LV_PCT(100));
        lv_obj_add_flag(s_catcher, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(s_catcher, catcher_pressed, LV_EVENT_PRESSED, NULL);
    } else if (!on && s_catcher) {
        lv_obj_delete(s_catcher);
        s_catcher = NULL;
    }
}

static void idle_wipe(void) {
    cfg_zone_wipe_all();
    cfg_master_wipe();          /* includes any revealed secret */
    zone_console_wipe_all();
    zone_replace_wipe();
    sys_wifi_wipe();
    sys_password_wipe();
    pnl_confirm_close();
    if (wdg_keyboard_is_open()) wdg_keyboard_close();
    pnl_nav_go(PNL_DEST_HOME, 0);
    ESP_LOGI(TAG, "idle: operator state wiped, back to Home");
}

static void idle_tick(lv_timer_t *tm) {
    (void)tm;
    const pnl_prefs_t *p = pnl_prefs_get();
    pnl_poll_latest(s_snap);
    pnl_local_t lt;
    pnl_local_time((int64_t)time(NULL), s_snap->st.utc_offset_s, s_snap->st.time_is_set, &lt);
    uint32_t idle = lv_display_get_inactive_time(NULL);
    pnl_dim_out_t out;
    pnl_dim_eval(&p->dim, &s_snap->sched, &lt, idle, &out);
    if (out.pct != s_cur_pct) {
        panel_hw_brightness(out.pct);
        s_cur_pct = out.pct;
    }
    catcher_show(out.pct != p->dim.day_pct);
    if (idle >= (uint32_t)p->wipe_idle_s * 1000u) {
        if (!s_wiped && !pnl_worker_pending()) {   /* never wipe a buffer a job still owns */
            idle_wipe();
            s_wiped = 1;
        }
    } else {
        s_wiped = 0;
    }
}

void pnl_idle_start(void) {
    if (!s_snap) s_snap = heap_caps_calloc(1, sizeof *s_snap, MALLOC_CAP_SPIRAM);
    if (!s_snap) { ESP_LOGE(TAG, "no PSRAM for the idle snapshot -- dimming and the idle wipe are off"); return; }
    lv_timer_create(idle_tick, 1000, NULL);
}
```

- [ ] **Step 6: Start it and add the sources**

In `components/panel_ui/panel_ui.c`: add `#include "pnl_idle.h"`, and inside `panel_start()`'s locked block (between the
successful `panel_lock(2000)` and `panel_unlock()`), directly after `pnl_shell_start();`, add `pnl_idle_start();` —
`lv_timer_create` needs the display lock or the LVGL task, and the shell must exist before the timer can navigate.

In `components/panel_ui/CMakeLists.txt`, inside the gated block:
```cmake
    list(APPEND PANEL_SRCS "pnl_dim.c" "pnl_idle.c")
```

- [ ] **Step 7: Run the gates**

1. GATE-HOST. Expected: `out of 50`.
2. GATE-ESP32. Expected: three `Project build complete`, empty lock diff.
3. GATE-P4. Expected: `Project build complete`, 0 warnings.
4. FLASH-P4 (dry run, then real).
5. The on-glass checks are the Stage 3 gate step 9.

- [ ] **Step 8: Commit**

```powershell
git -C C:\Projects\HillGrov add components/panel_ui/pnl_dim.h components/panel_ui/pnl_dim.c components/panel_ui/pnl_idle.h components/panel_ui/pnl_idle.c components/panel_ui/panel_ui.c components/panel_ui/CMakeLists.txt tests/host/test_pnl_dim.c tests/host/CMakeLists.txt
git -C C:\Projects\HillGrov commit -m "feat(panel_ui): night dimming, wake-on-touch, idle wipe" -m "Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>"
git -C C:\Projects\HillGrov push
```

---

### Stage 3 bench gate (owner, about 60 min; P4 master on COM28, zones 1 and 2 on the ring, a phone on the `HillGrow` AP)

**Before you start (agent):** the HEAD of `main` has passed GATE-HOST (50 tests), GATE-ESP32 and GATE-P4, and has been
flashed with FLASH-P4 (dry run first, every path `build_p4`):
```powershell
python C:\Projects\HillGrov\tools\flash_app.py --app master --target esp32p4 --build-dir C:\Projects\HillGrov\master\build_p4 --port COM28 --dry-run
python C:\Projects\HillGrov\tools\flash_app.py --app master --target esp32p4 --build-dir C:\Projects\HillGrov\master\build_p4 --port COM28
```
The master must not be in an OTA trial when the gate starts: Panel → About shows `VALID`.

Every step is done **with a finger on the glass**. A step fails if the panel freezes (the clock stops) at any point.

1. **Zone console** (rail → Zone → zone 2).
   - Tap the command line, type `GET ID`, OK. **Pass:** a reply line starting `OK ID` appears under `> GET ID`.
   - Tick "Forward to zone 2", send `GET WATER 1`. **Pass:** the log shows `> GET ZONE 2 WATER 1` and zone 2's reply.
   - Tap Prev twice, then Next. **Pass:** the line walks back to `GET ID`, then forward to `GET WATER 1`.
   - Type a line of 192 characters (hold one key). **Pass:** "Line too long -- 191 characters at most" and nothing
     is sent (no new log entry).
2. **Replace board** (same screen).
   - Enter `aa:bb:cc` → Replace. **Pass:** "Enter a MAC like aa:bb:cc:dd:ee:ff".
   - Enter zone 2's own MAC (the MAC row at the top of the zone view) → Replace. **Pass:** the reply line is shown
     verbatim (an `OK NODE ...` line), and zone 2 stays ONLINE.
3. **Wi-Fi** (System → Wi-Fi).
   - Scan. **Pass:** "Scanning... (the AP pauses briefly)", then a list of networks with open/secured and dBm; the clock
     keeps ticking during the scan.
   - Tap your house network, set its password, Join. **Pass:** "Saved -- joining...", and within 30 s the STA line
     shows an IP address.
   - Enter any AP SSID and an 8-character password, Set AP. **Pass:** the dialog "Changing the AP drops every phone
     connected to it" appears. Cancel it (changing the AP and back is optional; if you do, reconnect the phone).
4. **Time** (System → Time).
   - Change the TZ to `EST5EDT,M3.2.0,M11.1.0` → Set TZ. **Pass:** the reply is shown and the Home clock moves by the
     offset difference within 2 s. Set it back to `CET-1CEST,M3.5.0,M10.5.0/3`.
   - With the house Wi-Fi out of reach (unplug the house AP, or clear the STA SSID in Config → Master and save), reboot.
     **Pass:** Home shows `--:--` and "Clock not set". Then Set clock to your local time. **Pass:** Home shows the
     correct local time, and the web console (phone: Dashboard → a zone → console, forward off) answers `GET TIME`
     with the **UTC** equivalent. Afterwards plug the house AP back in, or restore the STA SSID (and re-enter its
     password in System → Wi-Fi → Join), so NTP returns.
5. **Web password** (System → Password).
   - Log in on the phone first. Enter a new password (8+ characters) → Set password → confirm. **Pass:** a large
     "Web password changed" box; the phone's next page load goes to the login screen; the new password logs in.
   - Set the old password back the same way. **Pass:** the phone logs in with it again.
6. **Fleet** (System → Fleet).
   - Update zone 2. **Pass:** "Update queued for zone 2."; while the status line is not `IDLE` the Update buttons and
     Update all are disabled and Abort is enabled; the Alarms history ends with `FW 2 TRIAL PASS`.
   - Update zone 2 again and tap Abort while it runs. **Pass:** "Fleet update aborted." and the status returns to `IDLE`.
7. **Reboot** (System → Fleet → Reboot master → Reboot). **Pass:** "Rebooting..." fills the screen, the master restarts,
   and the panel lights again on its own.
8. **Panel settings** (Home → Panel).
   - Drag the day brightness slider. **Pass:** the backlight follows the finger.
   - Clock face Analogue, go Home. **Pass:** the round face shows the right time. Switch back to Digital.
   - Orientation Flipped → Restart now → Reboot. **Pass:** after the restart the picture is upside down relative to
     before, and Panel → About → Touch test's five targets each light under the finger (never the opposite one).
     Restore Normal the same way and repeat the five-target test.
9. **Dimming and idle** (Panel).
   - Set Night dimming = Fixed hours with a window that covers now (for example from the current hour to the next),
     idle 30 s, idle return 2 min, and leave the Zone 2 console with one line in its log. Do not touch the glass.
   - **Pass:** after about 30 s the backlight drops to the night level.
   - Tap a button that is visible (for example the rail's Alarms). **Pass:** the panel wakes to day brightness and does
     **not** open Alarms; a second tap does.
   - Leave it untouched for 2 minutes. **Pass:** the panel is on Home; opening Zone 2 shows an empty console log.
   - Restore Night dimming = Follow the lights, idle 1 min, idle return 5 min.

Record in the task report: internal RAM free/min, LVGL peak and the four "Stack free" figures (Panel → About) before
step 1 and after step 9, and any step that needed a retry. **Fail criteria:** any step without its Pass, a frozen clock,
a reboot other than steps 4, 7 and 8, internal minimum below 64 KB, any "Stack free" figure below 1024 bytes, or LVGL peak above 75 % of its internal pool (then raise `CONFIG_LV_MEM_SIZE_KILOBYTES` to
128 per the Global Constraints, only if internal minimum stays at or above 64 KB).

---

## Stage 4 — Firmware and config from microSD (sequenced against the recovery design's shared helpers)

The recovery design (approved, not yet planned) creates two shared helpers this stage needs: one OTA-trial predicate in
`components/ota_trial` (its §2.3) and one image-identity helper holding the project-name constants (its §4.2 and §5.7).
Task 28 lands them **only if the recovery plan has not**, at the names and semantics that design specifies, so the
recovery plan later consumes them instead of creating a second copy. Tasks 29 and 30 then make the firmware-install path
safe to call from a task other than `httpd`, and Tasks 31-33 add the microSD face.

### Task 28: Firmware prerequisites shared with the recovery design — `ota_trial_running_on_trial()` and `hg_image` (Stage 4; skip-if-landed)

Today the trial test exists twice: `static running_slot_on_ota_trial()` in `master/main/app_main.c:66-71` and an inline
copy in `master_ready()` (`components/http_srv/http_upload_master.c:100-105`); `ota_trial_start()` makes the same query a
third time (`components/ota_trial/ota_trial.c:57-63`). Image identity is `identity_ok()` in
`components/http_srv/http_upload.c:95-100` (magic `0xE9` and the project name at +80, no chip id, no descriptor magic).

**Files:**
- Create: `components/hg_image/CMakeLists.txt`, `components/hg_image/hg_image.h`, `components/hg_image/hg_image.c` (P, every app)
- Create: `tests/host/test_hg_image.c`
- Modify: `components/ota_trial/ota_trial.h`, `components/ota_trial/ota_trial.c`
- Modify: `master/main/app_main.c` (delete the static predicate and its comment block at :32-71; both call sites use the shared one)
- Modify: `components/http_srv/http_upload_master.c` (`master_ready()` uses the shared predicate), `components/http_srv/CMakeLists.txt` (`PRIV_REQUIRES` gains `ota_trial`)
- Modify: `tests/host/CMakeLists.txt` (include `${COMP}/hg_image`; the `test_hg_image` row)

**Interfaces:**
- Consumes: nothing new.
- Produces:
  ```c
  /* ota_trial.h -- recovery design §2.3's one shared copy */
  int ota_trial_running_on_trial(void);   /* 1 while the running slot is ESP_OTA_IMG_PENDING_VERIFY; 0 for VALID, UNDEFINED,
                                             factory/non-OTA (ESP_ERR_NOT_SUPPORTED) and any failed query */
  /* hg_image.h (pure) -- recovery design §4.2 / §5.7 */
  #define HG_IMG_ID_BYTES   112u
  #define HG_IMG_MAGIC      0xE9u
  #define HG_IMG_DESC_MAGIC 0xABCD5432u
  #define HG_CHIP_ESP32     0x0000u
  #define HG_CHIP_ESP32C6   0x000Du
  #define HG_CHIP_ESP32P4   0x0012u
  #define HG_PROJ_MASTER    "hillgrow_master"
  #define HG_PROJ_ZONE      "hillgrow_zone"
  #define HG_PROJ_RESCUE_P4 "hillgrow_rescue_p4"
  #define HG_PROJ_CP        "eh_cp_wifi_softap"
  typedef struct { uint16_t chip_id; char project[33]; char version[33]; } hg_image_id_t;
  int hg_image_parse(const uint8_t *b, size_t n, hg_image_id_t *out);
      /* +0 magic, +12 chip_id u16 LE, +32 desc magic u32 LE, +48 version[32], +80 project_name[32] (NUL-bounded, non-printables
         -> '.'); 0 ok / -1 n < HG_IMG_ID_BYTES / -2 image magic / -3 desc magic */
  int hg_image_is(const uint8_t *b, size_t n, uint16_t chip, const char *want);   /* 1 = parse ok && chip && project == want */
  ```
  Offsets, checked against IDF's structs: `esp_image_header_t` is 24 bytes with `chip_id` (u16) at +12, then one
  `esp_image_segment_header_t` (8 bytes), so `esp_app_desc_t` starts at +32: `magic_word` +32, `version[32]` +48,
  `project_name[32]` +80.

- [ ] **Step 0: The skip check**

```powershell
git -C C:\Projects\HillGrov grep -n "ota_trial_running_on_trial" -- components/ota_trial/ota_trial.h
git -C C:\Projects\HillGrov grep -n "0xABCD5432" -- components
```
- Both print a match: the recovery plan landed both. Record the exact names of its predicate and its identity helper
  (function names, header, and the project-name constants) in the task report, make no change in this task, and **use
  those names wherever Tasks 30-32 say `hg_image_*`, `HG_PROJ_*`, `HG_CHIP_*` or `HG_IMG_ID_BYTES`** (same semantics).
  Continue at Task 29.
- Only the first matches: do Steps 1-4 (hg_image) and skip Steps 5-6.
- Only the second matches: skip Steps 1-4 and do Steps 5-6, using the landed helper's names from then on.
- Neither matches: do every step.

- [ ] **Step 1: Write the failing test**

Create `tests/host/test_hg_image.c`:
```c
#include <stdio.h>
#include <string.h>
#include "unity.h"
#include "hg_image.h"

/* hg_image identifies an app image from its first 112 bytes BEFORE anything is erased (recovery design 4.2): image magic,
   chip id, app-descriptor magic, project name. esp_ota_end() checks chip and checksums but not the project, so this is
   the only thing that stops a zone image landing in a master slot. The project-name constants are checked against the
   CMakeLists.txt that defines each project (recovery design 5.7), so a rename cannot drift silently. */

static uint8_t g_b[HG_IMG_ID_BYTES];

void setUp(void) {}
void tearDown(void) {}

static void mk(uint16_t chip, const char *proj, const char *ver) {
    memset(g_b, 0x5A, sizeof g_b);
    g_b[0] = HG_IMG_MAGIC;
    g_b[12] = (uint8_t)chip;
    g_b[13] = (uint8_t)(chip >> 8);
    uint32_t m = HG_IMG_DESC_MAGIC;
    g_b[32] = (uint8_t)m; g_b[33] = (uint8_t)(m >> 8); g_b[34] = (uint8_t)(m >> 16); g_b[35] = (uint8_t)(m >> 24);
    memset(g_b + 48, 0, 32);
    memcpy(g_b + 48, ver, strlen(ver));
    memset(g_b + 80, 0, 32);
    size_t n = strlen(proj);
    memcpy(g_b + 80, proj, n > 32 ? 32 : n);
}

static void test_p4_master(void) {
    hg_image_id_t id;
    mk(HG_CHIP_ESP32P4, HG_PROJ_MASTER, "1.4.0");
    TEST_ASSERT_EQUAL_INT(0, hg_image_parse(g_b, sizeof g_b, &id));
    TEST_ASSERT_EQUAL_HEX16(0x0012, id.chip_id);
    TEST_ASSERT_EQUAL_STRING("hillgrow_master", id.project);
    TEST_ASSERT_EQUAL_STRING("1.4.0", id.version);
    TEST_ASSERT_EQUAL_INT(1, hg_image_is(g_b, sizeof g_b, HG_CHIP_ESP32P4, HG_PROJ_MASTER));
}

static void test_esp32_master_is_not_a_p4_master(void) {
    mk(HG_CHIP_ESP32, HG_PROJ_MASTER, "1.4.0");
    TEST_ASSERT_EQUAL_INT(0, hg_image_is(g_b, sizeof g_b, HG_CHIP_ESP32P4, HG_PROJ_MASTER));
    TEST_ASSERT_EQUAL_INT(1, hg_image_is(g_b, sizeof g_b, HG_CHIP_ESP32, HG_PROJ_MASTER));
}

static void test_zone_image(void) {
    mk(HG_CHIP_ESP32, HG_PROJ_ZONE, "1.4.0");
    TEST_ASSERT_EQUAL_INT(1, hg_image_is(g_b, sizeof g_b, HG_CHIP_ESP32, HG_PROJ_ZONE));
    TEST_ASSERT_EQUAL_INT(0, hg_image_is(g_b, sizeof g_b, HG_CHIP_ESP32, HG_PROJ_MASTER));
}

static void test_c6_radio_image(void) {
    mk(HG_CHIP_ESP32C6, HG_PROJ_CP, "3.0.7");
    TEST_ASSERT_EQUAL_INT(1, hg_image_is(g_b, sizeof g_b, HG_CHIP_ESP32C6, HG_PROJ_CP));
}

static void test_wrong_image_magic(void) {
    hg_image_id_t id;
    mk(HG_CHIP_ESP32P4, HG_PROJ_MASTER, "1.4.0");
    g_b[0] = 0xE8;
    TEST_ASSERT_EQUAL_INT(-2, hg_image_parse(g_b, sizeof g_b, &id));
    TEST_ASSERT_EQUAL_INT(0, hg_image_is(g_b, sizeof g_b, HG_CHIP_ESP32P4, HG_PROJ_MASTER));
}

static void test_wrong_descriptor_magic(void) {
    hg_image_id_t id;
    mk(HG_CHIP_ESP32P4, HG_PROJ_MASTER, "1.4.0");
    g_b[33] ^= 0x01;
    TEST_ASSERT_EQUAL_INT(-3, hg_image_parse(g_b, sizeof g_b, &id));
}

static void test_truncated_at_111(void) {
    hg_image_id_t id;
    mk(HG_CHIP_ESP32P4, HG_PROJ_MASTER, "1.4.0");
    TEST_ASSERT_EQUAL_INT(-1, hg_image_parse(g_b, 111, &id));
    TEST_ASSERT_EQUAL_INT(0, hg_image_is(g_b, 111, HG_CHIP_ESP32P4, HG_PROJ_MASTER));
}

static void test_32_char_project_without_nul(void) {
    hg_image_id_t id;
    char p32[33];
    memset(p32, 'a', 32); p32[32] = '\0';
    mk(HG_CHIP_ESP32P4, p32, "1");
    TEST_ASSERT_EQUAL_INT(0, hg_image_parse(g_b, sizeof g_b, &id));
    TEST_ASSERT_EQUAL_size_t(32, strlen(id.project));
    TEST_ASSERT_EQUAL_INT(1, hg_image_is(g_b, sizeof g_b, HG_CHIP_ESP32P4, p32));
    p32[31] = '\0';                                             /* a 31-char prefix is not the same project */
    TEST_ASSERT_EQUAL_INT(0, hg_image_is(g_b, sizeof g_b, HG_CHIP_ESP32P4, p32));
}

static void test_non_printables_become_dots(void) {
    hg_image_id_t id;
    mk(HG_CHIP_ESP32P4, "ab\x01" "c", "v\x7f");
    TEST_ASSERT_EQUAL_INT(0, hg_image_parse(g_b, sizeof g_b, &id));
    TEST_ASSERT_EQUAL_STRING("ab.c", id.project);
    TEST_ASSERT_EQUAL_STRING("v.", id.version);
}

static void test_null_arguments(void) {
    mk(HG_CHIP_ESP32P4, HG_PROJ_MASTER, "1");
    TEST_ASSERT_EQUAL_INT(-1, hg_image_parse(NULL, 200, NULL));
    TEST_ASSERT_EQUAL_INT(0, hg_image_parse(g_b, sizeof g_b, NULL));       /* out may be NULL: validity only */
    TEST_ASSERT_EQUAL_INT(0, hg_image_is(NULL, 200, HG_CHIP_ESP32P4, HG_PROJ_MASTER));
    TEST_ASSERT_EQUAL_INT(0, hg_image_is(g_b, sizeof g_b, HG_CHIP_ESP32P4, NULL));
}

static int file_has(const char *path, const char *needle) {
    static char buf[16384];
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = '\0';
    return strstr(buf, needle) != NULL;
}

static void test_project_names_match_their_cmakelists(void) {
    TEST_ASSERT_TRUE_MESSAGE(file_has(HG_MASTER_CMAKELISTS, "project(" HG_PROJ_MASTER ")"), HG_MASTER_CMAKELISTS);
    TEST_ASSERT_TRUE_MESSAGE(file_has(HG_ZONE_CMAKELISTS, "project(" HG_PROJ_ZONE ")"), HG_ZONE_CMAKELISTS);
    TEST_ASSERT_TRUE_MESSAGE(file_has(HG_COPROC_CMAKELISTS, "project(" HG_PROJ_CP ")"), HG_COPROC_CMAKELISTS);
    /* HG_PROJ_RESCUE_P4: rescue_p4/CMakeLists.txt does not exist yet; the recovery plan adds its line here when it lands. */
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_p4_master);
    RUN_TEST(test_esp32_master_is_not_a_p4_master);
    RUN_TEST(test_zone_image);
    RUN_TEST(test_c6_radio_image);
    RUN_TEST(test_wrong_image_magic);
    RUN_TEST(test_wrong_descriptor_magic);
    RUN_TEST(test_truncated_at_111);
    RUN_TEST(test_32_char_project_without_nul);
    RUN_TEST(test_non_printables_become_dots);
    RUN_TEST(test_null_arguments);
    RUN_TEST(test_project_names_match_their_cmakelists);
    return UNITY_END();
}
```
In `tests/host/CMakeLists.txt`: add `${COMP}/hg_image` to the `include_directories(...)` list, and after the
`test_pnl_dim` row add:
```cmake
hg_test(test_hg_image ${COMP}/hg_image/hg_image.c)
target_compile_definitions(test_hg_image PRIVATE
    HG_MASTER_CMAKELISTS="${CMAKE_CURRENT_LIST_DIR}/../../master/CMakeLists.txt"
    HG_ZONE_CMAKELISTS="${CMAKE_CURRENT_LIST_DIR}/../../zone/CMakeLists.txt"
    HG_COPROC_CMAKELISTS="${CMAKE_CURRENT_LIST_DIR}/../../coproc/CMakeLists.txt")
```
(`master/CMakeLists.txt:16`, `zone/CMakeLists.txt:21` and `coproc/CMakeLists.txt:6` hold the three `project()` lines.)

- [ ] **Step 2: Run the test to verify it fails**

Run GATE-HOST with ` -R test_hg_image`.
Expected: `Cannot find source file: .../components/hg_image/hg_image.c`.

- [ ] **Step 3: Write `hg_image`**

Create `components/hg_image/CMakeLists.txt`:
```cmake
# Pure image identification (recovery design 4.2 / 5.7): no IDF dependency, so every app can link it and the host suite
# compiles it straight from source (tests/host/test_hg_image.c).
idf_component_register(SRCS "hg_image.c" INCLUDE_DIRS ".")
```
Create `components/hg_image/hg_image.h`:
```c
#pragma once
/* Pure: identify an ESP-IDF app image from its first HG_IMG_ID_BYTES bytes, before anything is erased. The one copy of
 * this rule and of the project names (recovery design 4.2 / 5.7); the web upload, the panel's microSD install and
 * rescue_p4 all use it. Layout: esp_image_header_t (24 B, chip_id u16 at +12) + one esp_image_segment_header_t (8 B),
 * then esp_app_desc_t at +32: magic_word +32, version[32] +48, project_name[32] +80. */
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HG_IMG_ID_BYTES   112u
#define HG_IMG_MAGIC      0xE9u
#define HG_IMG_DESC_MAGIC 0xABCD5432u
#define HG_CHIP_ESP32     0x0000u
#define HG_CHIP_ESP32C6   0x000Du
#define HG_CHIP_ESP32P4   0x0012u
#define HG_PROJ_MASTER    "hillgrow_master"
#define HG_PROJ_ZONE      "hillgrow_zone"
#define HG_PROJ_RESCUE_P4 "hillgrow_rescue_p4"
#define HG_PROJ_CP        "eh_cp_wifi_softap"
typedef struct { uint16_t chip_id; char project[33]; char version[33]; } hg_image_id_t;
int hg_image_parse(const uint8_t *b, size_t n, hg_image_id_t *out);
    /* +0 magic, +12 chip_id u16 LE, +32 desc magic u32 LE, +48 version[32], +80 project_name[32] (NUL-bounded, non-printables
       -> '.'); 0 ok / -1 n < HG_IMG_ID_BYTES / -2 image magic / -3 desc magic */
int hg_image_is(const uint8_t *b, size_t n, uint16_t chip, const char *want);   /* 1 = parse ok && chip && project == want */

#ifdef __cplusplus
}
#endif
```
Create `components/hg_image/hg_image.c`:
```c
#include <string.h>
#include "hg_image.h"

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void copy_name(char out[33], const uint8_t *src) {
    size_t i;
    for (i = 0; i < 32 && src[i]; i++) out[i] = (src[i] >= 0x20 && src[i] < 0x7F) ? (char)src[i] : '.';
    out[i] = '\0';
}

int hg_image_parse(const uint8_t *b, size_t n, hg_image_id_t *out) {
    if (!b || n < HG_IMG_ID_BYTES) return -1;
    if (b[0] != HG_IMG_MAGIC) return -2;
    if (rd32(b + 32) != HG_IMG_DESC_MAGIC) return -3;
    if (out) {
        out->chip_id = rd16(b + 12);
        copy_name(out->version, b + 48);
        copy_name(out->project, b + 80);
    }
    return 0;
}

int hg_image_is(const uint8_t *b, size_t n, uint16_t chip, const char *want) {
    hg_image_id_t id;
    if (!want || hg_image_parse(b, n, &id) != 0) return 0;
    return id.chip_id == chip && strcmp(id.project, want) == 0;
}
```

- [ ] **Step 4: Run the test to verify it passes**

Run GATE-HOST with ` -R test_hg_image`. Expected: `11 Tests 0 Failures 0 Ignored`.

- [ ] **Step 5: Add the shared trial predicate**

In `components/ota_trial/ota_trial.h`, after `int  ota_trial_confirm(void);`, add:
```c
/* Recovery design 2.3's ONE shared copy of "is the running image still on OTA trial". Read-only: it never confirms or
 * touches the trial (ota_trial_confirm() is an override, not a query). Callers: ota_trial_start() itself, app_main's
 * co-processor gate and cp_ota_restart_for_new_radio(), the master upload sink's ready(), and (later) the boot record,
 * hg_reboot_to_rescue() and RESCUE CONFIRM. */
int  ota_trial_running_on_trial(void);   /* 1 while the running slot is ESP_OTA_IMG_PENDING_VERIFY; 0 for VALID, UNDEFINED,
                                            factory/non-OTA (ESP_ERR_NOT_SUPPORTED) and any failed query */
```
In `components/ota_trial/ota_trial.c`, add above `ota_trial_start()` (this carries over the rationale from
`app_main.c:32-65`, which Step 6 deletes):
```c
/* Every state the running slot can be observed in, and why "not on trial" is the safe answer for all the others:
 *   PENDING_VERIFY  the trial -- the one state that must gate (any reset now retires the image: the bootloader rewrites
 *                   every PENDING_VERIFY otadata entry to ABORTED before it selects a partition)
 *   VALID           the trial already passed; there is nothing left to protect
 *   UNDEFINED       the normal bench state, what tools/hg_otadata.py writes
 *   NEW             unreachable at runtime: the bootloader rewrites NEW -> PENDING_VERIFY before it hands over
 *   factory / any non-OTA running partition -- esp_ota_get_state_partition() returns ESP_ERR_NOT_SUPPORTED, the
 *                   "== ESP_OK" conjunct fails and this reports 0. Correct: the bootloader's rollback logic only inspects
 *                   otadata entries for OTA slots, so a factory boot has no unconfirmed image to vote against.
 * A query that fails for any other reason lands in the same place, and that is the safe direction: reporting "no trial"
 * only re-enables work that is unconditionally fine whenever there really is no trial. `state` is pre-initialised but
 * never read after a failed call (short-circuit &&). */
int ota_trial_running_on_trial(void) {
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    return running && esp_ota_get_state_partition(running, &state) == ESP_OK &&
           state == ESP_OTA_IMG_PENDING_VERIFY;
}
```
and replace the start of `ota_trial_start()`'s body -- from `(void)is_master;` through the closing `}` of the
"not an OTA-pending boot" `if` (today `ota_trial.c:57-63`) -- with:
```c
    (void)is_master;
    if (!ota_trial_running_on_trial()) return;   /* not an OTA-pending boot (e.g. flashed by tools): nothing to trial */
```

- [ ] **Step 6: Point every caller at it**

In `master/main/app_main.c`:
- delete the comment block and the function `static int running_slot_on_ota_trial(void) { ... }` (today lines 32-71,
  from `/* Is the image this master is running still on OTA trial` to the function's closing `}`);
- replace both calls `running_slot_on_ota_trial()` (in `cp_ota_restart_for_new_radio()` and at the `cp_ota_sync()`
  gate) with `ota_trial_running_on_trial()`;
- in the comment above `cp_ota_restart_for_new_radio()`, change "Both tests read running_slot_on_ota_trial() above" to
  "Both tests read ota_trial_running_on_trial() (components/ota_trial)".
Find any other mention with `git -C C:\Projects\HillGrov grep -n running_slot_on_ota_trial -- master components` and
update it the same way (docs are left to Task 34). `ota_trial.h` is already included (`app_main.c:13`).

In `components/http_srv/http_upload_master.c`: add `#include "ota_trial.h"` and replace the trial check in
`master_ready()` (the `esp_ota_img_states_t st;` line through the closing `}` of its `if`, today :100-105) with:
```c
    if (ota_trial_running_on_trial()) {
        const esp_partition_t *run = esp_ota_get_running_partition();
        ESP_LOGW(TAG, "upload refused: %s is still on trial", run ? run->label : "?");
        return -2;
    }
```
In `components/http_srv/CMakeLists.txt`, add `ota_trial` to `PRIV_REQUIRES`.

- [ ] **Step 7: Run the gates**

1. GATE-HOST. Expected: `out of 51`.
2. GATE-ESP32. Expected: three `Project build complete` (the zone and rescue link `ota_trial`; the new function
   compiles there too), empty lock diff.
3. GATE-P4. Expected: `Project build complete`, 0 warnings.
4. No flash is needed for this task on its own; the Stage 4 gate step 3 exercises the trial path (a master install is
   refused with TRIAL_PENDING while a trial runs, and the co-processor gate still skips).

- [ ] **Step 8: Commit**

```powershell
git -C C:\Projects\HillGrov add components/hg_image/CMakeLists.txt components/hg_image/hg_image.h components/hg_image/hg_image.c components/ota_trial/ota_trial.h components/ota_trial/ota_trial.c master/main/app_main.c components/http_srv/http_upload_master.c components/http_srv/CMakeLists.txt tests/host/test_hg_image.c tests/host/CMakeLists.txt
git -C C:\Projects\HillGrov commit -m "refactor(ota_trial): one trial predicate; feat(hg_image): shared image identity (recovery design 2.3/4.2)" -m "Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>"
git -C C:\Projects\HillGrov push
```

---

### Task 29: `fw_srv` — writer claim and a separate validation buffer (Stage 4)

`fw_srv_revalidate()` is "httpd task ONLY" today (`components/fw_srv/fw_srv.h:56-60`): `validate_image()` and the
`GET /fw/zone.bin` send loop share one static 4 KB `s_buf` (`fw_srv.c:28`), which is safe only because both run on the
single `httpd` task. A panel install runs on `pnl_work` and would race a zone pulling the image. This task gives
validation its own buffer, publishes the verdict under a spinlock, and adds a writer claim: while a writer holds it, the
GET answers `404 FW_NO_IMAGE` without reading flash; while a GET streams, a writer cannot claim.

**Files:**
- Modify: `components/fw_srv/fw_srv.h` (the contract at :50-61 is rewritten; two functions added)
- Modify: `components/fw_srv/fw_srv.c`

**Interfaces:**
- Consumes: nothing new.
- Produces:
  ```c
  int  fw_srv_writer_claim(void);    /* [ANY] 0 = caller owns zone_fw rewrites until release; -1 = a GET /fw/zone.bin is streaming */
  void fw_srv_writer_release(void);  /* [ANY] */
  /* fw_srv_revalidate(): now safe from ANY task that holds the writer claim -- validate_image uses its own static s_vbuf[4096];
     s_buf is the GET send loop's alone. While a writer holds the claim, GET /fw/zone.bin answers 404 FW_NO_IMAGE without
     reading flash. State: {streaming count, writer flag} under a portMUX. */
  ```
  Also `-1` when another writer already holds the claim (one writer at a time).

**What proves what:** `fw_srv` is IDF glue with no host seam. GATE-ESP32 and GATE-P4 prove it builds on both masters.
The interlock is proven on the bench by the Stage 4 gate steps 2 and 4 (a zone install from microSD, then a fleet pull of
that image) and by `web_test --only uploads` (the web's zone upload now takes the claim too, in Task 30).

- [ ] **Step 1: Rewrite the contract**

In `components/fw_srv/fw_srv.h`, replace the comment and declaration of `fw_srv_revalidate` (today :50-61) with:
```c
/* Re-runs the validation after the zone_fw partition has been rewritten (POST /api/fw/zone on httpd, or the panel's
 * microSD install on pnl_work -- both through panel_svc's install core) and updates the cached verdict + length.
 * 0 = the partition now holds a good HGFW-prefixed image, -1 = it does not (the normal answer while the header sector is
 * erased, and after a failed upload).
 *
 * Callable from ANY task that holds the writer claim below: validation reads through its own static buffer (s_vbuf),
 * never the GET send loop's (s_buf), and the verdict is published under a spinlock. */
int fw_srv_revalidate(void);

/* The zone_fw writer claim. A writer (the install core, for a zone image) claims BEFORE the first erase and releases
 * after its last revalidate. 0 = claimed; -1 = a GET /fw/zone.bin is streaming right now, or another writer holds it.
 * While claimed, GET /fw/zone.bin answers 404 FW_NO_IMAGE without reading flash, so a zone never streams a partition that
 * is being rewritten. Both are spinlock-only and safe from any task. */
int  fw_srv_writer_claim(void);
void fw_srv_writer_release(void);
```

- [ ] **Step 2: Implement the claim, the second buffer and the guarded GET**

In `components/fw_srv/fw_srv.c`:

(a) Add `#include "freertos/FreeRTOS.h"` as the first include.

(b) Replace the `s_buf` comment and declaration (today :23-28) with:
```c
/* Two buffers, one owner each: s_buf is the GET /fw/zone.bin send loop's (httpd task only); s_vbuf is validate_image()'s
 * (fw_srv_validate() at boot, and fw_srv_revalidate() from whichever task holds the writer claim). They used to be one
 * buffer, which was safe only while every caller lived on the httpd task. */
static uint8_t s_buf[FW_CHUNK];
static uint8_t s_vbuf[FW_CHUNK];

/* {writer flag, streaming count} and the published verdict, under one spinlock (copies only, never across I/O). */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static uint8_t      s_writer;
static uint8_t      s_streaming;
```

(c) In `validate_image()`, change both uses of `s_buf` to `s_vbuf` (the `esp_partition_read(part, off, s_buf, n)` and
`hg_crc32(crc, s_buf, n)` lines).

(d) Replace `fw_srv_revalidate()` with:
```c
int fw_srv_revalidate(void) {
    if (!s_part) return -1;
    uint32_t len = 0;
    int ok = validate_image(s_part, &len) == 0;
    portENTER_CRITICAL(&s_mux);
    s_img_ok = ok ? 1 : 0;
    s_img_len = ok ? len : 0;   /* a stale length behind s_img_ok = 0 is a trap for anything reading the two together */
    portEXIT_CRITICAL(&s_mux);
    ESP_LOGI(TAG, "zone_fw revalidated: %s (%lu B)", ok ? "ok" : "no image", (unsigned long)(ok ? len : 0));
    return ok ? 0 : -1;
}

int fw_srv_writer_claim(void) {
    int got = 0;
    portENTER_CRITICAL(&s_mux);
    if (!s_writer && s_streaming == 0) { s_writer = 1; got = 1; }
    portEXIT_CRITICAL(&s_mux);
    return got ? 0 : -1;
}

void fw_srv_writer_release(void) {
    portENTER_CRITICAL(&s_mux);
    s_writer = 0;
    portEXIT_CRITICAL(&s_mux);
}

static void stream_end(void) {
    portENTER_CRITICAL(&s_mux);
    if (s_streaming) s_streaming--;
    portEXIT_CRITICAL(&s_mux);
}
```

(e) In `zone_bin_get()`, replace the opening `if (!s_img_ok) { ... }` block with:
```c
    uint8_t ok;
    uint32_t img_len;
    portENTER_CRITICAL(&s_mux);
    ok = s_img_ok && !s_writer;   /* a writer is rewriting zone_fw: never stream it, never even read it */
    img_len = s_img_len;
    if (ok) s_streaming++;
    portEXIT_CRITICAL(&s_mux);
    if (!ok) {
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_send(req, "FW_NO_IMAGE", HTTPD_RESP_USE_STRLEN);   /* identity by default: fine as-is */
        return done_or_close(req);   /* answered; close rather than purge an unread body (see above) */
    }
```
then, in the rest of `zone_bin_get()`:
- use `img_len` in place of `s_img_len` (the `Content-Length` `snprintf` and `uint32_t rem = s_img_len`);
- the header-send failure line becomes
  `if (hn < 0 || (size_t)hn >= sizeof head || send_all(req, head, (size_t)hn, deadline) != 0) { stream_end(); return ESP_FAIL; }`;
- directly after `esp_task_wdt_delete(NULL);` add `stream_end();`.

(f) In `fw_srv_validate()`, wrap the verdict store so boot and runtime publish the same way:
```c
    uint32_t len = 0;
    int ok = validate_image(s_part, &len) == 0;
    portENTER_CRITICAL(&s_mux);
    s_img_ok = ok ? 1 : 0;
    s_img_len = ok ? len : 0;
    portEXIT_CRITICAL(&s_mux);
    if (!ok)
        ESP_LOGW(TAG, "zone_fw image missing/invalid (magic/len/crc) -- GET /fw/zone.bin will 404 FW_NO_IMAGE");
```
in place of its `s_img_ok = (validate_image(...) == 0) ? 1 : 0;` line and the `if (!s_img_ok) ESP_LOGW(...)` after it.

Update the comment on `fw_srv_revalidate` in the file (today :121-129) to say "callable from any task holding the writer
claim" instead of "Callable ONLY from the httpd task".

- [ ] **Step 3: Run the gates**

1. GATE-HOST. Expected: `out of 51` (nothing host-compiled changed).
2. GATE-ESP32. Expected: three `Project build complete`, empty lock diff.
3. GATE-P4. Expected: `Project build complete`, 0 warnings.
4. The bench proof comes with Task 30's adapter (the only caller that claims): Stage 4 gate steps 2, 4 and 5.

- [ ] **Step 4: Commit**

```powershell
git -C C:\Projects\HillGrov add components/fw_srv/fw_srv.h components/fw_srv/fw_srv.c
git -C C:\Projects\HillGrov commit -m "fix(fw_srv): make zone_fw rewrites safe off the httpd task" -m "Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>"
git -C C:\Projects\HillGrov push
```

---

### Task 30: Firmware-install core into `panel_svc`; the web upload becomes an adapter; the fleet gate is installed at boot (Stage 4)

`fw_upload()` (`components/http_srv/http_upload.c:170-307`) holds, bound to `httpd_req_t`, everything a second source
needs: the one-upload claim (`:43-65`), the claim-then-fleet ordering (`:189-204`), the ready/heap/size guards
(`:207-212`), identity before the first write (`:260-272`), the 4 KB buffer with a yield every 8 blocks (`:236`,
`:276`), progress, the TWDT pair (`:233`, `:300`) and cancel-on-failure (`:283-284`). This task moves that sequence into
a pure core behind an injected environment (host-tested), moves both sinks into `panel_svc`, and leaves `http_upload.c`
as the HTTP framing, a recv source, the drain and the response. `node_mgr`'s single fleet-gate slot then holds the core's
claim, installed from `app_main` on every boot rather than from `http_srv_start()` only when the AP came up.

The one deliberate web change is D22: the identity check now also requires the chip id and the app-descriptor magic
(`hg_image_is`). An ESP32 master image posted to the P4 answers `422 IMAGE_MISMATCH` before any erase, instead of
`422 WRITE_FAILED` after `esp_ota_end()` refused it.

**Files:**
- Move: `components/http_srv/http_upload_master.c` → `components/panel_svc/fw_sink_master.c`; `components/http_srv/http_upload_zone.c` → `components/panel_svc/fw_sink_zone.c` (with the edits below)
- Create: `components/panel_svc/psvc_fw_env.c`; `tests/host/test_psvc_fw.c`
- Modify: `components/panel_svc/psvc_fw.h`, `components/panel_svc/psvc_fw.c` (the core), `components/panel_svc/CMakeLists.txt`
- Modify: `components/http_srv/http_upload.c` (the adapter), `components/http_srv/http_upload.h`, `components/http_srv/http_srv.c`, `components/http_srv/CMakeLists.txt`
- Modify: `master/main/app_main.c` (`node_mgr_set_fw_gate(psvc_fw_busy);` immediately before `node_mgr_start();`, both targets)
- Modify: `tests/host/CMakeLists.txt`

**Interfaces:**
- Consumes: `hg_image_is`, `HG_IMG_ID_BYTES`, `HG_CHIP_ESP32`, `HG_PROJ_MASTER`, `HG_PROJ_ZONE` (Task 28, or the landed
  helper); `ota_trial_running_on_trial` (Task 28); `fw_srv_writer_claim`, `fw_srv_writer_release`, `fw_srv_revalidate`
  (Task 29); `psvc_fw_progress_set`, `psvc_fw_progress` (Task 9); `psvc_fleet_idle` (Task 25); `node_mgr_fw_status`,
  `node_mgr_set_fw_gate` (`components/node_mgr/node_mgr.h:116,129`); `psvc_rc_token` (Task 3).
- Produces:
  ```c
  typedef enum { PSVC_FW_MASTER = 0, PSVC_FW_ZONE } psvc_fw_kind_t;
  typedef struct { char slot[17]; char version[33]; uint32_t len; } psvc_fw_result_t;
  typedef struct { size_t consumed; uint8_t started; uint8_t src_failed; } psvc_fw_stats_t;
  #define PSVC_FW_SRC_AGAIN     0     /* no bytes yet: the core kicks the TWDT and calls again (the source decides when to give up) */
  #define PSVC_FW_SRC_STALLED (-1)
  #define PSVC_FW_SRC_FAILED  (-2)
  typedef int (*psvc_fw_read_fn)(void *src, void *buf, size_t cap);   /* >0 bytes / AGAIN / STALLED / FAILED */
  typedef struct { int (*ready)(void); size_t (*max)(void); int (*begin)(size_t len);
                   int (*write)(const void *buf, size_t n); int (*finish)(psvc_fw_result_t *res); void (*cancel)(void); } psvc_fw_sink_t;
       /* ready: 0 go / -1 NO_SLOT / -2 TRIAL_PENDING; max 0 = partition missing; others 0 / -1 WRITE_FAILED (http_upload.h:44-64) */
  typedef struct {
      int (*claim)(void); void (*release)(void);            /* the ONE upload claim (test-and-set) */
      int (*fleet_idle)(void); uint32_t (*heap_free)(void);
      int (*wdt_begin)(void); void (*wdt_kick)(void); void (*wdt_end)(int token);   /* begin: subscribe unless already subscribed */
      void (*yield)(void);
      int (*zone_fw_claim)(void); void (*zone_fw_release)(void);
      uint16_t self_chip;                                   /* CONFIG_IDF_FIRMWARE_CHIP_ID in production */
  } psvc_fw_env_t;
  #define PSVC_FW_LOW_HEAP_B   (40u * 1024u)
  #define PSVC_FW_BUF          4096u   /* static, internal RAM */
  #define PSVC_FW_YIELD_BLOCKS 8
  psvc_rc_t psvc_fw_install(psvc_fw_kind_t kind, size_t len, psvc_fw_read_fn rd, void *src,
                            psvc_fw_result_t *res, psvc_fw_stats_t *st);
  psvc_rc_t psvc_fw_install_with(const psvc_fw_env_t *env, const psvc_fw_sink_t *sink, psvc_fw_kind_t kind, size_t len,
                                 psvc_fw_read_fn rd, void *src, psvc_fw_result_t *res, psvc_fw_stats_t *st);
  int  psvc_fw_busy(void);                           /* [ANY] psvc_fw_env.c: the claim flag -- node_mgr's fleet gate */
  const psvc_fw_env_t  *psvc_fw_env_default(void);   /* psvc_fw_env.c */
  const psvc_fw_sink_t *psvc_fw_sink_master(void);   /* fw_sink_master.c */
  const psvc_fw_sink_t *psvc_fw_sink_zone(void);     /* fw_sink_zone.c */
  void psvc_fw_wdt_kick(void);                       /* esp_task_wdt_status(NULL)==ESP_OK ? esp_task_wdt_reset() : no-op */
  ```
  Precisions this task fixes (the outline leaves them implicit): `claim()` returns 1 when it took the claim and 0 when
  it was held; `st->started` is 1 once every guard passed and the read loop began (it is the web's "drain before
  answering" condition, together with `!src_failed`); `cancel` runs only when `begin` succeeded, and a failed `begin` is
  `WRITE_FAILED` without `cancel`, exactly as `http_upload.c:270` does today; the zone identity is always checked
  against `HG_CHIP_ESP32` (the zone boards), the master identity against `env->self_chip`.

  The web adapter's status mapping (unchanged, D22 aside):

  | `psvc_rc_t` | Response |
  |---|---|
  | UPLOAD_ACTIVE, FLEET_ACTIVE, TRIAL_PENDING, ZONE_FW_BUSY | 409 |
  | NO_SLOT, INTERNAL | 500 |
  | LOW_HEAP | 503 |
  | TOO_LARGE | 413 |
  | STALLED, RECV_FAILED | 400 |
  | IMAGE_MISMATCH, WRITE_FAILED | 422 |
  | OK, master | 200 `{"ok":true,"slot":"%s","version":"%s"}` |
  | OK, zone | 200 `{"ok":true,"len":%lu}` |

**What proves what:** `test_psvc_fw` pins the guard order, identity-before-erase, the claim/TWDT/zone-claim pairing on
every path, progress, yields and cancel rules. The sinks and the environment are IDF glue: GATE-ESP32 and GATE-P4 prove
they build on both masters, and `web_test --only uploads` (Stage 4 gate step 5) proves the web codes did not move,
including the D22 change.

- [ ] **Step 1: Write the failing test**

Create `tests/host/test_psvc_fw.c`:
```c
#include <string.h>
#include "unity.h"
#include "hg_image.h"
#include "psvc_rc.h"
#include "psvc_fw.h"

/* The firmware-install core, behind a fake environment, a fake sink that records its calls, and a fake reader that serves
   an image in scripted chunk sizes. The web upload (http_upload.c) and the panel's microSD install both run exactly this
   sequence, so every refusal means the same thing in both faces. */

/* ---- fake environment ---- */
static int f_claim_ok, f_claimed, f_releases, f_fleet_idle, f_wdt_begins, f_wdt_open, f_zclaim_ok, f_zclaims, f_zreleases,
           f_yields;
static uint32_t f_heap;
static int e_claim(void) { if (!f_claim_ok || f_claimed) return 0; f_claimed = 1; return 1; }
static void e_release(void) { f_claimed = 0; f_releases++; }
static int e_fleet_idle(void) { return f_fleet_idle; }
static uint32_t e_heap(void) { return f_heap; }
static int e_wdt_begin(void) { f_wdt_begins++; f_wdt_open++; return 1; }
static void e_wdt_kick(void) {}
static void e_wdt_end(int token) { if (token) f_wdt_open--; }
static void e_yield(void) { f_yields++; }
static int e_zclaim(void) { f_zclaims++; return f_zclaim_ok ? 0 : -1; }
static void e_zrelease(void) { f_zreleases++; }
static const psvc_fw_env_t ENV = {
    .claim = e_claim, .release = e_release, .fleet_idle = e_fleet_idle, .heap_free = e_heap,
    .wdt_begin = e_wdt_begin, .wdt_kick = e_wdt_kick, .wdt_end = e_wdt_end, .yield = e_yield,
    .zone_fw_claim = e_zclaim, .zone_fw_release = e_zrelease, .self_chip = HG_CHIP_ESP32P4 };

/* ---- fake sink ---- */
static int k_ready_rc, k_ready_calls, k_begin_rc, k_begins, k_write_rc, k_writes, k_finish_rc, k_finishes, k_cancels;
static size_t k_max, k_written;
static int k_ready(void) { k_ready_calls++; return k_ready_rc; }
static size_t k_maxfn(void) { return k_max; }
static int k_begin(size_t len) { (void)len; k_begins++; return k_begin_rc; }
static int k_write(const void *b, size_t n) { (void)b; k_writes++; k_written += n; return k_write_rc; }
static int k_finish(psvc_fw_result_t *r) {
    k_finishes++;
    if (r) { strcpy(r->slot, "ota_1"); strcpy(r->version, "9.9.9"); r->len = (uint32_t)k_written; }
    return k_finish_rc;
}
static void k_cancel(void) { k_cancels++; }
static const psvc_fw_sink_t SINK = { .ready = k_ready, .max = k_maxfn, .begin = k_begin, .write = k_write,
                                     .finish = k_finish, .cancel = k_cancel };

/* ---- fake reader ---- */
static uint8_t g_img[40960];
static size_t  g_len, r_pos, r_chunk, r_fail_at;
static int     r_fail_rc, r_again, r_saw_kind, r_monotonic, r_last_pct;
static int rd(void *src, void *buf, size_t cap) {
    (void)src;
    const char *kind = NULL;
    uint8_t pct = 0;
    if (psvc_fw_progress(&kind, &pct)) {
        r_saw_kind = 1;
        if ((int)pct < r_last_pct) r_monotonic = 0;
        r_last_pct = pct;
    }
    if (r_again > 0) { r_again--; return PSVC_FW_SRC_AGAIN; }
    if (r_fail_rc && r_pos >= r_fail_at) return r_fail_rc;
    size_t n = cap < r_chunk ? cap : r_chunk;
    if (n > g_len - r_pos) n = g_len - r_pos;
    memcpy(buf, g_img + r_pos, n);
    r_pos += n;
    return (int)n;
}

static void mk_image(size_t len, uint16_t chip, const char *proj) {
    g_len = len;
    for (size_t i = 0; i < sizeof g_img; i++) g_img[i] = (uint8_t)(i * 7u);
    g_img[0] = HG_IMG_MAGIC;
    g_img[12] = (uint8_t)chip; g_img[13] = (uint8_t)(chip >> 8);
    g_img[32] = 0x32; g_img[33] = 0x54; g_img[34] = 0xCD; g_img[35] = 0xAB;     /* 0xABCD5432 LE */
    memset(g_img + 48, 0, 32); memcpy(g_img + 48, "1.2.3", 5);
    memset(g_img + 80, 0, 32); memcpy(g_img + 80, proj, strlen(proj));
}

void setUp(void) {
    f_claim_ok = 1; f_claimed = 0; f_releases = 0; f_fleet_idle = 1; f_heap = 200000u;
    f_wdt_begins = 0; f_wdt_open = 0; f_zclaim_ok = 1; f_zclaims = 0; f_zreleases = 0; f_yields = 0;
    k_ready_rc = 0; k_ready_calls = 0; k_begin_rc = 0; k_begins = 0; k_write_rc = 0; k_writes = 0;
    k_finish_rc = 0; k_finishes = 0; k_cancels = 0; k_max = 1u << 20; k_written = 0;
    r_pos = 0; r_chunk = 4096; r_fail_at = 0; r_fail_rc = 0; r_again = 0;
    r_saw_kind = 0; r_monotonic = 1; r_last_pct = -1;
    mk_image(10000, HG_CHIP_ESP32P4, HG_PROJ_MASTER);
}
void tearDown(void) {}

static psvc_rc_t install(psvc_fw_kind_t kind, psvc_fw_result_t *res, psvc_fw_stats_t *st) {
    return psvc_fw_install_with(&ENV, &SINK, kind, g_len, rd, NULL, res, st);
}

static void assert_all_released(void) {
    const char *k = NULL;
    uint8_t p = 99;
    TEST_ASSERT_EQUAL_INT(0, f_claimed);
    TEST_ASSERT_EQUAL_INT(0, f_wdt_open);
    TEST_ASSERT_EQUAL_INT(f_zclaims == 0 || !f_zclaim_ok ? 0 : 1, f_zreleases);
    TEST_ASSERT_EQUAL_INT(0, psvc_fw_progress(&k, &p));
    TEST_ASSERT_EQUAL_STRING("", k);
}

static void test_master_ok(void) {
    psvc_fw_result_t res; psvc_fw_stats_t st;
    TEST_ASSERT_EQUAL_INT(PSVC_OK, install(PSVC_FW_MASTER, &res, &st));
    TEST_ASSERT_EQUAL_INT(1, k_begins);
    TEST_ASSERT_EQUAL_size_t(10000, k_written);
    TEST_ASSERT_EQUAL_INT(1, k_finishes);
    TEST_ASSERT_EQUAL_INT(0, k_cancels);
    TEST_ASSERT_EQUAL_size_t(10000, st.consumed);
    TEST_ASSERT_EQUAL_UINT8(1, st.started);
    TEST_ASSERT_EQUAL_UINT8(0, st.src_failed);
    TEST_ASSERT_EQUAL_STRING("9.9.9", res.version);
    TEST_ASSERT_EQUAL_INT(1, f_wdt_begins);
    TEST_ASSERT_EQUAL_INT(0, f_zclaims);                 /* a master install never touches zone_fw */
    assert_all_released();
}

static void test_len_zero_is_invalid_and_claims_nothing(void) {
    TEST_ASSERT_EQUAL_INT(PSVC_E_INVALID, psvc_fw_install_with(&ENV, &SINK, PSVC_FW_MASTER, 0, rd, NULL, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(0, f_releases);
    TEST_ASSERT_EQUAL_INT(0, k_ready_calls);
}

static void test_claim_held_is_upload_active_before_ready(void) {
    psvc_fw_stats_t st;
    f_claim_ok = 0;
    TEST_ASSERT_EQUAL_INT(PSVC_E_UPLOAD_ACTIVE, install(PSVC_FW_MASTER, NULL, &st));
    TEST_ASSERT_EQUAL_INT(0, k_ready_calls);
    TEST_ASSERT_EQUAL_INT(0, f_releases);                /* never claimed, never released */
    TEST_ASSERT_EQUAL_UINT8(0, st.started);
}

static void test_fleet_running_is_fleet_active_before_ready(void) {
    f_fleet_idle = 0;
    TEST_ASSERT_EQUAL_INT(PSVC_E_FLEET_ACTIVE, install(PSVC_FW_MASTER, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(0, k_ready_calls);
    TEST_ASSERT_EQUAL_INT(1, f_releases);
    assert_all_released();
}

static void test_ready_codes(void) {
    k_ready_rc = -2;
    TEST_ASSERT_EQUAL_INT(PSVC_E_TRIAL_PENDING, install(PSVC_FW_MASTER, NULL, NULL));
    k_ready_rc = -1;
    TEST_ASSERT_EQUAL_INT(PSVC_E_NO_SLOT, install(PSVC_FW_MASTER, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(0, k_begins);
    assert_all_released();
}

static void test_low_heap_internal_too_large(void) {
    psvc_fw_stats_t st;
    f_heap = 1000;
    TEST_ASSERT_EQUAL_INT(PSVC_E_LOW_HEAP, install(PSVC_FW_MASTER, NULL, NULL));
    f_heap = 200000u; k_max = 0;
    TEST_ASSERT_EQUAL_INT(PSVC_E_INTERNAL, install(PSVC_FW_MASTER, NULL, NULL));
    k_max = 5000;
    TEST_ASSERT_EQUAL_INT(PSVC_E_TOO_LARGE, install(PSVC_FW_MASTER, NULL, &st));
    TEST_ASSERT_EQUAL_UINT8(0, st.started);
    TEST_ASSERT_EQUAL_INT(0, f_wdt_begins);              /* guards run before the TWDT is touched */
    TEST_ASSERT_EQUAL_size_t(0, r_pos);                  /* and before a single byte is read */
    assert_all_released();
}

static void test_wrong_project_refused_before_any_erase(void) {
    psvc_fw_stats_t st;
    mk_image(10000, HG_CHIP_ESP32, HG_PROJ_ZONE);
    TEST_ASSERT_EQUAL_INT(PSVC_E_IMAGE_MISMATCH, install(PSVC_FW_MASTER, NULL, &st));
    TEST_ASSERT_EQUAL_INT(0, k_begins);
    TEST_ASSERT_EQUAL_INT(0, k_cancels);
    TEST_ASSERT_EQUAL_UINT8(1, st.started);              /* past the guards: the web drains before answering */
    TEST_ASSERT_EQUAL_UINT8(0, st.src_failed);
    assert_all_released();
}

static void test_wrong_chip_refused(void) {
    mk_image(10000, HG_CHIP_ESP32, HG_PROJ_MASTER);      /* an ESP32 master image offered to the P4 (D22) */
    TEST_ASSERT_EQUAL_INT(PSVC_E_IMAGE_MISMATCH, install(PSVC_FW_MASTER, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(0, k_begins);
}

static void test_identity_accumulates_across_one_byte_reads(void) {
    mk_image(300, HG_CHIP_ESP32P4, HG_PROJ_MASTER);
    r_chunk = 1;
    TEST_ASSERT_EQUAL_INT(PSVC_OK, install(PSVC_FW_MASTER, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(1, k_begins);
    TEST_ASSERT_EQUAL_size_t(300, k_written);
    assert_all_released();
}

static void test_image_shorter_than_the_header(void) {
    mk_image(50, HG_CHIP_ESP32P4, HG_PROJ_MASTER);
    TEST_ASSERT_EQUAL_INT(PSVC_E_IMAGE_MISMATCH, install(PSVC_FW_MASTER, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(0, k_begins);
}

static void test_stall_after_begin_cancels(void) {
    psvc_fw_stats_t st;
    r_fail_at = 5000; r_fail_rc = PSVC_FW_SRC_STALLED;
    TEST_ASSERT_EQUAL_INT(PSVC_E_STALLED, install(PSVC_FW_MASTER, NULL, &st));
    TEST_ASSERT_EQUAL_UINT8(1, st.src_failed);
    TEST_ASSERT_EQUAL_INT(1, k_cancels);
    assert_all_released();
}

static void test_read_failure_before_identity_does_not_cancel(void) {
    psvc_fw_stats_t st;
    r_chunk = 50; r_fail_at = 50; r_fail_rc = PSVC_FW_SRC_FAILED;
    TEST_ASSERT_EQUAL_INT(PSVC_E_RECV_FAILED, install(PSVC_FW_MASTER, NULL, &st));
    TEST_ASSERT_EQUAL_UINT8(1, st.src_failed);
    TEST_ASSERT_EQUAL_size_t(50, st.consumed);
    TEST_ASSERT_EQUAL_INT(0, k_begins);
    TEST_ASSERT_EQUAL_INT(0, k_cancels);
    assert_all_released();
}

static void test_zone_claim_refused_is_zone_fw_busy(void) {
    mk_image(10000, HG_CHIP_ESP32, HG_PROJ_ZONE);
    f_zclaim_ok = 0;
    TEST_ASSERT_EQUAL_INT(PSVC_E_ZONE_FW_BUSY, install(PSVC_FW_ZONE, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(0, k_begins);
    TEST_ASSERT_EQUAL_INT(0, f_zreleases);
    assert_all_released();
}

static void test_zone_ok_claims_and_releases_zone_fw(void) {
    mk_image(10000, HG_CHIP_ESP32, HG_PROJ_ZONE);
    TEST_ASSERT_EQUAL_INT(PSVC_OK, install(PSVC_FW_ZONE, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(1, f_zclaims);
    TEST_ASSERT_EQUAL_INT(1, f_zreleases);
    assert_all_released();
}

static void test_zone_image_checked_against_esp32_not_self(void) {
    mk_image(10000, HG_CHIP_ESP32P4, HG_PROJ_ZONE);      /* a "zone" built for the P4 is no zone image */
    TEST_ASSERT_EQUAL_INT(PSVC_E_IMAGE_MISMATCH, install(PSVC_FW_ZONE, NULL, NULL));
}

static void test_write_failure_cancels(void) {
    k_write_rc = -1;
    TEST_ASSERT_EQUAL_INT(PSVC_E_WRITE_FAILED, install(PSVC_FW_MASTER, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(1, k_cancels);
    assert_all_released();
}

static void test_finish_failure_cancels(void) {
    k_finish_rc = -1;
    TEST_ASSERT_EQUAL_INT(PSVC_E_WRITE_FAILED, install(PSVC_FW_MASTER, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(1, k_cancels);
    assert_all_released();
}

static void test_begin_failure_does_not_cancel(void) {
    k_begin_rc = -1;                                     /* http_upload.c:270 -- started never set, no cancel */
    TEST_ASSERT_EQUAL_INT(PSVC_E_WRITE_FAILED, install(PSVC_FW_MASTER, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(0, k_cancels);
    assert_all_released();
}

static void test_progress_runs_and_ends_empty(void) {
    mk_image(40000, HG_CHIP_ESP32P4, HG_PROJ_MASTER);
    TEST_ASSERT_EQUAL_INT(PSVC_OK, install(PSVC_FW_MASTER, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(1, r_saw_kind);
    TEST_ASSERT_EQUAL_INT(1, r_monotonic);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(90, r_last_pct);    /* last sample before the final read */
    assert_all_released();
}

static void test_again_is_retried(void) {
    r_again = 3;
    TEST_ASSERT_EQUAL_INT(PSVC_OK, install(PSVC_FW_MASTER, NULL, NULL));
    assert_all_released();
}

static void test_yields_every_eight_blocks(void) {
    mk_image(40000, HG_CHIP_ESP32P4, HG_PROJ_MASTER);    /* 9 full 4 KB writes + 1 partial = 10 */
    TEST_ASSERT_EQUAL_INT(PSVC_OK, install(PSVC_FW_MASTER, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(10, k_writes);
    TEST_ASSERT_EQUAL_INT(1, f_yields);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_master_ok);
    RUN_TEST(test_len_zero_is_invalid_and_claims_nothing);
    RUN_TEST(test_claim_held_is_upload_active_before_ready);
    RUN_TEST(test_fleet_running_is_fleet_active_before_ready);
    RUN_TEST(test_ready_codes);
    RUN_TEST(test_low_heap_internal_too_large);
    RUN_TEST(test_wrong_project_refused_before_any_erase);
    RUN_TEST(test_wrong_chip_refused);
    RUN_TEST(test_identity_accumulates_across_one_byte_reads);
    RUN_TEST(test_image_shorter_than_the_header);
    RUN_TEST(test_stall_after_begin_cancels);
    RUN_TEST(test_read_failure_before_identity_does_not_cancel);
    RUN_TEST(test_zone_claim_refused_is_zone_fw_busy);
    RUN_TEST(test_zone_ok_claims_and_releases_zone_fw);
    RUN_TEST(test_zone_image_checked_against_esp32_not_self);
    RUN_TEST(test_write_failure_cancels);
    RUN_TEST(test_finish_failure_cancels);
    RUN_TEST(test_begin_failure_does_not_cancel);
    RUN_TEST(test_progress_runs_and_ends_empty);
    RUN_TEST(test_again_is_retried);
    RUN_TEST(test_yields_every_eight_blocks);
    return UNITY_END();
}
```
In `tests/host/CMakeLists.txt`, after the `test_hg_image` rows:
```cmake
hg_test(test_psvc_fw ${COMP}/panel_svc/psvc_fw.c ${COMP}/panel_svc/psvc_rc.c ${COMP}/hg_image/hg_image.c)
```
and change Task 9's `test_psvc_state` row, which also links `psvc_fw.c` (from Step 3 on it calls `hg_image_is()`, so
without this the row fails with LNK2019 and GATE-HOST fails, and with it every master and zone build that runs the host
suite first), to:
```cmake
hg_test(test_psvc_state ${COMP}/panel_svc/psvc_state_conv.c ${COMP}/panel_svc/psvc_fw.c ${COMP}/state_snap/state_snap.c ${COMP}/hg_image/hg_image.c)
```
Keep its `target_link_libraries(test_psvc_state cjson_host)` line as it is.

- [ ] **Step 2: Run the test to verify it fails**

Run GATE-HOST with ` -R test_psvc_fw`.
Expected: compile errors in `test_psvc_fw.c` such as `'psvc_fw_env_t': undeclared identifier` / `'PSVC_FW_MASTER':
undeclared identifier` (psvc_fw.h so far holds only Task 9's progress functions).

- [ ] **Step 3: Write the core**

In `components/panel_svc/psvc_fw.h`, after Task 9's two progress declarations, append the whole "Produces" block of this
task (the types, the three `PSVC_FW_SRC_*` values, the three `PSVC_FW_*` constants and the eight functions), with these
comments on the two install functions:
```c
psvc_rc_t psvc_fw_install(psvc_fw_kind_t kind, size_t len, psvc_fw_read_fn rd, void *src,
                          psvc_fw_result_t *res, psvc_fw_stats_t *st);
     /* [WORKER or httpd] DEFINED IN psvc_fw_env.c (glue) = psvc_fw_install_with(psvc_fw_env_default(),
        kind == PSVC_FW_MASTER ? psvc_fw_sink_master() : psvc_fw_sink_zone(), ...) -- so the pure psvc_fw.c the host
        test links never references a glue symbol. The calling task is TWDT-subscribed for the flash loop only (and
        makes no esp_hosted RPC inside it); the install never reboots. */
psvc_rc_t psvc_fw_install_with(const psvc_fw_env_t *env, const psvc_fw_sink_t *sink, psvc_fw_kind_t kind, size_t len,
                               psvc_fw_read_fn rd, void *src, psvc_fw_result_t *res, psvc_fw_stats_t *st);
     /* psvc_fw.c (pure) -- the core; host-tested (tests/host/test_psvc_fw.c).
        order (== http_upload.c:189-306): len 0 -> INVALID; claim (UPLOAD_ACTIVE); fleet_idle else FLEET_ACTIVE; sink->ready
        (-2 TRIAL_PENDING, other != 0 NO_SLOT); heap < PSVC_FW_LOW_HEAP_B LOW_HEAP; max 0 INTERNAL; len > max TOO_LARGE ->
        progress(kind,0) -> wdt_begin -> read loop (AGAIN: kick+retry; STALLED/FAILED -> STALLED/RECV_FAILED, src_failed=1)
        -> identity BEFORE the first write: hg_image_is(buf, fill, kind==MASTER ? env->self_chip : HG_CHIP_ESP32,
        kind==MASTER ? HG_PROJ_MASTER : HG_PROJ_ZONE) else IMAGE_MISMATCH -> (ZONE: zone_fw_claim else ZONE_FW_BUSY) ->
        begin (fail: WRITE_FAILED, no cancel) -> write per PSVC_FW_BUF, yield every PSVC_FW_YIELD_BLOCKS -> finish ->
        failures: cancel if begun -> wdt_end -> zone_fw_release -> progress("",0) -> release.
        claim() returns 1 = taken / 0 = held. st->started = 1 once the guards passed (the web drains the rest of the body
        only when started && !src_failed). res and st may be NULL. */
```
and add `#include <stddef.h>` and `#include "psvc_rc.h"` next to the `#include <stdint.h>` that Task 9's header already
has (`size_t` and `psvc_rc_t` are new here). The header must be self-contained: among others `psvc_fw.c`, both sinks,
`http_upload.c`, `app_main.c`, `test_psvc_state.c`, `pnl_sd.c` and `sys_firmware.c` include `psvc_fw.h` before (or
without) `psvc_rc.h`.

In `components/panel_svc/psvc_fw.c`, add `#include <string.h>` and `#include "hg_image.h"` to the includes, and append:
```c
/* ---- the install core (pure; the environment and the sink are injected) ---- */

/* One buffer for every install: the claim makes installs mutually exclusive. Static and in internal RAM (never PSRAM): it
 * is handed to flash writes. */
static uint8_t s_buf[PSVC_FW_BUF];

psvc_rc_t psvc_fw_install_with(const psvc_fw_env_t *env, const psvc_fw_sink_t *sink, psvc_fw_kind_t kind, size_t len,
                               psvc_fw_read_fn rd, void *src, psvc_fw_result_t *res, psvc_fw_stats_t *st) {
    psvc_fw_stats_t st_local;
    psvc_fw_result_t res_local;
    if (!st) st = &st_local;
    if (!res) res = &res_local;
    memset(st, 0, sizeof *st);
    memset(res, 0, sizeof *res);
    if (!env || !sink || !rd || len == 0) return PSVC_E_INVALID;

    /* Claim BEFORE reading the fleet status (http_upload.c:189-193): node_mgr reads the claim inside the lock it starts a
     * sequence under, so a simultaneous pair always has exactly one loser. */
    if (!env->claim()) return PSVC_E_UPLOAD_ACTIVE;

    psvc_rc_t rc = PSVC_OK;
    int rdy = 1;
    size_t max = 0;
    if (!env->fleet_idle())                          rc = PSVC_E_FLEET_ACTIVE;
    else if ((rdy = sink->ready()) == -2)            rc = PSVC_E_TRIAL_PENDING;
    else if (rdy != 0)                               rc = PSVC_E_NO_SLOT;
    else if (env->heap_free() < PSVC_FW_LOW_HEAP_B)  rc = PSVC_E_LOW_HEAP;
    else if ((max = sink->max()) == 0)               rc = PSVC_E_INTERNAL;
    else if (len > max)                              rc = PSVC_E_TOO_LARGE;
    if (rc != PSVC_OK) {
        env->release();
        return rc;
    }

    const char *kname = kind == PSVC_FW_MASTER ? "master" : "zone";
    const uint16_t want_chip = kind == PSVC_FW_MASTER ? env->self_chip : (uint16_t)HG_CHIP_ESP32;
    const char *want_proj = kind == PSVC_FW_MASTER ? HG_PROJ_MASTER : HG_PROJ_ZONE;
    st->started = 1;
    psvc_fw_progress_set(kname, 0);
    int wdt = env->wdt_begin();

    size_t got = 0, fill = 0;
    int blocks = 0, begun = 0, zclaim = 0;
    while (got < len) {
        env->wdt_kick();
        size_t want = len - got;
        if (want > PSVC_FW_BUF - fill) want = PSVC_FW_BUF - fill;
        int n = rd(src, s_buf + fill, want);
        if (n == PSVC_FW_SRC_AGAIN) continue;                          /* the source bounds its own silence */
        if (n == PSVC_FW_SRC_STALLED) { rc = PSVC_E_STALLED; st->src_failed = 1; break; }
        if (n < 0 || (size_t)n > want) { rc = PSVC_E_RECV_FAILED; st->src_failed = 1; break; }
        fill += (size_t)n;
        got += (size_t)n;
        st->consumed = got;
        psvc_fw_progress_set(kname, (uint32_t)((uint64_t)got * 100u / len));

        /* Identify BEFORE the first write, so nothing is erased for a file that was never going to be accepted. */
        if (!begun && (fill >= HG_IMG_ID_BYTES || got == len)) {
            if (!hg_image_is(s_buf, fill, want_chip, want_proj)) { rc = PSVC_E_IMAGE_MISMATCH; break; }
            if (kind == PSVC_FW_ZONE) {
                if (env->zone_fw_claim() != 0) { rc = PSVC_E_ZONE_FW_BUSY; break; }
                zclaim = 1;
            }
            if (sink->begin(len) != 0) { rc = PSVC_E_WRITE_FAILED; break; }
            begun = 1;
        }
        if (begun && (fill == PSVC_FW_BUF || got == len)) {
            if (sink->write(s_buf, fill) != 0) { rc = PSVC_E_WRITE_FAILED; break; }
            fill = 0;
            if (++blocks % PSVC_FW_YIELD_BLOCKS == 0) env->yield();
        }
    }

    if (rc == PSVC_OK && sink->finish(res) != 0) rc = PSVC_E_WRITE_FAILED;
    if (rc != PSVC_OK && begun) sink->cancel();
    env->wdt_end(wdt);
    if (zclaim) env->zone_fw_release();
    psvc_fw_progress_set("", 0);
    env->release();
    return rc;
}
```

- [ ] **Step 4: Run the test to verify it passes**

Run GATE-HOST with ` -R test_psvc_fw`. Expected: `21 Tests 0 Failures 0 Ignored`.

- [ ] **Step 5: Move the sinks and adapt them**

```powershell
git -C C:\Projects\HillGrov mv components/http_srv/http_upload_master.c components/panel_svc/fw_sink_master.c
git -C C:\Projects\HillGrov mv components/http_srv/http_upload_zone.c components/panel_svc/fw_sink_zone.c
```
In `components/panel_svc/fw_sink_master.c`:
- replace `#include "http_upload.h"` with `#include "psvc_fw.h"`; `TAG` becomes `"fw_sink_master"`;
- in the header comment, "streamed in by http_upload.c" becomes "streamed in by panel_svc's install core
  (psvc_fw_install_with), from the web upload or the panel's microSD";
- add `static uint32_t s_len;` next to `s_ota`; in `master_begin()` add `s_len = 0;`; in `master_write()`, on success,
  `s_len += (uint32_t)n;`;
- replace `master_finish` with:
  ```c
  static int master_finish(psvc_fw_result_t *res) {
      esp_err_t rc = esp_ota_end(s_ota);
      s_ota = 0;   /* esp_ota_end frees the handle on EVERY path -- never abort it now */
      if (rc != ESP_OK) {
          ESP_LOGE(TAG, "esp_ota_end: %s", esp_err_to_name(rc));
          return -1;
      }
      rc = esp_ota_set_boot_partition(s_ota_part);
      if (rc != ESP_OK) {
          ESP_LOGE(TAG, "esp_ota_set_boot_partition(%s): %s", s_ota_part->label, esp_err_to_name(rc));
          return -1;
      }
      esp_app_desc_t d = { 0 };
      if (esp_ota_get_partition_description(s_ota_part, &d) != ESP_OK)
          ESP_LOGW(TAG, "no app description in %s after a good write", s_ota_part->label);
      snprintf(res->slot, sizeof res->slot, "%s", s_ota_part->label);
      json_safe(d.version, sizeof d.version, res->version, sizeof res->version);   /* the web echoes it into JSON */
      res->len = s_len;
      ESP_LOGW(TAG, "master image written to %s (%s) -- awaiting REBOOT CONFIRM", res->slot, res->version);
      return 0;
  }
  ```
- the sink table's type and getter become:
  ```c
  static const psvc_fw_sink_t MASTER_SINK = {
      .ready = master_ready, .max = master_max, .begin = master_begin,
      .write = master_write, .finish = master_finish, .cancel = master_cancel
  };

  const psvc_fw_sink_t *psvc_fw_sink_master(void) { return &MASTER_SINK; }
  ```

In `components/panel_svc/fw_sink_zone.c`:
- replace `#include "esp_task_wdt.h"` and `#include "http_upload.h"` with `#include "psvc_fw.h"`; `TAG` becomes
  `"fw_sink_zone"`;
- the last paragraph of the header comment ("All of this runs on the httpd task ...") becomes: "This runs on whichever
  task called psvc_fw_install() -- httpd for the web, pnl_work for the panel -- always inside the zone_fw writer claim
  (fw_srv_writer_claim(), taken by the install core before begin()), which is what makes fw_srv_revalidate() safe from
  here. The TWDT resets are psvc_fw_wdt_kick(): silent when the caller is not subscribed.";
- replace all four `esp_task_wdt_reset();` calls with `psvc_fw_wdt_kick();`;
- replace `zone_finish`'s signature and its last lines: `static int zone_finish(psvc_fw_result_t *res) {` and, in
  place of the `snprintf(resp, cap, "{\"ok\":true,\"len\":%lu}", ...)` line,
  ```c
      snprintf(res->slot, sizeof res->slot, "zone_fw");
      res->version[0] = '\0';
      res->len = s_len;
  ```
- the sink table and getter become:
  ```c
  static const psvc_fw_sink_t ZONE_SINK = {
      .ready = zone_ready, .max = zone_max, .begin = zone_begin,
      .write = zone_write, .finish = zone_finish, .cancel = zone_cancel
  };

  const psvc_fw_sink_t *psvc_fw_sink_zone(void) { return &ZONE_SINK; }
  ```

- [ ] **Step 6: Write the production environment**

Create `components/panel_svc/psvc_fw_env.c`:
```c
#include <stdint.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_task_wdt.h"
#include "esp_system.h"       /* esp_get_free_heap_size */
#include "esp_log.h"
#include "node_mgr.h"         /* node_mgr_fw_status */
#include "fw_srv.h"           /* fw_srv_writer_claim / release */
#include "psvc_fleet.h"       /* psvc_fleet_idle */
#include "psvc_fw.h"

static const char *TAG = "psvc_fw";

/* ---- the ONE install claim (http_upload.c's s_busy, moved): test-and-set under a spinlock ---- */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static uint8_t      s_busy;

static int claim(void) {
    int got = 0;
    portENTER_CRITICAL(&s_mux);
    if (!s_busy) { s_busy = 1; got = 1; }
    portEXIT_CRITICAL(&s_mux);
    return got;
}

static void release(void) {
    portENTER_CRITICAL(&s_mux);
    s_busy = 0;
    portEXIT_CRITICAL(&s_mux);
}

int psvc_fw_busy(void) {
    portENTER_CRITICAL(&s_mux);
    int b = s_busy;
    portEXIT_CRITICAL(&s_mux);
    return b;
}

static int fleet_idle(void) {
    char f[40] = "";
    node_mgr_fw_status(f, sizeof f);
    return psvc_fleet_idle(f);
}

static uint32_t heap_free(void) { return esp_get_free_heap_size(); }   /* the same figure the web guard used; includes PSRAM on the P4, so it never trips there (D22) */

/* Subscribe for the flash loop only, and only when not already subscribed: deleting a subscription someone else holds
 * would silently unwatch that task (map-svc 1, the httpd TWDT note). Token 1 = we added it and must delete it. */
static int wdt_begin(void) {
    if (esp_task_wdt_status(NULL) == ESP_OK) return 0;
    if (esp_task_wdt_add(NULL) == ESP_OK) return 1;
    ESP_LOGW(TAG, "esp_task_wdt_add failed -- this install runs unwatched");
    return 0;
}

void psvc_fw_wdt_kick(void) {
    if (esp_task_wdt_status(NULL) == ESP_OK) esp_task_wdt_reset();
}

static void wdt_end(int token) { if (token) esp_task_wdt_delete(NULL); }

static void yield(void) { vTaskDelay(1); }   /* KraftWerk lesson: let Wi-Fi and IDLE run during a flash burst */

static int zone_fw_claim(void) { return fw_srv_writer_claim(); }
static void zone_fw_release(void) { fw_srv_writer_release(); }

static const psvc_fw_env_t ENV = {
    .claim = claim, .release = release, .fleet_idle = fleet_idle, .heap_free = heap_free,
    .wdt_begin = wdt_begin, .wdt_kick = psvc_fw_wdt_kick, .wdt_end = wdt_end, .yield = yield,
    .zone_fw_claim = zone_fw_claim, .zone_fw_release = zone_fw_release,
    .self_chip = CONFIG_IDF_FIRMWARE_CHIP_ID,   /* 0x0012 on the P4 (build_p4/sdkconfig), 0x0000 on the ESP32 */
};

const psvc_fw_env_t *psvc_fw_env_default(void) { return &ENV; }

psvc_rc_t psvc_fw_install(psvc_fw_kind_t kind, size_t len, psvc_fw_read_fn rd, void *src,
                          psvc_fw_result_t *res, psvc_fw_stats_t *st) {
    return psvc_fw_install_with(&ENV, kind == PSVC_FW_MASTER ? psvc_fw_sink_master() : psvc_fw_sink_zone(),
                                kind, len, rd, src, res, st);
}
```
In `components/panel_svc/CMakeLists.txt`: add `"psvc_fw_env.c" "fw_sink_master.c" "fw_sink_zone.c"` to the
`hillgrow_master` source list, and add `app_update esp_partition fw_srv hg_blob hg_image ota_trial esp_system freertos`
to `PRIV_REQUIRES`.

- [ ] **Step 7: Turn the web upload into an adapter**

Replace the whole of `components/http_srv/http_upload.c` with:
```c
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "esp_timer.h"
#include "esp_log.h"
#include "psvc_fw.h"
#include "psvc_rc.h"
#include "http_upload.h"
#include "http_srv_internal.h"

static const char *TAG = "http_upload";

/* rescue's rule verbatim: this many CONSECUTIVE recv timeouts (~60 s of no data at all at httpd's 5 s
 * recv_wait_timeout) gives up rather than letting a client that walked out of AP range mid-upload pin the single httpd
 * task with a half-written slot open. */
#define MAX_TIMEOUTS 12

static int type_is_octet_stream(httpd_req_t *req) {
    char ct[48];
    /* TRUNC means the value was longer than this buffer -- a legal header with a long parameter list. The 24-byte type
     * prefix is fully present either way, which is all this compares, so a truncated read is accepted (fix round 1). */
    esp_err_t rc = httpd_req_get_hdr_value_str(req, "Content-Type", ct, sizeof ct);
    if (rc != ESP_OK && rc != ESP_ERR_HTTPD_RESULT_TRUNC) return 0;
    static const char want[] = "application/octet-stream";
    size_t n = sizeof want - 1;
    if (strncasecmp(ct, want, n) != 0) return 0;
    return ct[n] == '\0' || ct[n] == ';' || ct[n] == ' ';   /* parameters are allowed */
}

/* Reads away the rest of a body this handler has already decided to refuse, so the answer goes into a quiet socket (the
 * Task 13 bench finding: closing on a still-streaming client RSTs the connection and the peer loses the response body).
 * 1 = fully consumed, 0 = the peer stalled or went away. Two bounds: 3 consecutive timeouts (~15 s) and a 10 s wall-clock
 * budget, because either alone is escapable (fix round 1). It runs after the install core released its TWDT
 * subscription, so the kick below is a silent no-op; the budget is the bound. */
#define DRAIN_MAX_TIMEOUTS 3
#define DRAIN_BUDGET_US    (10 * 1000 * 1000LL)

static int drain_body(httpd_req_t *req, uint8_t *buf, size_t cap, size_t got) {
    int timeouts = 0;
    int64_t deadline = esp_timer_get_time() + DRAIN_BUDGET_US;
    while (got < req->content_len) {
        psvc_fw_wdt_kick();
        if (esp_timer_get_time() > deadline) {
            ESP_LOGW(TAG, "drain budget spent with %u B still unread -- closing", (unsigned)(req->content_len - got));
            return 0;
        }
        size_t want = req->content_len - got;
        if (want > cap) want = cap;
        int n = httpd_req_recv(req, (char *)buf, want);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            if (++timeouts > DRAIN_MAX_TIMEOUTS) return 0;
            continue;
        }
        if (n <= 0) return 0;
        timeouts = 0;
        got += (size_t)n;
    }
    return 1;
}

/* The install core's byte source for an HTTP body. */
typedef struct { httpd_req_t *req; int timeouts; const char *kind; } recv_src_t;

static int recv_src(void *src, void *buf, size_t cap) {
    recv_src_t *s = (recv_src_t *)src;
    int n = httpd_req_recv(s->req, (char *)buf, cap);
    if (n == HTTPD_SOCK_ERR_TIMEOUT) {
        if (++s->timeouts > MAX_TIMEOUTS) {
            ESP_LOGW(TAG, "%s upload stalled (~%d s of silence), aborting", s->kind, MAX_TIMEOUTS * 5);
            return PSVC_FW_SRC_STALLED;
        }
        return PSVC_FW_SRC_AGAIN;
    }
    if (n <= 0) return PSVC_FW_SRC_FAILED;
    s->timeouts = 0;
    return n;
}

static int status_for(psvc_rc_t rc) {
    switch (rc) {
    case PSVC_E_UPLOAD_ACTIVE: case PSVC_E_FLEET_ACTIVE: case PSVC_E_TRIAL_PENDING: case PSVC_E_ZONE_FW_BUSY: return 409;
    case PSVC_E_LOW_HEAP:                                                                                   return 503;
    case PSVC_E_TOO_LARGE:                                                                                  return 413;
    case PSVC_E_STALLED: case PSVC_E_RECV_FAILED:                                                           return 400;
    case PSVC_E_IMAGE_MISMATCH: case PSVC_E_WRITE_FAILED:                                                   return 422;
    default:                                                                                                return 500;
    }
}

/* Every exit answers exactly once and returns through http_srv_done(): drained = 1 only when the whole body was read,
 * so a refusal with a body still on the wire closes the socket instead of letting httpd purge megabytes. */
static esp_err_t fw_upload(httpd_req_t *req, psvc_fw_kind_t kind) {
    const char *kname = kind == PSVC_FW_MASTER ? "master" : "zone";
    /* Framing first, before anything is claimed (fix round 1): malformed whatever the target's state is. */
    if (httpd_req_get_hdr_value_len(req, "Transfer-Encoding") > 0) {
        http_srv_error(req, 400, "CHUNKED_UNSUPPORTED", NULL);
        return http_srv_done(req, 0);
    }
    if (!type_is_octet_stream(req)) {
        http_srv_error(req, 400, "BAD_TYPE", NULL);
        return http_srv_done(req, 0);
    }
    if (req->content_len == 0) {
        http_srv_error(req, 400, "EMPTY_BODY", NULL);
        return http_srv_done(req, 0);
    }

    ESP_LOGW(TAG, "%s upload: %u B", kname, (unsigned)req->content_len);
    recv_src_t src = { .req = req, .timeouts = 0, .kind = kname };
    psvc_fw_result_t res;
    psvc_fw_stats_t st;
    psvc_rc_t rc = psvc_fw_install(kind, req->content_len, recv_src, &src, &res, &st);

    if (rc != PSVC_OK) {
        ESP_LOGE(TAG, "%s upload refused after %u/%u B: %s", kname, (unsigned)st.consumed,
                 (unsigned)req->content_len, psvc_rc_token(rc));
        int drained = st.consumed == req->content_len;
        /* A guard refusal (started == 0) closes without reading: content_len is whatever the client claimed, and reading
         * megabytes away to be polite would hand the single httpd task to any logged-in client. Past the guards, drain
         * BEFORE answering (curl stops sending once it sees an error status), unless the socket itself failed. */
        static uint8_t dbuf[1024];
        if (!drained && st.started && !st.src_failed) drained = drain_body(req, dbuf, sizeof dbuf, st.consumed);
        http_srv_error(req, status_for(rc), psvc_rc_token(rc), NULL);
        return http_srv_done(req, drained);
    }

    char resp[128];
    if (kind == PSVC_FW_MASTER)
        snprintf(resp, sizeof resp, "{\"ok\":true,\"slot\":\"%s\",\"version\":\"%s\"}", res.slot, res.version);
    else
        snprintf(resp, sizeof resp, "{\"ok\":true,\"len\":%lu}", (unsigned long)res.len);
    http_srv_json(req, 200, resp);
    return http_srv_done(req, 1);
}

esp_err_t h_fw_master(httpd_req_t *req) { return fw_upload(req, PSVC_FW_MASTER); }

esp_err_t h_fw_zone(httpd_req_t *req) { return fw_upload(req, PSVC_FW_ZONE); }
```
Replace the whole of `components/http_srv/http_upload.h` with:
```c
#pragma once
/* Browser firmware upload -- POST /api/fw/master (a raw app image into the inactive OTA slot) and POST /api/fw/zone (a
 * raw zone app image into the zone_fw partition behind the 16-byte HGFW header fw_srv.c validates).
 *
 * http_upload.c is only the HTTP face: the framing checks (Transfer-Encoding -> 400 CHUNKED_UNSUPPORTED, Content-Type
 * must be application/octet-stream -> 400 BAD_TYPE, content_len 0 -> 400 EMPTY_BODY), a recv source for the install
 * core, the drain-before-answer rule and the response. Every other guard -- one install at a time, never while the fleet
 * sequencer runs, the target's ready/heap/size checks, the image identity (magic, chip id, app-descriptor magic and
 * project name) before anything is erased -- is panel_svc's install core (psvc_fw.h), shared with the panel's microSD
 * install. The route's auth bit (http_routes.c) has already run before either handler is reached. */
```
In `components/http_srv/http_srv.c`, delete the three lines
```c
    /* The other half of upload/fleet exclusivity: from here the sequencer
     * refuses to start while a browser upload holds the flash (node_mgr.h). */
    node_mgr_set_fw_gate(http_upload_busy);
```
In `components/http_srv/CMakeLists.txt`, remove `"http_upload_master.c" "http_upload_zone.c"` from the source list and
remove `ota_trial` from `PRIV_REQUIRES` (Task 28 added it for `http_upload_master.c`, which has moved).

- [ ] **Step 8: Install the fleet gate at boot**

In `master/main/app_main.c`, add `#include "psvc_fw.h"` after `#include "http_srv.h"`, and immediately before
`node_mgr_start();` add:
```c
    /* The fleet sequencer's gate (node_mgr.h:118-129): it refuses to start a fleet update while ANY firmware install --
     * a web upload or the panel's microSD install -- holds panel_svc's one install claim. Installed here on every boot,
     * before node_mgr_start(), rather than from http_srv_start(), which never runs on a boot without the AP. */
    node_mgr_set_fw_gate(psvc_fw_busy);
```
Also update the `node_mgr_set_fw_gate` comment in `components/node_mgr/node_mgr.h:118-128`: "http_srv_start() installs
http_upload_busy() here" becomes "app_main installs panel_svc's psvc_fw_busy() here". (One comment line; stage the file.)

- [ ] **Step 9: Run the gates**

1. GATE-HOST. Expected: `out of 52`.
2. GATE-ESP32. Expected: three `Project build complete` (the ESP32 master now builds the moved sinks inside `panel_svc`,
   with `CONFIG_IDF_FIRMWARE_CHIP_ID` 0x0000), empty lock diff.
3. GATE-P4. Expected: `Project build complete`, 0 warnings.
4. FLASH-P4 (dry run, then real).
5. Web regression now, not only at the gate (this rewrote a bench-verified path):
   ```powershell
   python C:\Projects\HillGrov\tools\web_test.py 192.168.7.7 --password hillgrow1 --only uploads --master-bin C:\Projects\HillGrov\master\build_p4\hillgrow_master.bin --zone-bin C:\Projects\HillGrov\zone\build\hillgrow_zone.bin --fleet 2
   ```
   Expected: all PASS, including the zone image sent to `/api/fw/master` → `422 IMAGE_MISMATCH` with nothing erased.
   Then `C:\Python311\python C:\Projects\HillGrov\tools\uart_test.py COM28 --role MASTER` → PASS.

- [ ] **Step 10: Commit**

```powershell
git -C C:\Projects\HillGrov add components/panel_svc/psvc_fw.h components/panel_svc/psvc_fw.c components/panel_svc/psvc_fw_env.c components/panel_svc/fw_sink_master.c components/panel_svc/fw_sink_zone.c components/panel_svc/CMakeLists.txt components/http_srv/http_upload.c components/http_srv/http_upload.h components/http_srv/http_srv.c components/http_srv/CMakeLists.txt components/node_mgr/node_mgr.h master/main/app_main.c tests/host/test_psvc_fw.c tests/host/CMakeLists.txt
git -C C:\Projects\HillGrov commit -m "refactor(panel_svc): one firmware-install core for web and panel" -m "Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>"
git -C C:\Projects\HillGrov push
```
(`git mv` already staged both renames. Name only the new paths: an old path is in neither the index nor the working
tree any more, so naming it makes `git add` fail with `fatal: pathspec ... did not match any files` and stage nothing.)

---

### Task 31: microSD layer and firmware file picking (Stage 4)

The microSD slot is SDMMC **slot 0** (IO39-44, `docs/pin-mapping.md:68`); the C6 is slot 1 on the same, single SDMMC
controller, which esp_hosted creates with the legacy `sdmmc_host_init()` before `app_main`
(`CONFIG_ESP_HOSTED_AUTO_CALL_INIT_BEFORE_APP_MAIN=y`). `bsp_sdcard_mount()` would call the real `sdmmc_host_init()` a
second time, which cannot claim the controller, and it never deletes its LDO handle. So the panel mounts with the BSP's
slot settings (`esp32_p4_wifi6_touch_lcd_7b.c:112-148` in the BSP 3.0.1 source: `SDMMC_HOST_DEFAULT()`, slot 0,
`SDMMC_FREQ_HIGHSPEED`, LDO channel 4, width 4, no CD/WP) but a no-op `host.init`, keeps the default
`deinit_p = sdmmc_host_deinit_slot` (it removes slot 0 and leaves the controller to slot 1:
`esp_driver_sdmmc/legacy/src/sdmmc_host.c:150-164` maps the "still in use" delete to `ESP_OK`), and mounts transiently on
the worker. FAT32 only (IDF 6.0.1 FATFS has no exFAT), and never `format_if_mount_failed`.

**Files:**
- Create: `components/panel_ui/pnl_sd_pick.h`, `components/panel_ui/pnl_sd_pick.c` (P); `components/panel_ui/pnl_sd.h`, `components/panel_ui/pnl_sd.c` (G)
- Create: `tests/host/test_pnl_sd_pick.c`
- Modify: `components/panel_ui/CMakeLists.txt` (`PRIV_REQUIRES` gains `hg_image`), `tests/host/CMakeLists.txt`

**Interfaces:**
- Consumes: `esp_vfs_fat_sdmmc_mount`, `esp_vfs_fat_sdcard_unmount` (`fatfs/vfs/esp_vfs_fat.h:184,243`);
  `SDMMC_HOST_DEFAULT()`, `SDMMC_HOST_SLOT_0`, `sdmmc_host_init` (`esp_driver_sdmmc/legacy/include/driver/sdmmc_default_configs.h`,
  `driver/sdmmc_host.h`); `sd_pwr_ctrl_new_on_chip_ldo`, `sd_pwr_ctrl_del_on_chip_ldo` (`sdmmc/include/sd_pwr_ctrl_by_on_chip_ldo.h:36,47`);
  `hg_image_parse`, `HG_*` (Task 28); `PSVC_FW_SRC_FAILED` (Task 30); `pnl_on_lvgl_task` (Task 8).
- Produces:
  ```c
  /* pnl_sd_pick.h (pure) */
  #define PNL_SD_NAME_MAX 64
  typedef struct { char name[PNL_SD_NAME_MAX]; char path[96]; uint32_t size; uint8_t hdr[HG_IMG_ID_BYTES]; uint8_t hdr_len; } pnl_sd_file_t;
  typedef enum { PNL_FW_UNKNOWN = 0, PNL_FW_MASTER, PNL_FW_ZONE, PNL_FW_WRONG_CHIP, PNL_FW_RADIO } pnl_fw_class_t;
  int            pnl_sd_is_bin_name(const char *name);   /* case-insensitive ".bin" */
  pnl_fw_class_t pnl_sd_classify(const pnl_sd_file_t *f, uint16_t self_chip, char version[33]);
       /* master on self_chip -> MASTER; hillgrow_master on another chip -> WRONG_CHIP; hillgrow_zone on ESP32 -> ZONE;
          eh_cp_wifi_softap on C6 -> RADIO (listed, not installable here); else UNKNOWN; size 0 -> UNKNOWN */
  void pnl_sd_sort(pnl_sd_file_t *v, pnl_fw_class_t *cls, int n);   /* MASTER, ZONE, RADIO, WRONG_CHIP, UNKNOWN; then by name */
  /* pnl_sd.h (glue) */
  typedef enum { PNL_SD_OK = 0, PNL_SD_NO_CARD, PNL_SD_NO_FS, PNL_SD_BUSY, PNL_SD_IO } pnl_sd_rc_t;
  #define PNL_SD_MOUNT "/sdcard"
  #define PNL_SD_DIR   "/sdcard/hillgrow"
  pnl_sd_rc_t pnl_sd_mount(void);
  void        pnl_sd_unmount(void);
  int         pnl_sd_mounted(void);   /* [ANY] the recovery plan's esp_hosted deinit/reconnect must refuse while this is 1 (D20) */
  int         pnl_sd_list_bins(pnl_sd_file_t *out, int cap);   /* [WORKER] *.bin in "/" and PNL_SD_DIR, first 112 bytes each */
  typedef struct { FILE *f; uint32_t left; } pnl_sd_src_t;
  int         pnl_sd_read(void *src, void *buf, size_t cap);   /* psvc_fw_read_fn: >0, or PSVC_FW_SRC_FAILED on a short read */
  ```
  One precision: `hillgrow_zone` on a chip other than the ESP32 is `WRONG_CHIP` too (the outline names only the master
  case; a zone image built for the wrong chip is exactly as uninstallable).
- Produces **(addition)**:
  ```c
  const char *pnl_sd_rc_text(pnl_sd_rc_t rc);   /* [ANY] operator text: NO_FS -> "Card is not FAT32 -- exFAT cards (64 GB and up)
                                                   must be reformatted to FAT32"; NO_CARD -> "No microSD card found -- insert a
                                                   FAT32 card"; BUSY -> "microSD busy -- try again"; IO -> "microSD read failed" */
  ```

**What proves what:** `test_pnl_sd_pick` pins classification and ordering. The mount is glue whose correctness depends on
the real controller shared with esp_hosted, so only the bench proves it: Task 32's spot check and the Stage 4 gate steps
1, 2 and 7 (a listing, an install, an exFAT card refused, a card pulled mid-install) — and the Wi-Fi/AP must keep working
across every mount/unmount.

- [ ] **Step 1: Write the failing test**

Create `tests/host/test_pnl_sd_pick.c`:
```c
#include <string.h>
#include "unity.h"
#include "hg_image.h"
#include "pnl_sd_pick.h"

/* Which .bin files on a card can be installed where. Classification reads only the 112-byte header through hg_image, so
   an unrelated .bin, a truncated file and an image for the wrong chip are all visible but never installable. */

void setUp(void) {}
void tearDown(void) {}

static void mk(pnl_sd_file_t *f, const char *name, uint16_t chip, const char *proj, const char *ver, uint32_t size) {
    memset(f, 0, sizeof *f);
    strcpy(f->name, name);
    strcpy(f->path, "/sdcard/");
    strcat(f->path, name);
    f->size = size;
    f->hdr[0] = HG_IMG_MAGIC;
    f->hdr[12] = (uint8_t)chip; f->hdr[13] = (uint8_t)(chip >> 8);
    f->hdr[32] = 0x32; f->hdr[33] = 0x54; f->hdr[34] = 0xCD; f->hdr[35] = 0xAB;
    memcpy(f->hdr + 48, ver, strlen(ver));
    memcpy(f->hdr + 80, proj, strlen(proj));
    f->hdr_len = HG_IMG_ID_BYTES;
}

static void test_bin_names(void) {
    TEST_ASSERT_EQUAL_INT(1, pnl_sd_is_bin_name("hillgrow_master.bin"));
    TEST_ASSERT_EQUAL_INT(1, pnl_sd_is_bin_name("ZONE.BIN"));
    TEST_ASSERT_EQUAL_INT(1, pnl_sd_is_bin_name("a.Bin"));
    TEST_ASSERT_EQUAL_INT(0, pnl_sd_is_bin_name("notes.bin.txt"));
    TEST_ASSERT_EQUAL_INT(0, pnl_sd_is_bin_name("bin"));
    TEST_ASSERT_EQUAL_INT(0, pnl_sd_is_bin_name(NULL));
}

static void test_classify_master_zone_radio(void) {
    pnl_sd_file_t f;
    char v[33];
    mk(&f, "m.bin", HG_CHIP_ESP32P4, HG_PROJ_MASTER, "1.5.0", 900000);
    TEST_ASSERT_EQUAL_INT(PNL_FW_MASTER, pnl_sd_classify(&f, HG_CHIP_ESP32P4, v));
    TEST_ASSERT_EQUAL_STRING("1.5.0", v);
    mk(&f, "z.bin", HG_CHIP_ESP32, HG_PROJ_ZONE, "1.5.0", 700000);
    TEST_ASSERT_EQUAL_INT(PNL_FW_ZONE, pnl_sd_classify(&f, HG_CHIP_ESP32P4, v));
    mk(&f, "c6.bin", HG_CHIP_ESP32C6, HG_PROJ_CP, "3.0.7", 1100000);
    TEST_ASSERT_EQUAL_INT(PNL_FW_RADIO, pnl_sd_classify(&f, HG_CHIP_ESP32P4, v));
}

static void test_classify_wrong_chip(void) {
    pnl_sd_file_t f;
    mk(&f, "old_master.bin", HG_CHIP_ESP32, HG_PROJ_MASTER, "1.3.0", 900000);   /* the DevKitC master image */
    TEST_ASSERT_EQUAL_INT(PNL_FW_WRONG_CHIP, pnl_sd_classify(&f, HG_CHIP_ESP32P4, NULL));
    mk(&f, "p4zone.bin", HG_CHIP_ESP32P4, HG_PROJ_ZONE, "1.3.0", 900000);
    TEST_ASSERT_EQUAL_INT(PNL_FW_WRONG_CHIP, pnl_sd_classify(&f, HG_CHIP_ESP32P4, NULL));
}

static void test_classify_unknowns(void) {
    pnl_sd_file_t f;
    char v[33] = "junk";
    mk(&f, "other.bin", HG_CHIP_ESP32, "some_other_app", "9", 1000);
    TEST_ASSERT_EQUAL_INT(PNL_FW_UNKNOWN, pnl_sd_classify(&f, HG_CHIP_ESP32P4, v));
    mk(&f, "empty.bin", HG_CHIP_ESP32P4, HG_PROJ_MASTER, "1.5.0", 0);             /* a 0-byte file with a header? no */
    TEST_ASSERT_EQUAL_INT(PNL_FW_UNKNOWN, pnl_sd_classify(&f, HG_CHIP_ESP32P4, v));
    mk(&f, "short.bin", HG_CHIP_ESP32P4, HG_PROJ_MASTER, "1.5.0", 50);
    f.hdr_len = 50;
    TEST_ASSERT_EQUAL_INT(PNL_FW_UNKNOWN, pnl_sd_classify(&f, HG_CHIP_ESP32P4, v));
    memset(f.hdr, 0xFF, sizeof f.hdr);
    f.hdr_len = HG_IMG_ID_BYTES;
    f.size = 5000;
    TEST_ASSERT_EQUAL_INT(PNL_FW_UNKNOWN, pnl_sd_classify(&f, HG_CHIP_ESP32P4, v));
    TEST_ASSERT_EQUAL_STRING("", v);                                              /* version cleared when unknown */
    TEST_ASSERT_EQUAL_INT(PNL_FW_UNKNOWN, pnl_sd_classify(NULL, HG_CHIP_ESP32P4, v));
}

static void test_sort_by_class_then_name(void) {
    pnl_sd_file_t v[5];
    pnl_fw_class_t c[5] = { PNL_FW_UNKNOWN, PNL_FW_ZONE, PNL_FW_WRONG_CHIP, PNL_FW_MASTER, PNL_FW_ZONE };
    const char *names[5] = { "x.bin", "zb.bin", "w.bin", "m.bin", "za.bin" };
    for (int i = 0; i < 5; i++) { memset(&v[i], 0, sizeof v[i]); strcpy(v[i].name, names[i]); }
    pnl_fw_class_t radio = PNL_FW_RADIO;
    (void)radio;
    pnl_sd_sort(v, c, 5);
    TEST_ASSERT_EQUAL_STRING("m.bin", v[0].name);  TEST_ASSERT_EQUAL_INT(PNL_FW_MASTER, c[0]);
    TEST_ASSERT_EQUAL_STRING("za.bin", v[1].name); TEST_ASSERT_EQUAL_INT(PNL_FW_ZONE, c[1]);
    TEST_ASSERT_EQUAL_STRING("zb.bin", v[2].name); TEST_ASSERT_EQUAL_INT(PNL_FW_ZONE, c[2]);
    TEST_ASSERT_EQUAL_STRING("w.bin", v[3].name);  TEST_ASSERT_EQUAL_INT(PNL_FW_WRONG_CHIP, c[3]);
    TEST_ASSERT_EQUAL_STRING("x.bin", v[4].name);  TEST_ASSERT_EQUAL_INT(PNL_FW_UNKNOWN, c[4]);
}

static void test_sort_puts_radio_between_zone_and_wrong_chip(void) {
    pnl_sd_file_t v[3];
    pnl_fw_class_t c[3] = { PNL_FW_WRONG_CHIP, PNL_FW_RADIO, PNL_FW_ZONE };
    for (int i = 0; i < 3; i++) { memset(&v[i], 0, sizeof v[i]); strcpy(v[i].name, "a.bin"); }
    pnl_sd_sort(v, c, 3);
    TEST_ASSERT_EQUAL_INT(PNL_FW_ZONE, c[0]);
    TEST_ASSERT_EQUAL_INT(PNL_FW_RADIO, c[1]);
    TEST_ASSERT_EQUAL_INT(PNL_FW_WRONG_CHIP, c[2]);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_bin_names);
    RUN_TEST(test_classify_master_zone_radio);
    RUN_TEST(test_classify_wrong_chip);
    RUN_TEST(test_classify_unknowns);
    RUN_TEST(test_sort_by_class_then_name);
    RUN_TEST(test_sort_puts_radio_between_zone_and_wrong_chip);
    return UNITY_END();
}
```
In `tests/host/CMakeLists.txt`, after the `test_psvc_fw` row:
```cmake
hg_test(test_pnl_sd_pick ${COMP}/panel_ui/pnl_sd_pick.c ${COMP}/hg_image/hg_image.c)
```

- [ ] **Step 2: Run the test to verify it fails**

Run GATE-HOST with ` -R test_pnl_sd_pick`.
Expected: `Cannot find source file: .../components/panel_ui/pnl_sd_pick.c`.

- [ ] **Step 3: Write the pure picker**

Create `components/panel_ui/pnl_sd_pick.h`:
```c
#pragma once
/* Pure (host-tested, tests/host/test_pnl_sd_pick.c): classify and order the .bin files found on the microSD card. */
#include <stdint.h>
#include "hg_image.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PNL_SD_NAME_MAX 64
typedef struct { char name[PNL_SD_NAME_MAX]; char path[96]; uint32_t size; uint8_t hdr[HG_IMG_ID_BYTES]; uint8_t hdr_len; } pnl_sd_file_t;
typedef enum { PNL_FW_UNKNOWN = 0, PNL_FW_MASTER, PNL_FW_ZONE, PNL_FW_WRONG_CHIP, PNL_FW_RADIO } pnl_fw_class_t;
int            pnl_sd_is_bin_name(const char *name);   /* case-insensitive ".bin" */
pnl_fw_class_t pnl_sd_classify(const pnl_sd_file_t *f, uint16_t self_chip, char version[33]);
     /* master on self_chip -> MASTER; hillgrow_master on another chip -> WRONG_CHIP; hillgrow_zone on ESP32 -> ZONE
        (on another chip -> WRONG_CHIP); eh_cp_wifi_softap on C6 -> RADIO (listed, not installable here); else UNKNOWN;
        size 0 -> UNKNOWN. version (may be NULL) gets the image's version, "" when UNKNOWN. */
void pnl_sd_sort(pnl_sd_file_t *v, pnl_fw_class_t *cls, int n);   /* MASTER, ZONE, RADIO, WRONG_CHIP, UNKNOWN; then by name */

#ifdef __cplusplus
}
#endif
```
Create `components/panel_ui/pnl_sd_pick.c`:
```c
#include <string.h>
#include "pnl_sd_pick.h"

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }

int pnl_sd_is_bin_name(const char *name) {
    if (!name) return 0;
    size_t n = strlen(name);
    if (n < 4) return 0;
    const char *e = name + n - 4;
    return e[0] == '.' && lower(e[1]) == 'b' && lower(e[2]) == 'i' && lower(e[3]) == 'n';
}

pnl_fw_class_t pnl_sd_classify(const pnl_sd_file_t *f, uint16_t self_chip, char version[33]) {
    if (version) version[0] = '\0';
    if (!f || f->size == 0) return PNL_FW_UNKNOWN;
    hg_image_id_t id;
    if (hg_image_parse(f->hdr, f->hdr_len, &id) != 0) return PNL_FW_UNKNOWN;
    pnl_fw_class_t c = PNL_FW_UNKNOWN;
    if (strcmp(id.project, HG_PROJ_MASTER) == 0)                                   c = id.chip_id == self_chip ? PNL_FW_MASTER : PNL_FW_WRONG_CHIP;
    else if (strcmp(id.project, HG_PROJ_ZONE) == 0)                                c = id.chip_id == HG_CHIP_ESP32 ? PNL_FW_ZONE : PNL_FW_WRONG_CHIP;
    else if (strcmp(id.project, HG_PROJ_CP) == 0 && id.chip_id == HG_CHIP_ESP32C6) c = PNL_FW_RADIO;
    if (c != PNL_FW_UNKNOWN && version) memcpy(version, id.version, sizeof id.version);
    return c;
}

static int rank(pnl_fw_class_t c) {
    switch (c) {
    case PNL_FW_MASTER:     return 0;
    case PNL_FW_ZONE:       return 1;
    case PNL_FW_RADIO:      return 2;
    case PNL_FW_WRONG_CHIP: return 3;
    default:                return 4;
    }
}

void pnl_sd_sort(pnl_sd_file_t *v, pnl_fw_class_t *cls, int n) {
    for (int i = 1; i < n; i++) {           /* insertion sort: n <= 16 and it is stable */
        pnl_sd_file_t key = v[i];
        pnl_fw_class_t kc = cls[i];
        int j = i - 1;
        while (j >= 0 && (rank(cls[j]) > rank(kc) || (rank(cls[j]) == rank(kc) && strcmp(v[j].name, key.name) > 0))) {
            v[j + 1] = v[j];
            cls[j + 1] = cls[j];
            j--;
        }
        v[j + 1] = key;
        cls[j + 1] = kc;
    }
}
```

- [ ] **Step 4: Run the test to verify it passes**

Run GATE-HOST with ` -R test_pnl_sd_pick`. Expected: `6 Tests 0 Failures 0 Ignored`.

- [ ] **Step 5: Write the mount layer**

Create `components/panel_ui/pnl_sd.h`:
```c
#pragma once
/* Glue: transient microSD access on SDMMC slot 0, beside esp_hosted on slot 1 of the same controller. Mount, use,
 * unmount -- always on the panel worker, never held. */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "pnl_sd_pick.h"

typedef enum { PNL_SD_OK = 0, PNL_SD_NO_CARD, PNL_SD_NO_FS, PNL_SD_BUSY, PNL_SD_IO } pnl_sd_rc_t;
#define PNL_SD_MOUNT "/sdcard"
#define PNL_SD_DIR   "/sdcard/hillgrow"
pnl_sd_rc_t pnl_sd_mount(void);     /* [WORKER] no-op host.init (esp_hosted holds the controller); if the slot add fails for want
     of a controller, retry once with the real sdmmc_host_init; format_if_mount_failed false; max_files 2; ESP_FAIL -> NO_FS
     ("Card is not FAT32 -- exFAT cards (64 GB and up) must be reformatted to FAT32"); card timeouts -> NO_CARD; already
     mounted -> BUSY */
void        pnl_sd_unmount(void);   /* [WORKER] unmount + delete the LDO handle; slot 0 removed, controller stays for the C6 */
int         pnl_sd_mounted(void);   /* [ANY] the recovery plan's esp_hosted deinit/reconnect must refuse while this is 1 (D20) */
int         pnl_sd_list_bins(pnl_sd_file_t *out, int cap);   /* [WORKER] *.bin in "/" and PNL_SD_DIR, first 112 bytes each;
                                                                 n >= 0, or -1 when not mounted */
typedef struct { FILE *f; uint32_t left; } pnl_sd_src_t;
int         pnl_sd_read(void *src, void *buf, size_t cap);   /* psvc_fw_read_fn: >0, or PSVC_FW_SRC_FAILED on a short read */
const char *pnl_sd_rc_text(pnl_sd_rc_t rc);                  /* [ANY] operator text for a mount result */
```
Create `components/panel_ui/pnl_sd.c`:
```c
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include "psvc_fw.h"          /* PSVC_FW_SRC_FAILED */
#include "pnl_worker.h"
#include "pnl_sd.h"

static const char *TAG = "pnl_sd";

#define SD_LDO_CHAN 4   /* bsp_sdcard_mount(): on-chip LDO VO4 powers the SD IO (the DSI PHY is VO3) */

static sdmmc_card_t         *s_card;
static sd_pwr_ctrl_handle_t  s_pwr;
static volatile uint8_t      s_mounted;

static esp_err_t noop_host_init(void) { return ESP_OK; }   /* esp_hosted already created the one SDMMC controller */

static pnl_sd_rc_t map_err(esp_err_t e) {
    switch (e) {
    case ESP_OK:                return PNL_SD_OK;
    case ESP_FAIL:              return PNL_SD_NO_FS;     /* no FAT volume: exFAT, unformatted or damaged -- never formatted */
    case ESP_ERR_TIMEOUT:
    case ESP_ERR_NOT_FOUND:
    case ESP_ERR_INVALID_RESPONSE:
    case ESP_ERR_INVALID_CRC:
    case ESP_ERR_NOT_SUPPORTED: return PNL_SD_NO_CARD;
    case ESP_ERR_INVALID_STATE: return PNL_SD_BUSY;
    default:                    return PNL_SD_IO;
    }
}

const char *pnl_sd_rc_text(pnl_sd_rc_t rc) {
    switch (rc) {
    case PNL_SD_OK:      return "OK";
    case PNL_SD_NO_CARD: return "No microSD card found -- insert a FAT32 card";
    case PNL_SD_NO_FS:   return "Card is not FAT32 -- exFAT cards (64 GB and up) must be reformatted to FAT32";
    case PNL_SD_BUSY:    return "microSD busy -- try again";
    default:             return "microSD read failed";
    }
}

pnl_sd_rc_t pnl_sd_mount(void) {
    if (pnl_on_lvgl_task()) { ESP_LOGE(TAG, "pnl_sd_mount called on the LVGL task -- refused"); return PNL_SD_IO; }
    if (s_mounted) return PNL_SD_BUSY;
    sd_pwr_ctrl_ldo_config_t ldo = { .ldo_chan_id = SD_LDO_CHAN };
    esp_err_t e = sd_pwr_ctrl_new_on_chip_ldo(&ldo, &s_pwr);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "SD power (LDO %d): %s", SD_LDO_CHAN, esp_err_to_name(e));
        s_pwr = NULL;
        return PNL_SD_IO;
    }
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_0;
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;
    host.pwr_ctrl_handle = s_pwr;
    host.init = &noop_host_init;           /* deinit_p stays sdmmc_host_deinit_slot: removes slot 0 only */
    const sdmmc_slot_config_t slot = { .cd = SDMMC_SLOT_NO_CD, .wp = SDMMC_SLOT_NO_WP, .width = 4, .flags = 0 };
    const esp_vfs_fat_sdmmc_mount_config_t mc = {
        .format_if_mount_failed = false,   /* never: an exFAT card is refused, not wiped */
        .max_files = 2,
        .allocation_unit_size = 16 * 1024,
    };
    e = esp_vfs_fat_sdmmc_mount(PNL_SD_MOUNT, &host, &slot, &mc, &s_card);
    if (e == ESP_ERR_INVALID_ARG) {
        /* the slot add found no controller: esp_hosted did not create it this boot (radio down). Create it here; the
         * unmount's sdmmc_host_deinit_slot() deletes it again because slot 0 is then its only slot. */
        ESP_LOGW(TAG, "no SDMMC controller (radio down this boot?) -- creating it for slot 0");
        host.init = &sdmmc_host_init;
        e = esp_vfs_fat_sdmmc_mount(PNL_SD_MOUNT, &host, &slot, &mc, &s_card);
    }
    if (e != ESP_OK) {
        sd_pwr_ctrl_del_on_chip_ldo(s_pwr);
        s_pwr = NULL;
        s_card = NULL;
        ESP_LOGW(TAG, "microSD mount failed: %s", esp_err_to_name(e));
        return map_err(e);
    }
    s_mounted = 1;
    return PNL_SD_OK;
}

void pnl_sd_unmount(void) {
    if (!s_mounted) return;
    esp_err_t e = esp_vfs_fat_sdcard_unmount(PNL_SD_MOUNT, s_card);
    if (e != ESP_OK) ESP_LOGW(TAG, "microSD unmount: %s", esp_err_to_name(e));
    if (s_pwr) { sd_pwr_ctrl_del_on_chip_ldo(s_pwr); s_pwr = NULL; }   /* the BSP's unmount never does this */
    s_card = NULL;
    s_mounted = 0;
}

int pnl_sd_mounted(void) { return s_mounted; }

static int scan_dir(const char *dir, pnl_sd_file_t *out, int cap, int n) {
    DIR *d = opendir(dir);
    if (!d) return n;
    struct dirent *de;
    while (n < cap && (de = readdir(d)) != NULL) {
        if (!pnl_sd_is_bin_name(de->d_name)) continue;
        pnl_sd_file_t *f = &out[n];
        memset(f, 0, sizeof *f);
        int w = snprintf(f->path, sizeof f->path, "%s/%s", dir, de->d_name);
        if (w < 0 || (size_t)w >= sizeof f->path) continue;          /* a name too long to open: skip it */
        struct stat st;
        if (stat(f->path, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        snprintf(f->name, sizeof f->name, "%.*s", (int)(sizeof f->name - 1), de->d_name);
        f->size = (uint32_t)st.st_size;
        FILE *fp = fopen(f->path, "rb");
        if (fp) {
            f->hdr_len = (uint8_t)fread(f->hdr, 1, HG_IMG_ID_BYTES, fp);
            fclose(fp);
        }
        n++;
    }
    closedir(d);
    return n;
}

int pnl_sd_list_bins(pnl_sd_file_t *out, int cap) {
    if (!s_mounted) return -1;
    int n = scan_dir(PNL_SD_MOUNT, out, cap, 0);
    return scan_dir(PNL_SD_DIR, out, cap, n);
}

int pnl_sd_read(void *src, void *buf, size_t cap) {
    pnl_sd_src_t *s = (pnl_sd_src_t *)src;
    if (!s || !s->f || s->left == 0) return PSVC_FW_SRC_FAILED;
    size_t want = cap < s->left ? cap : s->left;
    size_t got = fread(buf, 1, want, s->f);
    if (got != want) return PSVC_FW_SRC_FAILED;   /* short read: the card was pulled or the file is damaged */
    s->left -= (uint32_t)got;
    return (int)got;
}
```
In `components/panel_ui/CMakeLists.txt`: inside the gated block add
```cmake
    list(APPEND PANEL_SRCS "pnl_sd_pick.c" "pnl_sd.c")
```
and add `hg_image` to the component's `PRIV_REQUIRES` (it exists in every build, so the early requirements pass for the
zone and rescue projects still resolves). `fatfs`, `sdmmc` and `esp_driver_sdmmc` are already there (Task 6).

- [ ] **Step 6: Run the gates**

1. GATE-HOST. Expected: `out of 53`.
2. GATE-ESP32. Expected: three `Project build complete`, empty lock diff.
3. GATE-P4. Expected: `Project build complete`, 0 warnings (this proves the mount code matches IDF 6.0.1's
   `sdmmc_host_t`, `sdmmc_slot_config_t` and `esp_vfs_fat_mount_config_t`).
4. FLASH-P4 (dry run, then real). Nothing calls `pnl_sd_*` yet; the first mount on glass is Task 32's spot check.

- [ ] **Step 7: Commit**

```powershell
git -C C:\Projects\HillGrov add components/panel_ui/pnl_sd_pick.h components/panel_ui/pnl_sd_pick.c components/panel_ui/pnl_sd.h components/panel_ui/pnl_sd.c components/panel_ui/CMakeLists.txt tests/host/test_pnl_sd_pick.c tests/host/CMakeLists.txt
git -C C:\Projects\HillGrov commit -m "feat(panel_ui): transient microSD mount alongside esp_hosted; firmware file picking" -m "Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>"
git -C C:\Projects\HillGrov push
```

---

### Task 32: System — firmware from microSD (Stage 4)

The web's Firmware card uploads a picked file (`web/app.js:970-976,1866-1907`): progress, then "Uploaded v<version> to
<slot>." with **Reboot now** for a master image, "Uploaded (<len> bytes)." for a zone image. The panel does the same from
the card through the same install core, so every refusal the web can give, the panel gives with the same meaning.

**Files:**
- Create: `components/panel_ui/sys_firmware.c`
- Modify: `components/panel_ui/scr_system.c` (registry FIRMWARE), `components/panel_ui/CMakeLists.txt`

**Interfaces:**
- Consumes: `pnl_sd_mount`, `pnl_sd_unmount`, `pnl_sd_list_bins`, `pnl_sd_read`, `pnl_sd_src_t`, `pnl_sd_classify`,
  `pnl_sd_sort`, `pnl_sd_rc_text` (Task 31); `psvc_fw_install`, `psvc_fw_progress`, `psvc_fw_result_t`, `PSVC_FW_*`
  (Task 30); `pnl_msg(PNL_CTX_FW_MASTER|FW_ZONE)` (Task 18); `sys_reboot_confirm` (Task 25); `pnl_kit_*`, `pnl_confirm`
  (Task 22).
- Produces: `PNL_SYS_FIRMWARE`.

**What proves what:** glue only. GATE-P4 proves it links. The Stage 4 gate steps 1-5 and 7 prove listing, both installs,
the interlocks and the card-failure texts on glass.

- [ ] **Step 1: Write the section**

Create `components/panel_ui/sys_firmware.c`:
```c
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "sdkconfig.h"
#include "esp_heap_caps.h"
#include "lvgl.h"
#include "psvc_fw.h"
#include "pnl_sd.h"
#include "pnl_msg.h"
#include "pnl_worker.h"
#include "pnl_ui_kit.h"
#include "scr_system.h"

#define FW_MAX_FILES 16

typedef struct { char path[96]; uint32_t size; uint8_t kind; } inst_arg_t;   /* 101 B by value */

static pnl_sd_file_t  *s_files;                 /* PSRAM FW_MAX_FILES; the worker owns it while s_reading */
static pnl_fw_class_t  s_cls[FW_MAX_FILES];
static char            s_ver[FW_MAX_FILES][33];
static int             s_n = -1;                /* -1 = the card has not been read */
static char            s_read_err[128];
static uint8_t         s_reading, s_installing;
static char            s_fw_last[128];          /* the last install's outcome: survives leaving and re-entering System */
static uint8_t         s_fw_last_ok, s_fw_last_kind; /* kind: 0 none, else 1 + psvc_fw_kind_t */
static lv_obj_t       *s_list, *s_read_btn, *s_msg, *s_bar, *s_reboot_btn, *s_hint;
static lv_timer_t     *s_prog;

/* Results are kept in module state and drawn through NULL-safe helpers, never gated on screen_gen: the section's
 * teardown NULLs every widget pointer, so a done() that lands while the operator is elsewhere draws nothing, and one
 * that lands after they came back reaches the rebuilt widgets (a gen check would leave "Reading the card..." or a
 * frozen bar on screen until yet another navigation). */

static const char *kind_text(pnl_fw_class_t c) {
    switch (c) {
    case PNL_FW_MASTER:     return "master";
    case PNL_FW_ZONE:       return "zone";
    case PNL_FW_RADIO:      return "radio (not installable here)";
    case PNL_FW_WRONG_CHIP: return "wrong chip";
    default:                return "unknown";
    }
}

static void install_click(lv_event_t *e);

static void render_list(void) {
    if (!s_list) return;
    lv_obj_clean(s_list);
    pnl_kit_enable(s_read_btn, !s_reading && !s_installing);
    lv_obj_t *m;
    if (s_reading) { m = pnl_kit_msg(s_list); pnl_kit_msg_set(m, "Reading the card...", PNL_KIT_INFO); return; }
    if (s_read_err[0]) { m = pnl_kit_msg(s_list); pnl_kit_msg_set(m, s_read_err, PNL_KIT_ERR); return; }
    if (s_n < 0) {
        m = pnl_kit_msg(s_list);
        pnl_kit_msg_set(m, "Put .bin files in the card's root or in /hillgrow, then Read card.", PNL_KIT_INFO);
        return;
    }
    if (s_n == 0) { m = pnl_kit_msg(s_list); pnl_kit_msg_set(m, "No .bin files on the card.", PNL_KIT_INFO); return; }
    for (int i = 0; i < s_n; i++) {
        lv_obj_t *r = pnl_kit_row(s_list);
        lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        char t[160];
        snprintf(t, sizeof t, "%s  |  %s%s%s  |  %lu KB", s_files[i].name, kind_text(s_cls[i]), s_ver[i][0] ? " v" : "",
                 s_ver[i], (unsigned long)(s_files[i].size / 1024u));
        lv_obj_t *l = lv_label_create(r);
        lv_label_set_text(l, t);
        lv_obj_set_flex_grow(l, 1);
        if (s_cls[i] == PNL_FW_MASTER || s_cls[i] == PNL_FW_ZONE) {
            lv_obj_t *b = pnl_kit_button(r, "Install", install_click, (void *)(intptr_t)i);
            pnl_kit_enable(b, !s_installing);
        }
    }
}

/* ---- read the card ---- */
static void read_run(pnl_job_t *j) {
    pnl_sd_rc_t rc = pnl_sd_mount();
    int n = -1;
    if (rc == PNL_SD_OK) {
        n = pnl_sd_list_bins(s_files, FW_MAX_FILES);
        pnl_sd_unmount();
        if (n < 0) rc = PNL_SD_IO;
    }
    if (n > 0) {
        for (int i = 0; i < n; i++) s_cls[i] = pnl_sd_classify(&s_files[i], CONFIG_IDF_FIRMWARE_CHIP_ID, NULL);
        pnl_sd_sort(s_files, s_cls, n);
        for (int i = 0; i < n; i++) (void)pnl_sd_classify(&s_files[i], CONFIG_IDF_FIRMWARE_CHIP_ID, s_ver[i]);
    }
    j->irc = (int)rc;
    memcpy(j->out, &n, sizeof n);
}

static void read_done(pnl_job_t *j) {
    int n;
    memcpy(&n, j->out, sizeof n);
    s_reading = 0;
    if (j->irc != PNL_SD_OK) { s_n = -1; snprintf(s_read_err, sizeof s_read_err, "%s", pnl_sd_rc_text((pnl_sd_rc_t)j->irc)); }
    else { s_n = n; s_read_err[0] = '\0'; }
    render_list();                              /* NULL-safe: draws only if the section is up (see the note above) */
}

static void read_click(lv_event_t *e) {
    (void)e;
    if (s_reading || s_installing) return;
    if (!s_files) s_files = heap_caps_calloc(FW_MAX_FILES, sizeof *s_files, MALLOC_CAP_SPIRAM);
    if (!s_files) { snprintf(s_read_err, sizeof s_read_err, "Card listing unavailable (no memory)"); render_list(); return; }
    if (pnl_worker_submit(read_run, read_done, NULL, 0) != 0) {
        snprintf(s_read_err, sizeof s_read_err, "Panel busy -- try again");
    } else {
        s_reading = 1;
        s_read_err[0] = '\0';
    }
    render_list();
}

/* ---- install ---- */
static void prog_tick(lv_timer_t *t) {
    (void)t;
    const char *k = NULL;
    uint8_t pct = 0;
    if (psvc_fw_progress(&k, &pct) && s_bar) lv_bar_set_value(s_bar, pct, LV_ANIM_OFF);   /* no animation: flash erases stall UI */
}

static void prog_stop(void) {
    if (s_prog) { lv_timer_delete(s_prog); s_prog = NULL; }
}

static void inst_run(pnl_job_t *j) {
    const inst_arg_t *a = (const inst_arg_t *)j->arg;
    psvc_fw_result_t res;
    memset(&res, 0, sizeof res);
    pnl_sd_rc_t sd = pnl_sd_mount();
    j->irc = (int)sd;
    j->rc = PSVC_E_RECV_FAILED;
    if (sd == PNL_SD_OK) {
        FILE *f = fopen(a->path, "rb");
        if (f) {
            pnl_sd_src_t src = { .f = f, .left = a->size };
            psvc_fw_stats_t st;
            j->rc = psvc_fw_install((psvc_fw_kind_t)a->kind, a->size, pnl_sd_read, &src, &res, &st);
            fclose(f);
        }
        pnl_sd_unmount();
    }
    memcpy(j->out, &res, sizeof res);
}

static void reboot_click(lv_event_t *e) { (void)e; sys_reboot_confirm("to run the new image"); }

/* Draws the running install or the kept outcome on whatever widgets exist now; a no-op while torn down. */
static void show_outcome(void) {
    if (!s_msg) return;
    if (s_installing) {
        lv_obj_remove_flag(s_bar, LV_OBJ_FLAG_HIDDEN);
        pnl_kit_msg_set(s_msg, "Installing -- the screen may pause while flash is erased.", PNL_KIT_INFO);
        return;
    }
    lv_obj_add_flag(s_bar, LV_OBJ_FLAG_HIDDEN);
    if (!s_fw_last_kind) return;
    pnl_kit_msg_set(s_msg, s_fw_last, s_fw_last_ok ? PNL_KIT_OK : PNL_KIT_ERR);
    if (s_fw_last_ok && s_fw_last_kind == 1 + PSVC_FW_MASTER) lv_obj_remove_flag(s_reboot_btn, LV_OBJ_FLAG_HIDDEN);
    if (s_fw_last_ok && s_fw_last_kind == 1 + PSVC_FW_ZONE) pnl_kit_msg_set(s_hint, "Push it to zones from Fleet.", PNL_KIT_INFO);
}

static void inst_done(pnl_job_t *j) {
    const inst_arg_t *a = (const inst_arg_t *)j->arg;
    psvc_fw_result_t res;
    memcpy(&res, j->out, sizeof res);
    s_installing = 0;
    prog_stop();
    if (j->irc != PNL_SD_OK) {
        snprintf(s_fw_last, sizeof s_fw_last, "%s", pnl_sd_rc_text((pnl_sd_rc_t)j->irc));
    } else {
        pnl_msg_arg_t ma = { .zone = 0, .version = res.version, .slot = res.slot, .len = res.len };
        pnl_msg(a->kind == PSVC_FW_MASTER ? PNL_CTX_FW_MASTER : PNL_CTX_FW_ZONE, j->rc, &ma, s_fw_last, sizeof s_fw_last);
    }
    s_fw_last_ok = (uint8_t)(j->irc == PNL_SD_OK && j->rc == PSVC_OK);
    s_fw_last_kind = (uint8_t)(1 + a->kind);    /* module state first; the widgets may be gone (note above) */
    show_outcome();
    render_list();
}

static void install_go(void *ctx) {
    int i = (int)(intptr_t)ctx;
    if (s_installing || i < 0 || i >= s_n) return;
    inst_arg_t a;
    memset(&a, 0, sizeof a);
    snprintf(a.path, sizeof a.path, "%s", s_files[i].path);
    a.size = s_files[i].size;
    a.kind = (uint8_t)(s_cls[i] == PNL_FW_MASTER ? PSVC_FW_MASTER : PSVC_FW_ZONE);
    if (pnl_worker_submit(inst_run, inst_done, &a, sizeof a) != 0) { pnl_kit_msg_set(s_msg, "Panel busy -- try again", PNL_KIT_ERR); return; }
    s_installing = 1;
    s_fw_last_kind = 0;                         /* the previous outcome is superseded */
    s_fw_last[0] = '\0';
    lv_bar_set_value(s_bar, 0, LV_ANIM_OFF);
    lv_obj_add_flag(s_reboot_btn, LV_OBJ_FLAG_HIDDEN);
    pnl_kit_msg_set(s_hint, "", PNL_KIT_INFO);
    show_outcome();                             /* the bar and "Installing --" */
    s_prog = lv_timer_create(prog_tick, 250, NULL);
    render_list();
}

static void install_click(lv_event_t *e) {
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= s_n) return;
    char t[200];
    if (s_cls[i] == PNL_FW_MASTER)
        snprintf(t, sizeof t, "Write %s (master v%s) to the inactive slot? The master keeps running; reboot afterwards to "
                 "start it.", s_files[i].name, s_ver[i]);
    else
        snprintf(t, sizeof t, "Store %s (zone v%s) as the zone image? The stored zone image is replaced.",
                 s_files[i].name, s_ver[i]);
    pnl_confirm("Install firmware?", t, "Install", install_go, (void *)(intptr_t)i);
}

/* ---- section ---- */
static void fw_build(lv_obj_t *parent) {
    lv_obj_t *c = pnl_kit_card(parent, "Firmware from microSD");
    lv_obj_t *note = pnl_kit_msg(c);
    pnl_kit_msg_set(note, "FAT32 cards only. Master and zone images are recognised from their own header.", PNL_KIT_INFO);
    s_read_btn = pnl_kit_button(c, "Read card", read_click, NULL);
    s_list = lv_obj_create(c);
    lv_obj_set_width(s_list, LV_PCT(100));
    lv_obj_set_height(s_list, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_list, 0, 0);
    s_bar = lv_bar_create(c);
    lv_obj_set_width(s_bar, LV_PCT(100));
    lv_bar_set_range(s_bar, 0, 100);
    lv_obj_add_flag(s_bar, LV_OBJ_FLAG_HIDDEN);
    s_msg = pnl_kit_msg(c);
    s_hint = pnl_kit_msg(c);
    s_reboot_btn = pnl_kit_button(c, "Reboot now", reboot_click, NULL);
    lv_obj_add_flag(s_reboot_btn, LV_OBJ_FLAG_HIDDEN);
    if (s_installing && !s_prog) s_prog = lv_timer_create(prog_tick, 250, NULL);   /* rebuilt while an install runs */
    show_outcome();                             /* the running install, or the kept outcome (and Reboot now) */
    render_list();
}

static void fw_teardown(void) {
    prog_stop();
    s_list = s_read_btn = s_msg = s_bar = s_reboot_btn = s_hint = NULL;
}

const pnl_sys_section_t PNL_SYS_FIRMWARE = { .title = "Firmware", .build = fw_build, .update = NULL,
                                             .teardown = fw_teardown };
```

- [ ] **Step 2: Register it**

In `components/panel_ui/scr_system.c`, change `[PNL_SYS_SEC_FIRMWARE] = { "Firmware", &PNL_SYS_PLACEHOLDER },` to
`[PNL_SYS_SEC_FIRMWARE] = { "Firmware", &PNL_SYS_FIRMWARE },`. In `components/panel_ui/CMakeLists.txt`, inside the
gated block:
```cmake
    list(APPEND PANEL_SRCS "sys_firmware.c")
```

- [ ] **Step 3: Run the gates**

1. GATE-HOST. Expected: `out of 53`.
2. GATE-ESP32. Expected: three `Project build complete`, empty lock diff.
3. GATE-P4. Expected: `Project build complete`, 0 warnings.
4. FLASH-P4 (dry run, then real).
5. Bench spot check (agent): a FAT32 card holding `C:\Projects\HillGrov\zone\build\hillgrow_zone.bin` → System →
   Firmware → Read card lists it as "zone v<version>"; the phone on the AP keeps loading `/api/state` during the read
   (the SD and the C6 share the controller). The full checks are the Stage 4 gate.

- [ ] **Step 4: Commit**

```powershell
git -C C:\Projects\HillGrov add components/panel_ui/sys_firmware.c components/panel_ui/scr_system.c components/panel_ui/CMakeLists.txt
git -C C:\Projects\HillGrov commit -m "feat(panel_ui): install master and zone firmware from microSD" -m "Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>"
git -C C:\Projects\HillGrov push
```

---

### Task 33: Config export/import via microSD (Stage 4; D13)

The web exports the cached config document as a download and imports a picked file by PUTting it unchanged; for zone 0 it
first drops a blank `STA_PASS`/`AP_PASS`, because an export taken with `?secrets=0` has none and a blank `AP_PASS` would
fail the 8..63 rule (`web/app.js:1756-1784`). The panel writes and reads the same JSON on the card, through the same
merge and the same edit paths as Save. The one C change: the "drop blank secrets" rule the web applies in JavaScript moves
into `hg_json` as an option, so the panel does not re-implement it.

**Files:**
- Create: `components/panel_ui/cfg_card.c`
- Modify: `components/hg_json/hg_json.h`, `components/hg_json/hg_json_mcfg.c`; `components/panel_svc/psvc_mcfg.h`, `components/panel_svc/psvc_mcfg.c`
- Modify: `tests/host/test_hg_json.c`, `tests/host/test_psvc_mcfg.c`
- Modify: `components/panel_ui/scr_config.h`, `components/panel_ui/scr_config.c` (Export/Import in the save bar), `components/panel_ui/CMakeLists.txt`

**Interfaces:**
- Consumes: `hg_json_export_cfg`, `hg_json_export_mcfg(m, 0, ...)` (secrets omitted) (`hg_json.h:31,53`);
  `psvc_zone_cfg_get`, `psvc_zone_cfg_edit`, `psvc_zone_json_fn` (Task 5); `psvc_mcfg_edit`, `psvc_mcfg_get`,
  `PSVC_LOCK_PANEL_MS` (Task 3); `pnl_sd_mount`, `pnl_sd_unmount`, `pnl_sd_rc_text`, `PNL_SD_DIR`, `PNL_SD_MOUNT`
  (Task 31); `pnl_msg(PNL_CTX_ZONE_LOAD|ZONE_SAVE|MCFG_SAVE)` (Task 18); `cfg_set_status` (Task 20); `pnl_confirm`,
  `pnl_kit_button` (Task 22); `pnl_poll_kick` (Task 11).
- Produces:
  ```c
  /* hg_json.h */
  int hg_json_merge_mcfg_opts(hg_mcfg_t *m, const char *json, int skip_blank_secrets, char *err_path, size_t err_cap);
      /* hg_json_merge_mcfg(m,json,e,c) == hg_json_merge_mcfg_opts(m,json,0,e,c); skip_blank_secrets: "" STA_PASS/AP_PASS ignored
         -- the rule the web client applies before a PUT and on Import (app.js:1441-1474, 1775-1780) */
  /* psvc_mcfg.h */
  int psvc_mcfg_json_import_fn(hg_mcfg_t *m, void *ctx /* const char *json */, char *err, size_t errcap);   /* opts(...,1,...) */
  /* cfg_card.c (glue) */
  void cfg_card_export(uint8_t zone);   /* job: mount -> doc -> PNL_SD_DIR "/zone<N>.json" | "/master.json" -> unmount */
  void cfg_card_import(uint8_t zone);   /* job: mount -> read (<= 4096 B, the web body cap) -> psvc_zone_cfg_edit(zone,
                                           psvc_zone_json_fn, json) | psvc_mcfg_edit(psvc_mcfg_json_import_fn, json,
                                           PSVC_LOCK_PANEL_MS, "PANEL IMPORT") -> unmount; the same outcomes as Save
                                           (hw keys -> warnings); unparseable -> "Invalid JSON file" */
  ```
- Produces **(additions)**, declared in `scr_config.h`:
  ```c
  void cfg_card_bar(lv_obj_t *bar);        /* [LVGL] adds [Export to card] [Import from card] to the save bar */
  void cfg_card_set_zone(uint8_t zone);    /* [LVGL] the zone the editor shows (0 = Master); called where it opens one */
  void cfg_card_teardown(void);            /* [LVGL] drop the two button pointers */
  ```
  Import reads the file Export writes (`/hillgrow/zone<N>.json` or `/hillgrow/master.json`); to import another file,
  copy it to that name. An import file larger than 4096 bytes is refused ("File too large (4096 bytes max)"), like the
  web's 413.

**What proves what:** `test_hg_json` and `test_psvc_mcfg` pin the blank-secret rule on both layers. `cfg_card.c` is
glue: GATE-P4 proves it links; the Stage 4 gate step 6 proves export and import on a real card.

- [ ] **Step 1: Write the failing tests**

In `tests/host/test_hg_json.c`, add above `main()`:
```c
static void test_mcfg_merge_opts_skip_blank_secrets(void) {
    hg_mcfg_t m;
    hg_mcfg_defaults(&m);
    hg_json_set_tz_check(NULL);
    char err[64] = "";
    TEST_ASSERT_EQUAL_INT(0, hg_json_merge_mcfg_opts(&m, "{\"WIFI\":{\"AP_PASS\":\"\",\"STA_PASS\":\"\",\"AP_SSID\":\"Glass\"}}",
                                                     1, err, sizeof err));
    TEST_ASSERT_EQUAL_STRING("hillgrow1", m.ap_pass);          /* blank = unchanged */
    TEST_ASSERT_EQUAL_STRING("Glass", m.ap_ssid);
}

static void test_mcfg_merge_opts_skip_keeps_a_real_secret(void) {
    hg_mcfg_t m;
    hg_mcfg_defaults(&m);
    hg_json_set_tz_check(NULL);
    char err[64] = "";
    TEST_ASSERT_EQUAL_INT(0, hg_json_merge_mcfg_opts(&m, "{\"WIFI\":{\"AP_PASS\":\"newpass99\"}}", 1, err, sizeof err));
    TEST_ASSERT_EQUAL_STRING("newpass99", m.ap_pass);
}

static void test_mcfg_merge_without_skip_refuses_blank_ap_pass(void) {
    hg_mcfg_t m;
    hg_mcfg_defaults(&m);
    hg_json_set_tz_check(NULL);
    char err[64] = "";
    TEST_ASSERT_EQUAL_INT(-2, hg_json_merge_mcfg_opts(&m, "{\"WIFI\":{\"AP_PASS\":\"\"}}", 0, err, sizeof err));
    TEST_ASSERT_EQUAL_STRING("WIFI.AP_PASS", err);             /* hg_mcfg_validate: AP_PASS 8..63 */
    TEST_ASSERT_EQUAL_STRING("hillgrow1", m.ap_pass);          /* untouched on failure */
    err[0] = '\0';
    TEST_ASSERT_EQUAL_INT(-2, hg_json_merge_mcfg(&m, "{\"WIFI\":{\"AP_PASS\":\"\"}}", err, sizeof err));   /* same rule */
    TEST_ASSERT_EQUAL_STRING("WIFI.AP_PASS", err);
}
```
and in `main()` before `return UNITY_END();`:
```c
    RUN_TEST(test_mcfg_merge_opts_skip_blank_secrets);
    RUN_TEST(test_mcfg_merge_opts_skip_keeps_a_real_secret);
    RUN_TEST(test_mcfg_merge_without_skip_refuses_blank_ap_pass);
```
In `tests/host/test_psvc_mcfg.c` (Task 3's file), add above `main()`:
```c
static void test_import_fn_keeps_a_blank_secret(void) {
    mcfg_store_init();                                        /* the fake store back to defaults (ap_pass "hillgrow1") */
    mcfg_ops_init();                                          /* idempotent; the lock fails closed until it exists */
    char err[64] = "";
    psvc_rc_t rc = psvc_mcfg_edit(psvc_mcfg_json_import_fn,
                                  (void *)"{\"WIFI\":{\"AP_SSID\":\"Glass\",\"AP_PASS\":\"\",\"STA_PASS\":\"\"}}",
                                  100, "TEST IMPORT", err, sizeof err);
    TEST_ASSERT_EQUAL_INT(PSVC_OK, rc);
    hg_mcfg_t m;
    psvc_mcfg_get(&m);
    TEST_ASSERT_EQUAL_STRING("Glass", m.ap_ssid);
    TEST_ASSERT_EQUAL_STRING("hillgrow1", m.ap_pass);
}

static void test_json_fn_still_refuses_a_blank_ap_pass(void) {
    mcfg_store_init();
    mcfg_ops_init();
    char err[64] = "";
    psvc_rc_t rc = psvc_mcfg_edit(psvc_mcfg_json_fn, (void *)"{\"WIFI\":{\"AP_PASS\":\"\"}}", 100, "TEST PUT", err, sizeof err);
    TEST_ASSERT_EQUAL_INT(PSVC_E_INVALID_FIELD, rc);          /* the web PUT is unchanged: its client drops blanks itself */
    TEST_ASSERT_EQUAL_STRING("WIFI.AP_PASS", err);
}
```
and in its `main()`:
```c
    RUN_TEST(test_import_fn_keeps_a_blank_secret);
    RUN_TEST(test_json_fn_still_refuses_a_blank_ap_pass);
```
(`mcfg_store_init()` is the fake store's reset, `tests/host/fakes/fake_mcfg_store.c:33-38`, as `test_mcfg_ops.c`'s
`setUp` uses it; `mcfg_ops_init()` is idempotent, `components/mcfg_ops/mcfg_ops.c:37-42`. Task 3's
`test_psvc_mcfg.c` already includes `mcfg_store.h` and `mcfg_ops.h`.)

- [ ] **Step 2: Run the tests to verify they fail**

Run GATE-HOST with ` -R "test_hg_json|test_psvc_mcfg"`.
Expected: compile errors `'hg_json_merge_mcfg_opts': undefined` in `test_hg_json.c` and
`'psvc_mcfg_json_import_fn': undeclared identifier` in `test_psvc_mcfg.c`.

- [ ] **Step 3: Implement the option on both layers**

In `components/hg_json/hg_json.h`, after `hg_json_merge_mcfg`'s declaration, add:
```c
/* hg_json_merge_mcfg(m,json,e,c) == hg_json_merge_mcfg_opts(m,json,0,e,c). skip_blank_secrets != 0: a "" STA_PASS or
 * AP_PASS is ignored (blank = unchanged) -- the rule the web client applies before a PUT and on Import
 * (web/app.js:1441-1474, 1775-1780), for the panel's microSD import, whose file came from an export that omits secrets. */
int hg_json_merge_mcfg_opts(hg_mcfg_t *m, const char *json, int skip_blank_secrets, char *err_path, size_t err_cap);
```
In `components/hg_json/hg_json_mcfg.c`, rename `hg_json_merge_mcfg` to `hg_json_merge_mcfg_opts` with the extra
parameter `int skip_blank_secrets` after `json`, add this `continue` directly after the `if (!f) continue;` line:
```c
            if (skip_blank_secrets && hg_mcfg_is_secret(f) && cJSON_IsString(item) && item->valuestring &&
                item->valuestring[0] == '\0')
                continue;   /* blank = unchanged */
```
and append the unchanged-behaviour wrapper:
```c
int hg_json_merge_mcfg(hg_mcfg_t *m, const char *json, char *err_path, size_t err_cap) {
    return hg_json_merge_mcfg_opts(m, json, 0, err_path, err_cap);
}
```
In `components/panel_svc/psvc_mcfg.h`, after `psvc_mcfg_json_fn`, add:
```c
int  psvc_mcfg_json_import_fn(hg_mcfg_t *m, void *ctx /* const char *json */, char *err, size_t errcap);
     /* psvc_mcfg_json_fn with hg_json_merge_mcfg_opts(..., 1, ...): a blank secret keeps the stored one (panel import) */
```
In `components/panel_svc/psvc_mcfg.c`, add next to `psvc_mcfg_json_fn` (the same mapping Task 3 gives it: -1 → BAD_JSON,
-2 → INVALID_FIELD with the path):
```c
int psvc_mcfg_json_import_fn(hg_mcfg_t *m, void *ctx, char *err, size_t errcap) {
    int rc = hg_json_merge_mcfg_opts(m, (const char *)ctx, 1, err, errcap);
    if (rc == -1) return PSVC_EDIT_BAD_JSON;
    if (rc == -2) return PSVC_EDIT_INVALID_FIELD;
    return 0;
}
```

- [ ] **Step 4: Run the tests to verify they pass**

Run GATE-HOST with ` -R "test_hg_json|test_psvc_mcfg"`. Expected: both pass, with the three and two new cases.

- [ ] **Step 5: Write the card export/import**

Create `components/panel_ui/cfg_card.c`:
```c
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>
#include "esp_heap_caps.h"
#include "lvgl.h"
#include "hg_json.h"
#include "psvc_mcfg.h"
#include "psvc_zcfg.h"
#include "pnl_sd.h"
#include "pnl_input.h"      /* pnl_zero */
#include "pnl_msg.h"
#include "pnl_poll.h"
#include "pnl_worker.h"
#include "pnl_ui_kit.h"
#include "scr_config.h"

#define CARD_DOC_MAX 4096u   /* the web's PUT body cap (http_api_cfg.c:257): an export that fits is an import that fits */

typedef struct { uint8_t zone; } card_arg_t;

static char     *s_doc;               /* CARD_DOC_MAX + 1, PSRAM; the worker owns it while s_busy */
static uint8_t   s_busy, s_zone;
static lv_obj_t *s_exp_btn, *s_imp_btn;

static void card_path(uint8_t zone, char *out, size_t cap) {
    if (zone == 0) snprintf(out, cap, "%s/master.json", PNL_SD_DIR);
    else snprintf(out, cap, "%s/zone%u.json", PNL_SD_DIR, (unsigned)zone);
}

static void export_run(pnl_job_t *j) {
    const card_arg_t *a = (const card_arg_t *)j->arg;
    char *out = (char *)j->out;
    j->irc = 1;
    pnl_sd_rc_t sd = pnl_sd_mount();
    if (sd != PNL_SD_OK) { snprintf(out, PNL_JOB_OUT_MAX, "%s", pnl_sd_rc_text(sd)); return; }
    int n = -1;
    if (a->zone == 0) {
        hg_mcfg_t m;
        psvc_mcfg_get(&m);
        n = hg_json_export_mcfg(&m, 0, s_doc, CARD_DOC_MAX);   /* secrets omitted: no password ever reaches the card */
        pnl_zero(&m, sizeof m);                 /* the copy holds both Wi-Fi passwords; memset here is a dead store */
    } else {
        hg_zone_cfg_t cfg;
        hg_zone_hw_t hw;
        uint32_t gen = 0;
        int hw_present = 0;
        psvc_rc_t rc = psvc_zone_cfg_get(a->zone, &cfg, &hw, &gen, &hw_present);   /* hw zeroed when absent, as the web */
        if (rc != PSVC_OK) {
            pnl_msg(PNL_CTX_ZONE_LOAD, rc, NULL, out, PNL_JOB_OUT_MAX);
            pnl_sd_unmount();
            return;
        }
        n = hg_json_export_cfg(&hw, &cfg, gen, s_doc, CARD_DOC_MAX);
    }
    char path[48];
    card_path(a->zone, path, sizeof path);
    if (n < 0) {
        snprintf(out, PNL_JOB_OUT_MAX, "Export failed (document too large)");
    } else {
        mkdir(PNL_SD_DIR, 0775);                               /* EEXIST is fine */
        FILE *f = fopen(path, "w");
        size_t w = f ? fwrite(s_doc, 1, (size_t)n, f) : 0;
        int ce = f ? fclose(f) : -1;
        if (!f || w != (size_t)n || ce != 0) snprintf(out, PNL_JOB_OUT_MAX, "microSD write failed");
        else { snprintf(out, PNL_JOB_OUT_MAX, "Exported to %s", path + strlen(PNL_SD_MOUNT)); j->irc = 0; }
    }
    memset(s_doc, 0, CARD_DOC_MAX + 1);
    pnl_sd_unmount();
}

static void import_run(pnl_job_t *j) {
    const card_arg_t *a = (const card_arg_t *)j->arg;
    char *out = (char *)j->out;
    j->irc = 1;
    pnl_sd_rc_t sd = pnl_sd_mount();
    if (sd != PNL_SD_OK) { snprintf(out, PNL_JOB_OUT_MAX, "%s", pnl_sd_rc_text(sd)); return; }
    char path[48];
    card_path(a->zone, path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f) {
        snprintf(out, PNL_JOB_OUT_MAX, "No %s on the card", path + strlen(PNL_SD_MOUNT));
        pnl_sd_unmount();
        return;
    }
    size_t n = fread(s_doc, 1, CARD_DOC_MAX + 1, f);
    fclose(f);
    pnl_sd_unmount();                                          /* the card is not needed for the apply */
    if (n > CARD_DOC_MAX) { snprintf(out, PNL_JOB_OUT_MAX, "File too large (4096 bytes max)"); memset(s_doc, 0, CARD_DOC_MAX + 1); return; }
    s_doc[n] = '\0';
    char err[96] = "", warn[160] = "";
    psvc_rc_t rc;
    pnl_msg_ctx_t ctx;
    if (a->zone == 0) {
        rc = psvc_mcfg_edit(psvc_mcfg_json_import_fn, s_doc, PSVC_LOCK_PANEL_MS, "PANEL IMPORT", err, sizeof err);
        ctx = PNL_CTX_MCFG_SAVE;
    } else {
        rc = psvc_zone_cfg_edit(a->zone, psvc_zone_json_fn, s_doc, err, sizeof err, warn, sizeof warn);
        ctx = PNL_CTX_ZONE_SAVE;
    }
    pnl_zero(s_doc, CARD_DOC_MAX + 1);                         /* an import file may carry secrets */
    if (rc == PSVC_E_BAD_JSON) {
        snprintf(out, PNL_JOB_OUT_MAX, "Invalid JSON file");   /* app.js:1770 */
        return;
    }
    char m[128];
    pnl_msg(ctx, rc, NULL, m, sizeof m);
    /* Bounded so -Wformat-truncation (-Wall -Werror) can prove the fit: 127 + " (" + 120 + ")" + NUL = 251 <= 256, and
     * 127 + ": " + 95 + NUL = 225 <= 256. A longer warning list is cut at 120 characters on the glass. */
    if (rc == PSVC_OK && warn[0]) snprintf(out, PNL_JOB_OUT_MAX, "%s (%.120s)", m, warn);   /* hw keys -> warnings, like Save */
    else if (rc != PSVC_OK && err[0]) snprintf(out, PNL_JOB_OUT_MAX, "%s: %s", m, err);
    else snprintf(out, PNL_JOB_OUT_MAX, "%s", m);
    j->irc = rc == PSVC_OK ? 0 : 1;
}

static void card_done(pnl_job_t *j) {
    s_busy = 0;
    pnl_kit_enable(s_exp_btn, 1);
    pnl_kit_enable(s_imp_btn, 1);
    if (j->run == import_run && j->irc == 0) pnl_poll_kick();
    if (j->screen_gen == pnl_screen_gen()) cfg_set_status((const char *)j->out, j->irc != 0);
}

static int card_submit(pnl_job_run_fn run, uint8_t zone) {
    if (s_busy) { cfg_set_status("microSD busy -- try again", 1); return -1; }
    if (!s_doc) s_doc = heap_caps_calloc(1, CARD_DOC_MAX + 1, MALLOC_CAP_SPIRAM);
    if (!s_doc) { cfg_set_status("microSD unavailable (no memory)", 1); return -1; }
    card_arg_t a = { .zone = zone };
    if (pnl_worker_submit(run, card_done, &a, sizeof a) != 0) { cfg_set_status("Panel busy -- try again", 1); return -1; }
    s_busy = 1;
    pnl_kit_enable(s_exp_btn, 0);
    pnl_kit_enable(s_imp_btn, 0);
    cfg_set_status("...", 0);
    return 0;
}

void cfg_card_export(uint8_t zone) { (void)card_submit(export_run, zone); }

static void import_go(void *ctx) { (void)card_submit(import_run, (uint8_t)(intptr_t)ctx); }

void cfg_card_import(uint8_t zone) {
    char path[48], t[200];
    card_path(zone, path, sizeof path);
    if (zone == 0)
        snprintf(t, sizeof t, "Apply %s to the master? It may change Wi-Fi: changing the AP drops every phone connected "
                 "to it.", path + strlen(PNL_SD_MOUNT));
    else
        snprintf(t, sizeof t, "Apply %s to zone %u? Unsaved edits on this screen are not part of it.",
                 path + strlen(PNL_SD_MOUNT), (unsigned)zone);
    pnl_confirm("Import from card?", t, "Import", import_go, (void *)(intptr_t)zone);
}

static void exp_click(lv_event_t *e) { (void)e; cfg_card_export(s_zone); }
static void imp_click(lv_event_t *e) { (void)e; cfg_card_import(s_zone); }

void cfg_card_bar(lv_obj_t *bar) {
    s_exp_btn = pnl_kit_button(bar, "Export to card", exp_click, NULL);
    s_imp_btn = pnl_kit_button(bar, "Import from card", imp_click, NULL);
    pnl_kit_enable(s_exp_btn, !s_busy);
    pnl_kit_enable(s_imp_btn, !s_busy);
}

void cfg_card_set_zone(uint8_t zone) { s_zone = zone; }

void cfg_card_teardown(void) { s_exp_btn = s_imp_btn = NULL; }
```
In `components/panel_ui/scr_config.h`, after `cfg_master_wipe`, add:
```c
void cfg_card_export(uint8_t zone);   /* job: mount -> doc -> PNL_SD_DIR "/zone<N>.json" | "/master.json" -> unmount */
void cfg_card_import(uint8_t zone);   /* confirm, then job: mount -> read (<= 4096 B) -> the Save path -> unmount */
void cfg_card_bar(lv_obj_t *bar);     /* [LVGL] adds [Export to card] [Import from card] to the save bar */
void cfg_card_set_zone(uint8_t zone); /* [LVGL] the zone the editor shows (0 = Master) */
void cfg_card_teardown(void);         /* [LVGL] drop the two button pointers */
```
In `components/panel_ui/scr_config.c`:
- in `cfg_frame_build()` (Task 20), the save bar is the local `bar` that holds the index selector and the FLOATING,
  right-aligned Save button. Directly after its last line, `lv_obj_set_style_min_height(bar, 56, 0);`, add:
  ```c
      /* Task 33: [Export to card] [Import from card], packed left of the floating Save button */
      lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
      lv_obj_set_style_pad_column(bar, 12, 0);
      cfg_card_bar(bar);
  ```
  With `SPACE_BETWEEN` the last card button would sit at the right edge, under Save. On glass (Stage 4 gate step 6),
  check with a shelf tab open (the 4-way index selector showing) that neither card button runs under Save; if one
  does, shorten the two labels in `cfg_card_bar()` to "Export" and "Import".
- in `open_editor()` (Task 21's version), after `cfg_zone_open(s_ui.body, (uint8_t)zone);` add
  `cfg_card_set_zone((uint8_t)zone);`, and after `cfg_master_open(s_ui.body);` add `cfg_card_set_zone(0);`. Both calls
  sit in an `if`/`else` without braces, so add the braces;
- in `cfg_teardown()`, add `cfg_card_teardown();` after `close_editor();`.

No new include is needed in `scr_config.c`: the `cfg_card_*` functions are declared in `scr_config.h`, and only
`cfg_card.c` calls the `pnl_kit_*` helpers.

In `components/panel_ui/CMakeLists.txt`, inside the gated block:
```cmake
    list(APPEND PANEL_SRCS "cfg_card.c")
```
and add `hg_json` to the component's `PRIV_REQUIRES` (Task 6's list does not have it, and no task since added it).
`hg_json` exists in every app build, so the early requirements pass stays valid for zone and rescue.

- [ ] **Step 6: Run the gates**

1. GATE-HOST. Expected: `out of 53` (cases added, no new file).
2. GATE-ESP32. Expected: three `Project build complete` (`hg_json` is shared), empty lock diff.
3. GATE-P4. Expected: `Project build complete`, 0 warnings.
4. FLASH-P4 (dry run, then real).
5. `python C:\Projects\HillGrov\tools\web_test.py 192.168.7.7 --password hillgrow1 --only config,mcfg` → PASS (the web's
   zone-0 PUT still refuses a blank AP_PASS; its client drops blanks, as before).
6. The card checks are the Stage 4 gate step 6.

- [ ] **Step 7: Commit**

```powershell
git -C C:\Projects\HillGrov add components/panel_ui/cfg_card.c components/panel_ui/scr_config.h components/panel_ui/scr_config.c components/panel_ui/CMakeLists.txt components/hg_json/hg_json.h components/hg_json/hg_json_mcfg.c components/panel_svc/psvc_mcfg.h components/panel_svc/psvc_mcfg.c tests/host/test_hg_json.c tests/host/test_psvc_mcfg.c
git -C C:\Projects\HillGrov commit -m "feat(panel_ui): config export/import via microSD" -m "Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>"
git -C C:\Projects\HillGrov push
```

---

### Stage 4 bench gate (owner, about 60 min; P4 master on COM28, zones 1 and 2 on the ring, a phone on the AP)

**Before you start (agent):**
1. HEAD has passed GATE-HOST (53 tests), GATE-ESP32 and GATE-P4, and is flashed with FLASH-P4:
   ```powershell
   python C:\Projects\HillGrov\tools\flash_app.py --app master --target esp32p4 --build-dir C:\Projects\HillGrov\master\build_p4 --port COM28 --dry-run
   python C:\Projects\HillGrov\tools\flash_app.py --app master --target esp32p4 --build-dir C:\Projects\HillGrov\master\build_p4 --port COM28
   ```
   Panel → About shows the running slot `VALID` (no OTA trial).
2. Prepare a **FAT32** microSD card (32 GB or smaller, formatted FAT32 on the PC) with, in its root:
   `C:\Projects\HillGrov\master\build_p4\hillgrow_master.bin`, `C:\Projects\HillGrov\zone\build\hillgrow_zone.bin`, and
   one unrelated `.bin` (for example `C:\Projects\HillGrov\master\build_p4\bootloader\bootloader.bin` copied as
   `other.bin`). Have a second card ready that is exFAT (any card of 64 GB or more straight from the pack), or format a
   spare card exFAT on the PC.

Every step is done with a finger on the glass. Console lines go through the phone's web UI console (Dashboard → Zone 2
→ console, forward off) unless a step says otherwise.

1. **Listing.** Insert the FAT32 card. System → Firmware → Read card.
   **Pass:** three rows: `hillgrow_master.bin | master v<version>`, `hillgrow_zone.bin | zone v<version>`, and
   `other.bin | unknown`; only the first two have Install. The phone keeps loading pages during the read.
2. **Zone image install and the interlocks.** Install on `hillgrow_zone.bin` → Install.
   - While the bar moves, on the phone: upload any file to Firmware → "Upload zone image". **Pass:** the phone shows
     `UPLOAD_ACTIVE`.
   - While the bar still moves, in the phone console type `SET FW ZONE 2`. **Pass:** `ERR FW_BUSY`.
   - **Pass:** the panel ends with "Uploaded (<N> bytes)." and "Push it to zones from Fleet."; console `GET FW ZONE`
     reports the stored image.
3. **Master image install.** Install on `hillgrow_master.bin` → Install. **Pass:** "Uploaded v<version> to <slot>." and a
   **Reboot now** button. Tap Reboot now → Reboot. **Pass:** the master restarts; the Alarms history shows
   `FW 0 TRIAL PASS` within about 60 s; Panel → About then shows the new slot `VALID`.
   While the trial is still running (the first minute), System → Firmware → Install the master image again. **Pass:**
   "An OTA trial is running -- wait for it to pass or SET OTA CONFIRM", nothing written.
   Still inside the trial (D16): System → Fleet. **Pass:** the red "OTA trial in progress -- rebooting now retires this
   image" line shows. Tap Reboot master → Reboot. **Pass:** a second dialog "OTA trial in progress" appears. Tap Cancel.
   **Pass:** nothing restarts, and `FW 0 TRIAL PASS` still arrives. (If the trial passes before you get there, repeat
   step 3's install and reboot, and go straight to Fleet.)
4. **Fleet from the staged image.** System → Fleet → Update zone 2. **Pass:** the Alarms history ends with
   `FW 2 TRIAL PASS`; zone 2 is ONLINE with its config intact.
5. **Web regression (agent, from the PC on the AP):**
   ```powershell
   python C:\Projects\HillGrov\tools\web_test.py 192.168.7.7 --password hillgrow1 --only uploads --master-bin C:\Projects\HillGrov\master\build_p4\hillgrow_master.bin --zone-bin C:\Projects\HillGrov\zone\build\hillgrow_zone.bin --fleet 2
   ```
   **Pass:** every check PASS, including a zone image posted to `/api/fw/master` → `422 IMAGE_MISMATCH` with nothing
   erased.
6. **Config export and import.** Config → zone 2 → a shelf tab (the 4-way index selector showing). **Pass:** Export to
   card and Import from card sit left of Save and neither runs under it (Task 33). Export to card. **Pass:** "Exported to /hillgrow/zone2.json".
   Then Import from card → Import. **Pass:** "Queued, pushing to zone" (with "(hw.... readonly ...)" warnings appended).
   Config → Master → Export to card; on the PC open `/hillgrow/master.json`. **Pass:** it has `WIFI`, `TIME` and `SYS`
   but no `STA_PASS` and no `AP_PASS`. Import it back. **Pass:** "Saved.", and the phone stays connected (the AP password
   was kept).
7. **Card failures.**
   - Insert the exFAT card → Read card. **Pass:** "Card is not FAT32 -- exFAT cards (64 GB and up) must be reformatted to
     FAT32", and the card still has its files on the PC afterwards (never formatted).
   - Reinsert the FAT32 card, start Install on `hillgrow_zone.bin`, and pull the card out while the bar moves.
     **Pass:** "microSD read failed"; console `GET FW ZONE` reports no image. Reinsert and install it again (this
     restores the staged zone image). The phone kept working throughout.

Record in the task report: the install times for both images, internal RAM free/min, LVGL peak and the four "Stack
free" figures (Panel → About) after step 7 (that is, after a microSD install and a config import), and whether the UI
paused visibly during flash erases. **Fail criteria:** any step without its Pass, a reboot other than step 3's, a lost AP
or phone connection during any card operation, internal minimum below 64 KB, any "Stack free" figure below 1024 bytes.

---

## Stage 5 — Owner bench acceptance

### Task 34: Owner bench acceptance and docs (Stage 5; the shape of SP4 Task 17)

**Files:**
- Modify: `docs/what_we_learned.md` (a new section "Master v2 panel UI" at the end)
- Modify: `docs/superpowers/specs/2026-09-18-master-v2-panel-ui-design.md` (the `**Status:**` line, line 3)
- Modify: `docs/superpowers/plans/2026-09-21-master-v2-followups.md` (close item 6; update the open decision and the
  housekeeping; add the follow-ups this plan left open)
- Stage each by explicit path. **Never** `docs/hillgrow-features.drawio`, never `git add docs/`.

**Interfaces:**
- Consumes: every task.
- Produces: the signed-off checklist in the task report, and the three doc edits.

**What proves what:** this task is the proof. Host tests cannot reach touch, rendering, the SD controller or the radio;
the checklist below is the only evidence for those, and it must be done by the owner's finger on real glass (spec
"Testing" 3: the 2026-09-18 bring-up had touch mapped 180° out while every log line reported success).

- [ ] **Step 1: Fresh flash of HEAD (agent)**

1. GATE-HOST (`out of 53`), GATE-ESP32, GATE-P4 on HEAD, all green.
2. FLASH-P4, dry run first:
   ```powershell
   python C:\Projects\HillGrov\tools\flash_app.py --app master --target esp32p4 --build-dir C:\Projects\HillGrov\master\build_p4 --port COM28 --dry-run
   python C:\Projects\HillGrov\tools\flash_app.py --app master --target esp32p4 --build-dir C:\Projects\HillGrov\master\build_p4 --port COM28
   ```
3. Stage the zone image into `zone_fw` from the GATE-ESP32 zone build, dry run first:
   ```powershell
   python C:\Projects\HillGrov\tools\flash_app.py --app zonefw --target esp32p4 --port COM28 --dry-run
   python C:\Projects\HillGrov\tools\flash_app.py --app zonefw --target esp32p4 --port COM28
   ```
   The zones stay as they are. Record `(Get-Item C:\Projects\HillGrov\master\build_p4\hillgrow_master.bin).Length`
   against the 4,194,304-byte slot.

- [ ] **Step 2: Tap every destination once on real glass (owner)**

Each item must visibly respond under the finger (a pressed state, then the right screen). Tick each:
- Home: the clock, one band tile (opens that zone), the alarm band (opens Alarms; hold a zone in reset to get one),
  HillGrow (Dashboard), Audio (the SP7 placeholder text), Panel.
- Rail: Home (the rail's own Home button, from any rail screen); Dashboard; Zone for zone 1 and zone 2; Config — Master
  and both zones, every tab (ZONECFG, SHELF, LIGHT, WATER, FAN, VIB, AUX, HW, HWSHELF, CAL for a zone; WIFI, TIME, SYS
  for the master); Alarms; System — Wi-Fi, Time, Password, Fleet, Firmware.
- Panel: About and Touch test (the five targets light under the finger, never the opposite one).

- [ ] **Step 3: Re-run the stage gates on HEAD (owner with agent)**

The earlier gates were written for the image of their own stage. On HEAD some of their steps describe screens that later
tasks replaced, so run exactly this list, in this order, on the one flash of Step 1 (skip every gate's own "flash" or
"before you start" re-flash: Step 1 already did it). A step that fails stops the acceptance.

On HEAD the diagnostics screen (the five touch targets, "Blocking job (3 s)", the heap and LVGL pool lines) is reached
through Home → Panel → About → Touch test, and the heap and LVGL figures the gates record are also on Panel → About.

- **Stage 0 gate (after Task 8):** steps 3 to 7 only. Step 1 (the pre-flash MCFG baseline) needs the pre-Stage-0 image
  and step 2 is the flash, so both are skipped. In step 3 the pass is "the panel lights on **Home**" (not the
  diagnostics screen; Task 13 changed the boot screen), with the same log lines. Steps 4 and 5 run on the diagnostics
  screen reached as above.
- **Stage 1 gate (after Task 16):** steps 1 to 6, with these HEAD expectations in step 5: Config → the real config
  editor (Task 20), System → the real System screen with its five sections (Task 23), and Panel → the Panel screen
  (Task 26), whose About → Touch test is where the five-target check passes again. In step 6 read the heap and LVGL
  figures from Panel → About.
- **Stage 2 gate (after Task 21):** in full. "The Diag screen" in its setup and step 10 is Panel → About (or the
  diagnostics screen reached as above).
- **Stage 3 gate (after Task 27):** in full.
- **Stage 4 gate (after Task 33):** in full. Its "before you start" item 1 is already satisfied by Step 1; item 2 (the
  two cards) still applies.

- [ ] **Step 4: The parity checklist (owner; map-parity §G)**

Each line is a Pass or a Fail in the report:
1. Dashboard badges follow a zone reset (hold zone 2 in reset: DEGRADED, then OFFLINE; release: ONLINE).
2. Zone 2 console `GET ID`; forwarded `GET WATER 1`.
3. Replace board: a bad MAC → "Enter a MAC like aa:bb:cc:dd:ee:ff"; zone 2's own MAC → the reply verbatim.
4. Config round trip: WATER TARGET 55 on zone 2 → console `GET ZONE 2 WATER 1` shows `Target : 55` → restore 61; a
   shelf LIGHT OFF equal to LIGHT ON → VALIDATION with the OFF field highlighted.
5. HW, HWSHELF and CAL are read-only.
6. Save while zone 2 is OFFLINE → "Zone is offline -- nothing was saved", not "busy".
7. Master config: change NTP → "Saved."; a blank STA_PASS keeps the stored password (STA stays joined).
8. Alarm blame line and ages match the web's Alarms page.
9. Wi-Fi scan, pick, join.
10. AP change shows the drop warning.
11. TZ change moves the clock.
12. Web password set from the panel without the old one → the phone is logged out → logs in with the new one; restore.
13. Master `.bin` from SD → Reboot now → `FW 0 TRIAL PASS`. While that trial still runs (the first minute after the
    reboot): System → Fleet shows the red "OTA trial in progress -- rebooting now retires this image" line; Reboot master
    → Reboot opens a second dialog "OTA trial in progress"; Cancel → nothing restarts and `FW 0 TRIAL PASS` still
    arrives (D16).
14. A zone `.bin` on the web's master endpoint → `422 IMAGE_MISMATCH`, nothing erased (web_test uploads suite).
15. Zone `.bin` from SD → fleet update of zone 2 → `FW 2 TRIAL PASS`.
16. Fleet Abort → "Fleet update aborted.".
17. Reboot → "Rebooting..." → the panel comes back.
18. Concurrency: a panel save of zone 2, then a web PUT of zone 2 at once → the web gets `409 BUSY`; a panel SD install
    in progress → a web upload gets `409 UPLOAD_ACTIVE` and a console `SET FW ZONE 2` gets `ERR FW_BUSY`.

- [ ] **Step 5: Soak (agent, owner watching the panel)**

With the panel on Home and Night dimming active (Fixed hours covering now, idle 30 s), run:
```powershell
python C:\Projects\HillGrov\tools\web_test.py 192.168.7.7 --password hillgrow1 --soak 1800
```
**Pass:** the soak passes; heap before and after within 8 KB (the soak's own report); Panel → About afterwards shows
internal RAM minimum ≥ 64 KB and LVGL peak < 75 % of the internal pool; the clock never stopped; no `W_PANEL_FROZEN` line in the Alarms history and no active `ALARM 0` (Alarms → Active, and the web's `#/alarms`).

- [ ] **Step 6: Budgets (agent)**

Record in the report: the image size against 4 MB (step 1), internal RAM free/min and LVGL used/peak (Panel → About),
and the panel's boot-to-lit time from the boot log line `panel: up in <N> ms`. If the owner can fit a jumper, also
boot-to-lit with the C6 held in reset by its EN pin (not GPIO54; recovery design Tier 2). Skipping the jumper test is
allowed; say so in the report.

- [ ] **Step 7: Write the docs**

(a) Append to `docs/what_we_learned.md`:
```markdown
## 2026-MM-DD — Master v2 panel UI: what the bench taught

The panel face (components/panel_ui) and the shared service layer (components/panel_svc) went through five bench gates
and an owner acceptance. What held, and what to carry forward:

- **Reads leave the LVGL task, not only writes.** `wifi_mgr_status()` is two esp_hosted RPCs on the P4 (up to 5 s each)
  and `nmgr_lock` waits forever, so the panel reads only snapshot copies published by `pnl_poll` (1 Hz) and `pnl_wifi`
  (5 s). The clock kept ticking through a 30 s Wi-Fi scan and through flash erases.
- **One door for both faces.** Master config, zone config, net ops, fleet, the state gather and the firmware install are
  all `panel_svc` functions now; the web handlers are thin HTTP mappers over them. A refusal means the same thing on the
  glass and on the phone because both print the same `psvc_rc_t` token.
- **The display lock is never ignored and never waited on forever.** `panel_lock()` promotes 0 to 1 ms and every caller
  checks the result; nothing blocking is called under it.
- **microSD shares the SDMMC controller with the C6.** Mount with a no-op `host.init`, keep `deinit_p =
  sdmmc_host_deinit_slot`, create and delete the LDO-4 handle per mount, mount transiently on the worker. FAT32 only.
- **Wake-only first touch.** A transparent catcher on the top layer swallows the first press on a dimmed panel; the
  adapter's `auto_sleep` stays off because it pauses the LVGL worker and freezes the clock.
- **Measured at acceptance:** image <size> B of 4,194,304; internal RAM free <free> KB / min <min> KB; LVGL used
  <used> KB / peak <peak> %; boot to lit <ms> ms<, and <ms> ms with the C6 held in reset | ; the C6-in-reset test was
  not run>.
- **Surprises on the bench:** <one bullet per surprise recorded in the Stage 0-5 task reports, or "none">.
```
Replace `2026-MM-DD` with the acceptance date and each `<...>` with the value recorded in steps 1, 5 and 6 or the gate
reports; delete the alternative that does not apply in the boot-to-lit bullet.

(b) In `docs/superpowers/specs/2026-09-18-master-v2-panel-ui-design.md`, replace line 3 (the `**Status:**` line) with:
```markdown
**Status:** implemented; bench-verified <acceptance date> (plan `docs/superpowers/plans/2026-09-23-master-v2-panel-ui.md`, owner acceptance Task 34).
```

(c) In `docs/superpowers/plans/2026-09-21-master-v2-followups.md`:
- Item 6 of "Open code and test items" becomes:
  `6. ~~components/http_srv/http_api_cfg.c -- cfg_put_zone0() hand-rolls the master-config read-modify-write~~ **Closed**
  by the panel UI plan (Tasks 3-4): cfg_put_zone0 is psvc_mcfg_edit(psvc_mcfg_json_fn, ..., PSVC_LOCK_WEB_MS); the lock,
  the per-op log and the error mapping are one copy.`
- In item 2, `running_slot_on_ota_trial()` becomes `ota_trial_running_on_trial()` (components/ota_trial; panel UI plan
  Task 28).
- Under "Open decision for the owner", append: `Status 2026-09-28: the owner has not decided ("no idea"). The panel UI
  plan took its default (D5): httpd and the LVGL task stay unsubscribed; the LVGL task beats a 1 s heartbeat and the
  poller raises the active alarm NOTIFY ALARM 0 W_PANEL_FROZEN after 10 s without one. Evidence from acceptance: <W_PANEL_FROZEN seen: yes/no;
  any hang of the web UI seen: yes/no>. Still open.`
- Under "Owner housekeeping": change the spike bullet to `- ~~Delete C:\Projects\hillgrow-p4-spike.~~ Done by the owner,
  2026-09-28.`; keep the drawio bullet and add to it: `What "stamp" means: in draw.io, open
  docs/hillgrow-features.drawio, go to the "Master v2 (P4) Topology" page, and add to its title the same kind of stamp the
  SP3 page carries ("— SP3 (bench-verified 2026-09-04, 3-board ring)"), e.g. "— Master v2 (bench-verified 2026-09-22, P4
  master + 2 zones)" (system spec 11.10 records 2026-09-22 as the hardware verification); save, and commit it with your
  own drawio edits. The agents never touch that file.`
- Append a new section:
  ```markdown
  ## Left open by the panel UI plan (2026-09-23 plan, decisions D1-D28)

  - **D7 per-target lock.** `idf_build_set_property(DEPENDENCIES_LOCK ...dependencies.lock.esp32p4)` in
    `master/CMakeLists.txt` would record the P4 resolution and retire the `git checkout master/dependencies.lock`
    reflex. Needs the owner's OK.
  - **D9 MCFG_F_AP_DEFAULT on zone-0 saves.** An AP_SSID/AP_PASS change through Config (web or panel) leaves the
    factory-password banner up; System > Set AP clears it. One line plus a test once approved.
  - **D10 BUSY vs STORAGE.** The CLI rows and POST /api/wifi still report master-config lock contention as
    ERR STORAGE / 503; only the panel's psvc_* functions say BUSY.
  - **D27 the web's "Zone busy, retry" for ZONE_NOT_ONLINE.** The panel splits the two; the web does not yet.
  - **D28 the web dashboard's time is UTC, unlabelled.** The panel shows local time with a (UTC+hh:mm) suffix.
  - **D20 recovery-plan interplay.** The recovery plan's esp_hosted deinit/reconnect must refuse or defer while
    `pnl_sd_mounted()` is 1. Its references to `http_upload.c` / `http_upload_master.c` are now
    `components/panel_svc/psvc_fw.c` and `fw_sink_master.c`; it consumes `ota_trial_running_on_trial()` and
    `hg_image` rather than creating them.
  - **D11 SHELF.PROFILE** is an editable 0..16 stepper until SP4b profiles exist.
  - **D22 LOW_HEAP guard on the P4.** The shared install core's guard reads esp_get_free_heap_size(), which includes
    PSRAM on the P4, so it never trips (web or panel). heap_caps_get_free_size(MALLOC_CAP_INTERNAL) would make it
    real; that is a deliberate web change and needs the owner's OK.
  ```

- [ ] **Step 8: Commit the docs**

```powershell
git -C C:\Projects\HillGrov status --short
git -C C:\Projects\HillGrov add docs/what_we_learned.md docs/superpowers/specs/2026-09-18-master-v2-panel-ui-design.md docs/superpowers/plans/2026-09-21-master-v2-followups.md
git -C C:\Projects\HillGrov commit -m "docs: Master v2 panel UI verified on the bench" -m "Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>"
git -C C:\Projects\HillGrov push
```
Before the `add`, `status --short` must still show ` M docs/hillgrow-features.drawio`, and it must still show it after
the commit (unstaged, untouched).

**Stage 5 gate:** Steps 2-6 all Pass, signed off by the owner in the task report.
