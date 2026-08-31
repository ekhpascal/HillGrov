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
