# What we learned

> Rule: every time an unexpected hardware or firmware issue is found and fixed, add an entry here — **Symptom / Root cause / Fix (file:line + code) / Rule** — so the same hole is never dug twice.

Entries inherited from sibling projects (verified during design, 2026-08):

## 2026-08 — Raw-struct NVS blobs corrupt silently on layout change (HillBT)

**Symptom:** after a firmware update, stored settings loaded as garbage without any error.
**Root cause:** NVS blobs were raw structs checked only by `sizeof`; a same-size layout change reinterpreted old bytes.
**Fix:** every HillGrow blob carries a 16-byte `{magic, version, length, generation, crc32}` envelope; newer-version blobs are rejected to defaults and left in flash (spec §4.2).
**Rule:** never persist a bare struct; version + CRC everything.

## 2026-08 — `uart_set_pin()` does not enable RX pull-ups on ESP32 (IDF 6.0.1)

**Symptom (predicted):** an unpowered upstream ring node floods the receiver with BREAK events.
**Root cause:** the driver's RX pull-up code is inside the LP-UART branch, which the ESP32 classic does not have (`esp_driver_uart/src/uart.c:267`).
**Fix:** explicit `gpio_set_pull_mode(GPIO18, GPIO_PULLUP_ONLY)` plus an external 10 kΩ (spec §2.1).
**Rule:** verify driver side-effects in the installed IDF source, not in folklore.

## 2026-08 — PCF8575 powers up all-HIGH and has no reset pin

**Symptom (predicted):** pumps energise at power-up or keep running through an ESP32 reset.
**Root cause:** quasi-bidirectional outputs default high; the chip keeps its last word across an MCU-only reset.
**Fix:** all loads active-low; first boot I²C transaction writes `0xFFFF` and reads it back; 1 s read-back audit (spec §3.1/§3.3).
**Rule:** every expander-driven load must be safe with the expander's power-on state.

## 2026-09-02 — SP1 execution notes (subagent-driven; whole-branch review shipped)

Three genuine plan-code defects were caught only by adversarial review — record the rules:

**Bootloader RWDT vs long button holds.** The 2nd-stage bootloader arms the RTC watchdog at `CONFIG_BOOTLOADER_WDT_TIME_MS` (default 9000) and never feeds it; any boot-path wait ≥ 9 s (our 10 s rescue hold) resets the chip in a loop. Fix: `CONFIG_BOOTLOADER_WDT_TIME_MS=30000` in every app's sdkconfig.defaults. **Rule:** any bootloader-time wait must be budgeted against the boot WDT — and a defaults change needs the generated sdkconfig deleted to take effect.

**Blank otadata + factory partition boots factory forever.** `ota_data_initial.bin` is all-0xFF; with a factory partition present the bootloader then selects FACTORY and nothing ever repairs otadata — once a real rescue image occupies factory, every fresh flash boots rescue permanently. Fix: `tools/hg_otadata.py` generates a valid otadata (seq=1 → ota_0, state UNDEFINED, CRC per IDF otatool) and both flash tools write it. **Rule:** never flash blank otadata on a layout that has both factory and OTA slots; `idf.py flash` is unsanctioned here (it puts the app at 0x30000/factory) — use `tools/flash_all.py` / `flash_app.py` only.

**RTC retain `custom[]` is CRC-excluded and word-cast-unsafe.** The rescue flag must be memcpy'd (LE) both sides, checked by value (never gated on `is_retain_mem_valid()`), and zeroed after consumption or the node boots to rescue forever. First-power-on garbage is neutralized because `bootloader_common_update_rtc_retain_mem(NULL, true)` zeroes the whole struct on invalid CRC — call it before reading.

### Hardware bench session — COMPLETED 2026-09-02 (all items below verified; original checklist retained for reference)

Results: zone + master flashed via tools/flash_all.py, both boot their app from ota_0 (generated otadata verified live); uart_test.py 49/49 (zone, incl. reboot-persist) and 20/20 (master); GPIO15 3.6 s → NVS erase (saved Target 61 reverted to default 45) and 10 s → rescue with no WDT reset (`hg_boot: button held 10000 ms -> factory`); rescue manual AP + DHCP + upload page → upload boots ota_1 VALID; `SET FW UPDATE` → `hg_boot: rescue flag set -> factory` → pull 200/230848 B → boots ota_0 VALID (slot ping-pong proven both directions); pull-fails→AP fallback transition exercised (3 STA failures → AP up); master `SET ZONE 2 …` → `ERR ZONE_UNKNOWN`; TWDT probe: hung cli0 named + panic at 8.3 s + reboot. **SP1 hardware verified.**
- Flash with `tools/flash_all.py --board zone --port COMx` ONLY (never `idf.py flash`). Verify normal boot → zone app from ota_0 (not rescue).
- GPIO15 hold 1–9 s → `erase nvs` log, defaults on boot; hold ≥ 10 s → factory/rescue boot **without a watchdog reset** (proves the 30 s WDT fix).
- Rescue: manual AP `HillGrow-Rescue-xxxxxx` / `hillgrow1` / 192.168.7.7 upload page → upload zone.bin → boots from OTA slot, `GET FW` shows VALID after auto-mark.
- **Specifically exercise the pull-fails→AP transition** (wrong URL in `SET FW UPDATE`, 3 attempts, then AP) — this path was reworked in review and has never run on silicon.
- Bench pull mode per Task 17 brief; `uart_test.py` green on both roles (`--allow-reboot`); TWDT probe (temporary `while(1);` in cli0 → panic ≤ 8 s); tick pin-mapping bring-up boxes; only then claim "SP1 hardware verified".

## 2026-09-02 — ESP32 classic cannot see a 5 GHz AP (bench pull-mode failure)

**Symptom:** rescue STA “connect failed/timed out” ×3 within seconds against a live Windows Mobile Hotspot with correct credentials; fell back to manual AP.
**Root cause:** the hotspot band was “Auto” → 5 GHz on this Wi-Fi 6E adapter; ESP32 classic is 2.4 GHz-only.
**Fix:** force the hotspot to 2.4 GHz (TetheringWiFiBand.TwoPointFourGigahertz); pull then succeeded first try.
**Rule:** every AP a node must reach (master AP in SP4, any bench hotspot) must be pinned to 2.4 GHz — and a fast STA failure (≪ the 20 s timeout) usually means “AP not visible”, not “wrong password”.

## 2026-09-02 — CLI-entered Wi-Fi credentials cannot contain spaces

**Symptom:** default Windows hotspot SSID (“LAP-… 1233”) is un-enterable via `SET FW UPDATE <ssid> <pass> <url>` — the tokenizer splits on whitespace.
**Root cause:** the CLI grammar has no quoting; by design (spec §5).
**Rule:** SSIDs/passwords with spaces are only usable through the SP4 web UI (JSON body); document the limitation in the web UI help, don’t add CLI quoting.

## 2026-09-02 — Windows bench quirks worth remembering

Windows drops no-internet Wi-Fi APs after a few seconds when Ethernet is up — reconnect immediately before each HTTP interaction (or expect one mid-test drop). COM ports open exclusively (CreateFile dwShareMode=0) — one owner at a time; close before reopen (this killed the first uart_test PERSIST run). pyserial resets a DevKitC on open unless RTS/DTR are deasserted before `open()`.

### SP2 entry checklist (from the final whole-branch review)
1. **Design decision first:** actuator-override grammar (`SET LIGHT <s> <w> <r> <minutes>`) collides with the 3-arg config rows on the same nouns — the static per-position arg typing cannot express both; decide (variable-arity rows vs renamed nouns) before writing SP2 rows.
2. Consolidate the duplicated app_if glue (zone/master `log_set`/`time_*`/`fw_*` are byte-identical) before adding callbacks.
3. Harden cmd_task before a second caller (httpd): abandoned-slot resp/ses lifetime — HTTP handlers must use static/per-session response buffers until then.
4. Wire `F_NVS` (hg_store 3-strike log placeholder) to the real fault store.
5. Add offsetof cross-check asserts to the hg_cfg field table before growing it.
6. Real §3.10 OTA trial criteria replace the SP1 `esp_ota_mark_app_valid_cancel_rollback()` boot placeholder.
7. Smaller ledgered items: hw-gen regression on MIGRATED rewrite (live once SP3 sync reads gens); hg_store_set_zid sync-write (SP3); MIN_OK/MAX_OK have no SET row (cal_dump_line fallback non-replayable if one is added); reboot_counter advances 2/boot.

## 2026-09-04 — SP3 ring: what the 3-board bench found that 23 green host suites did not

Master COM17 + two zones (COM24/COM25) wired 19→18. Every one of these passed host tests and two adversarial reviews; only the ring exposed them.

**Fixed-size codecs that return `0 = OK` next to variable-size codecs that return a length.** `hg_assign_pack`/`hg_ts_pack` return 0 on success; the master passed that as the wire length, so every ASSIGN_ID and TIME_SYNC was an empty frame — and the ring still *looked* healthy because empty TIME_SYNCs circulate fine. **Rule:** one return convention per header (`ring_proto.h` now documents each function and carries `HG_TS_LEN`/`HG_ASSIGN_LEN`); callers test `< 0`, never `!= 0`; a frame type that "works" must be proven by a field only its payload can set (here `LinkFlags` bit 2 = heard_by_master, unreachable without a parsed TIME_SYNC).

**Unassigned nodes must identify their own frames by MAC only.** Two fresh zones both hold id 0xFE: with id-based own-src/own-dst rules, the first hop consumed every ASSIGN_ID and the last hop dropped every other fresh zone's heartbeats. Enrolment worked perfectly with *one* zone. Spec §2.5 amended. **Rule:** any test of an addressing scheme needs two nodes in the same "no address yet" state.

**A fix that makes a dead frame parseable can arm a loop that was dormant.** Once ASSIGN_ID parsed, "master re-asserts id on every heartbeat → zone answers with an immediate heartbeat" ran at ring round-trip speed (~23 frames/s per zone vs ~1.5 nominal). The spec's 1/500 ms immediate-HB floor (§2.7, line 161) had never been implemented, and the storm was read as health because `Fwd` only ever went up. **Rule:** after any protocol fix, measure the *rate* of a counter over 10 s against the nominal you can compute from the cadences; and every "react immediately" path needs its rate limit implemented on the day the reaction is written, not when the trigger starts firing.

**Master-wins reconciliation reverts the operator's own forwarded edits.** `SET ZONE 2 …` executed on the zone as a local edit (gen+1), the master's cache stayed authoritative, §4.4 reverted it four seconds later with a `CFG_REVERTED`. The design had no path for "this edit came *through* the master". Spec §4.4 amended: on the OK ACK of a forwarded SET the master invalidates its cache for that zone and re-adopts. **Rule:** when one authority owns the truth, every write path that bypasses it must explicitly hand the truth back.

**Id order is not hop order.** Two fresh zones powered up together enrolled in reverse physical order (first hop became Z2), and the blame line then named the suspect wire by id ("wire Z1->Z2" for a leg that is physically M→Z2). The master already measures each zone's hop count from the TIME_SYNC TTL; the blame text must be derived from that. **Rule:** anything the operator is told about physical topology (which cable, which neighbour) is computed from measured hops, never from assigned ids.

**Bench results, 2026-09-04 (3 boards, bare jumpers):** enrolment of two fresh zones + id persistence across zone and master resets; forwarding (`ZONE_UNKNOWN`, ring-session `DEBUG … ENABLE`, unlock-gated rows, 125 B line-boundary truncation); §4.4 revert of a zone-console edit in 4 s while a master-forwarded SET sticks; `SET TIME` reaching both zones in < 5 s with `RING` as source; fleet OTA `SET FW ZONE 2` → rescue → pull from the master AP (245 kB) → ota_1 → `TRIAL PASS`, 8.6 s command-to-DONE; health ladder DEGRADED 5 s / OFFLINE 10 s / recovery ≤ 3 s with a zone held in reset. Not yet done: a true single-wire pull (both ends alive) — needs hands at the bench.

**Bench technique that paid off:** `SET RING TRACE ON` on the master (per-frame src/dst/type/ttl rows) plus `SET LOG DEBUG` on both zones, captured concurrently with three pyserial threads; diagnose from *both* ends of the ring (COM24 proved the consume-by-id bug and the len-0 bug; COM25 proved the drop-self bug by the *absence* of type-0x12 rows). Never `python -c` with quotes on Windows for pyserial helpers — write .py files.
