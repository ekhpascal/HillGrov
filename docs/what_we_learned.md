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

