# Master v2 — 7" touch panel UI: design

**Status:** design agreed in brainstorming 2026-09-18 (owner: ekh). Awaiting owner review of this document, then an implementation plan.

**Authority:** the system spec (`2026-08-31-hillgrow-system-design.md`) is binding. §11.10 retires the separate display node (§11.1) and absorbs it into the ESP32-P4 kit — this document is that absorbed work, and it lands as part of **Master v2**, not as a separate node. Where this document and the system spec disagree, the spec wins unless the point is listed under *Decisions*.

**Hardware, proven on the bench 2026-09-18** (see `hillgrow-p4-spike/SPIKE-NOTES.md`): Waveshare ESP32-P4-WIFI6-Touch-LCD-7B, 1024×600 EK79007 over MIPI-DSI, GT911 capacitive touch, LVGL 9 via `waveshare/esp32_p4_wifi6_touch_lcd_7b` 3.0.1, panel up 283 ms after boot, ~30 MB PSRAM free.

## Scope

A native LVGL touch UI on the Master, with **full functional parity** with the SP4 web UI: dashboard, per-zone view with console, schema-driven config editing for every zone and the master, alarms, and system functions (Wi-Fi, time, web password, firmware, fleet, reboot). Plus the home screen and panel-local settings.

**The Audio icon is a placeholder in this sub-project.** Audio playback belongs to SP7 (media & storage: microSD, I²S, PCM5102A), which is not built. The home screen reserves its place and the icon opens a screen saying so; the actual player is designed when SP7 is.

**Non-goals.** Rendering the SP4 web UI itself — there is no browser for the ESP32-P4, and this is not a gap to be closed (see *Decisions* 1). History and plots remain SP4b, deferred until SP2 produces real telemetry. Remote access stays the web UI's job; the panel is local-only.

**On size.** Full parity plus a shell is a large surface — comparable to SP4 itself. The implementation plan is expected to sequence it (shell and read-only views first, then config, then the system functions), so that each stage is independently benchable. That is a planning concern, not an open design question.

## Decisions (2026-09-18)

1. **"Same app on both" means same brain, two faces — not shared presentation code.** No browser exists for the P4, so the phone UI cannot render on the panel. What must not diverge is the *behaviour*, and that is guaranteed by three shared layers: one command surface (`cmd_core`), one state model (`state_snap`), one design language. A 7" panel at arm's length and a 360 px phone in the hand want different layouts anyway; forcing either into the other's shape makes both worse.
2. **Full parity, not ambient-only.** The panel does everything the phone does. Owner's call, against a recommendation of ambient-plus-quick-actions. The cost lands almost entirely on text entry and firmware upload (see *Consequences*); the config editor is cheaper than it looks because it is generated from the existing field tables.
3. **No authentication on the panel.** Physical access is the gate. Consequences are recorded below and accepted deliberately.
4. **Shared service layer**, not loopback HTTP and not unguarded direct calls. Both faces call the layer beneath HTTP.
5. **The clock is the hero of the home screen**, with an ambient status band beneath it. The panel is furniture most of the time; it should be worth looking at, and a glance should answer "is everything all right?" without a tap.

## Consequences of no panel auth (accepted)

- **The panel is a deliberate password-recovery path.** Anyone standing at it can change the web password without knowing the old one. This is useful — forget the password, walk to the greenhouse — and it makes the panel the weakest link by design. That is the trade being made.
- **Secrets are masked on screen by default anyway.** The web UI omits Wi-Fi passwords unless `?secrets=1`. With no panel login, rendering the house Wi-Fi password to anyone walking past is a poor default, so secret fields show masked with a deliberate reveal action. Absence of a login is not a reason to publish secrets to the room.

## Shell — the home screen

```
┌──────────────────────────────────────────────────────────────┐
│ ● ring OK                                        NTP    wifi │
│                                                              │
│                    14:32                    (very large)     │
│                                                              │
│              Thursday 18 September                           │
│                lights off in 2h 14m                          │
│                                                              │
│  ┌────┐┌────┐┌────┐┌────┐┌────┐┌────┐┌────┐┌────┐            │
│  │Z1 ●││Z2 ●││Z3 ●││Z4 ●││Z5 ●││Z6 ●││Z7 ●││Z8 ●│            │
│  │22.4││21.9││22.1││ -- ││22.8││21.4││22.0││21.7│            │
│  └────┘└────┘└────┘└────┘└────┘└────┘└────┘└────┘            │
│                                                              │
│      ┌────────┐      ┌────────┐      ┌────────┐              │
│      │HillGrow│      │ Audio  │      │ Panel  │              │
│      └────────┘      └────────┘      └────────┘              │
└──────────────────────────────────────────────────────────────┘
```

- **Clock.** Large enough to read across the greenhouse. LVGL's built-in fonts stop at 48 px and bitmap fonts do not scale up cleanly, so the clock needs either a TrueType subset (digits and colon, ~11 glyphs) compiled at the target size, or FreeType — which the BSP already pulls in — rasterising at runtime. The subset is crisper and free at runtime and is the default choice; FreeType stays available if arbitrary large text is wanted later. This is implementation latitude, not a design question.
- **Context line.** "Lights off in 2h 14m", "next watering 06:00". The master owns every schedule, so this costs nothing and is the most useful sentence a greenhouse clock can carry.
- **Status band.** One tile per *enrolled* zone, built from `state_snap`'s live node list — never a fixed count. A newly enrolled zone appears; a retired one disappears. It is a flex row dividing the available width, so **1 to `HG_MAX_ZONES` (8) all look deliberate**: at ~115 px per tile (8 zones on 1024 px) the content degrades to id, health dot and one reading; with fewer zones it grows to name, health and two readings. Tapping a tile opens that zone.
- **Alarms** turn the band red and pulse; tapping opens Alarms. No full-screen takeover except SAFE mode — a panel that hijacks itself is one the operator stops trusting.
- **Night dimming.** The panel knows the light schedule and dims when the lights go off, waking on touch.

The third icon is **Panel**, not "Settings": system settings (Wi-Fi, time, password, firmware, fleet) live inside the HillGrow app exactly as they do on the web, and Panel holds display-local preferences — brightness, dim schedule, clock face (including an analogue option), orientation, about. Two different places both called Settings is a trap worth avoiding up front.

## The HillGrow app

Five destinations, matching the web UI: Dashboard, Zone (with console), Config, Alarms, System.

**Navigation is a persistent left rail**, not the phone's stacked hamburger. At 1024×600 there is room for it, so navigation costs no taps and the operator always knows where they are. This is the one place the panel deliberately does not copy the phone.

**Config is generated, not hand-built.** It is the largest surface and the most text-entry-prone. The same field tables that feed `/api/schema` (`hg_cfg_fields`, `hg_mcfg`) drive an LVGL editor, so every zone, shelf and master field — including ones added later — comes from one generator. Fields render **by type, not as text boxes**: numbers get steppers bounded by the table's own min/max, booleans get switches, enums get segmented controls or rollers, and only genuinely free-text fields (name, SSID, timezone, MAC) summon the keyboard. Same fields and same validation as the web, with a fraction of the typing.

**Firmware comes from the microSD slot.** The phone picks a `.bin` from its filesystem; the panel has none. The board's microSD sits on SDIO **slot 0** while the C6 sits on slot 1, so they do not collide (confirmed from the schematic and the bring-up). "Drop a `.bin` on a card, walk to the greenhouse, tap update" is arguably a better story than the phone's for a device standing next to the hardware.

## Architecture

- **`panel_ui`** — the LVGL screens. Master-only. Its pure helpers (layout maths, the config-widget generator, formatting) are host-testable; the widget tree is not.
- **`panel_svc`** — the shared layer both faces call. Most of it already exists: `cmd_core`, `state_snap`, `node_mgr_cfg_get/set`, `mcfg_store`. The real work is **moving the master-config read-modify-write and its `net_ops` lock out of `master/main` into a component**, so the panel and the HTTP handler take the same lock rather than two different ones. `http_api_cfg.c` currently reaches that lock through `extern` declarations its own comments call awkward; this removes them.

The HTTP server keeps running. The phone remains a first-class client, and both faces now enter through the same door.

### The rule

**The LVGL task must never make a blocking call.**

`node_mgr_cfg_set` is safe — it queues and returns. A console command is not: it goes out over the ring and can take seconds. Run under the LVGL lock, that freezes the whole panel mid-animation, and on a touch UI a freeze reads as *broken*, not *busy*. So every write and every command is handed to a **panel worker task**; the UI shows a pending state and updates on completion. This is the same shape `http_cmd.c` already uses with its worker slots, so it is a known pattern rather than a new invention.

### Data flow

- **Read.** Poll `state_snap` about once a second, diff against what is displayed, update only what changed. Cheaper than the web UI's 2 s full re-render, and LVGL widgets keep their own state, so the class of bug behind SP4's caret jump cannot occur here.
- **Write.** UI thread validates locally against the field table, hands the change to the worker, shows pending. Worker calls `panel_svc`, which applies the same guards and the same `hg_cfg_validate` the web path uses. The result returns to the UI as success or the same error the web UI would have shown.

## Error handling

Failures reuse the web UI's semantics so a refusal means the same thing in both faces: busy (a save already in flight for that zone), validation (with the offending field named and highlighted, as the web does), zone offline, and master-config contention. The clock reports an unset time honestly rather than showing a plausible wrong one — SP4 established that a confidently wrong reading is worse than an obviously absent one.

## Testing

Three layers:

1. **Host tests** for the config-widget generator — given a field table, assert the widget kind, bounds and read-only flags it produces — and for `panel_svc`, which is largely already covered.
2. **Existing host suite** must stay green; the `panel_svc` refactor touches code the web UI depends on, so the 32 existing tests are the regression net for that move.
3. **Bench acceptance with the owner**, in the shape of SP4's Task 17. It must include **tapping every destination once on real glass**: the bring-up on 2026-09-18 had touch mapped 180° out while every log line reported success, and only a human finger found it.

## Open implementation latitude (not open questions)

The clock font route (TrueType subset vs FreeType), the exact dim schedule behaviour, and the analogue clock face are implementation choices to be settled in the plan. Nothing here blocks planning.
