# SP3 — Ring link: design

**Status:** approved in brainstorm 2026-09-02.
**Authority:** the system spec (`2026-08-31-hillgrow-system-design.md`) §2 defines the protocol and is binding; this document adds the SP3 integration design and the decisions taken 2026-09-02. Where this doc and §2 disagree, §2 wins unless the point is listed under *Decisions*.

## Scope

Deliver spec §7 SP3: `ring_proto` + `ring_link` + `node_mgr`, CLI forwarding, enrolment, config push/pull, TIME_SYNC, fleet FW_UPDATE — acceptance on the bench with Master + 2 zones (three DevKitC-V4, dupont-wire ring). Plus the minimal slice of SP4 pulled forward to make fleet OTA real (Decision 1).

**Non-goals (explicitly deferred):** web UI/dashboard (SP4); persisted per-zone config caches and `zc1..zc8` NVS keys (SP4); the latched fault store and master fault codes as faults (SP2 builds `fault`; SP3 surfaces ring health only via NOTIFY + `GET RING`/`GET NODES`); `SET WIFI`/`SET TZ`/NTP (SP4/SP5 — AP creds fixed, TZ offset 0); RS-422 escalation; profiles.

## Decisions (2026-09-02)

1. **Fleet OTA ships inside SP3.** Master gets a minimal AP (fixed SSID `HillGrow`, WPA2 `hillgrow1`, 192.168.7.7, max 4 stations — same fixed-IP bring-up pattern as rescue) and `fw_srv`: an httpd with exactly one endpoint, `GET /fw/zone.bin`, streamed from the `zone_fw` partition (esp_partition_read in 4 KB chunks; 404 with `FW_NO_IMAGE` semantics when the partition holds no valid image header). Getting the image into `zone_fw` is a bench operation until SP4's upload page: `tools/flash_app.py --app zonefw --port COMx` writes `zone/build/hillgrow_zone.bin` at the master's `zone_fw` offset (0x570000). The served length is the app-image length parsed from the image header (not the partition size).
2. **Master persistence:** NVS `ztab` only (MAC ↔ id ↔ name[16] ↔ unconfigured flag, 8 entries, blob-envelope wrapped, written through a small `node_store` on the SP1 pattern). Per-zone config/HW caches are **RAM-only** in SP3 and self-heal after a master reboot via HB `cfg_crc`/`hw_crc` mismatch → CFG_GET.
3. **Heartbeats are spec-layout from day one:** every field packed per §2.4; values that SP2 produces (soil raw/pct, actuator states, fault masks, mode) are zeros until SP2 fills them. No SP3-only frame variants.
4. **OTA-trial breadcrumb:** rescue, after a *successful* pull, writes NVS `hg`/`trial` (u8 = the consumed handover's `expect_link`) before rebooting. An app that boots `PENDING_VERIFY` reads-and-erases it: `1` → §3.10 fleet criteria (Master frame ≤ 180 s required); absent/0 → bench criteria (60 s self-test or `SET OTA CONFIRM`). SP1's boot-time `esp_ota_mark_app_valid_cancel_rollback()` placeholder is REMOVED from both apps and replaced by the §3.10 trial.
5. **`CMD_SRC_RING`** is added to `cmd_session_t`'s source enum. The zone's ring session is a **full session** — its own `echo`/`notify_mask`/`unlock_until_ms` — so `CMDF_SESSION` rows execute normally against it: a forwarded `DEBUG ENABLE <key>` unlocks the *zone's ring session* (600 s expiry as usual), which is exactly how unlock-gated rows (`SET HW …`) become reachable over the ring per §5.8. `NOT_LOCAL` remains an HTTP-source gate only; the CLI `SET FW UPDATE` row's handler-level console-only check is unaffected (the ring fleet path uses FW_UPDATE frames, never that row).
6. **TIME_SYNC validity:** flags.b0 `time_valid` = 0 until the master's own time quality is at least COARSE (i.e. after `SET TIME` on the master console in SP3; NTP in SP4). Zones never `settimeofday()` from an invalid sync. `utc_offset_s` = 0 in SP3.
7. **Fault-store deferral:** W_RING_OPEN / W_ZONE_OFFLINE / F_ID_CONFLICT / W_CFG_SYNC_FAILED exist in SP3 as NOTIFY lines + states visible in `GET RING`/`GET NODES`; they enter the latched fault store when SP2's `fault` component lands.

## Components

| Component | Kind | Contents |
|---|---|---|
| `components/ring_proto` | pure (host-tested) | crc16 (own 512 B table, CCITT-FALSE); COBS enc/dec + resync; frame codec (9 B header per §2.3, byte-offset packed); routing decision (`ring_route()`: DROP_SELF/CONSUME/CONSUME_AND_FORWARD/FORWARD/DROP_TTL); dup cache; pending tracker (stop-and-wait, priority insert, timeout formula, UNCLAIMED fast-fail); HEARTBEAT/TIME_SYNC/ASSIGN_ID/FW_UPDATE pack+parse; config assembler (both directions); health/break analyzer (state table + blame from hop counts + upstream_alive). All time injected. |
| `components/ring_link` | target | UART2 install per §2.10, `ring_rx` task (event-driven, decoder, routing, forward-in-place, consume-by-copy into a 4-deep frame queue), `ring_send()` (encode + `uart_write_bytes`), counters. Shared by both apps via role config. |
| `components/node_store` | target | NVS `ztab` blob (envelope-wrapped) — load/save/find-by-mac/assign-lowest-free/clear. |
| `components/node_mgr` | master target | the §6.1 task (prio 4, 6144, TWDT): 50 ms tick = tracker + TIME_SYNC schedule (every 2000 ms); 1 s sub-tick = health/break analysis, enrolment decisions, config push/pull state machines, fleet-OTA sequencer. Owns the RAM caches. Exposes `node_mgr_forward(zone, line, resp, len, timeout)` for the CLI path and the `master_cmds` row handlers. |
| `components/master_cmds` | pure rows + handlers | `GET RING/NODES/NODE/UNASSIGNED`, `SET NODE <z> NAME`, `CLEAR NODE <z>`, `SET RING TRACE`, `SET FW ZONE <z>|ZONES|ABORT`, `GET FW ZONE` (sequencer status). Host-tested against a fake node_mgr interface. |
| `components/wifi_ap` + `components/fw_srv` | master target | Decision 1. AP bring-up (canonical stop/set_mode/set_config/start order — rescue lesson), httpd stack 4096, one URI. |
| `zone/main/zone_ring.c` | zone glue | HB builder (2000 ms timer + edge triggers), ring-CMD dispatch task (static resp buffer, `CMD_SRC_RING`, dup cache in front), CFG assembler→verify→validate→apply→flush, CFG_GET streamer, FW_UPDATE handler (flush → handover expect_link=1 fixed URL → ACK → `uart_wait_tx_done` → RTC flag → reboot). |
| both apps | trial | `ota_trial.c` shared source: PENDING_VERIFY detection, breadcrumb consume, §3.10 criteria evaluation, `SET OTA CONFIRM` row, W_TRIAL/W_ROLLBACK NOTIFYs. |

Integration constraints carried from SP1: the ring dispatch task is `cmd_task_execute`'s second caller — its reply buffer is **static** (the SP1 cmd_task ownership protocol covers the claim race; the abandoned-slot lifetime rule is satisfied by the static buffer). `app_if` consolidation (SP1 review parking) rides along: the byte-identical zone/master `app_if` members move to a shared `app_if_common.c` before `zone_ring`/`node_mgr` add role-specific members.

## Master CLI forwarding path

`<VERB> ZONE <z> <tail>` (per §5.8 as amended): the master dispatcher's ZONE-prefix re-entry, instead of answering `ZONE_UNKNOWN` locally, hands `(z, reassembled tail)` to `node_mgr_forward()`: node known+ONLINE → CMD frame (ACK_REQ) via tracker → ACK detail returned verbatim as the reply (≤125 B, truncated at a line boundary per §2.4); failures map to `ZONE_UNKNOWN / ZONE_UNASSIGNED / ZONE_OFFLINE / ZONE_TIMEOUT / RING_DOWN / BUSY` (tracker full). The forward blocks the calling command slot up to `3 × ack_timeout + margin` — within `CMDF_SLOW`'s 1000 ms? No: forwarded rows run with a per-row budget override (forwarding is inherently multi-RTT; the master-side row carries `CMDF_SLOW` semantics with a 3500 ms budget, matching `cmd_task_execute`'s existing timeout).

## Fleet-OTA sequencer (`SET FW ZONE <z>` / `SET FW ZONES`)

Preconditions: `zone_fw` holds a valid image (else `FW_NO_IMAGE`), target ONLINE (else `ZONE_OFFLINE`), no sequence running (else `FW_BUSY`). Per zone: mark UPDATING + `break_expected` 180 s → FW_UPDATE frame (ssid/pass of the master AP, reboot_delay 500 ms) → await ACK → await HB with the *new* fw version (success) or 180 s timeout (`W_UPDATE_FAILED` NOTIFY, sequence aborts for ZONES). `SET FW ABORT` clears the sequence between zones (a zone already rebooted completes on its own). `GET FW ZONE` prints sequencer state.

## Testing

Host (`tests/host`, new executables): `test_ring_frame` (CRC vector + bit-flip, COBS 0..128 + resync + oversize, byte-exact worked example: CMD seq 1042 wire bytes), `test_ring_route` (zone + master tables, TTL, DROP_SELF), `test_ring_tracker` (stop-and-wait matrix: timeout ladder, priority insert, UNCLAIMED, per-zone cap, late ACK), `test_ring_dup` (IN_PROGRESS absorb, cached replay, 3000 ms window), `test_ring_telemetry` (HB/TIME_SYNC pack↔parse round-trips incl. zeros-policy), `test_ring_cfg` (assembler order/dup/gen-restart/timeout both directions), `test_ring_health` (state table + all §2.7 blame scenarios), `test_node_store` (envelope round-trip, assign-lowest-free, conflict), `test_ota_trial` (criteria matrix with fake clock, breadcrumb consume), `test_master_cmds` (row grammar + fake-mgr behaviors). Gate stays wired into all three app builds.

Bench acceptance (3 boards + wires): discovery from empty table (`NODE_NEW`, auto-ids); names persist across master reboot; `SET ZONE 2 WATER 1 TARGET 55` round-trip + `GET ZONE 2 WATER 1` truncation behavior; config push on master-side apply + revert per §4.4; wire-pull → `NOTIFY RING OPEN Z2 dead or wire Z2→Z3` blame correctness for each of the three links; TIME_SYNC time propagation after `SET TIME` on master; `SET FW ZONE 2` end-to-end through the master AP (watch: UPDATING → rescue repeater keeps Z3 online → new HB version); trial rollback demo (flash a build with a failing criterion → W_ROLLBACK).

## Size/budget notes

zone.bin grows by ring_link + zone_ring + trial (~25–40 KB est.) — far under the 1280 KB cap. master.bin adds Wi-Fi/httpd (~600 KB with the AP stack) — cap 2048 KB, expect ~850 KB total. `node_mgr` RAM: caches 8 × (368 B cfg + 176 B hw wire) + table + tracker ≈ < 8 KB.
