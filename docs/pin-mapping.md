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
| 0x40 | PCA9685 @ ~1017 Hz | ch 0..7 = shelf n WHITE `2(n−1)` / RED `2(n−1)+1`; ch 8..15 spare (future fan PWM) |
| 0x20 | PCF8575 (all pins **active-low**) | flat numbering P0–P15: P0–P3 pumps 1–4 · P4–P7 fans 1–4 · P8 pollination vibrator · P9–P15 spare relays |

## Master-only

| Addr | Device | Notes |
|---|---|---|
| 0x68 | DS3231 RTC | time authority; AT24C32 EEPROM at 0x57 unused in V1 |
| 0x20 | PCF8575 | relays (7 used, 9 spare): main fan, 3 dampers, shutter, refill solenoid, overall grow light — pin map fixed in SP5 |
| 0x44 | SHT31 (optional) | room temperature/humidity |

Reserved I²C: 0x70 (PCA9685 all-call — never use) · 0x48/0x49 (future ADS1115).

## Reserved / Unavailable

| GPIO | Reason |
|---|---|
| 6–11 | internal SPI flash — **NEVER USE** |
| 1, 3 | UART0 console/flashing |
| 12 | MTDI strapping — a pull-up selects 1.8 V flash and bricks boot; leave alone |
| 0 | boot strap (on-board BOOT button) |
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
- [ ] 4.7 kΩ I²C pull-ups on GPIO21/22 (once per bus).
- [ ] Rescue button: momentary switch GPIO15 → GND.
- [ ] 12 V supply sized for **all zones dosing simultaneously** (pump serialization is per-zone only) plus LED load.
- [ ] Master (SP5): NC high-level float switch wired **in series with the refill solenoid coil** as the hardware overfill backstop.
- [ ] Never connect 12/24 V to any ESP32 pin; MOSFET/relay boards isolate all loads.

## Power

| Rail | Feeds |
|---|---|
| 5 V USB / regulated | ESP32 DevKitC, PCA9685/PCF8575 logic (3.3 V from board) |
| 12 V | pumps, fans, vibrator (via drivers) |
| 24 V | V24S LED bars (via MOSFET drivers, PCA9685-controlled) |

All grounds common.
