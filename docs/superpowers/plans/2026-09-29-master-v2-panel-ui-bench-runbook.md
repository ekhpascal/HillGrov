# Master v2 panel UI -- combined owner bench runbook (Tasks 1-34)

**For:** the owner, in one session, top to bottom. **Plan:** `docs/superpowers/plans/2026-09-23-master-v2-panel-ui.md`.
This runbook contains every hardware step that the 34 tasks deferred, including the five stage bench gates (Stages 0-4)
and the Task 34 acceptance. Duplicate steps appear once. Each check names its sources: `T<n>` for a task report,
`S<stage>.<step>` for a stage bench gate, `P<n>` for Task 34's parity checklist (map-parity §G), and `A` for the Task 34
acceptance.

**Built and gated 2026-09-29 at `2223d2d` (the final-review fix wave; sections 10.5, 11.17a and 14.3 are its checks):**
- GATE-HOST: 53/53.
- GATE-ESP32: master, zone and rescue builds complete, and the lock diff is empty.
- GATE-P4: builds with `CONFIG_IDF_TARGET="esp32p4"`.
- P4 image: `hillgrow_master.bin` is 1,601,392 B, 76.4 % of the 2 MB factory partition that IDF checks against, and
  38.2 % of the 4 MB OTA slot.

Nothing has been flashed since the plan started. If HEAD later gains code changes, re-run the three gates (the commands
are in `.superpowers/sdd/2026-09-23-master-v2-panel-ui/global-constraints.md`) before Part 2.

**Time:** about 4 h, of which about 50 min is hands-off (the web suites and the 30 min soak).

**Rule for failures:** any FAIL fails the acceptance (Task 34 Step 3).
- **STOP the session** when a check says **STOP** (3.1, 4.1), or when any **STOP condition** of 0.4 appears at any
  point. A crash, an abort or an unexplained reboot is always a STOP. Then follow 0.4's "On a STOP". Do not go on to any
  later check, above all a `[STATE]` one.
- **Any other FAIL:** write down the exact text (a photo of the glass is best) and carry on, so that one fix round
  covers every failure. The failed checks are then re-run on the fixed image.

`[STATE]` marks a check that changes bench state; its restore step is in the check, or the check points to it.

---

## 0. Preconditions and preparation (at the desk, before touching the bench)

### 0.1 The rig
- **P4 master:** USB-UART on **COM28**. Its AP is `HillGrow` / `hillgrow1` at `192.168.7.7`.
- **Zones:** zone id 1 is the board on **COM25** (MAC ...2a:88), and zone id 2 is the board on **COM24** (MAC ...2a:d0).
  The owner's labels are the other way round, so trust `GET ID` and the Zone view's MAC row. The ring runs P4 IO28 →
  COM25 → COM24 → P4 IO29. Both zones must be ONLINE.
- **"Zone 2 offline"** below means: hold zone 2's EN button, or simply unplug the COM24 board's USB for the longer holds.
  Both open the ring and make zone 2 OFFLINE.
- **Phone:** joined to `HillGrow` and logged in to `http://192.168.7.7`.
- **PC:** joined to `HillGrow` (`netsh wlan show interfaces` shows it). Before reading a web timeout as a firmware fault,
  run `netsh` again.
- **House Wi-Fi:** the master's STA must be joined to it at the start (NTP). Have its SSID and password to hand: Part 9
  clears the STA and joins it again.
- **Web password:** `hillgrow1`. If the bench uses another, replace it everywhere below and in `hg_login.json` (0.3).

### 0.2 microSD cards
You need a **card A** (FAT32, 32 GB or smaller) and a **card C** (exFAT: any card of 64 GB or more, straight from the
pack, or a spare card formatted exFAT). A **card B** (FAT32) is optional; it replaces the PC edit in 11.17.

Prepare card A (set `$C` to its drive letter; the first line must print `FAT32`):
```powershell
$C = "E:"
(Get-Volume -DriveLetter $C[0]).FileSystem
Copy-Item C:\Projects\HillGrov\master\build_p4\hillgrow_master.bin "$C\"
Copy-Item C:\Projects\HillGrov\zone\build\hillgrow_zone.bin "$C\"
Copy-Item C:\Projects\HillGrov\master\build_p4\bootloader\bootloader.bin "$C\other.bin"
New-Item -ItemType Directory -Force "$C\hillgrow" | Out-Null
Copy-Item C:\Projects\HillGrov\master\build\hillgrow_master.bin "$C\hillgrow\esp32_master.bin"
Set-Content "$C\hillgrow\zone1.json" "this is not json" -Encoding ascii
Set-Content "$C\hillgrow\master.json" ("{" + (" " * 5000) + "}") -Encoding ascii
```
Card A must **not** hold `hillgrow\zone2.json` at the start.

On card C, put one file, `marker.txt`. For card B (optional), put a `hillgrow_zone.bin` in its root that is 16 bytes
longer than the real one:
```powershell
$z = [IO.File]::ReadAllBytes("C:\Projects\HillGrov\zone\build\hillgrow_zone.bin")
[IO.File]::WriteAllBytes("F:\hillgrow_zone.bin", [byte[]]($z + (New-Object byte[] 16)))
```

### 0.3 Two PowerShell windows
**W1, the console.** It stays open all session. Close it only for `flash_app.py`, `uart_test.py` and a power cycle.
Search its scrollback with Ctrl+Shift+F.
```powershell
C:\Python311\python -m serial.tools.miniterm --dtr 0 --rts 0 COM28 115200     # exit: Ctrl+]
```
**W2, the tools.** Run this once:
```powershell
& C:\esp\v6.0.1\esp-idf\export.ps1
$HG = "http://192.168.7.7"; $JAR = "$env:TEMP\hg_jar.txt"
Set-Content "$env:TEMP\hg_login.json" '{"password":"hillgrow1"}' -Encoding ascii
Set-Content "$env:TEMP\hg_blank_ap.json" '{"WIFI":{"AP_PASS":""}}' -Encoding ascii
function hglogin { curl.exe -s -c $JAR -H "Content-Type: application/json" --data-binary "@$env:TEMP\hg_login.json" "$HG/api/login" -o NUL -w "login HTTP %{http_code}`n" }
function hgpost([string]$path, [string]$file) { curl.exe -s -b $JAR -H "Content-Type: application/octet-stream" --data-binary "@$file" "$HG$path" -w "`nHTTP %{http_code}`n" }
function hgput0([string]$file) { curl.exe -s -b $JAR -X PUT -H "Content-Type: application/json" --data-binary "@$file" "$HG/api/config?zone=0" -w "`nHTTP %{http_code}`n" }
```
`hglogin` should print `login HTTP 204`. Run it again after any master reboot or password change.

### 0.4 Standing watch (W1, all session)
Glance at W1 after every check, and search its scrollback at the end of every Part.

**STOP conditions.** Each one stops the session wherever it appears:
- `panic`, `abort`, `Backtrace`, `Guru Meditation`, `task_wdt`, or `Stack canary` / `stack overflow`;
- a reboot that no step caused. The expected reboots are the power cycle in 9.3, the restarts in 10.3, 10.4 and 10.5, the
  Reboot now in 11.12, the master OTA inside 12.2, and the optional 15.x reboots;
- a frozen panel: the clock stops, or nothing responds under the finger for more than 5 s.

**On a STOP:**
1. **Hands off.** Do not press RESET, power-cycle, pull the card or tap anything more. After a panic the master
   usually restarts by itself; let it.
2. **Capture:**
   - Save the whole W1 scrollback to a file: select all, copy, then paste into `C:\tmp\hg_bench_stop.txt`. It must
     include the lines from the last check's start through the backtrace and the next boot banner.
   - Note the check number that was running and the last thing you did (the tap, the command, or the web_test suite).
   - Take a photo of the glass.
   - Do not rebuild `master\build_p4`: its `hillgrow_master.elf` is what decodes the backtrace.
3. **Make it safe:**
   - In W1, send `GET VERSION` and `GET FW ZONE`, and record both.
   - If the slot shows `PENDING`, an OTA trial is running. Leave the board powered and untouched until W1 shows
     `NOTIFY FW 0 TRIAL PASS` and `GET VERSION` shows `VALID`. Any reset before then retires the new image, so reboot
     inside a trial only if you decide to.
   - If a fleet update was running (System → Fleet status not `IDLE`), let it finish. Do not Abort, unless the zone
     stays OFFLINE for more than 2 min.
   - Then undo, from the phone's web UI, any `[STATE]` change whose restore you had not yet done: the password, the AP,
     the STA, the TZ and the dimming. Use the "End state" list near the end of this runbook.
4. Hand the capture file and the notes to the agent. The session resumes on a fixed image, from the stopped check.

**FAIL conditions.** Each one is a FAIL wherever it appears; carry on after recording it:
- `took N ms (budget 200)` (any screen);
- `Failed to acquire LVGL lock`;
- `W_PANEL_FROZEN`;
- `task not found`;
- `zone_fw writer claim released while not held`;
- `zone.bin transfer abandoned`;
- `panel console slot(s) degraded`;
- repeated `ESP32_P4_EV` INFO lines;
- `E SD_HOST: host controller with slot registered` after a **successful** Read card. One such line after a **failed**
  mount (no card or exFAT) is accepted (T32 ruling).

Benign, and not to be chased: `W ledc: GPIO 32 is not usable, maybe conflict with others`, and the gesture-recognition W
line.

### 0.5 Readings sheet (Panel → About)
Fill in one row at each of R1 to R4. Each row needs:
- `Internal RAM N KB free, M KB minimum`;
- `LVGL pool: max used N of 64 KB internal`;
- `Stack free (min, bytes): lvgl / pnl_work / pnl_poll / pnl_wifi`.

The figures are running minima and peaks, so a later row covers every earlier moment. The intermediate rows only show
which Part caused a failure.

**Pass (every row):**
- the internal minimum is at least 64 KB;
- LVGL max used is at most 48 KB (75 % of the 64 KB pool);
- every stack figure is at least 1024.

| Row | When | Int free / min (KB) | LVGL max (KB) | lvgl | pnl_work | pnl_poll | pnl_wifi |
|---|---|---|---|---|---|---|---|
| R1 | after boot (3.2) | | | | | | |
| R2 | after Part 9 (9.11) | | | | | | |
| R3 | after Part 11 (11.20) | | | | | | |
| R4 | after the soak (14.2) | | | | | | |

Also record:
- boot heap before/after the panel (3.1);
- `panel: up in N ms` (3.1);
- the zone and master install times (11.8, 11.12);
- whether the UI paused visibly during flash erases.

### 0.6 Not a bench item, but do it now
`python C:\Projects\HillGrov\tools\web_test.py --selftest` must print `SELFTEST OK (9 assertion groups)`. See the section
"Owner items that are not hardware".

---

## Part 1 -- Baseline (before anything is flashed; ruling C7)

### 1.1 MCFG suite against the pre-plan image -- T1, S0.1, C7 `[STATE]`
1. In W1, type `GET VERSION`. Confirm the master still runs the **pre-plan** image: the version string is `0.1.0` in
   every build, so the tell is the glass. The pre-plan master has no panel code, so the panel is dark and the boot log
   has no `panel:` lines. If the panel shows the HillGrow Home screen, skip this check and record "baseline lost (C7)".
2. In W2, run `python C:\Projects\HillGrov\tools\web_test.py 192.168.7.7 --password hillgrow1 --only mcfg`.

**PASS:** every MCFG line is PASS and the summary is `N/N passed`.
**FAIL:** any FAIL means the suite does not describe today's firmware. Note it; Tasks 3-5 must not be judged against it.

**State:**
- The suite changes the web password to `hillgrow1_mcfgT` and restores it, which logs out the phone (log in again).
- If the bench was still on the factory password, `MCFG_F_WEB_DEFAULT` clears **for good**, and `defaults.web` stays
  false from then on. That is expected and cannot be undone from the web.

---

## Part 2 -- Flash the final image (tools only; never `idf.py flash`)

### 2.1 Master image -- A (T34 Step 1), S0.2, and the flash step of T6-T33 `[STATE]`
1. In W1, `GET VERSION` must show the running slot `VALID`. If it shows `PENDING`, wait a minute and ask again. Then
   close W1 (Ctrl+]).
2. In W2, run the dry run:
   `python C:\Projects\HillGrov\tools\flash_app.py --app master --target esp32p4 --build-dir C:\Projects\HillGrov\master\build_p4 --port COM28 --dry-run`
3. Then run the same command without `--dry-run`.

**PASS:** every path the dry run prints contains `build_p4`, and the real run ends with the tool's success line.
Record the image size: 1,601,392 B against 4,194,304 (the plan's slot) and 2,097,152 (the factory partition IDF checks).

### 2.2 Stage the zone image into zone_fw -- A (T34 Step 1.3) `[STATE]`
1. `python C:\Projects\HillGrov\tools\flash_app.py --app zonefw --target esp32p4 --port COM28 --dry-run`
2. Then the same command without `--dry-run`.

**PASS:** the dry run reads `zone\build\hillgrow_zone.bin` and the real run succeeds. The zones themselves are not
touched.

---

## Part 3 -- Boot

### 3.1 Boot log and first light -- T6, T7, T8, T11, S0.3, S0.7, A (Step 6)
Open W1, then press the P4's RESET button.

**PASS:**
- The log shows, in order:
  - `panel: internal heap before panel: free N, min M`;
  - `panel_hw: LVGL pool: 64 KB internal + 256 KB PSRAM overflow` (a W line saying the overflow pool is unavailable is
    a FAIL);
  - `panel: up in N ms (lvgl c1/p3, touch ok)`;
  - `panel: internal heap after panel: free N, min M`, with M ≥ 65536;
  - after node_mgr starts, `pnl_work: worker up (c0/p2, 8-job pool, not TWDT-subscribed)`;
  - then `pnl_poll: poller up (1 Hz state, 5 s Wi-Fi, not TWDT-subscribed)`.
- Boot reaches the CLI prompt.
- The panel lights on **Home**, at the day brightness.
- There is no `not created`, `job queues unavailable` or `display lock not taken` line, and nothing from the 0.4 list.

**FAIL:** a dark panel, `touch UNAVAILABLE`, an abort, or a reboot loop. That is a **STOP**.

Record both heap pairs and `up in N ms`, which is the boot-to-lit time for Task 34 Step 6.

### 3.2 R1 readings -- S3 (before step 1), S4 (before you start)
Home → Panel → About. Fill in row R1. **PASS:** the 0.5 criteria, and the `Firmware:` line shows the slot `VALID`, not `PENDING`.

---

## Part 4 -- Touch and THE RULE (Home → Panel → About → Touch test)

### 4.1 Five-target touch test -- T7, T12, T13, S0.4, A
Tap each of the five targets (four corners and the centre) once.

**PASS:**
- The target under the finger turns green and shows `hit 1`. The diagonally opposite target never lights.
- The counter line shows `reads` > 0 and `errors 0 (last 0)`.
- `last x,y` is near the tapped corner: top left means small numbers, and bottom right is about `1023,599`.
- `Reset targets` returns all five targets to `hit 0`.
- No shell Home button sits over the top-left target. The screen's own Home button (bottom centre) returns Home.

**STOP** if a tap lights the opposite target. Do NOT enable the esp_lcd_touch mirrors: the fix is the orientation
constant.

### 4.2 THE RULE: blocking job -- T8, S0.5
1. Tap "Blocking job (3 s)". The label reads `running... (the spinner must keep turning)`.
2. Tap it again while the job is running: nothing changes.
3. When the job ends, run it once more.

**PASS:** the spinner turns smoothly for the whole wait. Each run ends with `done after ~3000 ms, N UI ticks during the
job`, with N ≥ 150 (about 187 is expected).
**FAIL:** the spinner stalls, N < 150, `FAILED: the job ran on the LVGL task`, or `worker unavailable`.

### 4.3 Poller on glass -- T11
**PASS:**
- `Poller: seq N` rises by about 1 per second.
- The same line's `LVGL pool: max used X of 64 KB internal` shows X ≤ 48.

### 4.4 Re-entry, and a job that spans a navigation -- T12
1. Leave the Touch test and come back five times.
2. Start a blocking job, then tap Home within 3 s.
3. Come back to the Touch test and run the job again.

**PASS:**
- There is no crash, and the heap and touch lines keep updating.
- The job that finished off-screen causes nothing.
- The re-run still reports N ≥ 150.

---

## Part 5 -- Home and the read-only views (phone side by side)

### 5.1 Home screen -- T13, S1.3, A
**PASS:**
- The screen is full width, with no rail and no top-left Home.
- The clock is readable from across the room. Write down how it reads.
- Top right: `NTP` next to the Wi-Fi symbol, in the normal colour while the STA is up.
- Top left: `• ring OK` in green.
- The band shows exactly two tiles, 240 px each and centred, each with name, health dot, soil % and `Light N%`.

Then tap, in turn:
- a tile → that zone's view, with the rail;
- the rail's Home → back to Home;
- HillGrow → Dashboard, with the rail;
- Audio → the SP7 sentence, centred. Its top-left Home returns;
- Panel → the Panel screen.

### 5.2 Context line -- T16, S1.2
On the phone, read LIGHT ON/OFF for every enabled shelf of zones 1 and 2.

**PASS:**
- While any of those lights is on, the line reads "Lights off in Xh Ym" for the earliest OFF among the lit shelves.
- Otherwise it reads "Lights on at HH:MM" for the soonest ON.
- Either figure is within a minute of your own arithmetic.
- With no enabled shelf, the line follows the watering rule, or is blank.

### 5.3 Rail and Dashboard vs the web -- T12, S1.5
**PASS:**
- The rail is 120 px, with Home, Dashboard, Zone, Config, Alarms and System. No label is clipped, and the highlight
  follows each tap.
- The Dashboard matches the phone value for value, allowing 1-2 s of poll skew:
  - the ring banner (`Ring: OK` on green);
  - the master card: `Master  v<version>`, the time with `(UTC+hh:mm)` and its source, STA, AP, and `Heap min N KB`;
  - each zone card: name, badge, `fw x.y.z`, `last seen Ns ago`, and Soil/Light/Pump. The tiles are not clipped;
    record the widest readings shown (T12 addition).
- The default-password banner shows exactly when the web shows one, naming the same flags. "Open System" opens System.
- Tapping a zone card opens that zone.

### 5.4 Zone view vs the web -- T14, S1.5
1. On a fresh visit, tap the rail's Zone: the first zone shows. The selector lists both zones, and the selected one is
   the accent colour.
2. Compare zone 1, then zone 2, with `#/zone/1` and `#/zone/2`.
3. Tap "Configure this zone".
4. View zone 2, go to another destination, then tap the rail's Zone.
5. Drag on a table.

**PASS:**
- All 14 rows and the shelf table match (Last heard and Uptime may differ by 1-2 s). A zone with 0 shelves shows "No
  shelf telemetry.".
- "Configure this zone" opens the Config editor on that zone.
- The rail's Zone reopens zone 2.
- The page scrolls when dragged, with no WARN.

---

## Part 6 -- Config editor (Home → HillGrow → Config)

### 6.1 Tabs and the read-only hardware plane -- T20, S2.1, P5, A
Open Config and pick zone 2.

**PASS:**
- The title reads `Config -- Zone 2 (gen G)`, with G equal to the web's.
- The tabs are in two rows: ZONECFG SHELF LIGHT WATER FAN / VIB AUX HW HWSHELF CAL. Tap each one.
- HW, HWSHELF and CAL show the muted note "hardware plane -- set at the zone console" once, above rows that have **no**
  editor.
- HW shows its addresses in hex (`0x40`, `0x20`, `0xFFFF` or the real values).
- On HWSHELF, each pin that W1 `GET ZONE 2 HWSHELF <n>` (n = index + 1) prints as `NONE` shows `none`.

**FAIL:** a tab out of order, an editor on a hardware row, a decimal address, or `255` for a pin.

### 6.2 Stepper save and push -- T20, S2.2, P4 `[STATE]`
WATER tab, shelf index 0. Note the "Target moisture" value V (61 on this bench). Step it to 55 → Save.

**PASS:**
- "Saving...", then `Queued, pushing to zone`, then `Landed on the zone.` within about 10 s.
- W1 `GET ZONE 2 WATER 1` shows `Target : 55`.
- About 3 s later, the title's gen has gone up by 1.

**Restore:** step it back to V → Save. W1 then shows `Target : V`.

### 6.3 Validation, highlighted -- T20, S2.3, P4
LIGHT tab, shelf index 1. Set "Lights off" equal to "Lights on" → Save.

**PASS:**
- Nothing is sent: no CFG traffic in W1, and the gen is unchanged.
- The status reads `VALIDATION: Lights off (shelf 1)`.
- The row is outlined red, with `VALIDATION` under its label.

Move the value back. The dot stays, and Save then succeeds.

### 6.4 Input bounds -- T19, T20, S2.4
**PASS:**
- **Zone name keyboard:** the space bar inserts nothing; with the text empty, OK is disabled; a 16th character is not
  accepted. Cancel.
- **"Link-loss timeout":** holding `++` stops at 600 and holding `--` stops at 10, and holding repeats. A **plain tap**
  on the value opens nothing. A **long-press** opens the keypad; typing 700 → OK shows `Out of range (10..600)` and
  leaves the value unchanged.
- **LIGHT.DLI:** a long-press does not open a keypad (it is a scaled row). At 0 it reads `off`, with no unit.

### 6.5 Segmented controls -- T19 (fix round)
Use any enum shown as a segmented control.

**PASS:**
- A scroll that starts with the finger on a segment changes nothing and adds no dot.
- Holding a segment for more than 1 s, then releasing, records exactly one change, on release.
- Re-tapping the selected segment adds no dot.

### 6.6 Tabs, picker and loads -- T20
**PASS:**
- Holding a tab does not make the list flicker. Pressing a tab and sliding off it switches nothing.
- Save zone 2, then switch the picker to zone 1 at once. Zone 1's Save shows "Saving..." while the save runs, and the
  status then reads `Zone 2: Queued, pushing to zone`.
- Leave Config while "Loading..." shows, then come straight back. The editor loads, and "Loading..." never sticks.

### 6.7 The two faces collide -- T20, S2.6, P18 (first half) `[STATE]`
1. On the phone, open Config zone 2 and type a new WATER Target, but do not save.
2. On the panel, change a zone 2 field → Save. Within 2 s, tap Save on the phone.

**PASS:** the phone shows `Zone busy, retry` (409 BUSY), and the panel's save lands.
**Restore:** set both values back.

### 6.8 A zone that never synced (optional) -- T20, S2.8
This needs a spare zone board, enrolled for the first time.

**PASS:** Config shows `Zone config not adopted yet -- the zone must come online and sync at least once before it can
be configured.` with [Retry]. The editor loads by itself, once, after the zone syncs. Otherwise record "not exercised".

### 6.9 Master editor, secrets masked -- T19, T21, S2.7, P7 `[STATE]`
Picker → Master. **PASS** on each line:
- The tabs are WIFI, TIME and SYS, with no WEB tab. Both password rows show `********` with [Reveal] and [Change].
- **NTP:** note the current value, set `time.google.com` → Save → `Saved.`. The phone's master Config shows it.
  **Restore** the old value → `Saved.`.
- **Hostname:**
  - The keyboard refuses upper case and `_`.
  - Note the current name, set `hillgrow2` → Save → `Saved.`.
  - Within about 30 s the Dashboard STA line shows the STA re-joined with an IP. That proves the blank STA_PASS kept
    the stored password.
  - **Restore** the old name.
- **Reveal:** WIFI → "Wi-Fi password" → [Reveal].
  - The stored password shows in clear, the button reads "Hide", and it re-masks by itself after 10 s.
  - [Reveal] again, then HillGrow → Home and come back: it is masked.
- **Change:**
  - [Change] opens an empty, masked keyboard. The eye shows the text, and it re-masks after 10 s. An empty OK changes
    nothing.
  - Type something → OK → [Reveal] shows the typed, unsaved value. Leave without saving.
- **AP name:** change one character → Save. The dialog reads "Changing the AP drops every phone connected to it." →
  [Cancel] → `Not saved.`, and the phone stays connected. Undo the edit.

---

## Part 7 -- Zone console (rail → Zone → zone 2)

### 7.1 Lines, forward, history -- T22, S3.1, P2
**PASS:**
- `GET ID` → OK. The log shows `> GET ID` ("..." while pending), then `OK ID ...` verbatim.
- Tick "Forward to zone 2", send `GET WATER 1`. The log shows `> GET ZONE 2 WATER 1` and zone 2's reply.
- Prev/Next walk the **original** lines (`GET WATER 1`, not the rewritten one). Next past the newest gives an empty
  draft.

### 7.2 Line limits and session semantics -- T22
**PASS:**
- A 192-character line (hold one key) shows "Line too long -- 191 characters at most", and nothing is sent.
- `DEBUG ENABLE` answers `ERR NOT_LOCAL`.
- Only spaces or tabs → OK sends nothing and shows no error.
- `GET ID` with a trailing space is logged as `> GET ID`, with no trailing whitespace.

### 7.3 Replace board -- T22, S3.2, P3 `[STATE]`
This re-writes zone 2's own MAC, which changes nothing.

**PASS:**
- `aa:bb:cc` → Replace shows "Enter a MAC like aa:bb:cc:dd:ee:ff".
- Zone 2's own MAC (the MAC row at the top of the zone view) → Replace shows the reply verbatim (`OK NODE ...`). The
  draft clears only on OK, and zone 2 stays ONLINE.

### 7.4 A full transcript -- T22
Send `GET STATUS` repeatedly until the oldest entries drop off (more than 8 KB of text).

**PASS:**
- The log stays scrolled to the newest entry, with no budget WARN.
- The Dashboard shows no "panel console slot(s) degraded" line.

Leave one line in zone 2's log for 9.6.

---

## Part 8 -- Zone 2 offline (one hold covers Stage 1 and Stage 2 items; phone on `#/alarms`)

Unplug the COM24 board, or hold its EN button. Plug it back in at 8.5.

### 8.1 Badges and band -- T13, T16, S1.4, P1, A
**PASS:**
- Zone 2's Dashboard badge and Home tile dot read DEGRADED (amber) at about 5 s, then OFFLINE (red) at about 10 s.
- The Home band turns red and pulses about once a second, with "N alarm(s) -- tap to view".
- Tapping the band background opens Alarms, tapping the badge opens Alarms, and a tile still opens Zone.
- Through at least 60 s of pulsing: the clock keeps ticking, and there is no TWDT line and no `W_PANEL_FROZEN`.

### 8.2 Alarms vs the web -- T16, S1.4, P8
**PASS:**
- Active lists `NODE 2` with its text, and its age ticks every second.
- History lists the RING/NODE events newest first.
- Against the phone's `#/alarms`: the same keys, the same texts (the blame line), and ages within 2 s.
- The band's N equals the Active row count.

### 8.3 Save while offline -- T20, S2.5, P6
In Config zone 2, change a field → Save, then at once open the Zone name keyboard.

**PASS:**
- The status reads `Zone is offline -- nothing was saved`, never `Zone busy, retry`.
- If the refusal arrives while the keyboard is open, the keyboard stays open and the banner shows the refusal.

Cancel the keyboard, but keep the changed field for 8.5.

### 8.4 A slow console line across a navigation -- T22
Zone 2 console, forward ticked → `GET WATER 1`. While "..." shows, go to Dashboard, then come back.

**PASS:** the reply (an error) replaces "...", and Replace is enabled again.

### 8.5 Release -- S1.4, S2.5, P1
Plug zone 2 back in, or release EN.

**PASS:**
- The tile and badge return to ONLINE (green), the pulse stops, and the badge hides.
- Alarms → Active shows "No active alarms." on the next poll.
- Config zone 2 → Save shows `Queued, pushing to zone`, then `Landed on the zone.`.

**Restore** the field you changed.

---

## Part 9 -- Clock, time, Wi-Fi, dimming, web password (Home → HillGrow → System)

### 9.1 System → Wi-Fi lines and tabs -- T23, T24
**PASS:**
- The "STA: ..." and "AP: ..." lines match the web's System page (IP | SSID | dBm; SSID | N client(s) | 192.168.7.7).
- Tapping the active tab again leaves it highlighted.
- All five tabs (Wi-Fi, Time, Password, Fleet, Firmware) open, with no WARN.

### 9.2 Scan -- T23, S3.3
**PASS:**
- Scan shows "Scanning... (the AP pauses briefly)", with Scan disabled. Then a list of `SSID | open/secured | -NN dBm`
  appears, and the rail stays responsive. Tapping an entry fills SSID.
- Scan again, go to Dashboard, and come back before it ends. The list replaces "Scanning...", and Scan is enabled again.

### 9.3 Clock not set -- T13, T16, S1.1a, S3.4 `[STATE]`
1. Config → Master → WIFI → clear "Wi-Fi network (SSID)" → Save → `Saved.`. The STA is restored in 9.8.
2. Close W1, unplug the P4's USB, plug it back in, and reopen W1. Send no `SET TIME`.

**PASS:**
- The Home clock reads `--:--` and the date line reads "Clock not set".
- The context line is blank or also "Clock not set". Any time or countdown is a FAIL.
- The top right reads `NONE`.

### 9.4 Unset clock: analogue face, and no dimming -- T26, T27 `[STATE: restored in 9.6 (face) and 9.10 (dimming)]`
Panel:
- Clock face Analogue → Home. **PASS:** "Clock not set" on the face, and no hands.
- Night dimming = Fixed hours, from the current hour to two hours later. "Dim after idle" 30 s. "Return to Home and wipe
  after" 2 min.
- Leave the glass untouched for 45 s. **PASS:** the backlight stays at the day level.

### 9.5 Set clock, and SET TIME -- T23, T24, S1.1b, S3.4
System → Time.

**PASS:**
- "Now" reads "Clock not set (NONE)", and the TZ field shows the configured TZ. Write it down for 9.10.
- 31 February → Set clock shows "Pick a real date between 2020 and 2099".
- Pick today's local date and time → Set clock. The reply `OK TIME <UTC>` shows verbatim, with no blank line under it,
  and W1 `GET TIME` matches it.
- **D14, expected, not a FAIL:** while the clock is unset the conversion uses the winter offset. On a summer date in a
  DST zone such as CET, the UTC is local − 1 h, and Home then shows one hour ahead.
- Then W1 `SET TIME <UTC date> <UTC time now>` (for example `SET TIME 2026-09-29 12:34:00`). Within about 1 s, Home
  shows the correct **local** time against the phone, and today's local date.

### 9.6 Analogue face, dimming, first-tap wake, idle wipe -- T26, T27, S3.8, S3.9
**PASS** on each:
- **Analogue face:** Home shows a 200 px round face with the right local time and a moving second hand. The date and
  context lines tick below it. Switch Panel → Clock face → Digital, and the digital clock is back.
- **Dim:** leave the glass untouched for about 30 s. The backlight drops to the night level, and the clock keeps
  ticking.
- **First-tap wake:** open Dashboard, let it dim, then tap the rail's Alarms. The panel wakes to day brightness and does
  **not** open Alarms. A second tap does.
- **Slider preview (C21):** Panel → drag the night slider. The preview is not overwritten during the drag, and about 3 s
  after release the backlight returns to the computed level.
- **Idle wipe:**
  - Confirm zone 2's console has a line (7.4).
  - Config → Master → [Reveal] the Wi-Fi password → [Change] (the keyboard is open) → leave the glass untouched for
    2 min.
  - Home shows dimmed, with no keyboard, no dialog and no backdrop.
  - Zone 2's console log is empty, and the master editor shows `********`.

### 9.7 Web password: change, box vs idle return, restore -- T24, T27, S3.5, P12 `[STATE]`
System → Password. The phone is logged in.

**PASS** on each:
- "Set password" stays disabled until 8+ characters are entered. The field shows "(not entered)", then `********`.
- The keyboard is masked, and the eye re-masks after 10 s. Leaving the section with the keyboard open closes it, and on
  return the field shows "(not entered)".
- A new password → Set shows "Change the web password?" / "This logs out every phone and browser using the web UI".
  Tap [Cancel]: nothing changes.
- Again → [Change password]:
  - A large "Web password changed" box appears, and the section reads "Password changed. Every phone and browser was
    logged out -- log in again with the new password."
  - The phone's next page load goes to login. The new password works and the old one is refused.
- **Do not tap OK.** Leave the glass untouched for 2 min. Home is shown uncovered, with no box and no backdrop.

**Restore:** set `hillgrow1` back the same way (tap OK this time). The phone logs in with it. Run `hglogin` in W2.

### 9.8 Re-join the STA: scan, pick, join; NTP -- T23, S1.1c, S3.3, P9 `[STATE: restores 9.3]`
System → Wi-Fi.

**PASS:**
- Clear the SSID → Join shows "Enter an SSID".
- Scan → tap the house network → its password → Join shows "...", then "Saved -- joining...". The password field
  returns to "(none)" at once.
- Within 30 s the STA line shows an IP.
- Soon after, Home's top right reads `NTP`, and the clock is correct. This also removes the D14 hour.

### 9.9 AP change warning -- T23, S3.3, P10
**PASS:**
- A 5-character password → Set AP shows "AP SSID required, password 8+ chars".
- A valid pair → "Change the AP?" → Cancel shows "Not saved.", and nothing changes.
- Optional `[STATE]`: "Change AP" shows "..." then "Saved.", and the phones drop. **Restore** `HillGrow` / `hillgrow1`
  the same way, then rejoin the PC and the phone.

### 9.10 Time zone -- T23, T24, S3.4, P11 `[STATE]`
System → Time.

**PASS:**
- `XYZ` → Set TZ is refused, with a message.
- `EST5EDT,M3.2.0,M11.1.0` → "Time zone saved.", and within 2 s Home's clock moves by the offset difference.
- Set another TZ, and while "..." is up, edit the field to a third value. Switch to Wi-Fi and back: the newer draft is
  still shown.

**Restore** the TZ noted in 9.5.

**Also restore the dimming set in 9.4:** Panel → Night dimming "Follow the lights", idle 1 min, return after 5 min.

### 9.11 R2 readings -- T16 (S1.6), T20, T22, T23, S2.10
Every destination has been visited, and more than 10 min have passed. Fill in row R2. **PASS:** the 0.5 criteria.

---

## Part 10 -- Panel settings and reboots (Home → Panel)

### 10.1 Panel screen and sliders -- T26, S3.8
**PASS:**
- The save line and the Brightness, Night dimming, Clock face, Orientation and About cards all show and scroll. None is
  hidden under Home, and there is no `Panel build took` WARN.
- **Day slider:** the backlight follows the finger and the label shows `N %`. After release, "Saving...", then "Saved"
  within about 1 s. Note the level.
- **Night slider:** the backlight shows the night level while you drag, never below 5 % and never dark. On release it
  returns to the day level.

### 10.2 About -- T26
**PASS:**
- Version, and `Firmware: slot X, VALID`.
- The Internal RAM and LVGL lines meet 0.5.
- The touch counters rise when you tap.
- All four stack figures are nonzero and at least 1024.

### 10.3 Orientation Flipped -- T26, S3.8 `[STATE]`
1. Orientation → Flipped. The note reads "Now Normal -- Flipped applies after a restart."
2. Restart now → the Reboot confirm → Reboot → "Rebooting...".

**PASS after the boot:**
- The picture is rotated 180°.
- Panel shows Flipped selected, with "Applies after a restart.".
- The backlight is at the day level saved in 10.1.
- About → Touch test: all five targets light under the finger, and `last x,y` stays within 0..1023 / 0..599, never near
  65000.

**If the touch mapping is wrong under Flipped** (taps land on the wrong target, so the glass cannot reach Orientation):
record the FAIL, and recover without touch. In W1 send `CLEAR PANEL CONFIRM`; it answers
`OK PANEL CLEARED REBOOT TO APPLY`. Then send `REBOOT CONFIRM`. The panel boots Normal, with the default brightness
(80 %) and dimming ("Follow the lights" / 1 min / 5 min). Do not touch the Panel screen between the two commands: a
Panel change saves the live (Flipped) preferences again. Then skip to 10.5.

### 10.4 Normal again, via Reboot master -- T25, T26, S3.7, P17 `[STATE]`
1. Panel → Orientation → Normal.
2. System → Fleet → Reboot master. There is one confirm, because no trial is running. [Cancel]: nothing happens.
3. Reboot master → Reboot.

**PASS:**
- "Rebooting..." fills the screen at once, the master restarts, and the panel lights again by itself, the right way up.
- The Touch test's five targets land in Normal too.
- If you can open Alarms within a second of the light, it shows "Loading..." and then the lists (T16, optional).

### 10.5 The no-touch panel reset -- final review M7 (REQUIRED) `[STATE]`
The recovery that 10.3 falls back on, proven while the glass still works.
1. Panel → Night slider to 30 %, and Clock face → Analogue. Wait for "Saved".
2. In W1: `CLEAR HELP` lists a `+ CLEAR PANEL <...>  -- panel prefs to defaults; reboot to apply` line. Then send
   `CLEAR PANEL` alone: it is refused with an `ERR` line, and nothing changes.
3. In W1: `CLEAR PANEL CONFIRM`. It answers `OK PANEL CLEARED REBOOT TO APPLY`. The glass does not change yet.
4. In W1: `REBOOT CONFIRM`.

**PASS after the boot:** Panel shows the defaults: Normal, Digital face, day 80 %, night 10 %, "Follow the lights",
idle 1 min, return after 5 min.

**Restore:** set the day level back to the one noted in 10.1.

---

## Part 11 -- microSD, firmware, fleet, config export/import (System → Firmware; Config)

The phone stays on the Dashboard throughout. **FAIL** if the AP or the phone connection drops during any card operation.

### 11.1 No card -- T31, T32
Read card with no card inserted.

**PASS:**
- "No microSD card found -- insert a FAT32 card", and no crash.
- A later mount with a card works, which shows the LDO handle was released.

### 11.2 exFAT card -- T31, T32, S4.7
Insert card C → Read card.

**PASS:** "Card is not FAT32 -- exFAT cards (64 GB and up) must be reformatted to FAT32". Its `marker.txt` is checked on
the PC in 11.15.

### 11.3 Listing -- T31, T32, S4.1
Insert card A → Read card.

**PASS:**
- "Reading the card..." shows first.
- The rows appear in this order, each as `name | kind vX | N KB`: `hillgrow_master.bin` master, `hillgrow_zone.bin`
  zone, `esp32_master.bin` wrong chip, `other.bin` unknown.
- Only the master and zone rows have Install.
- The phone keeps loading, and W1 shows no `E SD_HOST` line after the unmount.

### 11.4 Import failures before any export -- T33
**PASS:**
- Config zone 2 → Import from card → Import shows "No /hillgrow/zone2.json on the card".
- Config zone 1 → Import shows "Invalid JSON file".
- Config Master → Import shows "File too large (4096 bytes max)".
- An edit made before any of these (a dot) is still there afterwards.

### 11.5 Card buttons vs Save -- T33, S4.6
Config zone 2 → a shelf tab, with the 4-way index selector showing.

**PASS:** "Export to card" and "Import from card" sit left of Save, and neither runs under it. Check the master editor
too.
**If one overlaps:** record it. The fix is the labels "Export" / "Import" in `cfg_card_bar()`.

### 11.6 Zone export and import; unsaved edits discarded -- T33, S4.6 `[STATE: re-imports the same values]`
**PASS** on each:
- **Export:** zone 2 → Export to card shows "Exported to /hillgrow/zone2.json".
- **Import over unsaved edits:**
  - Change two fields (two dots) → Import from card. The confirm says "Unsaved edits in this editor are discarded."
  - Import shows "Queued, pushing to zone (hw.... readonly ...)", and both dots go at once.
  - "Landed on the zone." follows. About 3 s later the rows show the imported values and the gen has advanced.
  - Save then says "No changes to save".
- **Another zone's import:**
  - Edit zone 2, then switch to zone 1 → Export to card (this overwrites the bad `zone1.json`) → Import.
  - Back on zone 2, its edit is still there. Only zone 2's own import drops it.

### 11.7 Stale import outcome -- T33 (C2)
Zone 2 → Import from card → Import, then at once switch to zone 1, or leave Config.

**PASS:**
- There is no crash.
- The outcome shows with a "Zone 2: " prefix, now or in the next Config editor.
- After an idle wipe, the outcome is gone.

### 11.8 Zone install and the interlocks -- T30, T32, S4.2, P18 (second half) `[STATE]`
Read card → Install on `hillgrow_zone.bin` → Install. Time it.

**PASS:**
- The bar advances, and "Installing -- the screen may pause..." shows.
- **Web interlock:** while the bar moves, in W2 run `hglogin`, then
  `hgpost /api/fw/zone C:\Projects\HillGrov\zone\build\hillgrow_zone.bin`. It answers `HTTP 409` and `UPLOAD_ACTIVE`.
- **Console interlock:** run the install again. While the bar moves, send `SET FW ZONE 2` in W1; it answers
  `ERR FW_BUSY`. If the install had already ended, that command starts a real update of zone 2 from the same image: let
  it finish.
- Each run ends with "Uploaded (<len> bytes)." and "Push it to zones from Fleet.". Leaving System and coming back
  mid-install shows the running bar, then the kept outcome.
- W1 `GET FW ZONE` reports the stored image. There is no writer-claim line and no `task not found`.

### 11.9 A panel install during a web upload -- T32
In W2, start `hgpost /api/fw/zone C:\Projects\HillGrov\zone\build\hillgrow_zone.bin`. While it runs (a few seconds),
tap Install on the zone row.

**PASS:** the panel shows "Another upload is in progress", nothing is written by the panel, and the web upload ends
`HTTP 200`.

### 11.10 Fleet from the staged image; Abort -- T25, T29, T32, S3.6, S4.4, P15, P16 `[STATE]`
System → Fleet.

**PASS:**
- "Status: IDLE", with one row per enrolled zone. Update and Update all are enabled, and Abort is disabled.
- **Update on zone 2:**
  - It shows "Update queued for zone 2." The status leaves IDLE, Update and Update all disable, and Abort enables.
  - While it runs, System → Firmware → Install on the zone row shows "A fleet update is running" or "A zone is
    downloading the image -- retry in a minute", and nothing is written.
  - Leaving System and coming back shows the kept outcome, not a stuck "...".
- **At the end:**
  - The Alarms history ends with `FW 2 TRIAL PASS`, and zone 2 is ONLINE with its config intact (spot-check one value
    against 6.2).
  - W1 shows no `zone.bin transfer abandoned`.
- **Abort:** Update zone 2 again, then tap Abort while it runs. "Fleet update aborted." shows, and the status returns to
  IDLE.

**Restore:** if zone 2 is not ONLINE on its old or new firmware within 2 min, tap Update zone 2 once more and let it
finish.

### 11.11 Zone card pull mid-install -- T31, T32, S4.7 `[STATE]`
Install `hillgrow_zone.bin`, and pull card A out while the bar moves.

**PASS:**
- The install ends with "microSD read failed", and there is no TWDT reset.
- W1 `GET FW ZONE` reports no image.

**Restore:** reinsert the card → Read card → Install the zone image again, which succeeds.

### 11.12 Master install and the OTA trial window -- T25, T28, T32, S4.3, P13 `[STATE]`
1. Read card → Install on `hillgrow_master.bin` → Install. Time it. It ends with "Uploaded v0.1.0 to <slot>." and
   **Reboot now**.
2. Reboot now → the Reboot confirm. There is one confirm, because the running image is VALID. Reboot → "Rebooting...".
3. After the boot, W1 shows `this boot is an OTA trial (PENDING_VERIFY) -- SKIPPING the ...`.
4. Within about 60 s, in this order:
   - (a) **System → Fleet:** the red "OTA trial in progress -- rebooting now retires this image" line shows. Reboot
     master → Reboot → a second dialog "OTA trial in progress" → **Cancel**. **PASS:** nothing restarts.
   - (b) **System → Firmware → Read card → Install master.** **PASS:** "An OTA trial is running -- wait for it to pass
     or SET OTA CONFIRM", and nothing is written.
   - (c) **In W2**, `hglogin`, then `hgpost /api/fw/master C:\Projects\HillGrov\master\build_p4\hillgrow_master.bin`.
     **PASS:** `HTTP 409` with `TRIAL_PENDING`, and W1 shows `upload refused: ota_X is still on trial`.

**PASS:** the Alarms history shows `FW 0 TRIAL PASS`, and Panel → About then shows the new slot `VALID`.

If the trial passes before you finish (a)-(c), repeat steps 1-2 and do whatever is left first.

### 11.13 Master card pull mid-install -- T31, T32 `[STATE]`
Note About's slot. Install `hillgrow_master.bin`, and pull the card while the bar moves.

**PASS:**
- The install ends with "microSD read failed", and there is no TWDT reset.
- About shows the same slot, `VALID`, and W1 `GET VERSION` is unchanged. The next reboot (12.2) boots the uploaded image
  from the suite, never a half-written one.

Reinsert card A.

### 11.14 Master export and import; unsaved edits -- T33, S4.6 `[STATE: re-imports the same values]`
Config → Master → Export to card.

**PASS** on each:
- The export succeeds, overwriting the padded `master.json`.
- Type a new AP password (unsaved) and change HOSTNAME (unsaved) → Import from card → Import:
  - "Saved." shows, and the dots are gone.
  - [Reveal] AP_PASS shows the **stored** password, not the typed one.
  - Save says "No changes to save".
  - The phone stays connected, because the AP password was kept.

### 11.15 PC visit: master.json and the exFAT card -- T31, T33, S4.6, S4.7
Pull card A and open `E:\hillgrow\master.json` on the PC.

**PASS:** it has `WIFI`, `TIME` and `SYS`, and **no** `STA_PASS` and **no** `AP_PASS`.

Then edit it: right after `"WIFI":{` insert `"AP_PASS":"short",` and save.

Plug in card C. **PASS:** `marker.txt` is still there, so the card was never formatted.

Reinsert card A into the panel.

### 11.16 Master import refused, edits kept -- T33
Config → Master. Make one edit (a dot) → Import from card → Import.

**PASS:**
- The panel shows "INVALID_FIELD: WIFI.AP_PASS" (or that token's text), and nothing is changed.
- The dot is still there. Undo the edit.

### 11.17 The card changed since Read card -- T33 (T32 ride-along)
1. Read card, with the listing showing.
2. Change the zone image:
   - with card B: swap it in;
   - or: pull card A, run `$f="E:\hillgrow_zone.bin"; [IO.File]::WriteAllBytes($f, [byte[]]([IO.File]::ReadAllBytes($f) + (New-Object byte[] 16)))`,
     and reinsert it.
3. Do **not** Read card. Tap Install on the zone row → Install.

**PASS:** "The file on the card changed since Read card -- nothing was written. Read the card again.", and W1
`GET FW ZONE` is unchanged.

Card A now holds a padded zone image. Delete it after the session.

### 11.17a Card teardown under Wi-Fi traffic -- final review C1 (REQUIRED)
**Why:** stock IDF 6.0.1 panics the master on the C6's next SDIO interrupt after a card unmount or a failed mount
(`sd_host_isr()` dereferences the removed slot 0). The build carries a patched driver,
`components/esp_driver_sdmmc` (its `README.md` has the diff). This check is its bench proof. It runs straight after
11.17, while the Firmware listing is still stale.

Set-up: the phone on the Dashboard, a second phone (or a laptop) on `http://192.168.7.7/#/alarms`, both logged in. In
W2, start background traffic and leave it running:
`python C:\Projects\HillGrov\tools\web_test.py 192.168.7.7 --password hillgrow1 --soak 900`.

1. **Install refused, 7 times.** Leave the card from 11.17 in the slot and do not Read card. Tap Install on the zone
   row → Install, 7 times. Each one mounts the card, is refused ("The file on the card changed since Read card..."),
   and unmounts it.
2. **Export, 7 times.** Config → zone 2 → Export to card, 7 times. Each shows "Exported to /hillgrow/zone2.json".
3. **Read card, 6 times.** System → Firmware → Read card, 6 times. Each lists the card.
4. **Read card with no card, 10 times.** Pull the card. Read card 10 times. Each shows "No microSD card found --
   insert a FAT32 card". These are 10 failed mounts, and each one's cleanup removes slot 0.
5. Wait 60 s with the phones still polling.

**PASS:**
- Through all 30 operations and the 60 s after them, W1 shows no `panic`, `abort`, `Backtrace`, `Guru Meditation`,
  `LoadProhibited`, `StoreProhibited`, `task_wdt` or boot banner. Any of these is a **STOP** (0.4). If the backtrace
  names `sd_host_isr`, the driver override is not in the image: build with `CONFIG_HILLGROW_PANEL_SD=n` (the kill
  switch in `components/panel_ui/Kconfig`) until it is fixed.
- The phones' pages keep updating throughout, with no gap longer than a few seconds.
- Afterwards the AP is still serving:
  - `netsh wlan show interfaces` shows `HillGrow`;
  - W1 `GET STATUS` shows an uptime that covers the whole check (no reboot);
  - the W2 soak finishes and passes;
  - `python C:\Projects\HillGrov\tools\web_test.py 192.168.7.7 --password hillgrow1 --only state` passes.
- The only `E SD_HOST` lines follow the failed mounts of step 4 (0.4's rule).

Reinsert card A for 11.18.

### 11.18 20 mount cycles: the C6 and the AP survive -- T31
Tap Read card 20 times: 14 with card A, 3 with no card and 3 with card C.

**PASS:**
- The phone's Dashboard keeps updating throughout.
- There are no esp_hosted SDIO errors and no resets. The only `SD_HOST` E lines follow the failed mounts.
- Afterwards:
  - `netsh wlan show interfaces` still shows `HillGrow`;
  - System → Wi-Fi's STA line updates, and a Scan works;
  - `python C:\Projects\HillGrov\tools\web_test.py 192.168.7.7 --password hillgrow1 --only state` passes.

### 11.19 Idle wipe of the firmware outcome -- T32
After an install outcome is showing, leave the glass untouched for 5 min (the idle-return default).

**PASS:** Home is shown, and System → Firmware shows no kept outcome, only "Put .bin files in the card's root or in
/hillgrow, then Read card.".

### 11.20 R3 readings -- T32, T33, S4
This comes after a master install and a config import. Fill in row R3. **PASS:** the 0.5 criteria. Also record the two
install times, and whether the UI visibly paused during flash erases.

---

## Part 12 -- Web and CLI regressions (panel on Home)

### 12.1 Poller through a web Wi-Fi scan -- T11
Panel → About → Touch test (the `Poller: seq` line showing). In W2 run
`python C:\Projects\HillGrov\tools\web_test.py 192.168.7.7 --password hillgrow1 --only wifi`.

**PASS:** `seq` keeps rising about once a second for the whole scan, and the suite passes. The AP dropping briefly is
expected. Tap the Touch test's Home.

### 12.2 Full web suite with uploads and fleet -- T1, T4, T5, T9, T15, T25, T26, T28-T30, T33, S0.6, S1.6, S2.9, S4.5, P14 `[STATE]`
Run:
```powershell
python C:\Projects\HillGrov\tools\web_test.py 192.168.7.7 --password hillgrow1 --master-bin C:\Projects\HillGrov\master\build_p4\hillgrow_master.bin --zone-bin C:\Projects\HillGrov\zone\build\hillgrow_zone.bin --fleet 2
```

**PASS:**
- Every line PASS, and `N/N passed`. That includes `zone image -> /api/fw/master -> 422 IMAGE_MISMATCH`, the master OTA
  (PENDING, then VALID), the zone upload, the fleet update of zone 2, CONFIG, MCFG and STATE.
- The panel comes back by itself after the master reboot.
- W1 shows no writer-claim line, no `zone.bin transfer abandoned` and no `task not found`.

The run takes about 10 min.
- The phone is logged out (MCFG).
- LOGIN locks the web out for about 60 s at the end.
- The master ends on the other slot, holding the same build.

**Restore:** after the lockout (about 60 s), log the phone back in with `hillgrow1`.

### 12.3 Web by hand: image identity and AP_PASS -- T28, T30, T31, T33, D22
Wait 60 s after 12.2 (the lockout), then run `hglogin`.

**PASS:**
- `hgpost /api/fw/master C:\Projects\HillGrov\master\build\hillgrow_master.bin` (the ESP32 image) answers `HTTP 422`
  with `IMAGE_MISMATCH`, not `WRITE_FAILED`. About's slot and state are unchanged.
- `hgpost /api/fw/zone C:\Projects\HillGrov\master\build_p4\hillgrow_master.bin` answers `HTTP 422` with
  `IMAGE_MISMATCH`.
- `hgput0 $env:TEMP\hg_blank_ap.json` answers `HTTP 400`, with path `WIFI.AP_PASS`.

`[STATE]` If it answers 200 instead, restore the AP at once: System → Wi-Fi → Set AP `HillGrow` / `hillgrow1`.

### 12.4 CLI suite, and the TZ echo -- T4, T15, T28, T30, S0.6
1. Close W1.
2. Run `C:\Python311\python C:\Projects\HillGrov\tools\uart_test.py COM28 --role MASTER`. **PASS:** all PASS, and the
   NOTIFY lines are unchanged.
3. Reopen W1: `GET TZ`, then `SET TZ <exactly what GET TZ printed>`. **PASS:** `OK TZ <that value>`.

---

## Part 13 -- Task 34 acceptance: every destination on real glass

### 13.1 Tap every destination once -- A (T34 Step 2)
Each item must respond visibly under the finger: a pressed state, then the right screen. Most were tapped already; tick
those as done, and tap the rest now.

- [ ] Home: the clock, a band tile (opens that zone), the alarm band (8.1), HillGrow (Dashboard), Audio (the SP7
  text), Panel.
- [ ] Rail: Home (from any rail screen), Dashboard, Zone 1, Zone 2.
- [ ] Config zone 1: ZONECFG SHELF LIGHT WATER FAN VIB AUX HW HWSHELF CAL.
- [ ] Config zone 2: ZONECFG SHELF LIGHT WATER FAN VIB AUX HW HWSHELF CAL.
- [ ] Config Master: WIFI TIME SYS.
- [ ] Alarms.
- [ ] System: Wi-Fi, Time, Password, Fleet, Firmware.
- [ ] Panel: About, and Touch test (the five targets light under the finger, never the opposite one).

---

## Part 14 -- Soak and budgets

### 14.1 30-minute soak with night dimming -- A (T34 Step 5)
1. Panel → Night dimming = Fixed hours, from the current hour to two hours later, idle 30 s. Go Home and leave the
   glass alone.
2. In W2 run `python C:\Projects\HillGrov\tools\web_test.py 192.168.7.7 --password hillgrow1 --soak 1800`.

**PASS:**
- The soak passes, and the heap before and after are within 8 KB (the soak's own report).
- The clock never stopped (glance at it now and then).
- Alarms → Active and the web's `#/alarms` show no active `ALARM 0`, and the History has no `W_PANEL_FROZEN`.

For the D5 note, record: `W_PANEL_FROZEN` seen this session, yes/no; any hang of the web UI seen, yes/no.

**Restore:** Night dimming "Follow the lights", idle 1 min, return after 5 min.

### 14.2 R4 readings and budgets -- A (T34 Step 6), C9
Fill in row R4. **PASS:** the 0.5 criteria.

**C9, decided here.** If any row's LVGL max used is above 48 KB, the plan raises `CONFIG_LV_MEM_SIZE_KILOBYTES` to 128,
but only if the internal minimum stays at least 64 KB. The raise costs about 64 KB of internal RAM, so it is safe only
when R4's internal minimum is at least 128 KB. Otherwise leave the pool at 64 KB: the PSRAM overflow pool covers
exhaustion, at some speed cost. Record the decision.

**Budget summary for the report:**
- image 1,601,392 B of 4,194,304 (and of the 2 MB factory partition: 76.4 %);
- R4 internal free/min;
- R4 LVGL used/peak;
- boot-to-lit `up in N ms` (3.1), plus 15.1's figure if it was run.

### 14.3 ESP32 fallback master: heap-min after /api/alarms -- T15, C10, final review I2 (REQUIRED)
This is a hard gate, not optional: the ESP32 (DevKitC) master is the maintained fallback, and its heap margin is thin.
The final review moved `/api/state`'s ~1.9 KB gather back onto the httpd stack (h_state's frame is 2,160 B, against
2,224 B before Task 9; the httpd stack is 8 KB), and `/api/alarms` no longer mallocs a 6.6 KB snapshot per request.
This needs the DevKitC, and the P4 powered off so that only one `HillGrow` AP is up.
1. Flash it:
   - `python C:\Projects\HillGrov\tools\flash_app.py --app master --build-dir C:\Projects\HillGrov\master\build --port <DevKitC COM> --dry-run`
   - then the same command without `--dry-run`.
2. Run `python C:\Projects\HillGrov\tools\web_test.py 192.168.7.7 --password hillgrow1 --only state`. This exercises
   `/api/state` and `/api/alarms`.
3. Send `GET STATUS` on its UART.

**PASS:**
- heap min ≥ 64 KB (65,536 B). For reference, SP4 measured 68,488 B. This build's `.bss` is 92,288 B (94,200 B
  before the final review) with 70,797 B of DRAM free (68,885 B before).
- No `Stack canary` / `stack overflow` on its UART during step 2 (the httpd task carries h_state's gather again).
- **FAIL** below 64 KB. That fails the acceptance.

**Restore:** power the DevKitC off and the P4 on again.

---

## Part 15 -- Optional (record "not run" for any you skip)

### 15.1 C6 held in reset: boot-to-lit, and a radio-down Read card -- A (T34 Step 6), T32
Jumper the C6's **EN pin** (not GPIO54) to GND, then press RESET.

**PASS:**
- `panel: up in N ms` appears. Record it as boot-to-lit with the C6 in reset.
- The panel lights.
- Two Read card passes in a row both list card A, and the second one does not crash.

**Restore:** remove the jumper and press RESET.

### 15.2 Missing GT911 -- T7, S0 (optional)
Power off, disconnect the touch ribbon, and power on.

**PASS:**
- The log shows `E panel_hw: bsp_touch_new: ESP_ERR_NOT_FOUND -- display only (GT911 not answering?)` and
  `panel: up in N ms (lvgl c1/p3, touch UNAVAILABLE)`.
- The screen lights with `Touch UNAVAILABLE`.
- The CLI answers, the AP comes up, and nothing reboots.

**Restore:** reconnect the ribbon while powered off.

### 15.3 Remove and re-enrol zone 2 -- T16, S1.3 `[STATE]`
Note zone 2's MAC. W1 `CLEAR NODE 2 CONFIRM`.

**PASS:** zone 2's tile goes on the next poll, and the remaining tile re-centres.

**Restore:** `SET NODE 2 MAC <mac>`. The tile returns on the next heartbeat.

### 15.4 "Reboot anyway" retires a trial image -- T25 `[STATE]`
Repeat 11.12 steps 1-2. During the trial: Reboot master → Reboot → "Reboot anyway".

**PASS:** the previous slot boots, and About shows it `VALID`. Both slots hold the same build, so nothing is lost.

### 15.5 "Firmware state unknown" in the first second -- T25
Only if you can reach System → Fleet → Reboot master within about 1 s of the panel lighting.

**PASS:** the second confirm reads "Firmware state unknown". Otherwise record "not exercised".

### 15.6 (moved)
The ESP32 fallback master's heap-min check is now **14.3, REQUIRED** (final review I2).

**Not exercisable on this bench** (record as "not exercised"):
- the badge tap target with 8 tiles (T13; there are 2 zones);
- NVS writes disabled (T26 step 8);
- a frozen LVGL task (T11 step 6);
- a board whose display bring-up fails (T8 step 6);
- `SET FW ZONE` refused on a boot where the AP fails (T30 step 4);
- the boot race between a console `SET FW ZONE` and a web upload (T31 step 10);
- a failed-reboot overlay, unless a refusal can be provoked (T27);
- forcing the Dashboard's worst-case readings (T12; 5.3 records what showed).

---

## Coverage maps (for the Task 34 report)

**Parity checklist (Task 34 Step 4, map-parity §G):**

| § G | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 | 16 | 17 | 18 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| Check | 8.1/8.5 | 7.1 | 7.3 | 6.2/6.3 | 6.1 | 8.3 | 6.9 | 8.2 | 9.8 | 9.9 | 9.10 | 9.7 | 11.12 | 12.2 | 11.10 | 11.10 | 10.4 | 6.7/11.8 |

**Stage gates on HEAD (Task 34 Step 3):**

| Gate | Steps → checks |
|---|---|
| S0 | 1→1.1, 2→2.1, 3→3.1, 4→4.1, 5→4.2, 6→12.2/12.4, 7→3.1 + 14.2 |
| S1 | 1→9.3/9.5/9.8, 2→5.2, 3→5.1 (+15.3), 4→8.1/8.2/8.5, 5→5.1-5.4 + 13.1, 6→11.18/12.2 + 9.11 |
| S2 | 1→6.1, 2→6.2, 3→6.3, 4→6.4, 5→8.3/8.5, 6→6.7, 7→6.9, 8→6.8, 9→12.2, 10→9.11 |
| S3 | 1→7.1/7.2, 2→7.3, 3→9.2/9.8/9.9, 4→9.3/9.5/9.10, 5→9.7, 6→11.10, 7→10.4, 8→10.1/10.3/10.4/9.6, 9→9.4/9.6; readings R1/R2 |
| S4 | 1→11.3, 2→11.8, 3→11.12, 4→11.10, 5→12.2, 6→11.5/11.6/11.14/11.15, 7→11.2/11.11/11.15; readings R3 |

**End state to confirm before leaving the bench:**
- the web password is `hillgrow1`, and the AP is `HillGrow` / `hillgrow1`;
- the STA is joined, and the TZ, NTP server and hostname are the originals;
- zone 2's WATER target is V;
- the orientation is Normal, the clock face Digital, and the dimming "Follow the lights" / 1 min / 5 min;
- zone_fw is staged (`GET FW ZONE`), and the master is `VALID`;
- both zones are ONLINE;
- the cards are out of the slot.

---

## Owner items that are not hardware

1. **Run** `python C:\Projects\HillGrov\tools\web_test.py --selftest`. It is offline, and the session's permission
   check refused it for the agent. The expected output is `SELFTEST OK (9 assertion groups)`.
2. **Decisions the ledger leaves with you.** Each has a default that stands until you decide otherwise:

| Item | The question | Default in the code today |
|---|---|---|
| D22 LOW_HEAP guard | Switch the install core's guard to `heap_caps_get_free_size(MALLOC_CAP_INTERNAL)`, so that it can trip on the P4 (a deliberate web change) | Not switched; the guard never trips on the P4 |
| D5 httpd / LVGL TWDT | Subscribe httpd and the LVGL task to the TWDT, or add a loopback liveness probe | Unsubscribed, plus the LVGL heartbeat alarm `W_PANEL_FROZEN`; 14.1 gives the evidence |
| C9 LVGL pool | Raise `CONFIG_LV_MEM_SIZE_KILOBYTES` to 128 | 64 KB; decided from 14.2 |
| D7 per-target lock | `DEPENDENCIES_LOCK ...dependencies.lock.esp32p4` in `master/CMakeLists.txt`, retiring the `git checkout master/dependencies.lock` step | Not done |
| D9 AP_DEFAULT flag | Clear `MCFG_F_AP_DEFAULT` on a zone-0 AP_SSID/AP_PASS save | Not cleared; System → Set AP clears it |
| D10 BUSY vs STORAGE | The CLI rows and POST /api/wifi report lock contention as `ERR STORAGE` / 503 | Unchanged; only the panel says BUSY |
| D27 web ZONE_NOT_ONLINE | The web still says "Zone busy, retry" for an offline zone | Unchanged; the panel splits the two |
| D28 web dashboard time | The web shows UTC, unlabelled | Unchanged; the panel shows local time with `(UTC+hh:mm)` |
| D20 recovery plan | Its esp_hosted deinit/reconnect must hold off panel microSD mounts through a real handshake with the worker | To be carried into the recovery spec/plan |
| D11 SHELF.PROFILE | Stays a 0..16 stepper until SP4b profiles exist | As is |
| Stack secret copies | `mcfg_ops.c:78`, `web_auth.c:118` and `hg_json_merge_mcfg` leave password copies unwiped (they predate the plan) | For the follow-ups list |
| LVGL textarea residue | Password text lingers in the freed LVGL pool | Accepted: physical access is the gate |
| D14 manual clock | A manual Set clock while the clock is unset is 1 h off in DST until NTP syncs | As designed (9.5) |
| Config/rail zone memory | A zone picked in Config is not the one the rail's Zone reopens (T20, T21 parks) | Cosmetic; for the final review |
| drawio stamp | Stamp the "Master v2 (P4) Topology" page title in `docs/hillgrow-features.drawio` | Yours; the agents never touch that file |

---

## After the session (Task 34 Steps 7-8, done by the agent from your results)

1. **`task-34-report.md`** (`.superpowers/sdd/2026-09-23-master-v2-panel-ui/`) gets the signed-off checklist:
   - every check here, marked PASS, FAIL or not exercised, with the text of each failure;
   - the readings sheet;
   - the §G 1-18 list;
   - the C9 decision;
   - the D5 evidence.
2. **If everything passes, the agent makes the three doc edits of Task 34 Step 7:**
   - Append to `docs/what_we_learned.md` the section below, with every `<...>` filled from this session.
   - In `docs/superpowers/specs/2026-09-18-master-v2-panel-ui-design.md`, line 3 becomes
     `**Status:** implemented; bench-verified <date> (plan docs/superpowers/plans/2026-09-23-master-v2-panel-ui.md, owner acceptance Task 34).`
   - In `docs/superpowers/plans/2026-09-21-master-v2-followups.md`:
     - close item 6 (cfg_put_zone0 is now `psvc_mcfg_edit`);
     - in item 2, `running_slot_on_ota_trial()` becomes `ota_trial_running_on_trial()`;
     - add the D5 status line, with the 14.1 evidence;
     - mark the spike deletion done, and add the drawio "stamp" explanation;
     - add the new section "Left open by the panel UI plan" (D7, D9, D10, D27, D28, D20, D11, D22).
3. **Step 8:** commit those three files by explicit path. `docs/hillgrow-features.drawio` stays ` M` and unstaged,
   before and after. Then push.
4. **If C9 says raise:** that is a separate small task:
   - set `CONFIG_LV_MEM_SIZE_KILOBYTES=128` in `master/sdkconfig.defaults.esp32p4`;
   - delete `master\build_p4\sdkconfig`, then run GATE-P4 and grep for the key;
   - flash, and re-read About. The internal minimum must stay at least 64 KB.
5. **Any FAIL** becomes a fix task. Afterwards, re-run only the failed checks and the checks that share their code, on
   the fixed image.
6. Delete the padded `hillgrow_zone.bin` (11.17) and the test JSON files from card A.

**The `what_we_learned.md` section Task 34 records:**
```markdown
## 2026-MM-DD — Master v2 panel UI: what the bench taught

The panel face (components/panel_ui) and the shared service layer (components/panel_svc) went through five bench gates
and an owner acceptance. What held, and what to carry forward:

- **Reads leave the LVGL task, not only writes.** `wifi_mgr_status()` is two esp_hosted RPCs on the P4 (up to 5 s each)
  and `nmgr_lock` waits forever, so the panel reads only snapshot copies published by `pnl_poll` (1 Hz) and `pnl_wifi`
  (5 s). The clock kept ticking through a 30 s Wi-Fi scan and through flash erases.
- **One door for both faces.** Master config, zone config, net ops, fleet, the state gather and the firmware install are
  all `panel_svc` functions now; the web handlers are thin HTTP mappers over them. A refusal means the same thing on the
  glass and on the phone because both print the same `psvc_rc_t` token.
- **The display lock is never ignored and never waited on forever.** `panel_lock()` promotes 0 to 1 ms and every caller
  checks the result; nothing blocking is called under it.
- **microSD shares the SDMMC controller with the C6.** Mount with a no-op `host.init`, keep `deinit_p =
  sdmmc_host_deinit_slot`, create and delete the LDO-4 handle per mount, mount transiently on the worker. FAT32 only.
- **Wake-only first touch.** A transparent catcher on the top layer swallows the first press on a dimmed panel; the
  adapter's `auto_sleep` stays off because it pauses the LVGL worker and freezes the clock.
- **Measured at acceptance:** image <size> B of 4,194,304; internal RAM free <free> KB / min <min> KB; LVGL used
  <used> KB / peak <peak> %; boot to lit <ms> ms<, and <ms> ms with the C6 held in reset | ; the C6-in-reset test was
  not run>.
- **Surprises on the bench:** <one bullet per surprise recorded in the Stage 0-5 task reports, or "none">.
```
Where each value comes from:
- `<size>`: 1,601,392 (2.1).
- `<free>` and `<min>`: R4.
- `<used>`: R4. `<peak>`: R4 as a percentage of 64 KB.
- `<ms>`: 3.1, and 15.1 if it was run.
- "Surprises": at least the upstream BSP uninitialised-pointer error path at `esp32_p4_wifi6_touch_lcd_7b.c:432-440`
  (T7), plus anything this session found.
