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

**Bench results, 2026-09-04 (3 boards, bare jumpers):** enrolment of two fresh zones + id persistence across zone and master resets; forwarding (`ZONE_UNKNOWN`, ring-session `DEBUG … ENABLE`, unlock-gated rows, 125 B line-boundary truncation); §4.4 revert of a zone-console edit in 4 s while a master-forwarded SET sticks; `SET TIME` reaching both zones in < 5 s with `RING` as source; fleet OTA `SET FW ZONE 2` → rescue → pull from the master AP (245 kB) → ota_1 → `TRIAL PASS`, 8.6 s command-to-DONE; health ladder DEGRADED 5 s / OFFLINE 10 s / recovery ≤ 3 s with a zone held in reset. Single-wire pulls with both ends alive (2026-09-09, after the blame fix chain a49f3d0 → 936a7e3): master TX leg pulled → `no node reports a fault` at 5 s, `wire M->Z2` at 9 s, `RING CLOSED` on reconnect; zone-to-zone leg pulled → Z2 DEGRADED → OFFLINE → `Z2 dead or wire Z2->Z1`, `RING CLOSED` on reconnect; last leg proven by holding Z1 in reset → `Z1 dead or wire Z1->M`. No wrong cable named, no recovery transient. **SP3 bench plan fully closed.**

**A dead node cannot validate a blame line; only a pulled wire can.** SP3 signed off break blame against a zone held in reset — the one break a single pair of hands can make — and when the owner finally pulled single wires on 2026-09-09 the line was wrong for all three legs: the M→Z2 pull printed `Z1 dead or wire Z1->M`, the Z2→Z1 pull printed `Z2 dead or wire M->Z2`, the Z1→M pull printed `Z1 dead or wire Z2->Z1`. Two causes, both invisible to a reset board. First, blame was computed **once, at the OK→OPEN edge**, which falls at 5 s — a second before the zones' 6 s link timers can even report “master silent”, so the branch that was supposed to find the starved node was always empty and the verdict fell through to weaker ones. Second, those branches named the leg **upstream** of the silent node; the physics runs the other way, since a cut starves everything *downstream* of it while taking everything *upstream* OFFLINE. A board in reset hides both: it is a symmetrical break (both of its legs die at once) whose evidence has settled by the time anyone looks. The rule is now the segment between the most-downstream OFFLINE node and the most-upstream *witness* that has stopped hearing the master, re-derived every tick, with a 3-tick dwell on changes (§2.7). Worth recording what was *not* the cause, because the first fix's own commit message said it was: `upstream_alive` (b0) was never read during the pulls at all. It was stamped from the zone's consume queue, making it a duplicate of `master_alive` and useless as a discriminator — a real defect, fixed separately by re-basing it on the link layer, but not this one. **Rule:** a diagnostic that names a physical thing is only tested by breaking that physical thing, one at a time, in each direction; an approximation that breaks two things at once (a board in reset, a powered-down node) proves the alarm fires, never that it points the right way. And when a fix lands, the explanation has to be traced through the code that actually ran, not the code that looks guilty.

**Installer note (spec-intended, bench-proven):** once a zone is enrolled, a config edit typed at the zone's own console is reverted by the master within ~2–4 s (`NOTIFY … CFG_REVERTED`). Bring-up edits go through the master (`SET ZONE <z> …`) or are made before the ring is connected.

### SP4 entry checklist (from the SP3 final whole-branch review)
1. **Web-apply primitives:** `node_mgr_cfg_set(zone, cfg)` as a request-flag write (gen bump + source stamp inside the node_mgr tick) and `node_mgr_cfg_get` / `node_mgr_hw_get` snapshot readers under the node_mgr lock — the web UI reads node state from the master's caches, never via forwarded `GET`s (one forward slot ⇒ `BUSY` under concurrent CLI use).
2. **Master self-update path:** verify the master's `ota_trial` tick source (added in the SP3 fix wave) and the rescue breadcrumb end to end before any `SET FW MASTER`.
3. **Vocabulary gaps:** `SET NODE <z> MAC <mac>` (pre-seed a ztab row — the only way to hand a replacement board an old id) and `GET PING` were deferred out of SP3.
4. **Mixed-version fleets (zones first, master last):** treat `E_VERSION_*` on a config pull as terminal with a per-zone latch keyed on (hb_gen, hb_crc); HB/TIME_SYNC parsers already tolerate trailing bytes.
5. **Fleet sequencer:** `uptime_low` alone qualifies as DONE — parse the served image's `esp_app_desc` version so real upgrades require `fw_changed`, and surface "fw regressed after DONE".
6. **Store discipline:** `node_store_save` (NVS write) still runs on the node_mgr/cmd task; route through the store task when the master gains `mcfg`/profiles.
7. **Test debt:** a fake-link harness for `node_mgr_cfg`'s decision table (feed HBs + ACK events, assert push/pull/latch) — both bench-found glue defects lived exactly in this unharnessed seam.
8. Relayed NOTIFYs are emitted as the originating node (fix wave) — the web parser assumes one id per line. `httpd` `max_open_sockets` default 7 vs the §6.4 budget of 4; `SET WIFI AP` should be able to change the repo-published AP credentials before the HTTP API lands.

**Bench technique that paid off:** `SET RING TRACE ON` on the master (per-frame src/dst/type/ttl rows) plus `SET LOG DEBUG` on both zones, captured concurrently with three pyserial threads; diagnose from *both* ends of the ring (COM24 proved the consume-by-id bug and the len-0 bug; COM25 proved the drop-self bug by the *absence* of type-0x12 rows). Never `python -c` with quotes on Windows for pyserial helpers — write .py files.

## 2026-09-14 — SP4 web UI: what the reviews and the bench found that green suites did not

Master on the SP3 rig + the PC on the master's AP; every task reviewed, most with one or two fix rounds. The rules, in the order the bugs appeared:

**IDF 6 always advertises 802.11w on the softAP.** `pmf_cfg.capable/required` are ignored; a Windows 11 / Intel client disassociated every ~5 s after an unanswered SA Query, so the web UI was unreachable from the only client the product has. `esp_wifi_disable_pmf_config(WIFI_IF_AP)` before `esp_wifi_start()`. **Rule:** bring a real phone/PC client to the bench before declaring an AP usable.

**SNTP setup before `esp_netif_init()` crash-loops** (`tcpip_callback` asserts "Invalid mbox"). **Rule:** any lwIP-app init (SNTP, mDNS) goes after the netif/Wi-Fi start; comment the ordering at the call site.

**IDF resolves every discovered component's requirements, reachable or not.** A master-only component with `PRIV_REQUIRES espressif__cjson` broke the zone build until the zone declared the registry dependency too (nothing links into zone.bin — gc-sections drops it). **Rule:** a registry dependency added to any shared-directory component needs an `idf_component.yml` in every app.

**A stalled send loop can PANIC the master.** `fw_srv`'s `send_all` fed the task watchdog only per 4 KB chunk; partial 5 s sends exceeded the 8 s TWDT mid-fleet-update. Feed on progress (gated on `esp_task_wdt_status`) and bound the whole transfer (120 s). **Rule:** every network write loop needs both a per-progress feed and a total deadline.

**httpd purges an unread body when the handler returns `ESP_OK`.** A `Content-Length` lie plus a one-byte trickle pinned the single httpd task indefinitely (and stalled `/fw/zone.bin` mid-fleet). Answer, then return `ESP_FAIL`; drain only bounded bodies, with a wall-clock budget. **Rule:** on esp_http_server the return code is a socket decision, not a status.

**A lockout minted on the frozen clock became permanent when NTP arrived.** Sessions used uptime until the clock was set; `lock_until` from one base compared against the other. **Rule:** whenever a deadline can be minted on two clock bases, drop deadlines further ahead than the maximum legitimate span.

**A timed-out command worker kept writing into a recycled buffer.** `cmd_task_execute`'s orphan path and a caller wait equal to the forward budget (3500 ms) → torn or cross-user replies. Distinct orphan return code, quarantine the slot, wait longer than the forward budget. **Rule:** a blocking call that can orphan its worker must tell the caller so, and the caller must not reuse the buffer.

**Schema text in `id`/`for` attributes is XSS.** A tampered `/api/schema` executed script in an operator's session. Field ids are index-derived; server strings never reach an attribute unescaped or un-whitelisted. **Rule:** escape into text nodes, whitelist into attributes, never interpolate server text into an id.

**Poll-driven re-renders wipe typed input.** Only the console had draft preservation; the login password and the MAC field lost keystrokes every 2 s. One drafts map keyed per input (and per zone) restores values on render; the poll stops on the login page. **Rule:** any timer-driven full re-render needs a value-preservation contract for every text input.

**Native form validation silently blocked the server-error path**, and `#app{display:contents}` had been hiding a broken desktop grid since the shell landed. **Rule:** drive every UI flow through to its real success (or its real 400) on the bench — "request dispatched" hides form bugs.

**No host seam for `http_srv.c`** let a missing `case 202` (queued saves answered 500) and a path sanitiser folding `[]` reach the bench. Carried as test debt: a fake-httpd seam for status lines and JSON error bodies.

**The 100 KB heap bar was set before httpd, mDNS, cJSON, SNTP and APSTA existed.** Measured 87 KB fresh, 75 KB after OTA + upload + fleet; bar moved to ≥ 64 KB with the 40 KB `LOW_HEAP` guard as the floor.

### Owner acceptance on a real phone — 2026-09-18

**A caret bug that only a human thumb could find.** The 2 s poll re-render restored focus but could not restore the caret in `<input type=number>`, so on a phone every digit landed at position 0 (`1` then `2` → `21`). Headless CDP typing was too fast to span a poll tick and never saw it; the desktop bench never saw it. Fixed by skipping the poll-driven rebuild while a form field holds focus. **Rule:** a timer-driven re-render must be tested at human typing speed, on the slowest real client, not just asserted green by a driver that types instantly.

**Every session tied at the same expiry evicted the wrong one.** Before the clock is set all sessions share `expires_s = 0xFFFFFFFF`, so "evict min expiry, tie → lowest index" always chose slot 0 and each new login evicted the session just created — phone and desktop knocked each other out, which is the product's core requirement. Fixed with a RAM-only creation sequence and true LRU. **Rule:** when a sort key can be identical across all candidates, the tie-break *is* the algorithm — design it deliberately.

**A successful password change is invisible, and the next attempt lies.** The handler deliberately mints a fresh cookie so the operator is not bounced to the login page; the only success signal is a small inline line. The owner missed it, retried with the old password, and got "Old password is wrong" — a correct message that reads like a bug. **Rule:** when an action silently changes the credential the user is about to reuse, make the success state unmistakable.

**The master's softAP keeps dropping the Windows client.** Three association drops in one session (4+ in 40 min during the tools task), each presenting as `curl 000` and looking exactly like an httpd hang; the Android phone never dropped, and the same PC held 55/55 pings at 0 % loss against the P4's C6 radio. **Rule:** before reading a timeout as a firmware fault, confirm `netsh wlan show interfaces` says `connected` — and prefer a second client type when characterising an AP.

**The fleet sequence finished faster than the poll that was watching it.** `/api/state` sampled every 10 s never once caught `fleet != IDLE`; the whole zone OTA completed in ~35 s and only the serial `NOTIFY` capture witnessed it. **Rule:** an automated check for a transient state must watch the event stream, not sample a level — or sample far faster than the shortest possible transit.

### What the final whole-branch review found — 2026-09-18

**A mock kinder than the server hid two real bugs from four green suites.** The UI mock returned secret fields that the real `hg_json_export_mcfg` omits at `?secrets=0`, and answered an unparsable `?zone=` instead of the 400 the real `query_int` returns. Against that mock, touching and clearing the Wi-Fi password looked harmless (it silently wiped the stored credential on real hardware) and a mistyped `#/config/2x` looked inert (it produced an unbounded request storm). Four suites were green throughout. **Rule:** a fixture that is more forgiving than the server is not a test, it is a second implementation — pin every mock to the server's actual refusals, not just its happy path.

**A wrong comment kept a defect alive across two sub-projects.** `fw_srv` returned `ESP_OK` with an unread body, justified in-line by "this is a GET with no request body, so there is nothing to purge". IDF sets `content_len` from the header for *any* method, so the premise was simply false — but it read as settled reasoning, so the item was carried to Task 13 twice and closed both times without landing. **Rule:** when a review item keeps getting carried, re-derive the comment that says it is fine; a confident wrong comment is more durable than a bug.

**The save path and the render path disagreed about the document shape.** The master config page used the *zone* document accessor to render and the correct flat-document accessor to save, so `#/config/0` displayed every field blank while saving correctly. No bench caught it because every operator used the System page's own inline forms and nobody ever opened that page. **Rule:** when two code paths read the same document, they must share the accessor; and a page no test and no human ever opens is not covered, however green the suite.

**Escaping expands — size buffers for the encoded form.** The login body buffer was sized for the raw password, but JSON escapes `"` and `\` to two bytes and control characters to six (`\u00XX`), and the password rule accepts any bytes at 8..63. A legal password could therefore produce a body larger than the cap and lock the operator out permanently, with no recovery path through the UI. **Rule:** bound the wire form, not the value, and derive the bound from the validator's own limit rather than a guessed constant.

**Absolute stamps rendered as durations.** `alarm_mgr` stores monotonic uptime readings as absolute stamps; the alarms page printed them straight as "Ns ago", so a 107-second-old ring fault displayed as "2634s ago". The owner read that page on the bench and neither of us noticed, because the *text* beside it was right. **Rule:** a unit mismatch in a field nobody validates survives every review that reads the prose instead of the arithmetic.

**A presence check keyed on a structurally constant value — and the test that let it ship.** Fixing the config-save rejection, the wave gated hardware validation on `hw_gen ? &hw : NULL`. But the HW plane carries no generation anywhere on the wire: the zone sends `gen = 0` and the master caches `gen = 0` by an explicit earlier ruling, so `hw_gen` is 0 for a *present* plane exactly as for a missing one. The expression was NULL on every save, which silently disabled the pump over-run and daily-water-budget limits — a narrow false reject traded for a permanent false accept on a flood guard. The existing test asserted the generation was 0 when the plane was **absent** and never asserted anything when it was **present**, so "always absent" passed the suite. **Rules:** derive presence from the thing's own validity, never from a value that is allowed to be zero; and a presence signal must be positively asserted in the *present* case, or a constant satisfies the test. Verified on hardware afterwards in both directions — an over-limit dose now returns 400 naming `shelf[0].water.dose_s`, and a valid save still returns 202.

## 2026-09-18 — Master v2 P4 panel bring-up: what the throwaway spike found

Bring-up spike (`C:\Projects\hillgrow-p4-spike\p4_screen`, deleted once this entry promoted its findings) proved the 7" touch panel on the Waveshare ESP32-P4-WIFI6-Touch-LCD-7B. The registry has a BSP for exactly this board — **`waveshare/esp32_p4_wifi6_touch_lcd_7b` (3.0.1)**, pulling in `esp_lcd_ek79007`, `esp_lcd_touch_gt911`, LVGL 9 and `esp_lv_adapter`, and exposing the standard ESP-BSP surface (`bsp_display_start()`, `bsp_display_lock/unlock`, `bsp_display_backlight_on()`, `bsp_display_get_input_dev()`). **Do not hand-roll an EK79007 init sequence** — someone will otherwise lose a day rediscovering that this BSP already exists. Panel came up 283 ms after boot, 1024×600, touch device present, ~30 MB free heap.

**GT911 touch arrives mirrored 180°, and that is not a broken touchscreen.** `bsp_display_start()` sets BOTH `.rotation = ESP_LV_ADAPTER_ROTATE_180` **and** `.touch_flags = { .mirror_x = 1, .mirror_y = 1 }` — two *independent* 180° flips. `esp_lv_adapter` never calls `lv_display_set_rotation`: for 180° the flip lives entirely in its flush path, so LVGL's logical space stays unrotated, and the adapter applies no rotation to touch points either (scale 1.0, raw passthrough in the single-point path) — LVGL doesn't transform indev coordinates. So `esp_lcd_touch`'s mirror is the *only* mapping from panel frame to LVGL frame, and on this board that mapping is already wrong before the mirror is applied (presumably why the BSP rotates the display in the first place), so the mirror flips an already-correct frame. **This already produced an owner-visible bug once, reported as "touch is not responding"** — the opposite corner activated the button, and it cost real bench time before the two independent flips were understood. **Fix:** call `bsp_display_start_with_config()` (public, as is `bsp_display_cfg_t`) with the BSP's own defaults except `touch_flags = {0,0,0}` — keep the rotation, drop the mirrors. **Rule:** rotation and touch-mirroring are two separate knobs on this BSP that must be reasoned about independently; a rotated display does not automatically need a matching touch mirror.

Two related hazards in the same touch path, worth knowing before this stack is touched again:
- **A failed touch read is invisible.** The adapter discards `esp_lcd_touch_read_data()`'s return code and keeps only `esp_lcd_touch_get_data()`'s, which returns `ESP_OK` with the point count untouched when nothing is pressed. An I2C NACK, a GT911 that never reports a point, and a genuinely untouched panel are therefore indistinguishable, and none of them log anything (`CONFIG_LV_USE_LOG` is off). Install a `custom_touch_read` via `esp_lv_adapter_set_touch_callbacks()` and log both return codes plus the count — that one hook separates "transport broken" from "no points reported" from "points fine, mapping wrong".
- **The mirror math can underflow.** `esp_lcd_touch.c` does `x = config.x_max - x` on a `uint16_t` with no clamp, and the BSP passes `x_max`/`y_max` = 1024/600. If the GT911's flashed config ever reports outside that range, a point wraps to ~65000 and lands far off-screen — which also presents as "panel perfect, button dead". Mirrors off (the fix above) avoids the subtraction entirely.

**`bsp_display_lock(0)` does NOT mean "wait forever" in `esp_lv_adapter`.** The older `esp_lvgl_port` read a `0` timeout that way; `esp_lv_adapter` treats it as "try, do not wait", and fails while its own LVGL task holds the mutex. **Ignoring the return value and calling `lv_*` anyway panics the board** (`esp_lv_adapter_lock(751): Failed to acquire LVGL lock`). **Rule:** always pass a real timeout to `bsp_display_lock()` and check the result before making any `lv_*` call.

**LVGL ships only Montserrat 14, which is unreadable on a 7" panel at arm's length.** Each size is a separate compiled-in bitmap font — there is no runtime scaling — so a larger size must be turned on explicitly, e.g. `CONFIG_LV_FONT_MONTSERRAT_28=y`. This bears directly on the clock-hero home screen the panel UI design is built around: `docs/superpowers/specs/2026-09-18-master-v2-panel-ui-design.md` (line 54, deferred at line 104) discusses the clock's font route but assumes a size bigger than 14px is available without saying where that comes from.

**Benign boot noise, worth naming so nobody chases it:** `W ledc: GPIO 32 is not usable, maybe conflict with others` prints on every boot. GPIO 32 is the backlight PWM; the backlight works regardless.

**Rule for the group:** a bring-up that looks solved — registry BSP found, log says success, panel lights up — still needs a human finger on the glass. Every trap above either logs nothing or logs something reassuring while being wrong; only touching the hardware found the 180° mirror.

## 2026-09-21 — P4 co-processor keeps serving a dead AP after the host crashes

**Symptom:** erasing the P4 host's own app partition (`esptool erase-region 0x10000 0x100000`) did not take the softAP off the air. Clients still associated and got a DHCP lease, then every request hung with no server behind it.

**Root cause:** the radio lives on the ESP32-C6 co-processor, not the P4 host. The C6 holds whatever AP config the host last pushed over RPC, and with the host gone nothing resets it, so it keeps serving an AP that has no server behind it. Only a C6 reset clears it — a board power-cycle, or any P4 app starting at all, since `esp_hosted` asserts the CP-reset line (GPIO54) during transport init.

**Product implication, narrowed by the master's own config** (checked 2026-09-22, and the spike's wording was more pessimistic than the code warrants): an outright *crash* largely self-heals. `master/sdkconfig.defaults` sets `CONFIG_ESP_TASK_WDT_PANIC=y` at 8 s and `CONFIG_ESP_INT_WDT=y` at 300 ms, and the resolved config has `CONFIG_ESP_SYSTEM_PANIC_PRINT_REBOOT=y` with `CONFIG_ESP_SYSTEM_PANIC_REBOOT_DELAY_SECONDS=0`. So a panic — including one raised by either watchdog — reboots immediately, and the reboot itself resets the co-processor because `esp_hosted` asserts GPIO54 during transport init. The dead-AP window for a crash is seconds.

What is **not** covered is a **hang in a task the task watchdog does not watch**. The TWDT only catches subscribed tasks, and the master's `app_main` task is deliberately not subscribed (established during the `cp_ota` work). A wedged `httpd` task, with every subscribed task still feeding the watchdog, would leave the AP associating and serving nothing *indefinitely* — and that, not a crash, is the realistic field scenario.

**Fix:** open decision for the Master v2 migration project, not a closed bug. The spike offered two directions (a host watchdog that resets the co-processor, or accepting the failure and making it legible). The narrowed analysis suggests a third and cheaper one: **subscribe the long-lived service tasks — `httpd` first — to the existing task watchdog**, so a wedge there takes the already-working panic-and-reboot path and turns "AP up, nothing behind it" into "AP disappeared and came back", which is the legible failure. That reuses machinery that already works rather than inventing a watchdog. Not implemented; the owner has not chosen yet.

**Rule:** on this board, "the AP disappeared" and "the AP is up but nothing is behind it" are different failure modes with different operator experiences — a live co-processor does not imply a live host, and the migration should not ship without a deliberate answer for a host crash while the C6 is still radio-alive.

## 2026-09-22 — Master v2 platform migration: what running the real master on hardware found

The real `master.bin` (not a spike) on the ESP32-P4, with two ESP32 zones, put through a platform transplant, a master self-OTA and two fleet reflashes, with the owner present.

**Changing a partition table relocates NVS — "the flashing tool never writes `nvs`" is not the whole safety story.** The spike used `PARTITION_TABLE_SINGLE_APP_LARGE` with its table at `0x8000`, putting `nvs` at `0x9000`–`0xF000`; the real master's table sits at `0xF000` with `nvs` at `0x10000`–`0x20000`. The master read `0x10000` — the spike's own *factory app* region — found no valid NVS there and correctly started empty. This is why a removed zone's `ztab` row vanished instead of persisting: nothing was overwritten, the old data was simply never at the address the new table looks for. **Rule:** a partition-table change can orphan NVS data without a byte being touched — when data goes missing after a table change, check the *offset*, not the flash contents.

**`GET RING`'s CRC/forward counters are the SUM of the ZONES' own counters, carried in their heartbeats — not something the master counts itself.** `GET NODE 1` read `RxCrcErr 0 / Fwd 719` and `GET NODE 2` read `RxCrcErr 35 / Fwd 37819` while `GET RING` read `35 / ~38500`. That is why the ring counters survive a master reboot (the zones remember) and why they drop to 0 when a *zone* reboots (its own counters, and so its contribution to the sum, restart) — a debugger who assumes `GET RING` is the master's own tally will misread both behaviours. Zone 2's 35 CRC errors were spread over 28 hours of uptime — about one per 48 minutes, negligible line noise, not a fault. **Rule:** `GET RING`'s numbers describe the fleet, not the master; read `GET NODE <n>` on each zone before concluding anything from a ring-level counter's behaviour.

**A backgrounded serial capture cannot open a COM port here.** `nohup python ... &` from the shell tool fails with `could not open port 'COM28': PermissionError(13, 'Access is denied')`, while the identical `open()` call succeeds in the foreground. Two captures were lost to this before the cause was found, and both times the error was invisible because stderr had been redirected to `/dev/null` to keep the log clean. This repo already solved the underlying problem once, in `tools/ring_outage.py`: one process holds both ports and does the triggering itself, rather than splitting "open the port" and "drive the test" across a background/foreground pair. **Rule:** one process holds the port and does the triggering, and never hide stderr while diagnosing a capture that isn't producing data.

**Zone ids survive a completely blank master — not because the master remembers them, but because each zone remembers its own id and re-claims it.** Proven from a master booted with genuinely empty NVS (see the partition-table lesson above): both zones still came back as Z1 and Z2. Each zone persists its own assigned id locally and re-announces it on enrolment, and `ztab.c` deliberately honours a free claim for an id that isn't already taken rather than only ever handing out the next unused one — the master's `ztab` is a cache of what the zones told it, not the source of truth for zone identity. **Rule:** when diagnosing "did the master forget the zones," check whether the *zones* still know who they are before concluding the master's memory is what matters.

**An instrument that saturates cannot be used to derive the quantity it saturated on.** The bootloader's factory-rescue branch fires at exactly `HG_HOLD_FACTORY_MS` (10000 ms) and jumps immediately — its log line (`button held 10000 ms -> factory`) reads identical whether the button was actually held 10 s or 40 s, because the code stops measuring the instant it acts. After one bench result came back later than expected, the controller inferred the operator was "counting about 20% slow" and issued two calibrated retries built on that inference — an inference the clipped log could never have supported, since it carries no information above the threshold. **Rule:** when a reading is clipped (a timeout that fires-and-jumps, a counter at full-scale), say so explicitly and change the measurement rather than tuning behaviour against a number that stopped moving.

**A one-boot fallback is not a rollback.** With the factory slot unbootable (empty — see spec §11.10), the bootloader fell through to `ota_0` while the active slot was `ota_1`, by the ordinary "factory not bootable, try the next slot" fallback — and it left `otadata` untouched. The next *normal* boot (no rescue hold) therefore returned to `ota_1`, not to whatever `ota_0` was serving. This was first reported as "the board silently rolled back to the previous slot," which is a materially different and scarier claim (a persistent version regression) than what actually happened (a one-time, otadata-blind fallback). **Rule:** before calling a boot-selection anomaly a "rollback," check whether `otadata` itself changed — if it didn't, the next normal boot proves it wasn't one.

