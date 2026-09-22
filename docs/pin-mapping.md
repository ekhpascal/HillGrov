# HillGrow — Pin Mapping & Hardware Rules

**Status:** Work in progress — the Master global-equipment section (relay map, reservoir inputs, damper wiring) lands with the SP5 feature spec.
**Board:** ESP32-DevKitC-V4 (ESP32 classic), both roles. Companion to `docs/superpowers/specs/2026-08-31-hillgrow-system-design.md` §1.3 — **this file wins for pins.**

## Fixed GPIO assignments (Master and Zone)

| Function | GPIO | Notes |
|---|---|---|
| UART0 console + CLI | 1 (TX), 3 (RX) | on-board USB-UART, 115200 |
| Ring UART2 RX | 18 | from upstream node's TX; internal pull-up + external 10 kΩ |
| Ring UART2 TX | 19 | to downstream node's RX |
| I²C SDA / SCL | 21 / 22 | one bus, all expansion modules; 400 kHz (Zone) · 100 kHz (Master — larger bus, see checklist) |
| Rescue button | 15 → GND | bootloader: hold ≥10 s = rescue, 1–9 s = NVS erase |
| Status LED | 2 | on-board LED, driven only after boot |

## Zone-only

| Function | GPIO | Notes |
|---|---|---|
| PCA9685 OE | 23 | active-low output-enable; **external 10 kΩ pull-up to 3.3 V** |
| Soil ADC1 ch A1..A6 | 32, 33, 34, 35, 36, 39 | ADC1_CH4/5/6/7/0/3; 34–39 are input-only |
| Soil ADC2 ch A7..A8 | 25, 26 | ADC2_CH8/9 — valid **only** because `zone.bin` never starts Wi-Fi |
| Spare | 4, 5, 13, 14, 27 | 5 is high at reset (strapping) |

### Zone I²C devices

| Addr | Device | Channels |
|---|---|---|
| 0x40 | PCA9685 @ ~1017 Hz | ch 0..7 = shelf n WHITE `2(n−1)` / RED `2(n−1)+1`; ch 8..11 = vibrator shelf 1..4 (PWM intensity); ch 12..15 spare (future fan PWM) |
| 0x20 | PCF8575 (all pins **active-low**) | flat numbering P0–P15: P0–P3 pumps 1–4 · P4–P7 fans 1–4 · P8–P15 spare relays |

## Master-only

| Addr | Device | Notes |
|---|---|---|
| 0x68 | DS3231 RTC | time authority; AT24C32 EEPROM at 0x57 unused in V1 |
| 0x20 | PCF8575 | relays (9 used, 7 spare): main fan, 3 dampers, blind open (P8) + blind close (P9), refill solenoid, overall grow light, heater (P7, §11.8) — remaining pin map fixed in SP5 |
| 0x64 | STCC4 (DFRobot Gravity) | growth-room CO₂ 400–5000 ppm + T/RH via the module's companion SHT4x (typ. 0x44 — verify in scan) |
| 0x38 | AHT20 | workshop/“inside” temperature/humidity (AHT20+BMP280 combo module) |
| 0x76/0x77 | BMP280 | air pressure (same combo module; address per SDO strap — verify in scan); feeds STCC4 pressure compensation |
| 0x4D | SC16IS752 | I²C→UART bridge → RS-485 Modbus field bus: PAR sensor (400–700 nm); optional far T/RH transmitters later; 2nd UART spare |

### Master pin assignments (owner extensions 2026-09-01)

| Function | GPIO | Notes |
|---|---|---|
| Display link UART1 | 26 (TX → S3 RX), 25 (RX ← S3 TX) | ESP32-S3 touch panel, machine-mode CLI + NOTIFY, 115200 8N1 |
| microSD (SPI2) | 14 SCK · 13 MOSI · 27 MISO · 4 CS | 3.3 V SPI module, FAT; media only — alarms/history stay on internal flash |
| I²S audio out | 33 BCK · 32 WS/LRCK · 23 DOUT | GY-PCM5102 DAC; DAC SCK pin tied to GND (internal PLL) |
| Reservoir level (strain gauge) | 34 (HX711 DOUT / analog in) · 5 (HX711 SCK) | GPIO5 high at reset = HX711 power-down (benign); no float switches — manual fill stop |
| Room presence sensor | 39 (digital in, input-only) | PIR / mmWave OUT; external pull per module if open-collector |
| Blind end-stops | 35 (closed) · 36 (open) | input-only — **external pull-ups required** |
| Outdoor temperature (DS18B20) | 0 (1-Wire) | strapping pin — the 4.7 kΩ pull-up to 3.3 V that 1-Wire needs is exactly what the boot strap wants (idle-high = boot-safe); powered 3-wire hookup, no parasite mode |
| Spare | none — master GPIO fully allocated | further master I/O goes on the buses |

Reserved I²C: 0x70 (PCA9685 all-call — never use) · 0x48–0x4B (future ADS1115 — optional; no current sensor needs an ADC: the DS18B20 is 1-Wire digital, not analog).

## Master v2 — ESP32-P4-WIFI6-Touch-LCD-7B (reserved allocations, spec §11.10; effective at the migration sub-project)

Onboard and therefore no longer external: 7" touch display · RTC + coin cell · RS-485 transceiver (own UART, 120 Ω jumpers, non-isolated) · microSD (SDMMC) · audio codec/PA (unused by us — see streamer) · ESP32-C6 Wi-Fi 6 (SDIO). I²C header carries PCF8575 + STCC4 + AHT20/BMP280.

**On-board pin assignments read off the schematic and confirmed on hardware 2026-09-18/21** (`assets/ESP32-P4-WIFI6-Touch-LCD-7B/`):

| On-board peripheral | P4 pins | How confirmed |
|---|---|---|
| ESP32-C6 Wi-Fi (SDIO slot 1) | CLK **18**, CMD **19**, D0–D3 **14–17**, CP reset **54** | schematic net labels, then echoed verbatim by the esp_hosted boot log; these are esp_hosted's stock P4 defaults, so no pin config is needed |
| microSD (SDMMC **slot 0**, IOMUX-fixed) | **39–44** | schematic; **no collision with the IO46–52 header** — the earlier VERIFY item is settled. Being on slot 0 while the C6 is on slot 1 means SD and Wi-Fi coexist |
| 7" LCD | backlight **32**, reset **33** (+ MIPI-DSI lanes) | BSP header `BSP_LCD_BACKLIGHT` / `BSP_LCD_RST` |
| RS-485 transceiver | TXD **26**, RXD **27** | schematic (`485_TXD`/`485_RXD` into the THVD1406 RO/DI) |
| CAN transceiver | TX **22**, RX **21** | schematic (`CANTX`/`CANRX` into the TJA1051) |
| I²C header | SCL **7**, SDA **8** (level-shifted to `D_SCL`/`D_SDA`) | schematic |
| UART0 console / flashing | TX **37**, RX **38** | boot log (`GPIO 38 and 37 are used as console UART I/O pins`) |
| Broken-out GPIO header P3 | 3V3, GND, **IO2–5, IO28–31, IO34, IO36** | silkscreen |
| Broken-out GPIO header P1 | BAT, GND, 3V3, VO4, GND, **IO46–52** | silkscreen |
| C6-UART header (CP recovery) | C6 `TXD`/`RXD`/`IO9` + GND | silkscreen + schematic — a 3.3 V USB-UART adapter here reflashes the co-processor if an OTA ever leaves it unusable |

**Ring UART: use IO28 (TX) and IO29 (RX) from the carefree local-I/O pool**, not the board's "UART" silkscreen header. The header's GPIOs could not be established from the schematic, and the only TTL UART pair broken out on that edge is the RS-485 transceiver's own (26/27), which §11.7 wants for the Modbus field bus. The P4 routes UART through the GPIO matrix, so a dedicated header was always a convenience rather than a requirement — spending two pins from the pool costs nothing and leaves RS-485 intact. (Supersedes the earlier "Ring = the board's dedicated UART header" note.)

| Function | P4 pin | Notes |
|---|---|---|
| Streamer I²S BCLK | IO46 | P4 I²S master TX → ESP32-DevKitC streamer (I²S slave RX); one ribbon: IO46–50 + GND |
| Streamer I²S WS/LRCK | IO47 | |
| Streamer I²S DOUT | IO48 | no MCLK — streamer's PCM5102A runs internal PLL (SCK→GND) |
| Streamer control UART TX | IO49 | → streamer RX; source/volume/DSP commands (future streamer project) |
| Streamer control UART RX | IO50 | ← streamer TX |
| Spare (streamer header) | IO51, IO52 | |
| Rescue button | IO34 → GND | bootloader: hold ≥10 s = rescue, 1–9 s = NVS erase — same contract as the ESP32. **Bench-verified 2026-09-22: IO34 is NOT a boot-mode strap** — the P4 boots normally with IO34 held low at reset (full ROM output, `SPI_FAST_FLASH_BOOT`, second stage runs), which retires the earlier "strapping, use last" caution below and means no pin change is needed. Defined independently in `bootloader_components/main/bootloader_start.c` (`HG_RESCUE_GPIO`) rather than included from `board.h`'s `HG_GPIO_RESCUE_BTN` — the bootloader subproject does not link the `board` component, so the two definitions (both `34`) are kept in sync by hand, per the comment at the P4 `#define` in each file |
| Local I/O pool | IO28–31 (carefree), IO2–5 (JTAG-shared — fine once USB-JTAG is the debug path), IO36 (strapping — use last, prefer inputs) | for 1-Wire DS18B20, HX711 ×2, presence, blind end-stops ×2, status LED (≈7 of 10; rescue button is IO34, its own row above) |

The DevKitC master tables above remain the SP3-bench reality until the migration sub-project executes.

## Reserved / Unavailable

| GPIO | Reason |
|---|---|
| 6–11 | internal SPI flash — **NEVER USE** |
| 1, 3 | UART0 console/flashing |
| 12 | MTDI strapping — a pull-up selects 1.8 V flash and bricks boot; leave alone |
| 0 | boot strap (on-board BOOT button) — Master: shared with DS18B20 1-Wire (idle-high, boot-safe; see checklist) |
| 2 | strapping + on-board LED — status LED only, no external load |
| 5, 15 | strapping, high at reset (15 doubles as rescue button to GND) |
| 16, 17 | PSRAM on WROVER modules — kept free so either module works |
| 34, 35, 36, 39 | input-only, no internal pulls |

## Hardware rules — bring-up checklist

- [ ] **Every PCF8575 load is active-low** (relay/driver input LOW = energised). The expander powers up all-HIGH and keeps its last word through an ESP32 reset — an active-high load would energise at power-up. Verify with a meter before connecting loads.
- [ ] 10 kΩ pull-up 3.3 V → PCA9685 **OE** (GPIO23): LED outputs must be disabled from power-on until firmware enables them.
- [ ] LED MOSFET gate drivers are non-inverting (gate low = LED off).
- [ ] 10 kΩ pull-up on every ring **RX** (GPIO18) so an unpowered upstream reads idle-high; twisted pair + GND per link; 3.3 V TTL point-to-point. — *Ring protocol itself bench-verified 2026-09-04 on a 3-board DevKitC ring (master + 2 zones, bare jumper wires, no external pull-ups): enrolment, forwarding, reconciliation, time sync. The pull-up rule is about the unpowered-neighbour case and is still unverified.*
- [ ] 100 kΩ pull-down on **every soil ADC input** — an unplugged SEN0193 must read ≈ 0 (open-sensor detection depends on it).
- [ ] 4.7 kΩ I²C pull-ups on GPIO21/22 (once per bus). Master: audit every breakout's onboard pull-ups (DS3231, STCC4, AHT20/BMP280, SC16IS752 boards all ship their own) — strip extras so the parallel total stays ≈ 2.2–4.7 kΩ; run 100 kHz; keep total SDA/SCL wiring < ~1 m (far sensors belong on the field bus).
- [x] Rescue button: momentary switch GPIO15 → GND. — **bench-verified 2026-09-02**: 3.6 s hold → `erase nvs` + defaults; 10 s hold → rescue boot with no WDT reset; ROM-log silence during the hold confirmed (MTDO).
- [ ] GPIO15 rescue button: holding it low at reset silences the ROM bootloader's UART log output (MTDO strap) — the console stays blank until `hg_boot` prints, which is expected, not a fault. JTAG probes on GPIO12–15 fight the button pull — disconnect JTAG before testing the button.
- [ ] 12 V supply sized for **all zones dosing simultaneously** (pump serialization is per-zone only) plus LED load.
- [ ] Refill is operator-supervised (no floats): verify at SP5 bring-up that the valve's max-run timeout closes it even when unattended.
- [ ] Never connect 12/24 V to any ESP32 pin; MOSFET/relay boards isolate all loads.
- [ ] PCM5102A board: SCK pin tied to GND; 3.3 V supply; short I²S wires.
- [ ] Load cell mounted under the reservoir, HX711 board on 3.3 V; tare/full calibration from the CLI (SP5).
- [ ] Display link: straight TX↔RX cross to the S3 (GPIO26→S3-RX, GPIO25←S3-TX), common GND.
- [ ] RS-485 field bus: isolated TTL↔RS-485 module with automatic flow direction (no DE/RE line — pairs with the SC16IS752 TX/RX directly, 3.3 V logic side); A/B twisted pair, 120 Ω termination at both ends; Modbus RTU 9600 8N1; unique slave addresses noted here when assigned.
- [ ] Heater: mechanical thermal cutout/thermostat **in series** with the relay-switched line — mandatory before first power-on (§11.8).
- [ ] Blind: external pull-ups on end-stop inputs GPIO35/36; motor stall-safe or fused; open/close relays verified never simultaneously closed (drive both = firmware bug → report).
- [ ] I²C scan at first boot must match the address map exactly (incl. the STCC4 module's second device, the SHT4x, and the BMP280's strap-dependent 0x76/0x77) — any surprise address = stop and resolve before drivers load.
- [ ] DS18B20: 4.7 kΩ pull-up GPIO0 → 3.3 V; powered 3-wire wiring (VDD/GND/DQ, parasite mode off); BOOT button still usable (pressing it during a read only corrupts that one read); verify auto-flash still triggers with the probe connected.

## Power

| Rail | Feeds |
|---|---|
| 5 V USB / regulated | ESP32 DevKitC, PCA9685/PCF8575 logic (3.3 V from board) |
| 12 V | pumps, fans, vibrator (via drivers) |
| 24 V | V24S LED bars (via MOSFET drivers, PCA9685-controlled) |

All grounds common.
