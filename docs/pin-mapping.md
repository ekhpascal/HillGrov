# HillGrow — Pin Mapping & Hardware Rules

**Status:** Work in progress — the Master global-equipment section (relay map, reservoir inputs, damper wiring) lands with the SP5 feature spec.
**Board:** ESP32-DevKitC-V4 (ESP32 classic), both roles. Companion to `docs/superpowers/specs/2026-08-31-hillgrow-system-design.md` §1.3 — **this file wins for pins.**

## Fixed GPIO assignments (Master and Zone)

| Function | GPIO | Notes |
|---|---|---|
| UART0 console + CLI | 1 (TX), 3 (RX) | on-board USB-UART, 115200 |
| Ring UART2 RX | 18 | from upstream node's TX; internal pull-up + external 10 kΩ |
| Ring UART2 TX | 19 | to downstream node's RX |
| I²C SDA / SCL | 21 / 22 | one bus, all expansion modules, 400 kHz |
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
| 0x20 | PCF8575 | relays (9 used, 7 spare): main fan, 3 dampers, blind open (P8) + blind close (P9), refill solenoid, overall grow light, heater — pin map fixed in SP5 |
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
- [ ] 10 kΩ pull-up on every ring **RX** (GPIO18) so an unpowered upstream reads idle-high; twisted pair + GND per link; 3.3 V TTL point-to-point.
- [ ] 100 kΩ pull-down on **every soil ADC input** — an unplugged SEN0193 must read ≈ 0 (open-sensor detection depends on it).
- [ ] 4.7 kΩ I²C pull-ups on GPIO21/22 (once per bus). Master: audit every breakout's onboard pull-ups (DS3231, STCC4, AHT20/BMP280, SC16IS752 boards all ship their own) — strip extras so the parallel total stays ≈ 2.2–4.7 kΩ; run 100 kHz; keep total SDA/SCL wiring < ~1 m (far sensors belong on the field bus).
- [ ] Rescue button: momentary switch GPIO15 → GND.
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
