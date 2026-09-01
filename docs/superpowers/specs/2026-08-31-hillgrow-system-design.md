# HillGrow — System Design Spec

**Date:** 2026-08-31  
**Status:** Approved 2026-08-31  
**Target hardware:** ESP32-DevKitC-V4 (ESP32 classic) — 1 × Master (8 MB flash, 16 MB optional), N × Zone (4 MB WROOM), one shared rescue image  
**Framework:** ESP-IDF v6.0.1, FreeRTOS SMP, C99/C11  
**Repo:** `c:\Projects\HillGrov` → https://github.com/ekhpascal/HillGrov.git (`main`)  
**Author:** ekh@pascal-audio.com

---

## 1. System shape, hardware and flash layout

### 1.1 Roles and images

Three IDF apps in one repository share `components/`:

| Image | Runs on | Contains | Never contains |
|---|---|---|---|
| `master.bin` | 1 × DevKitC-V4 (8 MB, 16 MB optional) | Wi-Fi APSTA (AP `192.168.7.7` + STA to house Wi-Fi), web UI + HTTP API, ring master / node manager, zone & shelf model, global control (ventilation, dampers, reservoir, shutter, overall grow light, alarms), DS3231 time authority, CLI on UART0, UART display link (ESP32-S3 touch panel), SD card + I²S audio player | shelf hardware control |
| `zone.bin` | N × DevKitC-V4 (4 MB WROOM OK) | 1–4 Shelf objects, PCA9685 / PCF8575 / soil-ADC drivers, lighting / watering / fan / actuator controllers, safety manager, ring node, CLI on UART0 | Wi-Fi, HTTP |
| `rescue.bin` | every node, `factory` partition | Wi-Fi + one upload page (manual mode) or pull-from-Master (fleet OTA) + OTA write | CLI, control logic, API |

Zone identity is the eFuse MAC; the Master maps MAC → zone id / name / shelf count. All Zones run the identical `zone.bin`.

### 1.2 Topology

```
 house Wi-Fi ──STA──┐                      phone/PC ──AP 192.168.7.7──┐
                    └──► MASTER (DevKitC-V4) ◄───────────────────────┘
       I²C: DS3231 0x68 · PCF8575 0x20 (relays: fan, 3 dampers, shutter, refill, overall grow light) · [SHT31 0x44]
       UART1 ↔ ESP32-S3 touch display (optional) · SPI2 → microSD · I²S → PCM5102A DAC · ADC 34/35/36 reservoir
       UART2 TX ──► ZONE 1 RX     ZONE 1 TX ──► ZONE 2 RX  …  ZONE N TX ──► MASTER UART2 RX
                     │ I²C: PCA9685 0x40 (8 LED ch WHITE/RED × 4 shelves, OE → GPIO23)
                     │      PCF8575 0x20 (4 pumps, 4 fans, vibrator, spare)
                     │ ADC: 8 × SEN0193 (6 on ADC1, 2 on ADC2)
                     └ USB UART0: CLI + logs (bring-up / standalone operation)
```

### 1.3 Fixed pins (identical on Master and Zone)

| Function | GPIO | Notes |
|---|---|---|
| UART0 console + CLI | 1 (TX), 3 (RX) | USB-UART on the DevKitC |
| Ring UART2 | 18 (RX from upstream), 19 (TX to downstream) | one point-to-point link in, one out; internal + external 10 kΩ pull-up on RX |
| Reserved | 16, 17 | PSRAM on WROVER modules — never use, so either module works |
| I²C | 21 (SDA), 22 (SCL) | single bus, all expansion modules; 4.7 kΩ external pull-ups |
| Rescue button | 15 → GND | bootloader hold pin; idle high at reset |
| Status LED | 2 | on-board LED; blink patterns per safety mode |
| PCA9685 OE (Zone) | 23 | active-low; **external pull-up** keeps LED outputs disabled until firmware enables |
| Soil ADC1 (Zone) | 32, 33, 34, 35, 36, 39 | ADC1_CH4/5/6/7/0/3 |
| Soil ADC2 (Zone) | 25, 26 | ADC2_CH8/9 — usable because `zone.bin` never starts Wi-Fi |
| Display link UART1 (Master) | 25 (RX ← S3), 26 (TX → S3) | machine-mode CLI + NOTIFY, 115200 |
| microSD SPI2 (Master) | 14 SCK, 13 MOSI, 27 MISO, 4 CS | FAT, media only |
| I²S audio (Master) | 33 BCK, 32 WS, 23 DOUT | PCM5102A (SCK pin → GND) |
| Reservoir (Master) | 34 level ADC, 35 float LOW, 36 float HIGH | input-only; external pull-ups on floats |
| Never | 6–11, 12 | flash; MTDI strapping |

Full table, GPIO budget and hardware rules live in `docs/pin-mapping.md`. Two rules carry a bring-up checkbox each: **PCF8575-driven loads must be active-low or gated** (the expander powers up with all pins HIGH); **PCA9685 OE needs a pull-up** (outputs disabled from power-on through boot/rescue/crash).

### 1.4 Flash and boot

Partition table at `0xE000` (the custom second-stage bootloader is ≈34 KB).

```
Zone 4 MB:   0x10000 nvs 64K | 0x20000 otadata 8K | 0x22000 phy 4K | 0x30000 factory(rescue) 1280K | 0x170000 ota_0 1280K | 0x2B0000 ota_1 1344K
Master 8 MB: same header | 0x30000 factory(rescue) 1280K | 0x170000 ota_0 2048K | 0x370000 ota_1 2048K | 0x570000 zone_fw 1536K | 0x6F0000 data(LittleFS) 1088K
```
(rescue image estimate is 900–1000 KB with size-optimised Wi-Fi/httpd, hence the 1280 K factory slot; zone app-slot offsets stay 64 KB-aligned and the smaller slot (1280 K) bounds the image — ~2× headroom over the ≈650 KB zone.bin; master ≈ 1.4–1.5 MB in 2048 K; 16 MB master only grows `data`/`zone_fw`)

Boot rules (bootloader override of `bootloader_start.c`, per IDF `examples/custom_bootloader/bootloader_override`):

| Condition at boot | Action |
|---|---|
| RTC-retained flag `0xB0FAAF0B` set by the app | boot `factory` (rescue) once, clear flag |
| GPIO15 held low ≥ 10 s | boot `factory` |
| GPIO15 held low 1–9 s | erase the whole `nvs` partition, boot normally (< 1 s = ignored, debounce) |
| New OTA image never called `esp_ota_mark_app_valid_cancel_rollback()` | roll back to previous slot (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`) |

Fleet OTA: Master UI uploads `zone.bin` into `zone_fw`; zones are updated one at a time via ring `FW_UPDATE` → zone writes handover record + RTC flag + reboots → rescue joins Master AP as STA, pulls `http://192.168.7.7/fw/zone.bin`, writes the inactive slot, reboots → new app self-tests, marks valid → Master sees new version in heartbeat → next zone. Master OTA: upload written directly to the Master's inactive slot. Rescue image: USB only. Manual rescue: AP `HillGrow-Rescue-<last 6 hex of MAC>` at 192.168.7.7, one upload page.

### 1.5 Repository skeleton

```
HillGrov/
  master/  zone/  rescue/        # IDF projects: CMakeLists, sdkconfig.defaults, partitions.csv, main/
  components/                    # shared: cli, ring, config, safety, drivers, ...  (Section 6)
  bootloader_components/main/    # bootloader_start.c override (shared by all three apps)
  tests/host/                    # Unity host tests, pre-build gate for every app's .elf
  tools/                         # uart_test.py, ring_test.py (pyserial)
  docs/pin-mapping.md  docs/what_we_learned.md  docs/superpowers/{specs,plans}/
```

### 1.6 Out of scope for V1

Cloud connectivity; ADS1115 soil-ADC backend; PWM fan speed; ring-carried OTA; OTA of the rescue image; any Wi-Fi in `zone.bin`; heater / dehumidifier control.

---

## 2. Ring protocol and link layer

Two components: `ring_proto` (pure C — CRC, COBS, frame codec, routing, dup cache, tracker, config assembler, health/break analysis; time injected as `now_ms`; all fields packed by explicit byte offset, little-endian, no struct casts) and `ring_link` (target — UART2 driver, `ring_rx` task, send functions).

### 2.1 Physical layer

UART2, GPIO19 TX → next node RX, GPIO18 RX ← previous node TX, **115200** 8N1, no flow control. 3.3 V TTL point-to-point (decision, brainstorm; CRC + per-node error counters surface a noisy run — RS-422 per link is the escalation path, protocol unaffected). After `uart_set_pin()`: `gpio_set_pull_mode(GPIO18, GPIO_PULLUP_ONLY)` **plus an external 10 kΩ pull-up on every RX** — IDF does not enable RX pulls on ESP32 HP UARTs, and an unpowered upstream must read idle-high, not a BREAK storm. Twisted pair with GND per link.

### 2.2 Framing and integrity

`wire = 0x00 + COBS(hdr[9] + payload[<=128] + crc16_le) + 0x00`. The leading delimiter terminates garbage from an upstream mid-frame reboot; the decoder resyncs on any 0x00. Decoder buffer 141 B; overrun → drop + resync. UART BREAK/FRAME/PARITY/FIFO-OVF events reset the decoder and count `rx_uart_err`. CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF, check `0x29B1`) over header+payload, own 512 B table (bit-identical MSVC/Xtensa), **recomputed at every forwarding hop** (TTL is covered) so corruption stops at the first hop and `rx_crc_err` localises the bad cable.

### 2.3 Header (9 B) and addressing

```
off  field  meaning
0    magic  0xA1 (protocol v1; other → drop)
1    src    originating node id
2    dst    target id; 0xFF broadcast
3    type   see 2.4
4    flags  b0 ACK_REQ, others must be 0
5    ttl    16 at origin; -1 per forward; drop at 0
6    len    0..128, cross-checked against decoded length
7    seq    u16 LE per-source counter = the command id
```

Ids: `0x00` Master · `0x01..0x10` reserved for zones by the protocol — V1 builds accept/assign only `0x01..0x08` (`HG_MAX_ZONES 8`, matching `zc1..zc8` and `online_mask` bits 1..8; frames from higher ids are forwarded but never enrolled, so raising the fleet cap is a build-constant change, not a protocol change) · `0xFE` unassigned · `0xFF` broadcast. `hops = 16 - ttl` at the receiver. MAC travels only in HEARTBEAT and ASSIGN_ID payloads.

### 2.4 Message types

| type | name | dir | ACK | payload (LE offsets) |
|---|---|---|---|---|
| 0x01 | HEARTBEAT | Z→M | – | 0 mac[6]; 6 fw maj/min/patch; 9 n_shelves; 10 uptime_s u32; 14 unix_time u32; 18 cfg_gen u32; 22 cfg_crc u32; 26 hw_crc u32; 30 cfg_src; 31 mode; 32 reset_reason; 33 time_quality; 34 active_faults u64; 42 shelf_faults[4] u16; 50 link_flags; 51 override_mask; 52..61 counters (rx_crc_err, rx_uart_err, rx_drop, fwd_count, min_free_heap_kb, u16 each); 62 shelf[n]×14 (raw_a/raw_b mV u16, pct_a/b, white, red, out_flags, light_state, water_state, rsvd, pump_today_s u16). 62+14n ≤ 118 B |
| 0x02 | ACK | Z→M | – | 0 acked_seq u16; 2 status (0 OK, 1 ERR); 3.. detail = the reply line ≤ 125 B |
| 0x03 | FAULT_EVENT | Z→M | – | code, scope, set, rsvd, detail u32, uptime_s u32 (12 B, best-effort; the HB mask is the authority) |
| 0x04 | NOTIFY | Z→M | – | text line ≤ 128 B (edge events; telemetry types off by default) |
| 0x10 | CMD | M→Z | yes | the CLI text line ≤ 128 chars, no CR/LF |
| 0x11 | TIME_SYNC | M→bcast | – | 0 utc u32; 4 utc_offset_s i32; 8 flags (b0 time_valid, b1 ntp); 9 ring_size; 10 online_mask u16; 12 inhibit_mask (b0 PUMPS b1 LIGHTS b2 ALL) = 13 B |
| 0x12 | ASSIGN_ID | M→Z | – | mac[6], zone_id; confirmed by next heartbeat (shelf count and all other hardware config travel only through the HW config plane, §4) |
| 0x13 | CFG_CHUNK | M↔Z | – | kind u8 (1 CFG, 2 HW); gen u32; idx u8; count u8; total u16; data ≤ 112 B |
| 0x14 | CFG_COMMIT | M→Z | yes | kind u8, gen u32 — the only tracked frame of a transfer (one RTT per push) |
| 0x16 | CFG_GET | M→Z | yes | kind u8 → zone streams CFG_CHUNKs back (adopt / hw-cache pull) |
| 0x15 | FW_UPDATE | M→Z | yes | reboot_delay_ms u16, ssid_len+ssid, pass_len+pass ≤ 99 B (URL fixed: `http://192.168.7.7/fw/zone.bin`) |

**CMD carries CLI text.** The zone executes it through the same table-driven `cmd_dispatch()` as its own console — one grammar, one range check, one safety gate, `GET`/`HELP` over the ring for free; the Master compiles in no zone vocabulary, so images cannot skew during one-at-a-time fleet updates. ACK detail is the verbatim reply line. Master-local failures map to CLI tokens `ZONE_TIMEOUT | ZONE_OFFLINE | ZONE_UNKNOWN | RING_DOWN | BUSY`.

Wire field values: `time_quality` {0 NONE, 1 COARSE (assumed/RTC-restored), 2 SYNCED}; `link_flags` b0 upstream_alive, b1 master_alive, b2 heard_by_master (from `online_mask`); shelf `out_flags` b0 pump, b1 fan, b2 aux, b3 light_on, b4 manual_override; `shelf_faults[n]` is the shelf-scoped fault subset of §3.8 with bit indices fixed in `fault.h`. Remote `GET`/`HELP` answers fit the single-ACK 125 B reply cap — longer replies truncate at a line boundary; full dumps (`GET CONFIG`, bare `HELP`) are Master-side operations (config cache / local table).

### 2.5 Store-and-forward

Whole frame validated before relay (cut-through would propagate corruption). Zone routing: own src → drop (loop guard); own dst → consume; broadcast → consume and forward; 0xFE → consume if MAC matches else forward; else ttl−1, re-CRC, forward. **The Master is the ring sink and never forwards** — its returned broadcast is the ring-closed probe. `ring_rx` never runs application code: consumed frames are copied by value into the command task's queue; forwarding happens in place. No TX task — every sender writes one complete encoded frame via `uart_write_bytes()` (driver `tx_mux` serialises; TX ring 1024 B).

### 2.6 ACK, retry, duplicates

End-to-end ACK; global stop-and-wait — one outstanding **ACK_REQ** frame ring-wide. Only CMD, FW_UPDATE, CFG_COMMIT and CFG_GET are tracked; CMD/FW_UPDATE are inserted ahead of CFG_COMMIT/CFG_GET in the 8-deep pending queue. UnACKed frames (TIME_SYNC, CFG_CHUNK, all zone-originated traffic) are written directly and may interleave with the outstanding tracked frame. `ack_timeout_ms = clamp(200 + 40·ring_size, 400, 900)` (520 ms at 8 zones vs ~230 ms worst RTT); 3 byte-identical attempts; an UNCLAIMED unicast returned to the Master fails the command immediately (`ZONE_UNKNOWN`). Zone dup cache `{last_seq, state IN_PROGRESS/DONE, cached ACK}`: the seq is recorded **before** dispatch, so a retransmit during a slow handler is silently absorbed and a retransmit after completion replays the cached ACK — an actuator command executes at most once. Dup window 3000 ms.

### 2.7 Heartbeat, time, health

HEARTBEAT every 2000 ms from the zone control task (plus immediately at link start, after ASSIGN_ID, and on any FAULT/CRITICAL edge, rate-limited 1/500 ms). TIME_SYNC broadcast every 2000 ms is simultaneously: time distribution (zones `settimeofday()` in the control tick when |Δ| > 2 s), the Master-alive signal, zone 1's upstream probe, the ring-closure probe (its return yields `ring_size`), per-zone reachability proof (`online_mask`), and the fleet inhibit channel (`inhibit_mask`, aged out by zones after 600 s).

Master node health: ONLINE → DEGRADED after 5 s without HB (or 3 command timeouts) → OFFLINE after 10 s (alarm, commands refused); UPDATING during fleet OTA (alarms suppressed 180 s); a node whose heartbeat carries a non-zero fault mask is additionally shown FAULT — health state and fault state are orthogonal. With an empty zone table the ring reports IDLE and the open-ring alarm is suppressed (a Master-only installation needs no loopback). Ring OPEN when the Master's own TIME_SYNC stops returning for 5 s; blame from hop counts + `upstream_alive` bits: `NOTIFY RING OPEN Z2 dead or wire Z2→Z3`. Zone link state: LOST after 6000 ms without a Master-sourced frame (informational; the safety LOCAL transition is §3).

### 2.8 Enrolment

No NVS id → boots as 0xFE (forwarding needs no id, the ring stays closed), heartbeats MAC. Master: known MAC → re-ASSIGN_ID; unknown → auto-assign lowest free id, persist flagged *unconfigured*, `NOTIFY NODE_NEW` — observable immediately, 0 shelves until configured. Stale id → corrected by MAC. Two boards on one id → `ID_CONFLICT`, intruder reset to 0xFE.

### 2.9 Config transfer and FW_UPDATE

Push trigger: HB `cfg_gen/cfg_crc` mismatch or operator apply. CHUNKs sent back-to-back unACKed; COMMIT is ACKed; zone assembles (768 B buffer, gen/kind mismatch restarts, 2 s idle abort), verifies the blob envelope CRC-32, validates, applies atomically, ACKs OK or `ERR <token> <path>`. Whole-transfer retry ×3 with 1/2/4 s back-off, then DEGRADED (`INVALID_FIELD`/`CFG_VERSION` never retried). CFG_GET: the zone ACKs immediately (OK = transfer starting), then streams its chunks; the Master assembler takes `count` from the first chunk, aborts after 2 s idle, and retries the whole CFG_GET ×3 with the same back-off. HW-cache rule: a heartbeat `hw_crc` differing from the Master's cached copy (or no cache yet) triggers `CFG_GET kind=HW`; the cache is display/export only and is never pushed back automatically. FW_UPDATE: zone enters safe state → writes handover (`expect_link=1`) → ACK → `uart_wait_tx_done` → RTC flag → reboot. The **handover record** is the app↔rescue contract, compiled from one shared component (`rescue_handover`): NVS namespace `hg`, key `hando`, layout `{magic 'HGRH', ver u8, expect_link u8, rsvd[2], ssid[33], pass[65], url[64], crc32}`; written by the zone's FW_UPDATE handler (fixed URL) or the bench `SET FW UPDATE` command (given URL); rescue reads **and erases** it before starting Wi-Fi — absent, short or CRC-bad → manual AP mode. Then: Master holds UPDATING with `break_expected` for 180 s; the rescue image runs a UART2 byte repeater so downstream zones stay online; the new app must see a Master frame within 180 s to mark itself valid (bench flashes without handover: 60 s self-test or `SET OTA CONFIRM`).

### 2.10 Driver and task

`uart_driver_install(UART2, rx 2048, tx 1024, evtq 16)`; `ring_rx` task core 0 prio 6 stack 4096, TWDT-subscribed, event-driven (≤1000 ms waits). Master tracker + TIME_SYNC scheduling tick every 50 ms in `node_mgr`; health/break analysis runs on its 1 s sub-tick (§6.1). Bench aids: TX→RX jumper = loopback counted by DROP_SELF (`GET RING` prints it), `SET RING TRACE ON` hex-dumps headers.

### 2.11 Host tests (ring_proto, Unity)

CRC vector + bit-flip; COBS round-trip 0..128 incl. resync and oversize; frame encode/parse against byte-exact worked examples (CMD seq 1042 → wire bytes, CRCs verified independently); forward rewrite TTL/CRC; routing tables (zone + master); dup cache incl. IN_PROGRESS; tracker stop-and-wait, priority insertion, timeout formula, UNCLAIMED fast-fail; HB/TIME_SYNC pack/parse; config assembler (order, dup, gen restart, timeout); health state table; break blame scenarios; enrolment flows; seq-gap accounting.

---

## 3. Safety, watchdog and fault model

### 3.1 Three rules

1. **One writer.** Controllers (light/watering/fan/pollination; Master: ventilation/reservoir/shutter) and every command source (CLI, HTTP, ring) call `safety_request(act, level, ttl_ms, src)`. The gate's checks + lease bookkeeping run synchronously in the command path (immediate `ERR` token); the physical output write happens only in the `ctrl` task's next 100 ms tick. `ctrl` is the **sole I²C caller** on each node (the registered shutdown handler is the one exception, guarded by the `i2c_bus` mutex).
2. **Every ON is a lease.** Controllers renew continuous outputs (light/fan) each tick with a 10 s TTL; one-shot outputs (pump/vibrator/valve/shutter) get `ttl = dose`. `safety_tick` (100 ms) forces off expired leases **and** anything whose continuous ON time reached its class `max_on_s` regardless of renewals (→ latched `F_PUMP_MAXRUN`, a controller-bug detector).
3. **Hardware holds the safe state.** PCA9685 `OE` (GPIO23, external pull-up) keeps all LED channels off through bootloader, rescue, crash, reset; MODE2 `OUTNE=00`; LED MOSFET gates are non-inverting (low = off). PCF8575 powers up all-HIGH and keeps its last word across an ESP32 reset ⇒ **every PCF8575 load is active-low** (`pcf_active_low_mask 0xFFFF`; wiring rule in `docs/pin-mapping.md`); the first I²C transaction at boot writes `0xFFFF` and reads it back. Accepted residual (decision, brainstorm: no ACT_EN rail gate): a pump ON at the instant of a hard crash runs ~0.3–0.5 s until that boot step — a deliberate, bounded relaxation of requirements §15 ("booting must not generate unintended output pulses"); the active-low wiring rule remains the hardware backstop.

### 3.2 Actuator classes and limits (config clamps to compiled hard caps, `W_CONFIG_CLAMPED`)

| Class | max_on default (hard cap) | min_off | daily default (cap) | Interlocks |
|---|---|---|---|---|
| PUMP | 60 s (300) | 600 s | 600 s (3600) | **one pump ON per zone** (per-zone only — PSU sized for all zones dosing simultaneously, decision, brainstorm); soil valid; calibration valid; not inhibited |
| LIGHT | — | — | 20 h (24 = unlimited) | per-shelf duty cap `led_max_pct` |
| FAN (shelf) | — | 30 s | — | — |
| VIB | 10 s (30) | 60 s | 600 s (1800) | — |
| VALVE_REFILL (M) | 300 s (900) | 300 s | 1800 s (7200) | level valid, not HIGH-float, below target |
| DAMPER (M) | per type (SP5) | — | — | open+close never together; closing last exhaust refused while fan ON |
| FAN_MAIN (M) | — | 30 s | — | intake OPEN and ≥1 exhaust OPEN, re-checked every tick |
| SHUTTER (M) | 60 s (120) | 5 s | — | — |
| LIGHT_MAIN (M) | — | 30 s | 20 h (24 = unlimited) | overall grow light relay: own on/off schedule + duration-bounded manual override; coordinated with the blackout shutter (SP5) |

### 3.3 Safe boot sequence

(0) bootloader/rescue never touch GPIO23 or I²C; (1) first statement of `app_main`: GPIO23 output high (+pull-up); (2) `esp_reset_reason()` → boot-record verdict NORMAL/ABNORMAL/HOLD/CRASH_LOOP; (3) `nvs_flash_init` (erase-and-retry once) + config load — invalid ⇒ defaults + fault, **never abort**; (4) I²C bus up, probe 0x20/0x40; (5) **PCF8575 ← 0xFFFF, read back**; (6) PCA9685 init to all-off @1017 Hz, read back MODE1; (7) `safety_init` (mode BOOT); (8) OTA state → TRIAL if PENDING_VERIFY; (9) register shutdown handler, create tasks (each subscribes to TWDT); (10) entering RUN/LOCAL → OE low. After step 3 no `ESP_ERROR_CHECK` on runtime driver calls — errors log, map to faults, degrade (HillAug lesson: an aborted board waters nothing). Per role: the Zone probes 0x20/0x40 and runs steps 5–6 and 10 as written; the Master probes 0x20/0x68 (and 0x44 when configured), skips step 6 and leaves GPIO23 unused; LOCAL exists only on Zones — the Master's mode set is BOOT/TRIAL/HOLD/RUN/SAFE.

### 3.4 Modes

`BOOT → RUN ⇄ LOCAL`; `TRIAL` (outputs muted until self-test → mark-valid, else rollback); `HOLD` (3 consecutive abnormal resets → 300 s outputs-off with CLI + heartbeat alive; 3 HOLDs → `SAFE` latched — a crash loop is bounded to ≤ 9 output pulses); `SAFE` (CRITICAL fault; GET/HELP/fault-clear still work). **LOCAL**: entered after `link_loss_timeout_s` (config 10..600, default 30) without a Master-sourced frame; *all plant care continues indefinitely* — schedules on the free-running clock, watering from local sensors under all §3.2 limits; ring-sourced overrides are released at entry; the Master `inhibit_mask` ages out after 600 s. Time never synced → schedule runs from assumed boot time 08:00 + uptime, `W_NO_TIME`. First valid Master frame → RUN.

### 3.5 Gate order and tick duties

Gate evaluation (first failure wins): NOT_CONFIGURED → SAFE_STATE (mode) → FAULT_ACTIVE (blocked scope) → LIMIT (level/TTL; command sources are *refused*, controllers clamp) → BUDGET (daily) → INTERLOCK (min_off, class hooks) → apply/lease. Tick: lease expiry; hard caps; daily accounting (midnight or 86 400 s uptime rollover); link timer; continuous Master interlocks; **audits** — PCF8575 read-back every 1 s (3 mismatches → rewrite 0xFFFF, latched `F_OUTPUT_READBACK`, PCF outputs blocked), PCA9685 MODE1 every 10 s (chip reset detected → re-init + `W_PCA_RESET`); I²C ladder per call: 20 ms timeout ×3 retries + 10 ms back-off → `i2c_master_bus_reset()`; 10 consecutive failures → `F_I2C_BUS` CRITICAL → SAFE, re-probe every 10 s (≤3 auto-exits/hour); status LED.

### 3.6 Soil sensor validity (all values mV; eFuse-Vref calibration; 16× oversampling; 100 kΩ pull-down per input)

| Test | Set (defaults) | Clear | Effect |
|---|---|---|---|
| OPEN | < 80 mV ×3 samples | 60 s in range | sensor INVALID |
| SHORT | > 3200 mV ×3 | 60 s | INVALID |
| RANGE | outside [cal_wet−250, cal_dry+250] ×3 | 60 s | INVALID |
| STUCK | Δ ≤ 2 mV for 12 h | change | warn only |
| A/B DIFF | Δ > 25 percentage points (calibrated 0–100 % scale) for 60 s | < 15 points for 60 s | use the **wetter** reading |
| CAL | cal_dry − cal_wet < 300 mV | reload | pump blocked |

One sensor invalid → daily pump budget halved; both → pump blocked; 3 doses with < 3 % response after `settle_min` → latched `F_PUMP_NO_RESPONSE`. Derived per-pair values (average, min, max, A−B difference — requirements §6) are computed by the Master from each heartbeat's raw pair for display and history; zone control uses the wetter valid reading, validity uses the difference.

### 3.7 Manual overrides

Any actuator `SET` from CLI/web/ring: pump/vib/valve/shutter **require a duration** ≤ class cap; light/fan default 3600 s (cap 43 200; the CLI takes minutes for light/fan — caps are stored in seconds). Overrides beat controllers, never beat caps/interlocks/budgets; released by `SET AUTO <shelf>` (all overrides on the shelf) or a 0-value on the actuator row (that actuator only — the controller resumes next tick); expiry → `NOTIFY OVERRIDE <act> EXPIRED`; none survive reboot; ring-sourced ones die at LOCAL entry.

### 3.8 Faults

One 64-bit node store plus a 16-bit per-shelf mask (the shelf-scoped subset: W_SOIL_INVALID, F_SOIL_BOTH_INVALID, W_SOIL_AB_DIFF, W_SOIL_STUCK, F_CAL_INVALID, F_PUMP_BUDGET, F_PUMP_NO_RESPONSE, F_PUMP_MAXRUN, W_LIGHT_BUDGET; bit indices fixed in `fault.h`), both carried in every heartbeat; each code has fixed severity (WARN / FAULT = blocks its scope / CRITICAL = SAFE) and fixed latch attribute; latched codes need `CLEAR FAULT <CODE|ALL>` (refused while the condition persists). Zone codes: W_LINK_LOST, W_NO_TIME, W_ABNORMAL_RESET, F_CRASH_LOOP, F_I2C_BUS, F_PCA9685, F_PCF8575, F_OUTPUT_READBACK, F_CONFIG_INVALID, W_CONFIG_CLAMPED, W_TRIAL, W_ROLLBACK, W_OVERRIDE_ACTIVE, W_PCA_RESET, W_SOIL_INVALID, F_SOIL_BOTH_INVALID, W_SOIL_AB_DIFF, W_SOIL_STUCK, F_CAL_INVALID, F_PUMP_BUDGET, F_PUMP_NO_RESPONSE, F_PUMP_MAXRUN, W_LIGHT_BUDGET, F_ADC_FAIL, W_RESTART_PENDING, F_NVS, F_HW_CFG, W_CFG_VERSION_NEWER, W_CFG_RESET. Master adds: F_RESERVOIR_HIGH, F_REFILL_NO_RISE, F_LEVEL_INVALID, W_VENT_INTERLOCK, F_DAMPER_STUCK, W_CLIMATE_SENSOR, F_LEAK, W_ZONE_OFFLINE, W_CFG_FORK, W_CFG_SYNC_FAILED, F_ID_CONFLICT, W_UPDATE_FAILED, W_RING_OPEN. Transport: mask in every heartbeat + edge heartbeat ≤ 500 ms + best-effort FAULT_EVENT detail; Master appends transitions to LittleFS alarm log and shows active+history.

### 3.9 Watchdog, reset, shutdown

`CONFIG_ESP_TASK_WDT_TIMEOUT_S=8`, `CONFIG_ESP_TASK_WDT_PANIC=y`, idle tasks watched on both cores (HillBT's disabled-idle pattern deliberately not copied); every app task subscribes and bounds blocking calls ≤ 1000 ms; never delete/re-add around long calls. The weak `esp_task_wdt_isr_user_handler()` (IRAM) raises OE via `GPIO_OUT_W1TS_REG` before the panic. `esp_register_shutdown_handler(safety_shutdown)` covers every `esp_restart()`: PCF ← 0xFFFF, ALL_LED_OFF, OE high. INT WDT 300 ms; brown-out level 7 (2.80 V); panic → print + reboot, delay 0. Boot record (RTC_NOINIT, magic+CRC): boot_count, abnormal_count (cleared after 600 s stable), hold_count, last fault — drives the HOLD/CRASH_LOOP ladder in §3.4. Rescue flag lives in the bootloader's separate rtc_retain_mem CRC domain.

### 3.10 OTA trial

Only when the slot is `PENDING_VERIFY`. Criteria are firmware-attributable (config parsed or defaulted; I²C bus created, ring driver installed, ADC unit created — device probes deliberately excluded so a bench board can't roll back a good image; `ctrl` ticked ≥ 50× with TWDT happy; heap > 40 KB; Master additionally: AP netif at 192.168.7.7 + httpd up). Handover `expect_link=1` (fleet update) additionally requires a valid Master frame within 180 s. Pass → `esp_ota_mark_app_valid_cancel_rollback()`; fail/timeout → rollback + `W_ROLLBACK` latched on the old image. Bench: `SET OTA CONFIRM`.

### 3.11 Master global safety (details in SP5 feature spec)

Refill valve: level valid ∧ not HIGH ∧ below target; HIGH while filling → off within one tick + latched fault; no rise in 120 s → latched fault; LOW float → `inhibit_mask |= PUMPS`. Recommended hardware backstop: NC high-level float in series with the solenoid coil. Ventilation: per-damper position model (CLOSED/OPENING/OPEN/CLOSING/UNKNOWN via end switches or travel time); fan path interlock every tick. `SET SAFE ON` → `inhibit_mask = ALL` (fleet dark ≤ 2 s, self-releasing after 600 s if the Master dies). **Overall grow light** (owner addition 2026-09-01): a room-wide supplemental light on a PCF8575 relay, class LIGHT_MAIN — photoperiod schedule of its own, daily-hours budget, duration-bounded manual override (`SET OUT`/web), included in `SET SAFE ON` and in blackout-shutter coordination. Damper/reservoir hardware variants deferred to SP5 (decision, brainstorm).

### 3.12 Status LED (GPIO2) and console

Priority: SAFE/HOLD 5 Hz → BOOT/TRIAL solid → fault 1 Hz → override 3 blinks/2 s → LOCAL 2 blinks/2 s → RUN 1 blink/2 s. Every fault transition: one log line + `NOTIFY FAULT <CODE> <scope> SET|CLR`. `GET STATUS` shows mode/faults/link/OE/reset; `GET FAULTS` lists entries with age and latch state.

### 3.13 Host tests

Gate refusal matrix per mode; lease clamp/refuse/expiry; pump hard cap under continuous renewal; one-pump-per-zone + min_off; daily budget latch + halved budget; inhibit TTL; light duty/day caps; override win/expire/release + ring-release-on-LOCAL; SAFE entry/exit rules; I²C failure ladder; fault latch vs self-clear + masks + notify-on-edge; link state 30 s; boot record ladder (PANIC ×3 → HOLD, ×3 → CRASH_LOOP, stable clears); trial criteria incl. expect_link; all soil validity rows; watering no-response latch; read-back audits; Master vent/reservoir interlocks; LED priority; RC→ERR token mapping.

---

## 4. Configuration, state and persistence

### 4.1 Four planes, one owner each

| Plane | Struct (size) | Writer | Persisted | Authoritative |
|---|---|---|---|---|
| HARDWARE | `hg_zone_hw_t` (160 B): shelf_count, PCA/PCF addresses, per-shelf channel/pin/ADC maps, calibration (mV), **installed safety limits** (`led_max_pct[2]`, `pump_max_run_s`, `pump_max_daily_s`), optional aux devices | installer (CLI, incl. forwarded) | Zone NVS `hw` | Zone (Master keeps a RAM cache via CFG_GET for display/export) |
| LOGICAL | `hg_zone_cfg_t` (272 B): name, per-shelf crop + light/water/fan/aux schedules, link_loss_timeout_s | Master (web/CLI); Zone CLI only during bring-up | Master NVS `zcN` + Zone NVS `cfg` (cache) | Master once one exists |
| RUNTIME | `hg_zone_rt_t` (224 B) | `ctrl` task only | only `hg_daily_t` (64 B: pump/dose/light/vib daily counters + coarse time) — RTC_NOINIT copy every tick + NVS every 15 min | Zone |
| TELEMETRY | heartbeat payload (§2.4) | built from RUNTIME | never (history = Master LittleFS) | — |

Safety limits live in HARDWARE config so no logical/crop edit can raise them (requirements §15); cross-field validation enforces `water.dose_s ≤ hw.pump_max_run_s` and `max_doses_day × dose_s ≤ pump_max_daily_s`. Board constants (pins, I²C GPIOs) are **not** configuration — they live in `board.h` / `docs/pin-mapping.md`. Crop names are display strings; no logic keys on them. All persisted/wire structs are fixed-size, offset-pinned (`_Static_assert`), no pack pragmas; persistence and ring transfer are `memcpy`.

### 4.2 Envelope and migration

`hg_blob_hdr_t {u32 magic; u16 version; u16 length; u32 generation; u32 crc32}` (16 B) precedes every blob in NVS and on the ring; CRC-32/ISO-HDLC (check `0xCBF43926`, pure-C, host/target identical) covers header+payload. Load policy, in order: short → magic → length → CRC → `version > current` = **E_VERSION_NEWER** (defaults + fault, blob left in flash — OTA rollback can never mis-read a newer layout) → `version < MIN_COMPAT` = defaults + fault → older-but-compatible = zero-fill + copy + mark dirty (rewritten in current layout). Migration is append-only, consume `rsvd[]` first, every new field must mean "default" at 0; a semantic change bumps MIN_COMPAT; a converter is added only with a captured-old-blob host test.

### 4.3 NVS layout and the store task

Namespace `hg`, one `nvs_open` at init. Zone keys: `zid` (u8) and `hw` — written on the store task's next pass (it wakes on a flush semaphore or every 1000 ms); `cfg` (5 s throttle); `daily` (15 min + rollover + pre-restart). Master: `mcfg` (Wi-Fi STA/AP creds, TZ, NTP host — only home, `CONFIG_ESP_WIFI_NVS_ENABLED=n`, `WIFI_STORAGE_RAM`), `ztab` (MAC→id table), `prof` (16 profiles), `zc1..zc8`. Writers set dirty bits; the `store` task (prio 2) is the **only flash writer** (OTA/zone_fw uploads excepted, guarded by `flash_busy` so `ctrl` skips I²C that tick); `nvs_set_blob`'s verified compare-and-skip is the wear backstop; 3 consecutive write failures → `F_NVS`. `hg_store_flush(2000 ms)` before every restart. Factory reset (CLI): erase the `hg` namespace + reboot; the bootloader 1–9 s hold erases the whole `nvs` partition — equivalent, since every HillGrow key lives in `hg`. Daily counters restore order: RTC copy (survives panics) → NVS copy if same day → zeros; coarse `unix_time` keeps light-schedule phase on a Master-less reboot.

### 4.4 Ownership reconciliation (two writers, one counter)

`generation` (u32, monotonic, in the envelope) + `source` (LOCAL/MASTER) + `cfg_crc`, all echoed in every heartbeat. **Enrolment** (ASSIGN of a MAC): Master has no config (`gen 0`) → **adopt** the zone's bring-up config; Master has one → **push** (operator may tick `ADOPT`); push always uses `max(gens)+1`. **Steady state**, evaluated per heartbeat: equal gen+crc → in sync; equal gen, different crc → push (fork, logged); zone gen 0 or lower → push; zone gen higher → **push (revert)** regardless of source — the Master always wins (decision, brainstorm); the revert emits `NOTIFY CFG_REVERTED <id> <gen>` so a console edit at a zone is never silently discarded. Master NVS loss self-heals: all zones re-appear as pending, re-assignment adopts their caches. HW-map changes (channels/pins/shelf_count) persist but apply only after the next `REBOOT <CONFIRM>` (`W_RESTART_PENDING` until then); calibration and safety limits apply live.

### 4.5 Distribution, discovery

Chunked transfer per §2.9 (`hg_xfer`: 112 B self-contained chunks, 768 B reassembly, validate-then-atomic-apply, one ACK per transfer carrying the ERR line verbatim). Discovery/identity per §2.8; `ztab` entries carry `{mac, zone_id, flags: assigned|retired}`; unassign keeps `zcN` so re-assignment restores the recipe; replacing a dead board = assign the new MAC to the old id.

### 4.6 One field table drives CLI, JSON and validation

`hg_field_t {group, key, offset, type (U8/U16/BOOL/HHMM/ENUM/STR/PIN), min, max, names}` — ~50 rows, integrity-checked by a host test and at boot. Derived: **CLI** `SET <GROUP> <shelf> <KEY> <value>` → `OK <GROUP> <shelf> <KEY> <value>`; `GET <GROUP> <shelf>` → `Label : value` lines; `GET CONFIG` = replayable `SET` lines (the zone's export/import — paste into PuTTY or uart_test.py); generated HELP with ranges. Groups: ZONECFG, SHELF (crop/enabled/profile), LIGHT, WATER, FAN, AUX, HW, HWSHELF, CAL. **JSON** (Master only, cJSON managed component): `/api/config` GET/PUT with merge semantics — missing keys keep values, unknown keys → `warnings[]`, first bad value aborts with its path (HTTP 400), whole document validated into a scratch copy before one atomic apply; HH:MM strings; export includes Wi-Fi passwords (`?secrets=0` omits) — it is the disaster-recovery backup. **Validation** (`hg_cfg_validate`/`hg_hw_validate`): table ranges + cross-field rules (duplicate channel/pin/ADC, light.on≠off, enabled shelf < shelf_count, dose vs pump caps); one source of truth for CLI, HTTP, ring receiver and NVS loader; error = field path used verbatim everywhere.

### 4.7 Profiles

16-row Master table of `hg_shelf_cfg_t` (name in `crop[]`; seeds "Leafy", "Fruiting", "Seedling"). Apply = copy into shelf + record `profile_id`; later shelf edits are plain edits (UI shows "modified"); `SET PROFILE <p> REAPPLY` re-copies explicitly. No live inheritance; zones never see profiles; 4 bytes reserved for a future override mask.

### 4.8 Concurrency

`cmd_task` is the sole caller of `hg_model_edit()`/`hg_model_apply_remote()` (§5); `hg_model_edit` copies live hw+cfg to scratch, runs the pure edit callback, validates, then commits atomically under the model mutex (µs hold, no I/O) — an invalid edit changes nothing. `ctrl` snapshots cfg/hw at tick start (seq-gated memcpy) and is the only RUNTIME writer, publishing `rt` once per tick. Nothing returns pointers into the model (HillAug lesson). Under HOST_TEST the mutex is a no-op.

### 4.9 Host tests

Struct sizes/offsets on MSVC+GCC; CRC-32 vector; envelope round-trip + ordered rejects + short-blob migration; defaults inert and valid; per-row range/path validation; field-table integrity; text round-trip per type; sync-decide truth table incl. enrolment resolution and revert-not-adopt; xfer split/reassemble/dup/restart/timeout/backoff; apply-remote atomicity; model edit validate-commit; persist throttle policy; daily restore sources; profile apply/reapply; ztab/pending/conflict flows; DUMP replay equality; JSON round-trip/merge/export-import.

---

## 5. Command core — one dispatcher for CLI, HTTP and ring

Components: `cmd_core` (tokenizer, table match/parse, HELP, dispatch, response formatting, forward-pending state machine — pure C), `cli_line` (byte-fed terminal editor — pure), `notify` (formatter + per-(type,idx) rate limits + sinks — pure), `cmd_task` (the executor task), `cli` (UART0 driver + log hook), `cmd_common` (rows shared by both roles), `zone_cmds` / master rows. Hardware reaches handlers only through an `app_if` callback struct, faked in host tests — no `#ifdef HOST_TEST` in pure files.

### 5.1 Grammar

HillBT wire grammar verbatim (brainstorm decision): CR/LF lines, case-insensitive whitespace tokens, `SET|GET <NOUN…> [args]`, 1-based indices, one `OK <NOUN> <canonical echo>` or `ERR <TOKEN>` first line, `+ ` help lines, two-space `Label : value` continuations, trailing `HELP` on any prefix, `NOTIFY <TYPE> <node> <payload>` pushes. Fixed vs HillBT: one table drives dispatch+arity+ranges+HELP; every line (incl. `DEBUG ENABLE`) goes through `cmd_dispatch()`; one CRLF conversion point; integer-only wire (HH:MM → minutes; mV; percent; seconds); string args keep case; locked rows visible as `[unlock]` → `ERR LOCKED`. Limits: line ≤ 191 chars, 12 tokens; forwarded line ≤ 128, forwarded reply ≤ 125 (truncated at a line boundary); CLI/HTTP reply buffer 4096.

ERR vocabulary (one token, superset of HillBT): syntax `EMPTY UNKNOWN_CMD BAD_ARGS OUT_OF_RANGE TOO_LONG`; persistence `NVS_OPEN NVS_READ NVS_WRITE CRC_FAIL CFG_VERSION INVALID_FIELD RESTART_REQUIRED MISSING_CHUNK`; gates `AUTH_FAILED LOCKED NOT_LOCAL MASTER_ONLY ZONE_ONLY`; addressing `WRONG_ZONE ZONE_UNKNOWN ZONE_UNASSIGNED ZONE_OFFLINE ZONE_TIMEOUT RING_DOWN ID_CONFLICT`; safety `SAFE_STATE FAULT_ACTIVE INTERLOCK LIMIT BUDGET NOT_CONFIGURED NO_SHELF NO_DEVICE`; misc `BUSY NOT_READY NOT_IMPLEMENTED INTERNAL FW_BUSY FW_NO_IMAGE`.

### 5.2 Table and dispatch

`cmd_entry_t {verbs, area, noun1, noun2, args[]{name,type ∈ INT/ENUM/STR/TIME/MAC, min, max, enums}, n_key, min_args, max_args, flags, handler, desc}`; flags: `ACTUATOR` (mandatory bounded duration arg, audited `NOTIFY CMD`), `UNLOCK`, `MASTER`/`ZONE`, `SESSION`, `SLOW` (≤1 s budget vs 50 ms default). Dispatch: length/tokenize → `ZONE <z>` prefix (strip; master z≠0 → forward; on a zone `0` means "this node" — the Master is never addressed via the prefix — and any id other than its own → `WRONG_ZONE`) → trailing HELP → row match → role/session/unlock gates → arity → typed parse with ranges → handler (pre-parsed values) → audit. `cmd_table_check()` validates the whole table in host tests and at boot. Destructive rows take a literal `<CONFIRM>` argument.

### 5.3 Execution model

One `cmd_task` per node (core 0, prio 5) is the only caller of `cmd_dispatch()`, `hg_model_edit/apply_remote()` and the `safety_request` gate. Sources — CLI, HTTP (2 static slots), ring, internal — allocate from a 4-slot static request pool (the four sources can momentarily want five — one then sees `BUSY`, accepted), enqueue (depth 8) and block on a task notification ≤ 3500 ms. Forwards move to a 4-entry pending table (2/zone) inside the task: immediate `ZONE_UNKNOWN`/`ZONE_OFFLINE`/`ZONE_UNASSIGNED` from node state without ring traffic; deadline = 3×ring-ACK-timeout + 200 ms, **measured from the frame's first transmission by the ring tracker** (the caller's 3500 ms wait bounds total enqueue-to-reply; an entry whose caller has given up is dropped and its late reply discarded); zone reply passed through verbatim; timeout → `ZONE_TIMEOUT` + node marked suspect. **No command-layer retry** (doses are not idempotent); the ring layer's dup cache gives at-most-once. `OK QUEUED`+`NOTIFY` style is used only by `SET FW ZONE` (genuinely outlives a request). Handlers never do NVS/I²C/blocking I/O — actuator rows call the pure shelf/safety model; `SAVE` waits on the store task's completion semaphore.

### 5.4 Vocabulary (V1, by area — full usage strings generated from the table)

SYSTEM `HELP · GET ID/VERSION/STATUS · REBOOT <CONFIRM> · FACTORY RESET <CONFIRM>`; CONFIG `SAVE · GET CONFIG` (replayable DUMP) + the field grammar `SET <GROUP> <shelf> <KEY> <v>` of §4.6; SESSION `SET ECHO · SET NOTIFY <TYPE|ALL> ON|OFF · SET LOG <level> [tag] · DEBUG ENABLE <key>`; TIME `GET/SET TIME`; FIRMWARE `GET FW · SET FW ROLLBACK <CONFIRM> [unlock] · SET FW UPDATE <ssid> <pass> <url> [zone, unlock, local console only — writes the handover verbatim; the ring path never carries a URL] · SET FW ZONE <z>/ZONES/ABORT [master]`; RING/NODES `GET RING · GET NODES/NODE/UNASSIGNED/PING · SET NODE <z> MAC|NAME · CLEAR NODE <z>`; SHELF/actuators (zone) `SET SHELVES <n> · SET SHELF <s> CROP|ENABLE · SET AUTO <s>` (releases every override on the shelf) · `SET LIGHT <s> <w> <r> <minutes> · SET PUMP <s> <seconds> · SET FAN <s> <minutes> · SET VIB <seconds>` — a 0 value releases that actuator's override, the controller resumes next tick; SENSORS `GET SOIL/SENSORS · SET CAL <s> <A|B> <DRY|WET> <mv> · SET SOILCAP <s> <A|B> <DRY|WET>` (captures the current filtered mV as that calibration point); FAULTS `GET FAULTS · CLEAR FAULT <CODE|ALL> [shelf] · GET SAFE · SET SAFE ON|OFF [master] · GET OUTPUTS`; MASTER `SET OUT <n> <min> · GET ALARMS · SET WIFI STA|AP · SET NTP · SET TZ · SET PROFILE …`; DEBUG (unlock) `GET I2C SCAN · SET GPIO/PCA RAW/PCF RAW <…> <seconds>` (auto-revert), `GET ADC RAW`.

### 5.5 NOTIFY

`NOTIFY <TYPE> <node> <payload>`; types BOOT, ALARM, SAFE, NODE, RING, WATER, LIGHT, SOIL, FW, CMD with per-(type,idx) minimum intervals; sinks: CLI session mask (`SET NOTIFY`), zone→ring session (edge events by default; SOIL/LIGHT are bring-up aids, off by default — periodic telemetry rides the heartbeat), Master alarm manager. No HTTP push in V1 (UI polls 1 Hz).

### 5.6 HTTP mapping

`POST /api/cmd` text/plain line → reply verbatim (200 `OK…`, 422 `ERR…`, 413 too long, 503 busy); `GET /api/help`; `GET /api/state` JSON snapshot from the model mirror (built outside the lock); `/api/config` per §4.6; fw endpoints per §1.4. HTTP session is permanently locked (`UNLOCK` rows → `ERR LOCKED`) and session rows → `ERR NOT_LOCAL`. Auth = AP password (WPA2).

### 5.7 Console line discipline

UART0 115200 (bootloader baud, never switched), driver-installed, one recursive TX mutex shared by replies, NOTIFY and the `esp_log` vprintf hook — lines never interleave. `CONFIG_LOG_VERSION_1=y` pinned with a static assert (v2 emits three vprintf calls per line — verified), `CONFIG_LOG_MAXIMUM_LEVEL_DEBUG=y` so `SET LOG DEBUG` works, colors off. Human mode: echo, BS/DEL, 10×96 B history (ESC[A/B), Ctrl-C, log lines erase/redraw the partial edit line. `SET ECHO OFF` = machine mode (uart_test.py sends it first). Noise bytes dropped and counted.

### 5.8 Debug unlock (decisions, brainstorm)

`DEBUG ENABLE <key>` — key from `CONFIG_HILLGROW_DEBUG_KEY` (default `hill`; release builds override in sdkconfig.defaults); per-session, 600 s idle expiry, never HTTP. **May be forwarded to a zone over the ring** (`SET ZONE 2 DEBUG ENABLE <key>` — the ring is inside the enclosure). Master relay outputs (`SET OUT`) are **not** unlock-gated (duration-bounded, safety-gated, needed by the web UI). AP password default `hillgrow1` (`SET WIFI AP`), same default on the rescue AP.

### 5.9 tools/uart_test.py

pyserial or HTTP (`--http 192.168.7.7`); prologue `SET ECHO OFF, SET LOG WARN, SET NOTIFY ALL OFF, GET ID`; suites: HELP self-consistency (every advertised line round-trips), table-derived error probing (missing arg → BAD_ARGS, max+1 → OUT_OF_RANGE, `[unlock]` → LOCKED), SET→GET round-trips, actuator safety (`--allow-actuators`: dose runs and self-stops, NOTIFY arrives), SAVE, notify filters, forwarding (`--zone N`), log level, soak. Exit code = failures.

### 5.10 Host tests

Tokenizer/limits; two-word noun precedence; typed parse loop over every row; HELP exactness and < 4 KB; role/session/unlock gates incl. expiry; ZONE prefix on both roles; audit hook; table check negatives; pending table full matrix (timeout, late reply, NACK-less design, per-zone cap, truncation); zone rows byte-identical SET→GET against the pure model with fake clock; master rows; notify formatter/rate limits/masks; cli_line editor incl. history, overflow, noise, machine-mode staleness.

---

## 6. Task topology, components, budgets

### 6.1 FreeRTOS tasks

Rules: every app task is created with `xTaskCreatePinnedToCore`, subscribes to the TWDT first, bounds every blocking call ≤ 1000 ms, uses `vTaskDelayUntil` for periodic loops. IDF tasks (wifi 23, esp_timer 22, sys_evt 20, tiT 18) sit above all app tasks; `main` runs the §3.3 boot sequence and deletes itself.

**Zone** — `ring_rx` 0/6/4096 (UART2 events; decode, CRC, forward; consumed frames → `cmd_q` by value; TIME_SYNC/ASSIGN → `hg_link_t` under a critical section; never runs app code) · `cmd_task` 0/5/6144 (§5.3) · `cli0` 0/4/4096 (line editor, replies, log mutex) · `store` 0/2/3072 (sole NVS writer) · `ctrl` 1/5/6144 (100 ms: cfg/hw snapshot, soil sampling one shelf per tick @1 Hz/sensor, light/water/fan/pollination controllers, `safety_tick`, PCA/PCF writes, heartbeat 2 s + fault-edge, RTC daily counters, `rt` publish; sole I²C+ADC caller). Stacks ≈ 23.5 KB.

**Master** — as zone (its `ctrl` drives DS3231 60 s / SHT31 10 s / reservoir inputs / vent-reservoir-shutter controllers / PCF relays) plus `node_mgr` 1/4/6144 (50 ms: ring tracker + TIME_SYNC broadcast; 1 s: health + break analysis, sync decisions, enrolment, fleet-OTA sequencer, alarm diff, history sampler) and `store` 0/2/6144 (NVS + LittleFS alarms/history/retention); IDF `httpd` 8192/core 0 (handlers only enqueue commands or copy snapshots). Stacks ≈ 41 KB.

**Rescue** — `main` (pull: read+erase handover → STA join 20 s → `esp_http_client` GET → `esp_ota_begin/write/end` → set boot → restart; 3 failures or no handover → manual AP `HillGrow-Rescue-<MAC6>` @192.168.7.7 with one upload page) · `ring_fwd` (UART2 byte repeater) · LED patterns via esp_timer. TWDT 30 s here.

Inter-task traffic is queues-by-value (`cmd_q` 8, `node_q` 16, `ring_tx_q` 8, `store_q` 32), task notifications for replies, two mutexes (model, app-state; µs holds, no I/O under either), one critical section (`hg_link_t`), atomics for flags (`flash_busy`, `inhibit_mask`, time state). Full channel-by-channel table maintained in the implementation plans.

### 6.2 Component map

~50 flat components (`components/<name>/{<name>.h,.c,CMakeLists.txt}`, files ≤ ~300 lines), split along one line — **pure C, no IDF headers, host-tested** vs **target glue**:

- Protocol: `ring_proto`* · `ring_link`
- Command: `cmd_core`* · `cli_line`* · `notify`* · `cmd_common`* · `zone_cmds`* · `cmd_task` · `cli`
- Config/persist: `hg_cfg`* · `hg_blob`* · `hg_xfer`* · `hg_sync`* · `hg_model`* · `hg_master_model`* · `hg_store` · `hg_json`* (Master) · `hg_fs` (Master)
- Safety: `safety`* · `fault`* · `soil_valid`* · `boot_record`* · `ota_trial`*
- Control: `sched`* · `light_ctrl`* · `water_ctrl`* · `fan_ctrl`* · `polli_ctrl`* · `shelf`* · `vent_ctrl`* · `reservoir_ctrl`* · `shutter_ctrl`* · `psychro`*
- Drivers: `i2c_bus` · `act_hal` · `pca9685`(codec*/glue) · `pcf8575`(codec*/glue) · `soil_adc` · `soil_filter`* · `ds3231`(codec*/glue) · `sht31`(codec*/glue)
- Master services: `node_mgr` · `fleet_ota`(sequencer*/glue) · `alarm_mgr`* · `history`* · `time_core`* · `time_svc` · `wifi_mgr` · `http_srv`
- Shared infra: `board` (pins per role) · `app_version`* · `rescue_handover`(codec*/glue)

(*) = pure, one `test_<module>.c` each. Managed components: `espressif/cjson`, `joltwallet/littlefs` (Master only); `dependencies.lock` committed. `bootloader_components/main/` holds the shared bootloader override.

### 6.3 Versioning

`version.txt` at repo root: strict `MAJOR.MINOR.PATCH`, the single `PROJECT_VER` for all three apps per release; a CMake check fails the build on any other format; `app_version` parses it to the 3 heartbeat bytes. Fleet-update ordering for grammar-affecting releases: zones first, Master last.

### 6.4 Budgets

| Image | Estimate | Slot | Headroom |
|---|---|---|---|
| zone.bin | ~650 KB | 1280 KB (smaller OTA slot) | 2× |
| master.bin | ~1.4–1.5 MB | 2048 KB | ~550 KB |
| rescue.bin | ~900–1000 KB (`-Os`, IPv6/WPA3/HTTPS off, log WARN) | 1280 KB | ~280 KB |

RAM: zone free heap ≈ 220 KB after boot; Master ≈ 95–110 KB (Wi-Fi buffer counts and `max_open_sockets 4` must not grow unmeasured; `GET STATUS` reports minimum free heap). Zone NVS wear ≥ 1000 years worst case.

### 6.5 History and export (Master `data` partition, 1088 KB LittleFS)

10-minute per-shelf samples kept 3 days (24 B: ts, zone, shelf, soil A/B mV, avg %, white, red, out_flags, pump_today_s) + hourly aggregates kept 30 days (32 B: min/avg/max soil, light minutes, pump seconds, doses) ≈ 1045 KB at the full 8-zone/32-shelf fleet — above the ~960 KB usable before the deletion threshold, so at maximum fleet hourly retention degrades to ≈ 26 days (retention is best-effort; the oldest-file sweep is the guarantee); day files `/data/h10/YYYYMMDD.bin`, `/data/h60/YYYYMM.bin`; oldest deleted when free < 128 KB. Alarm log 2×64 KB rotating. Web UI: `/api/history?range=…` for plots and **`/api/history/export`** streaming the hourly aggregates as a plain-text (tab-separated) file download (decision: owner requirement). All records written only by the `store` task; data comes from heartbeats — zones store nothing.

---

## 7. Sub-project roadmap and first milestone

| # | Sub-project | Delivers | Done criteria |
|---|---|---|---|
| 1 | Foundation | Repo + three app skeletons + bootloader override + host-test harness as pre-build gate + `cmd_core`/`cli` + `hg_cfg/blob/model/store` + `board`/`app_version` + rescue app | All three images build; host tests green and gating; both board types boot to a CLI that answers `HELP`/`GET ID`/`SET LOG`; config survives reboot; bootloader RTC-flag and GPIO15 paths land in rescue; rescue manual AP serves the upload page and flashes an OTA slot on the bench |
| 2 | Zone shelf control | `safety`/`fault`/`soil_*`/`act_hal`/`pca9685`/`pcf8575`/controllers/`shelf` + zone rows | A standalone zone grows a shelf from its CLI: calibrated soil readings, scheduled light with caps, hysteresis watering with all limits, manual overrides, faults latching/clearing; `uart_test.py --suite full` passes on hardware |
| 3 | Ring link | `ring_proto`/`ring_link`/`node_mgr` + forwarding + enrolment + config push + TIME_SYNC + FW_UPDATE handshake | Master + 2 zones: discovery, `SET ZONE 2 …` round-trips, heartbeats drive the node table, break blame correct when a node is unplugged, config pushed and reverted per §4.4, fleet OTA of one zone end-to-end via rescue pull |
| 4 | Master web UI | `wifi_mgr`/`http_srv`/`hg_json`/`hg_fs`/`alarm_mgr`/`history` + pages (gzipped `EMBED_FILES` assets, vanilla JS) | Phone on the AP at 192.168.7.7: dashboard of all zones/shelves, config editing, alarms, history plots + text export, both firmware uploads, zone fleet update button |
| 5 | Master global control | `time_svc`/`ds3231`/`sht31`/`psychro`/`vent_ctrl`/`reservoir_ctrl`/`shutter_ctrl` + Master pin map | Ventilation strategy on dew point with damper interlocks, reservoir refill with all guards, blackout shutter, overall-grow-light schedule, `inhibit_mask` propagation — hardware details fixed in the SP5 feature spec first |
| 6 | Display node | `display/` app: ESP32-S3-DevKitC-1 + ST7796S 480×320 touch (HillBT-s3 hardware), LVGL 9, UART API client; Master gains a UART1 machine-mode CLI session + NOTIFY sink | One-screen touch panel shows live status and runs API commands; Master unaffected when the display is absent (§11.1) |
| 7 | Media & storage | microSD (SPI2, FAT) + I²S → PCM5102A + audio player component | WAV playback from SD controlled via CLI/web/display without disturbing control loops (§11.2–11.3) |

Each sub-project gets its own dated feature spec (where §-level detail is still open: SP2 controller state machines + optional DLI; SP4 page inventory; SP5 hardware) and implementation plan. **First milestone** = SP1 done criteria; first hardware smoke test: two DevKitCs, `HELP` on both consoles, rescue upload page reachable.

---

## 8. Test strategy

> **The Rule:** any module with no hardware or RTOS dependency must have a host-compiled Unity test; a module is complete only when its test passes. Per-module test lists live in §2.11/§3.13/§4.9/§5.10.

Host harness: `tests/host/CMakeLists.txt`, FetchContent Unity 2.6.0 + cJSON (for `hg_json`), MSVC on this machine (`--config Release`), one executable per `test_<module>.c`, fakes in `tests/host/fakes/` (fake clock, fake `app_if`, fake ring sink — a missing fake is a link error). Each app's root CMake gates `<app>.elf` on the ctest stamp, skippable with `-DHILLGROW_SKIP_HOST_TESTS=ON`; the DEPENDS glob covers `components/*/*.c` **and** `tests/host/*.c` (HillBT's gap). On-hardware: `tools/uart_test.py` (§5.9) per role, plus a ring soak script in SP3. Wire-level correctness is anchored by byte-exact frame vectors computed independently during design.

---

## 9. Code quality rules

C (C11/gnu23 as compiled by IDF 6, `-Wall -Werror -Wextra`); one `.h`/`.c` pair per module, public API only in the header, `<name>_` prefix; no file > ~300 lines; no globals across layers (volatile single-writer flags documented at their definition); comments only where the why is non-obvious; every module enters with a test. No `ESP_ERROR_CHECK` on runtime driver calls — log, map to fault, degrade. No printf/blocking I/O in control loops or ISRs; no `delay()`-style waits in control paths — state machines + timestamps. All buffers static and bounded; no heap per request/frame. Wire/persisted data packed by explicit byte offset, little-endian, `_Static_assert`ed sizes — never struct casts. No NVS/I²C/UART under any mutex. Conventional commits `type(scope): summary`, scope = component.

---

## 10. Deferred and open items

- SP2 spec: light/watering/pollination state machines (tables + timers), ramp semantics across time steps, optional DLI accounting (per-shelf PPFD constant, early-off at target).
- SP4 spec: page inventory, asset pipeline (default: gzipped `EMBED_FILES`, vanilla JS, no bundler), `/api/history` query shape and the text-export format.
- SP5 spec: damper type (2-/3-wire, end switches), reservoir sensing (floats vs load cell), climate sensor (SHT31 default), Master PCF8575 relay map + `docs/pin-mapping.md` Master section, NC high-level float backstop.
- Hardware bring-up checklist lives in `docs/pin-mapping.md` (active-low PCF loads, OE pull-up, ring RX pull-ups, soil pull-downs, I²C pull-ups, measure before connecting).
- Pump limit defaults (60 s/600 s) are safe-direction placeholders — re-derive from measured peristaltic flow and pot volume during SP2 bring-up.

---

## 11. Owner extensions (added 2026-09-01)

### 11.1 Display node — ESP32-S3 touch panel (new sub-project 6)

A dedicated `display/` app on an **ESP32-S3-DevKitC-1 (N8R8)** with the proven HillBT-s3 display hardware, reused nearly unchanged: 3.5″ ST7796S 480×320 over SPI (40/80 MHz) + XPT2046 touch on a second SPI bus, LVGL 9 via `esp_lvgl_port`, no Wi-Fi/BT, single factory app (USB flashing; no rescue/AB for this node). It connects to the Master over a plain UART and is **just another API client**: it sends CLI lines and consumes `OK/ERR` + `NOTIFY` (the HillBT `wrover_link` pattern). Master side: a second machine-mode CLI session on **UART1 (GPIO26 TX → S3, GPIO25 RX ← S3, 115200)** plus a NOTIFY sink — no new command surface, no bypass of anything. Deliberately NOT a web-UI duplicate: V1 is **one screen** — status icons (zones online, faults, ring, reservoir) + a few live values + a couple of touch buttons mapped to API commands; expanded later. The Master runs identically with the display absent.

### 11.2 SD card on the Master (new sub-project 7)

MicroSD (3.3 V SPI module) on **SPI2: SCK GPIO14 · MOSI GPIO13 · MISO GPIO27 · CS GPIO4**, FAT via `esp_vfs_fat`/`sdspi`, hot-plug tolerant (mount on demand, all consumers survive a missing card). Purpose: **media storage** (audio, bulk file drops). Alarms and history stay on internal LittleFS — the system's records never depend on a removable card.

### 11.3 Audio out — I²S → PCM5102A (new sub-project 7)

**I²S: BCK GPIO33 · WS/LRCK GPIO32 · DOUT GPIO23** to a GY-PCM5102 DAC board (its SCK pin tied to GND → internal PLL). Player streams audio files from the SD card: **WAV in V1**, MP3 via a software decoder as a stretch goal decided in the SP7 feature spec. Control through the normal command surface (CLI + web + display node): PLAY/STOP/NEXT, playlist folder, soft volume; `NOTIFY` on track change. The player runs as an isolated task (I²S DMA + its own file-read task, ~24 KB buffers) and must never delay control, ring or safety work; audio RAM is accounted against the Master's free-heap budget (§6.4).

### 11.4 Reservoir sensing pins (allocated now, implemented in SP5)

**GPIO34 = analog level input (ADC1_CH6)** for a level/pressure sensor or a load-cell amplifier output; **GPIO35 = LOW float, GPIO36 = HIGH float** (input-only pins — external pull-ups required). GPIO39 stays the last spare analog input. The NC high-level float in series with the refill solenoid coil remains the hardware backstop. If SP5 chooses an HX711 load cell instead, its clock/data pair takes GPIO39 + one strapping-care pin, decided there.

With these allocations the Master's directly usable GPIO set is fully assigned except GPIO39 (and 0/5 with care); further Master I/O grows on the buses (PCF8575 has 9 spare pins, more I²C devices, PCA9685 option).

---

*Sections approved one by one in brainstorming, 2026-08 (repo layout & flash, ring, safety, config, command core, topology/budgets). Detailed rationale, rejected alternatives and verified-against-IDF citations are preserved in the design-panel records.*
