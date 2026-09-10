# SP4 — Master web UI: design

**Status:** approved in brainstorm 2026-09-10 (owner: ekh).
**Authority:** the system spec (`2026-08-31-hillgrow-system-design.md`) is binding; this document adds the SP4 integration design and the decisions taken 2026-09-10. Where this doc and the system spec disagree, the system spec wins unless the point is listed under *Decisions*.

## Scope

Deliver spec §7 SP4 **core**: `wifi_mgr` (AP+STA), `time_svc`/`time_core` (SNTP, timezone), `hg_mcfg`, `web_auth`, `hg_json`, `alarm_mgr` (RAM), `state_snap`, `http_srv` + the JSON/text API, and one responsive vanilla-JS page set; master and zone firmware upload from the browser; fleet update button; `node_mgr_cfg_set/get`; the small SP3 carries listed under *Carries*. Bench: Master DevKitC + 2 zones (the SP3 rig), desktop Chrome and Android Chrome.

**Non-goals (SP4b, right after SP2 — when heartbeats carry real telemetry):** LittleFS + the store-task path, history sampler, plots, TSV export, alarm-log persistence, profiles UI (§4.7). **Later or never:** WebSocket transport, TLS, per-user accounts, `GET PING`.

## Decisions (2026-09-10)

1. **Core now, history later.** SP2 is not built; the dashboard shows health, ring, names and full config today and renders the SP2 telemetry fields as a dash until they are non-zero. History infrastructure is deferred to SP4b so it is validated against real data.
2. **House Wi-Fi first, AP as fallback.** The master runs AP+STA. Daily access is over the house Wi-Fi at the STA address (router DHCP reservation recommended; `hillgrow.local` via mDNS for desktops — Android Chrome does not resolve `.local` reliably, so the phone uses the IP, which the master prints on the console at join, shows in `GET WIFI`, on the AP landing page and later on the P4 display). The AP stays on as fallback and for provisioning; Android's no-internet handling of the AP is accepted for that rarer path.
3. **Real server-side login.** One web password (default the literal `hillgrow1` — the same default as the AP password, but independent of it once either is changed; UI banner until both defaults are gone), salted SHA-256, `POST /api/login` → random 128-bit token in an `HttpOnly` cookie, up to 4 sessions with 30-day expiry persisted in NVS (`sess`) so a master reboot does not log the operator out. Every `/api/*` route except login requires it; the page and its assets are public. 5 failures → 60 s lockout. Plain HTTP (TLS would make Android Chrome refuse a self-signed page). KraftWerk's client-side gate is explicitly NOT copied.
4. **Approach A: the command core over HTTP + 2 s snapshot polling.** `POST /api/cmd` runs any CLI line through the dispatcher as an HTTP session (locked, `NOT_LOCAL` for session rows) — same validation, HELP and error texts as the console; SP5 rows appear in the web for free. One `GET /api/state` document polled every 2 s (the heartbeat cadence). WebSocket push (KraftWerk's transport) is deferred: it buys nothing visible at a 2 s data cadence and brings keepalive/reconnect/socket-budget work; the grammar can move onto a WebSocket later without API changes.
5. **esp_http_server, not Mongoose.** Already in master (fleet endpoint) and rescue, proven on the bench, runs unchanged on the P4 over esp_hosted. Mongoose's strengths (raw TCP listeners, reverse proxy, host-native serving) are not needed. KraftWerk's scars are kept: reject chunked uploads, yield to the watchdog during flash writes, static route table, mDNS.
6. **Vanilla JS, ≤ 64 KB gzipped total**, no framework, no build step beyond gzip; CMake fails the build above the cap.
7. **Timezone** stored as a POSIX TZ string (default `CET-1CEST,M3.5.0,M10.5.0/3`, i.e. Copenhagen); TIME_SYNC `utc_offset_s` carries the current offset so SP2 schedules run in local time.

## Network and access

`wifi_mgr` (glue, replaces `wifi_ap`): AP+STA; AP SSID/password configurable (defaults `HillGrow`/`hillgrow1`); STA credentials from `mcfg`; on boot joins with DHCP, hostname `hillgrow`, reconnect backoff 5 s → 60 s forever; STA failures never affect the AP or the ring; mDNS `_http._tcp` `hillgrow.local`; `NOTIFY WIFI STA UP <ip>` / `STA DOWN <reason>` / `AP <n> clients` edges (new `NTF_WIFI` type). Provisioning: AP page (scan → pick SSID → password) or CLI `SET WIFI STA <ssid> <pass>` (no spaces, as documented). `fw_srv` keeps `/fw/zone.bin` but registers on the shared httpd.

`time_core` (pure) + `time_svc` (glue): SNTP whenever STA is up (server from `mcfg`, default `pool.ntp.org`), quality `NTP`; `SET TIME` stays the manual path (`SET`); NTP steps of more than 2 s apply via `settimeofday`, smaller ones are ignored; POSIX TZ → offset at a given epoch (host-tested across DST). `GET TIME` shows `NTP`; `node_mgr` reads the offset for TIME_SYNC.

P4 portability: `wifi_mgr` is a thin layer over the esp_wifi API, which esp_wifi_remote reproduces on the P4; httpd and mDNS ride the same netif.

## HTTP API

One httpd instance: port 80, stack 8192, `max_open_sockets 4`, LRU purge, static route table `{method, path, auth_required, handler}`. JSON unless stated.

| Route | Behaviour |
|---|---|
| `POST /api/login` `{password}` → 204 + cookie; `POST /api/logout` | see Decision 3 |
| `POST /api/cmd` (text/plain line) | reply verbatim as text/plain: 200 `OK…`, 422 `ERR…`, 413 too long, 503 busy. One in-flight per HTTP session (two static sessions) |
| `GET /api/help` | HELP text |
| `GET /api/state` | master {version, uptime_s, heap_min, time, time_src, wifi{sta{up,ip,ssid,rssi,reason}, ap{ssid,clients,ip}}, fw{slot,state,other,upload}}, ring {state, blame, rx_crc, rx_uart, rx_drop, fwd}, nodes[] {id, name, mac, health, fw, gen, hops, link_flags, cfg_sync, hb_age_s, hb{…SP2 fields as carried}}, fleet {state, zone, progress}, alarms {active, total}. Streamed per node from a fixed 4 KB buffer |
| `GET /api/schema` | field table as JSON (group, key, type, min, max, enums, per_shelf) + the forwardable row list; fetched once per page load |
| `GET /api/config?zone=N[&secrets=0]` / `PUT /api/config?zone=N` | §4.6 JSON. GET exports cfg+hw from the master cache (zone 0 = the master's own `mcfg`; `secrets=0` omits passwords). PUT: merge into scratch, `hg_cfg_validate`/`hg_hw_validate`, first bad value → 400 `{error:"INVALID_FIELD", path}`, unknown keys → `warnings[]`, then `node_mgr_cfg_set` (gen bump + source stamp in the tick, push). 409 while a push for that zone is in flight |
| `GET /api/alarms` | `{active:[…], events:[…64]}` from `alarm_mgr` |
| `POST /api/fw/master`, `POST /api/fw/zone` | raw binary body (`application/octet-stream`; multipart and chunked rejected 400); master → inactive slot; zone → `zone_fw` with the HGFW prefix; `esp_app_desc.project_name` must be `hillgrow_master` / `hillgrow_zone` else 422 `IMAGE_MISMATCH`; size vs slot else 413; exclusive with each other and the fleet sequencer (409); yield every 8 × 4 KB blocks; failure → `esp_ota_abort`, slot untouched. Progress in `/api/state.fw.upload` |
| `POST /api/fleet` `{zone}` or `{all:true}`; `DELETE /api/fleet` | → `node_mgr_fw_zone/all/abort` |
| `GET /api/wifi/scan`; `POST /api/wifi` `{sta:{ssid,pass}}` or `{ap:{ssid,pass}}` | scan (≤ 20 results); set credentials → `mcfg` + reconnect |
| `POST /api/password` `{old,new}` | change web password |
| `GET /`, `/app.js`, `/app.css` | gzipped `EMBED_FILES`, `Content-Encoding: gzip`, `Cache-Control: max-age=86400`, ETag = build hash |

Error model: `{ "error": "<CODE>", "path": "<field>" }` with 400/401/409/413/422/503; the command endpoint keeps the console's text errors verbatim. Heap guard: config PUT and uploads refused with 503 `LOW_HEAP` when free heap < 40 KB.

## Master-side services and data flow

| Component | Kind | Contents |
|---|---|---|
| `wifi_mgr` | glue | AP+STA bring-up, credentials, backoff, mDNS, NOTIFY WIFI, scan; `wifi_mgr_status(struct*)` |
| `time_core`* / `time_svc` | pure / glue | POSIX-TZ offset, quality state machine / SNTP lifecycle tied to STA |
| `hg_mcfg`* | pure | master config struct + defaults + envelope + field rows (WIFI STA/AP, WEB hash+salt, TZ, NTP, hostname); `hg_store` gains channel `mcfg` (master build) |
| `web_auth`* | pure | salted SHA-256 verify (hash fn injected), 4-token table + expiry, lockout, cookie parse; NVS `sess` via the store task at login/logout |
| `hg_json`* | pure (cJSON) | field table ↔ JSON for cfg/hw/mcfg, schema generation, merge semantics, path errors |
| `alarm_mgr`* | pure | NOTIFY sink → 64-event ring + derived active set (latest state per type/node, single-id NOTIFY form); JSON export |
| `state_snap`* | pure | `/api/state` builder from node snapshots + ring + wifi + time + fw; copies under `nmgr_lock`, serializes outside, per-node streaming |
| `http_srv` | glue | httpd instance, route table, auth middleware, handlers = parse → service → serialize; uploads via `httpd_req_recv` 4 KB blocks |
| `node_mgr` (+) | glue | `node_mgr_cfg_get(zone, cfg*, hw*, gens*)` snapshot readers; `node_mgr_cfg_set(zone, kind, blob)` request-flag write consumed by the tick (gen bump, source stamp, push); `SET NODE <z> MAC` pre-seed; version-mismatch pull → terminal latch keyed (hb_gen, hb_crc) |
| `master_cmds` (+) | pure rows | `GET/SET WIFI STA|AP`, `GET WIFI`, `SET WEB PASSWORD`, `GET/SET TZ`, `SET NODE <z> MAC <mac>` |
| `web/` + CMake | assets | `index.html`, `app.js`, `app.css` → gzip at build → `EMBED_FILES`; size gate 64 KB |

Flows: page load → `/` → login if no cookie → `/api/schema` once → `/api/state` every 2 s. Action = line to `/api/cmd` (immediate reply) or `PUT /api/config` (merge → validate → cache → push; next poll shows `cfg_sync`). Upload → stream → `esp_ota_end` + set boot → 200 → UI offers `REBOOT CONFIRM` via `/api/cmd` → §3.10 trial (master ticks it since 936a7e3). RAM budget ≈ 40 KB of the 151 KB minimum heap measured 2026-09-10; `GET STATUS` heap-min before/after is part of the bench evidence.

Concurrency: httpd handlers never touch node_mgr state directly (snapshot readers under the lock, request flags for writes); `cmd_task` remains the sole model editor; the store task the sole NVS writer.

## Frontend

`web/index.html` + `app.js` + `app.css`, vanilla ES2017, hash routes `#/dashboard #/zone/N #/config/N #/alarms #/system #/login`. Phone-first: single column ≤ 480 px, 44 px tap targets, bottom nav; ≥ 900 px: sidebar. System fonts, light/dark via `prefers-color-scheme`, viewport meta without zoom lock, no external resources.

- Dashboard: ring banner (state + blame), master card (time + quality, STA/AP IPs, heap), one card per zone (name, health badge, fw, link flags, HB age; soil/light/pump show a dash until non-zero; stale badge on OFFLINE rows' link flags).
- Zone: node details, "replace board" (SET NODE MAC), and a console box: any line → `/api/cmd`, reply shown, last 20 kept.
- Config: tabs per group (ZONECFG, SHELF, LIGHT, WATER, FAN, VIB, AUX, HW, HWSHELF, CAL), shelf selector, forms generated from `/api/schema`, one merge document per Save, rejected path highlighted, Export/Import JSON.
- Alarms: active list + event log. System: Wi-Fi (scan/join/AP), time/TZ, password, master + zone upload with progress, fleet button + status, reboot.
- Polling pauses when hidden (Page Visibility), exponential backoff on errors, offline banner; `?mock=1` static fixture for layout work without a board.

## Error handling and safety

Auth 401 → login view; lockout as above. HTTP session permanently locked (no unlock rows), `NOT_LOCAL` for session rows, actuator rows duration-bounded by the dispatcher — the web can do exactly what the console can. Config PUT: validate → atomic cache → push; push failure = zone `cfg_sync` state + `CFG_SYNC_FAILED` alarm. Uploads: exclusivity, size, chunked rejection, project-name check, abort-on-failure, watchdog-friendly yields. Wi-Fi: STA never impairs AP or ring; wrong password = STA down with reason. Memory: per-node streaming of the state document, 503 `LOW_HEAP` guard. Ring: web forwards share the single forward slot; busy → 503 with retry hint.

## Testing

Host (pure, Unity, MSVC gate as before): `hg_json` (schema, per-type round trips, merge, unknown-key warnings, first-bad-value path, secrets omission), `hg_mcfg` (defaults, envelope, validation), `web_auth` (hash vectors, expiry/eviction, lockout, cookie parse), `time_core` (POSIX TZ across DST), `alarm_mgr` (ring wrap, active-set derivation from real NOTIFY sequences), `state_snap` (golden JSON from a fixture table), route table as data (match + auth flags), and the SP3 test debt: a fake-link harness for `node_mgr_cfg`'s decision table (feed heartbeats + ACK events, assert push/pull/latch) landed before `node_mgr_cfg_set`.

Bench: `tools/uart_test.py --http <ip>` (login step added) runs every CLI suite through `/api/cmd`; new `tools/web_test.py`: login/lockout, schema, state shape, config round trip on zone 2, upload rejections (wrong image, chunked, oversize), master OTA → trial pass, zone upload → fleet update end to end, STA join to a 2.4 GHz hotspot + mDNS lookup, 30-minute two-client polling soak with heap-min before/after. Phone checklist (Android Chrome via house Wi-Fi and via the AP; desktop Chrome): login, dashboard, config edit, upload, offline banner across a master reboot. CMake gate: gzipped assets ≤ 64 KB.

## Carries

From the SP3 final review, addressed here: `node_mgr_cfg_set/get`; `SET NODE <z> MAC`; version-mismatch pull latch; single-id NOTIFY consumed by `alarm_mgr`; stale link-flag rendering on OFFLINE rows; httpd socket budget 4; AP credentials changeable; fake-link harness for `node_mgr_cfg`. Deferred to SP4b: everything under *Non-goals*.

## Size/budget notes

master.bin 835 KB today in a 2048 KB slot; SP4 adds ≈ 60 KB assets + ≈ 120–180 KB code (httpd already linked; mDNS, SNTP, cJSON, mbedtls SHA already in the IDF image or small). Heap: ≈ 40 KB against 151 KB min. LWIP sockets 10, httpd 4.
