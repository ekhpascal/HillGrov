# Master v2 Platform Migration Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the real `master/` application build, flash and run on the ESP32-P4 board, with everything SP4 already delivers still working — web UI, ring, fleet OTA, alarms, NTP — so the P4 becomes the Master.

**Architecture:** No rewrite. All 31 master-side components already compile for RISC-V unchanged (proven; see *Spike evidence*). What is missing is platform glue: a retargetable build, a P4 partition table, a P4-capable custom bootloader, the co-processor firmware moved out of the app into its own partition, and one refactor that pulls the master-config read-modify-write out of `master/main` into a component so a second caller cannot drift from it.

**Tech Stack:** ESP-IDF 6.0.1, ESP32-P4 (rev v1.x silicon) + on-board ESP32-C6 over SDIO, `espressif/esp_hosted` 3.0.7, `espressif/esp_wifi_remote`, C11, Unity host tests under MSVC.

**Spec:** `docs/superpowers/specs/2026-08-31-hillgrow-system-design.md` §11.10 (platform decision and migration gate) and `docs/pin-mapping.md` (the Master v2 pin table). This plan is the durable record of the bring-up spike's findings; the throwaway spike directory is deleted in the final task.

## Global Constraints

- **ESP-IDF 6.0.1 only**, at `C:\esp\v6.0.1\esp-idf`. Run `& C:\esp\v6.0.1\esp-idf\export.ps1` in PowerShell before any `idf.py`. The Bash tool cannot source the IDF environment — `export.sh` fails and `idf.py` is then not found, while the shell still reports exit 0, so **always read build output rather than trusting the exit code**.
- **Never `idf.py flash` a HillGrow app.** Use `python tools/flash_app.py --app master|zone|rescue|zonefw --port COMx`. `idf.py flash` writes the factory/rescue slot.
- **The ESP32 build must stay byte-for-byte behaviourally unchanged** at every task boundary. `master`, `zone` and `rescue` all build clean and the host suite passes 100%. This is the regression gate for every task.
- **How the host suite is counted.** `tests/host/CMakeLists.txt` defines `hg_test(NAME)` which calls `add_test()` **once per file**, so CTest reports one entry per test *file*, not per Unity `RUN_TEST` case. The baseline is **32 entries** (containing 446 RUN_TEST cases); each new test file adds exactly **one** CTest entry however many cases it holds. Verified empirically 2026-09-21 after an earlier draft of this plan got the arithmetic wrong.
- **P4 silicon is rev v1.3.** IDF 6.0.1 defaults to rev v3.1 and the two families are mutually exclusive — esptool refuses the flash outright with *"requires chip revision in range [v3.1 - v3.99]"*. Every P4 config carries `CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y` and `CONFIG_ESP32P4_REV_MIN_100=y`. A binary built this way runs **only** on P4 rev 0.x/1.x.
- **The board has 32 MB of flash**; the stock default declares 2 MB in the image header and the bootloader clamps to it. `CONFIG_ESPTOOLPY_FLASHSIZE_32MB=y`.
- **P4 flash offsets differ from ESP32:** bootloader `0x2000` (not `0x1000`), partition table `0xF000` (not `0xE000`). Note this is **not** IDF's P4 default of `0x8000`. The custom bootloader is `0x60f0` bytes, and a table at `0x8000` leaves only `0x6000` of room above the bootloader at `0x2000`, so the build fails a post-link assertion by 240 bytes. `0xF000` gives the P4 the same `0xD000` bootloader window the ESP32 has, using the dead space below `nvs` at `0x10000`, and moves no partition. The stock P4 bootloader already fills ~96% of the `0x6000` window, so this is not caused by HillGrow's own bootloader features (~830 bytes). Measured and built clean 2026-09-21.
- **How to configure a P4 build.** `SDKCONFIG_DEFAULTS` does **not** select the target. `tools/cmake/targets.cmake` builds its search list as `"${SDKCONFIG}" "${CMAKE_SOURCE_DIR}/sdkconfig" "${defaults}"` and stops at the first file carrying a `CONFIG_IDF_TARGET` line, so the existing ESP32 `master/sdkconfig` wins — and within the defaults list `sdkconfig.defaults` (`CONFIG_IDF_TARGET="esp32"`) beats `sdkconfig.defaults.esp32p4`. A P4 build therefore **requires `-DIDF_TARGET=esp32p4`**, and must always be proved by grepping the generated sdkconfig for `CONFIG_IDF_TARGET="esp32p4"`. Without it the build silently produces an ESP32 image while still picking up the P4 flash size and table offset, which looks entirely convincing. Also: never run `idf.py set-target` from inside `master/` — it damages `master/build` even when `-B` points elsewhere.
- **Main-only by owner consent.** Commit directly to `main`; push when a task is green. Commit trailer: `Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>`.
- **Heap floor:** master heap-min ≥ 64 KB (spec §6.4), with the 40 KB `LOW_HEAP` guard as the hard floor.

## Spike evidence this plan builds on

All verified on hardware 2026-09-18/21. Do not re-litigate these; they are inputs.

| Fact | Consequence for this plan |
|---|---|
| All 31 master-side components compile for esp32p4, zero warnings | No component source changes needed. `wifi_mgr` compiles against `esp_wifi_remote` unmodified |
| softAP + DHCP + `esp_http_server` serving 246 KB at 1571 kB/s | The SP4 web stack works over the C6 |
| Panel + GT911 touch work via `waveshare/esp32_p4_wifi6_touch_lcd_7b` 3.0.1 | Out of scope here; see the panel UI plan |
| C6 co-processor OTA over RPC works; CP now 3.0.7 matching the host | Task 6 productises it |
| P4 master + 3 real zones: enrolment, health ladder, correct blame `Z2 dead or wire Z2->Z3`, config generations preserved across the master transplant | The ring works; Task 8 re-verifies against the real app |
| `board.h` is already target-aware (commit `825c494`) | Ring on IO28/IO29, I²C on 8/7 |

---

### Task 1: Make the master build retargetable, and add the P4 registry deps

`master/CMakeLists.txt` does `set(SDKCONFIG_DEFAULTS "sdkconfig.defaults")` unconditionally, which shadows any `-D SDKCONFIG_DEFAULTS=` override — so the app cannot currently be built for a second target at all. This task changes nothing for ESP32 and gets the P4 build as far as its first real failure.

**Files:**
- Modify: `master/CMakeLists.txt`
- Create: `master/sdkconfig.defaults.esp32p4`
- Modify: `master/main/idf_component.yml`

**Interfaces:**
- Consumes: nothing.
- Produces: a P4 build that configures and compiles. Later tasks configure it with, from `master/`:
  `idf.py -B <ABSOLUTE build dir> -DIDF_TARGET=esp32p4 -DSDKCONFIG=<ABSOLUTE sdkconfig> -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.esp32p4" build`
  Both paths must be absolute (a relative `-B` resolves against the shell cwd, not `-C`), and `-DIDF_TARGET` is mandatory — see Global Constraints for why `SDKCONFIG_DEFAULTS` alone silently yields an ESP32 image.

- [ ] **Step 1: Make SDKCONFIG_DEFAULTS overridable**

In `master/CMakeLists.txt`, replace the unconditional `set`:

```cmake
# Overridable so the app can be configured for a second target without editing
# this file. IDF appends sdkconfig.defaults.<target> to whatever this names, so
# the P4 settings live in sdkconfig.defaults.esp32p4 and the ESP32 build is
# unaffected.
if(NOT DEFINED SDKCONFIG_DEFAULTS)
    set(SDKCONFIG_DEFAULTS "sdkconfig.defaults")
endif()
```

- [ ] **Step 2: Add the P4 target defaults**

Create `master/sdkconfig.defaults.esp32p4`. IDF loads this *in addition to* `sdkconfig.defaults`, so it only needs the deltas — but `CONFIG_IDF_TARGET` in the base file pins esp32, so override it here:

```
CONFIG_IDF_TARGET="esp32p4"

# Early P4 silicon (rev v1.3). IDF 6.0.1 defaults to rev v3.1 and the families
# are mutually exclusive: without these, esptool refuses the flash with
# "requires chip revision in range [v3.1 - v3.99] (this chip is revision v1.3)".
CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y
CONFIG_ESP32P4_REV_MIN_100=y

# 32 MB on the board; the stock default declares 2 MB and the bootloader clamps.
CONFIG_ESPTOOLPY_FLASHSIZE_32MB=y

# The partition table sits at 0xF000, NOT IDF's P4 default of 0x8000. The custom
# bootloader is 0x60f0 bytes and 0x8000 leaves only 0x6000 above the bootloader
# at 0x2000, which fails a post-link size assertion by 240 bytes. 0xF000 gives
# the same 0xD000 window the ESP32 has, using dead space below nvs at 0x10000.
CONFIG_PARTITION_TABLE_OFFSET=0xF000
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions_p4.csv"

# The MIPI-DSI frame buffer and LVGL need PSRAM (ESP32-P4NRW32, in package).
# Harmless here and required by the panel plan that follows.
CONFIG_SPIRAM=y
```

- [ ] **Step 3: Add the P4-only registry dependencies**

`wifi_mgr` calls `esp_wifi_*`, and on the P4 there is no local radio — the calls must route to the C6. Without these the build fails at `mdns.h` and the `esp_wifi_remote` hooks. Rewrite `master/main/idf_component.yml`:

```yaml
dependencies:
  idf: ">=6.0"
  espressif/cjson: "^1.7.18"
  espressif/mdns: "^1.8.0"
  # P4 only: esp_wifi has no radio of its own on this target, so every
  # esp_wifi_* call routes to the on-board C6 over SDIO. The rules key keeps
  # the ESP32 build's dependency set unchanged.
  espressif/esp_hosted:
    version: "*"
    rules:
      - if: "target == esp32p4"
  espressif/esp_wifi_remote:
    version: "*"
    rules:
      - if: "target == esp32p4"
```

- [ ] **Step 4: Verify the ESP32 build is untouched**

Run:
```
& C:\esp\v6.0.1\esp-idf\export.ps1
idf.py -C master build
idf.py -C zone build
idf.py -C rescue build
```
Expected: all three `Project build complete`, host suite `100% tests passed ... out of 32`. If `master/sdkconfig` was modified by any P4 experiment, `git checkout master/sdkconfig` first — it is a tracked working file.

- [ ] **Step 5: Verify the P4 build now configures and compiles**

Run:
```
idf.py -C master -B build_p4 -D SDKCONFIG=build_p4/sdkconfig set-target esp32p4
idf.py -C master -B build_p4 -D SDKCONFIG=build_p4/sdkconfig build
```
Expected: components compile; the build then FAILS in the bootloader subproject on `esp32/rom/gpio.h: No such file or directory`. That exact failure is success for this task — it means every application component built and only the custom bootloader remains. Record the message in the task report.

- [ ] **Step 6: Commit**

```bash
git add master/CMakeLists.txt master/sdkconfig.defaults.esp32p4 master/main/idf_component.yml
git commit -m "build(master): retargetable config plus the P4 defaults and registry deps"
```

---

### Task 2: P4 partition table, with the co-processor image in its own partition

The ESP32 table is 8 MB with app slots of 2 MB. The P4 has 32 MB, the master image will grow (LVGL, fonts), and the C6 firmware needs a home. Embedding the 1.09 MB CP image inside the app — which the spike did — bloats every master OTA by a megabyte of unchanged bytes. Give it a partition.

**Files:**
- Create: `master/partitions_p4.csv`
- Create: `tests/host/test_partitions_p4.c`
- Modify: `tests/host/CMakeLists.txt`

**Interfaces:**
- Consumes: Task 1's `CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions_p4.csv"`.
- Produces: partition names `factory`, `ota_0`, `ota_1`, `zone_fw` (subtype `0x40`), `cp_fw` (subtype `0x41`), `data`. Task 6 reads `cp_fw`. `fw_srv` already looks up `zone_fw` by name, so that name must not change.

- [ ] **Step 1: Write the failing test**

The value of a test here is pinning the invariants a hand-edited CSV gets wrong: app-partition 64 KB alignment, no overlaps, nothing before the table, and the names the firmware looks up by string. Create `tests/host/test_partitions_p4.c`:

```c
/* The P4 partition table is hand-maintained and every mistake in it is a
   brick-or-silent-truncation class bug: an app slot that is not 64 KB aligned
   will not boot, an overlap corrupts whichever neighbour is written second, and
   a renamed data partition makes esp_partition_find_first() return NULL at
   runtime (fw_srv looks up "zone_fw" by name, Task 6 looks up "cp_fw"). Parsing
   the real CSV keeps this honest -- a test over a hard-coded copy of the table
   would pass while the shipped file was wrong. */
#include "unity.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

typedef struct { char name[24], type[8]; unsigned long off, size; } part_t;

static int load(part_t *p, int cap) {
    FILE *f = fopen(HG_PARTITIONS_P4_CSV, "r");
    TEST_ASSERT_NOT_NULL_MESSAGE(f, "cannot open partitions_p4.csv");
    char line[256];
    int n = 0;
    while (fgets(line, sizeof line, f) && n < cap) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
        char nm[24], ty[8], sub[16], o[16], sz[16];
        if (sscanf(line, "%23[^,], %7[^,], %15[^,], %15[^,], %15[^,\n]",
                   nm, ty, sub, o, sz) != 5) continue;
        /* trim trailing spaces the CSV uses for column alignment */
        for (char *e = nm + strlen(nm) - 1; e > nm && *e == ' '; e--) *e = 0;
        for (char *e = ty + strlen(ty) - 1; e > ty && *e == ' '; e--) *e = 0;
        snprintf(p[n].name, sizeof p[n].name, "%s", nm);
        snprintf(p[n].type, sizeof p[n].type, "%s", ty);
        p[n].off  = strtoul(o, NULL, 0);
        p[n].size = strtoul(sz, NULL, 0);
        n++;
    }
    fclose(f);
    return n;
}

static const part_t *find(const part_t *p, int n, const char *name) {
    for (int i = 0; i < n; i++) if (strcmp(p[i].name, name) == 0) return &p[i];
    return NULL;
}

void test_p4_table_has_the_partitions_the_firmware_looks_up_by_name(void) {
    part_t p[16];
    int n = load(p, 16);
    TEST_ASSERT_NOT_NULL_MESSAGE(find(p, n, "factory"), "factory (rescue) missing");
    TEST_ASSERT_NOT_NULL_MESSAGE(find(p, n, "ota_0"),   "ota_0 missing");
    TEST_ASSERT_NOT_NULL_MESSAGE(find(p, n, "ota_1"),   "ota_1 missing");
    TEST_ASSERT_NOT_NULL_MESSAGE(find(p, n, "zone_fw"), "zone_fw missing (fw_srv finds it by name)");
    TEST_ASSERT_NOT_NULL_MESSAGE(find(p, n, "cp_fw"),   "cp_fw missing (C6 image)");
}

void test_p4_app_partitions_are_64k_aligned(void) {
    part_t p[16];
    int n = load(p, 16);
    for (int i = 0; i < n; i++) {
        if (strcmp(p[i].type, "app") != 0) continue;
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, p[i].off % 0x10000, p[i].name);
    }
}

void test_p4_partitions_do_not_overlap_and_clear_the_table(void) {
    part_t p[16];
    int n = load(p, 16);
    for (int i = 0; i < n; i++) {
        /* table at 0xF000 on the P4 and IDF reserves 0x1000 for it, so 0x10000
         * is the first byte a partition may use */
        TEST_ASSERT_TRUE_MESSAGE(p[i].off >= 0x10000, p[i].name);
        for (int j = i + 1; j < n; j++) {
            unsigned long ae = p[i].off + p[i].size, be = p[j].off + p[j].size;
            TEST_ASSERT_TRUE_MESSAGE(p[i].off >= be || p[j].off >= ae, p[i].name);
        }
    }
}

void test_p4_app_slots_are_large_enough_for_the_master_plus_the_panel_ui(void) {
    part_t p[16];
    int n = load(p, 16);
    /* The ESP32 master is already 966 KB and the panel adds LVGL, a large
       subsetted font and an image decoder. 2 MB (the ESP32 slot size) leaves too
       little; require 4 MB so an OTA cannot be blocked by a slot ceiling. */
    TEST_ASSERT_TRUE_MESSAGE(find(p, n, "ota_0")->size >= 0x400000, "ota_0 under 4 MB");
    TEST_ASSERT_TRUE_MESSAGE(find(p, n, "ota_1")->size >= 0x400000, "ota_1 under 4 MB");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(find(p, n, "ota_0")->size,
                                     find(p, n, "ota_1")->size,
                                     "OTA slots must match in size");
}

void test_p4_cp_fw_holds_the_coprocessor_image(void) {
    part_t p[16];
    int n = load(p, 16);
    /* The built CP image is 1 145 984 B and its own OTA layout allows 1.75 MB
       per slot, so give it 1.5 MB of headroom rather than a snug fit. */
    TEST_ASSERT_TRUE_MESSAGE(find(p, n, "cp_fw")->size >= 0x180000, "cp_fw under 1.5 MB");
}
```

- [ ] **Step 2: Register the test and pass it the CSV path**

In `tests/host/CMakeLists.txt`, add `test_partitions_p4.c` alongside the existing test executables, following the pattern already used there, and define the CSV path so the test reads the shipped file rather than a copy:

```cmake
target_compile_definitions(test_partitions_p4 PRIVATE
    HG_PARTITIONS_P4_CSV="${CMAKE_CURRENT_LIST_DIR}/../../master/partitions_p4.csv")
```

- [ ] **Step 3: Run the test to verify it fails**

Run: `idf.py -C master build` (the host gate runs `ctest` as a dependency of the ELF).
Expected: FAIL — `cannot open partitions_p4.csv`, because the file does not exist yet.

- [ ] **Step 4: Write the partition table**

Create `master/partitions_p4.csv`:

```
# Master v2 (ESP32-P4, 32 MB). Bootloader 0x2000, partition table 0xF000 --
# not the ESP32 table's 0x1000/0xE000. 0x2000 is the P4's fixed bootloader
# offset; 0xF000 is ours, NOT IDF's P4 default of 0x8000, because the custom
# bootloader (0x60f0) does not fit in the 0x6000 that 0x8000 would leave it.
#
# App slots are 4 MB, double the ESP32's: the master is already 966 KB and the
# panel UI adds LVGL, a large subsetted clock font and an image decoder. cp_fw
# holds the ESP32-C6 co-processor image so a master OTA does not have to carry
# a megabyte of unchanged radio firmware (the bring-up spike embedded it in the
# app, which worked but bloated every update).
# Name,     Type, SubType, Offset,    Size
nvs,        data, nvs,     0x10000,   0x10000
otadata,    data, ota,     0x20000,   0x2000
phy_init,   data, phy,     0x22000,   0x1000
factory,    app,  factory, 0x30000,   0x200000
ota_0,      app,  ota_0,   0x230000,  0x400000
ota_1,      app,  ota_1,   0x630000,  0x400000
zone_fw,    data, 0x40,    0xA30000,  0x180000
cp_fw,      data, 0x41,    0xBB0000,  0x180000
data,       data, spiffs,  0xD30000,  0x400000
```

- [ ] **Step 5: Run the tests to verify they pass**

Run: `idf.py -C master build`
Expected: `100% tests passed ... out of 33` — the 5 new cases live in ONE new CTest entry, so the count goes 32 → 33, not 32 → 37. Then confirm IDF agrees the table is well-formed:
```
idf.py -C master -B build_p4 -D SDKCONFIG=build_p4/sdkconfig partition-table
```
Expected: the table prints without a gap/overlap warning.

- [ ] **Step 6: Commit**

```bash
git add master/partitions_p4.csv tests/host/test_partitions_p4.c tests/host/CMakeLists.txt
git commit -m "feat(master): P4 partition table with a dedicated cp_fw partition"
```

---

### Task 3: Make the flashing tools target-aware

Both flashing tools hard-code ESP32 offsets. This is not cosmetic: `flash_app.py`
writes the master app at `0x170000`, and on the P4 table from Task 2 that address
falls **inside the `factory` partition** — flashing a master would silently
destroy the rescue image, and the board would appear fine until someone needed
rescue. `flash_all.py` would put the bootloader at `0x1000` where the P4 expects
`0x2000`, producing a board that simply does not boot with no useful diagnostic.

Both tools must therefore derive offsets from the target, and must refuse rather
than guess when they cannot tell.

**Files:**
- Modify: `tools/flash_app.py`
- Modify: `tools/flash_all.py`

**Interfaces:**
- Consumes: Task 2's `master/partitions_p4.csv` offsets.
- Produces: both tools accept `--target esp32|esp32p4` (default `esp32`, so every
  existing invocation in the docs and in muscle memory keeps working unchanged).
  Task 7 uses `--target esp32p4`.

- [ ] **Step 1: Add the per-target offset tables to flash_app.py**

Replace the single `APP_OFFSET` dict (line 35) with a per-target mapping, keeping
the ESP32 numbers exactly as they are:

```python
# Per target, because the P4 partition table is not the ESP32 one. Getting this
# wrong is silent and destructive: the ESP32 master offset (0x170000) lands
# inside the P4 table's factory/rescue partition, so a mis-targeted master flash
# would overwrite the rescue image and nothing would complain until rescue was
# needed. Offsets must match master/partitions.csv (esp32) and
# master/partitions_p4.csv (esp32p4).
APP_OFFSET = {
    "esp32": {
        "zone": 0x170000, "master": 0x170000, "rescue": 0x30000,
        "zonefw": 0x570000,
    },
    "esp32p4": {
        "master": 0x230000, "rescue": 0x30000,
        "zonefw": 0xA30000, "cpfw": 0xBB0000,
    },
}
OTADATA_OFFSET = 0x20000        # same on both tables
```

Note `zone` is absent from the esp32p4 map on purpose — zones are ESP32 boards
and always will be (the no-Wi-Fi-in-zone rule). Asking for `--app zone --target
esp32p4` is a mistake, and the next step makes it an error rather than a guess.

- [ ] **Step 2: Add --target and make an unsupported combination fail loudly**

```python
parser.add_argument("--target", default="esp32", choices=sorted(APP_OFFSET),
                    help="chip the OFFSETS are for; default esp32")
...
offsets = APP_OFFSET[args.target]
if args.app not in offsets:
    parser.error(f"--app {args.app} is not a thing on {args.target} "
                 f"(valid: {', '.join(sorted(offsets))})")
```

Use `offsets[...]` everywhere the old `APP_OFFSET[...]` was used, and
`OTADATA_OFFSET` in place of the literal `0x20000` at line 121.

- [ ] **Step 3: Add --target to flash_all.py**

Replace the two module-level constants (lines 25-26) with a table and select on
the argument, again leaving the ESP32 values untouched:

```python
# P4 moves both: the second-stage bootloader starts at 0x2000 rather than
# 0x1000, and the partition table at 0xF000 rather than 0xE000 (0xF000, not
# IDF's P4 default 0x8000, because the custom bootloader does not fit under it).
FLASH_LAYOUT = {
    "esp32":   {"bootloader": 0x1000, "partition_table": 0xE000},
    "esp32p4": {"bootloader": 0x2000, "partition_table": 0xF000},
}
```

- [ ] **Step 4: Verify the ESP32 path is byte-for-byte unchanged**

Both tools build an esptool argument list. Print it without flashing and compare
against the pre-change output:

```
python tools/flash_app.py --app master --port COM99 --help
```

Then confirm by inspection that with no `--target`, `flash_app.py` still resolves
master to `0x170000`, rescue to `0x30000`, zonefw to `0x570000`, otadata to
`0x20000`, and `flash_all.py` still uses `0x1000`/`0xE000`. If either tool has a
`--dry-run`, use it; if not, add one — a tool that can only be tested by writing
to a real board is a tool nobody will test.

- [ ] **Step 5: Verify the P4 offsets agree with the partition table**

Cross-check each `esp32p4` offset against `master/partitions_p4.csv` by eye and
record the comparison in the task report: `master` → `ota_0` `0x230000`,
`rescue` → `factory` `0x30000`, `zonefw` → `zone_fw` `0xA30000`, `cpfw` →
`cp_fw` `0xBB0000`. A mismatch here is the destructive failure this task exists
to prevent, so state explicitly in the report that you checked all four.

- [ ] **Step 6: Commit**

```bash
git add tools/flash_app.py tools/flash_all.py
git commit -m "fix(tools): derive flash offsets from the target, not ESP32 constants"
```

---

### Task 4: Make the custom bootloader build and work on the P4

Two hard breaks, both invisible to `board.h`:

1. `bootloader_start.c` includes `esp32/rom/gpio.h` — an ESP32-only ROM path.
2. It hard-codes `#define HG_RESCUE_GPIO 15`, and **on the P4 GPIO15 is the C6 SDIO D1 line**. Holding the rescue button would drive a Wi-Fi bus line; conversely the C6 driving D1 looks like a button press.

The rescue GPIO must stop being defined twice. `board.h` already has `HG_GPIO_RESCUE_BTN` per target, but the bootloader subproject does not link the `board` component — so it gets its own target-conditional block with a comment tying the two together.

**Files:**
- Modify: `bootloader_components/main/bootloader_start.c`
- Modify: `bootloader_components/main/CMakeLists.txt` (only if the include path needs it)

**Interfaces:**
- Consumes: Tasks 1 and 2 (a P4 build that reaches the bootloader).
- Produces: a P4 bootloader binary. The rescue contract is unchanged on both targets: RTC flag → factory once; button ≥10 s → factory; 1–9 s → erase `nvs`; <1 s → ignored.

- [ ] **Step 1: Make the ROM include and the rescue pin target-aware**

Replace the ESP32-only include:

```c
#if CONFIG_IDF_TARGET_ESP32P4
#include "esp32p4/rom/gpio.h"
#else
#include "esp32/rom/gpio.h"
#endif
#include "soc/gpio_periph.h"
#include "soc/io_mux_reg.h"
```

and the hard-coded pin:

```c
/* MUST agree with HG_GPIO_RESCUE_BTN in components/board/board.h. It is
 * duplicated rather than included because the bootloader subproject does not
 * link the board component. On the P4, GPIO15 (the ESP32 value) is the C6 SDIO
 * D1 line -- using it here would drive a Wi-Fi bus line from a button and read
 * the C6 as a button press. */
#if CONFIG_IDF_TARGET_ESP32P4
#define HG_RESCUE_GPIO     34
#else
#define HG_RESCUE_GPIO     15
#endif
```

- [ ] **Step 2: Build for the P4 and read the failures**

Run: `idf.py -C master -B build_p4 -D SDKCONFIG=build_p4/sdkconfig build`
Expected: either it completes, or it fails on further ESP32-isms. The file's own header comment lists the ones IDF 6.0.1 needed on ESP32 (`rom_gpio_pad_*` naming, explicit `io_mux_reg.h`, picolibc `__getreent`); resolve each the same way — target-conditional, never by deleting the ESP32 path. Record every change and why in the task report.

- [ ] **Step 3: Verify the ESP32 bootloader is unchanged**

Run: `idf.py -C master build` and `idf.py -C rescue build`
Expected: both complete; host suite 33/33. Compare the reported `Bootloader binary size` against the pre-change value (`0x6c70`, 48% free) — **it must be identical**. A changed ESP32 bootloader size means a `#if` fell the wrong way.

- [ ] **Step 4: Commit**

```bash
git add bootloader_components/main/
git commit -m "fix(bootloader): build for the P4 and move the rescue pin off the C6 SDIO bus"
```

- [ ] **Step 5: Bench-verify the rescue paths on the P4**

There is no host test for a bootloader. Flash the P4 (`python tools/flash_all.py --port COM28 --target esp32p4`, which writes bootloader + table + apps) and verify all four paths on hardware, reading the console for `hg_boot` lines:

| Action | Expected |
|---|---|
| Plain reset | normal boot, `hg_boot` silent about rescue |
| Hold rescue pin low ≥10 s at reset | `rescue flag set -> factory` path, boots the rescue app |
| Hold 1–9 s | `nvs` erased, normal boot continues, config back to defaults |
| Hold <1 s | ignored, normal boot |

Note in the report that holding the pin low may silence the ROM log until `hg_boot` prints (the ESP32 MTDO strap effect) — check whether the P4 behaves the same, as `docs/pin-mapping.md` records that quirk for the ESP32 only.

---

### Task 5: Move the master-config read-modify-write into a component

`master_net_ops_try_lock()` / `master_net_ops_unlock()` and the mcfg snapshot→modify→commit→apply sequence live in `master/main/net_ops_master.c` — an *app* file. `components/http_srv/http_api_cfg.c` reaches them through `extern` declarations, which its own comment calls awkward. Any second caller (the panel UI, next plan) would have to replicate the lock discipline, and a replicated guard is how two faces drift apart.

This is the one refactor in this plan, and unlike the rest it is host-testable.

**Files:**
- Create: `components/mcfg_ops/mcfg_ops.c`, `components/mcfg_ops/mcfg_ops.h`, `components/mcfg_ops/CMakeLists.txt`
- Modify: `master/main/net_ops_master.c` (delegate to the component; keep `master_net_ops()` as the CLI's `net_ops_t` provider)
- Modify: `master/main/net_ops_master.h` (drop the re-exported lock)
- Modify: `components/http_srv/http_api_cfg.c` (call the component; delete the `extern` block at lines 24-25)
- Create: `tests/host/test_mcfg_ops.c`
- Modify: `tests/host/CMakeLists.txt`

**Interfaces:**
- Consumes: `hg_mcfg_t`, `mcfg_get()`, `mcfg_commit()` from `hg_mcfg`.
- Produces, and both the HTTP handlers and the panel UI must use exactly these:
  - `int mcfg_ops_lock(uint32_t ms);` — 0 acquired, -1 not acquired within `ms` (including "mutex not created yet", which is a failure to take, never an open lock).
  - `void mcfg_ops_unlock(void);`
  - `int mcfg_ops_edit(int (*fn)(hg_mcfg_t *m, void *ctx), void *ctx, void (*apply)(void *ctx), const char *what);` — takes the lock, snapshots `mcfg_get()`, calls `fn` on the copy, commits if `fn` returned 0, **then calls `apply` STILL HOLDING THE LOCK** (pass `NULL` when there is nothing to apply), and only then releases. `what` is the log label the commit path prints, e.g. `"SET TZ"`. The apply-under-lock is not a convenience: every pre-existing writer held the lock across its `wifi_mgr_apply()` / `time_svc_apply_mcfg()` / `http_auth_sessions_drop()`, and `GET /api/wifi/scan` takes the same lock precisely so a radio reconfigure cannot land underneath a scan. An `apply` that runs after the release silently removes that exclusion. **This is the entry point that makes the read-modify-write atomic for every caller.** Return contract, which is split three ways on purpose: `fn` returns 0 to commit or a **positive** value to refuse (returned unchanged, nothing written); all negative returns are reserved for the component, namely -1 lock unavailable, **-2 commit failed on storage (NVS or mutex)** and **-3 commit rejected as invalid**. Negative-for-infrastructure / positive-for-refusal keeps the bands from ever colliding. The -2/-3 split exists because `mcfg_commit()` itself returns `-1 invalid / -2 nvs-or-mutex-unavailable` and the CLI and web surfaces map those to *different* owner-visible errors (`ERR INVALID` vs `ERR STORAGE`). Collapsing them would tell an owner who typed a bad POSIX TZ that their storage failed.
  - `void mcfg_ops_init(void);` — creates the mutex; idempotent. **Must be called before any task that can write config is started** — in `master/main/app_main.c` that means before `cmd_task_start()`, not merely after `mcfg_store_init()`, because the CLI and command task come up first and a `SET` from the console in between would find no mutex. `lock_take()` therefore fails CLOSED on a missing mutex: an uninitialised lock refuses the write rather than proceeding unsynchronised.

- [ ] **Step 1: Write the failing test**

Create `tests/host/test_mcfg_ops.c`. The lock is a FreeRTOS mutex, so the host build needs the existing `tests/host/fakes` seam — follow how `test_node_mgr_cfg.c` fakes its RTOS primitives.

```c
/* mcfg_ops exists so the HTTP handlers and the panel UI cannot each grow their
   own version of the master-config read-modify-write. The properties worth
   pinning are the ones a second caller would get wrong: that a failed lock is
   reported as a failure rather than silently proceeding unsynchronised, that a
   rejecting edit function does NOT commit, and that the edit sees a private
   copy so a rejected edit cannot leave the live config half-modified. */
#include "unity.h"
#include "mcfg_ops.h"
#include "hg_mcfg.h"
#include "mcfg_store.h"   /* mcfg_get(); mcfg_ops.h does not pull this in */
#include <stdio.h>        /* snprintf */
#include <string.h>

static int set_hostname(hg_mcfg_t *m, void *ctx) {
    /* hg_mcfg_t is FLAT -- there is no `sys` sub-struct. */
    snprintf(m->hostname, sizeof m->hostname, "%s", (const char *)ctx);
    return 0;
}

static int reject(hg_mcfg_t *m, void *ctx) {
    (void)ctx;
    snprintf(m->hostname, sizeof m->hostname, "scribbled");
    return 3;    /* a refusal from fn: POSITIVE, since negatives are reserved */
}

void test_edit_commits_when_the_edit_function_accepts(void) {
    mcfg_ops_init();
    TEST_ASSERT_EQUAL_INT(0, mcfg_ops_edit(set_hostname, "greenhouse", NULL, "TEST"));
    TEST_ASSERT_EQUAL_STRING("greenhouse", mcfg_get()->hostname);
}

void test_a_rejecting_edit_does_not_commit_and_leaves_no_trace(void) {
    mcfg_ops_init();
    mcfg_ops_edit(set_hostname, "before", NULL, "TEST");
    TEST_ASSERT_EQUAL_INT(3, mcfg_ops_edit(reject, NULL, NULL, "TEST"));
    /* The edit function scribbled on its copy and then refused. The live config
       must still read "before" -- if mcfg_ops handed out a pointer to the live
       buffer instead of a copy, this reads "scribbled". */
    TEST_ASSERT_EQUAL_STRING("before", mcfg_get()->hostname);
}

void test_lock_is_not_recursive_so_a_second_take_fails_fast(void) {
    mcfg_ops_init();
    TEST_ASSERT_EQUAL_INT(0, mcfg_ops_lock(10));
    TEST_ASSERT_EQUAL_INT(-1, mcfg_ops_lock(10));
    mcfg_ops_unlock();
    TEST_ASSERT_EQUAL_INT(0, mcfg_ops_lock(10));
    mcfg_ops_unlock();
}

void test_edit_reports_failure_when_the_lock_is_held(void) {
    mcfg_ops_init();
    TEST_ASSERT_EQUAL_INT(0, mcfg_ops_lock(10));
    /* Must NOT proceed unsynchronised. */
    TEST_ASSERT_EQUAL_INT(-1, mcfg_ops_edit(set_hostname, "racer", NULL, "TEST"));
    mcfg_ops_unlock();
}

void test_init_is_idempotent(void) {
    mcfg_ops_init();
    TEST_ASSERT_EQUAL_INT(0, mcfg_ops_lock(10));
    mcfg_ops_unlock();
    mcfg_ops_init();            /* must not destroy or replace a live mutex */
    TEST_ASSERT_EQUAL_INT(0, mcfg_ops_lock(10));
    mcfg_ops_unlock();
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `idf.py -C master build`
Expected: FAIL — `mcfg_ops.h` not found.

- [ ] **Step 3: Write the component**

`components/mcfg_ops/mcfg_ops.h`:

```c
#pragma once
#include <stdint.h>
#include "hg_mcfg.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The master-config read-modify-write, owned by a component rather than by
 * master/main, so every caller takes the SAME lock. http_srv used to reach the
 * app's copy through extern declarations; the panel UI would have been a second
 * such reach, and a replicated guard is how two callers drift apart.
 *
 * mcfg_commit() serializes commits against each other but NOT the
 * snapshot-modify-commit sequence, which is why this lock exists on top. */
/* Idempotent. Call once at boot BEFORE starting any task that can write config
 * -- in app_main.c that is before cmd_task_start(), since the CLI comes up
 * first. lock_take() fails closed if the mutex is missing, so an uninitialised
 * lock refuses the write rather than running unsynchronised. */
void mcfg_ops_init(void);

/* 0 acquired / -1 not acquired within ms. "Not created yet" counts as NOT
 * acquired -- a caller that somehow runs before boot wiring answers busy
 * rather than proceeding unsynchronised. NOT recursive: a caller that invokes
 * mcfg_ops_edit() must not already hold the lock. */
int  mcfg_ops_lock(uint32_t ms);
void mcfg_ops_unlock(void);

/* Atomic snapshot -> fn(copy) -> commit. fn receives a PRIVATE copy, so a
 * refusal cannot leave the live config half-modified.
 *
 * fn returns 0 to commit, or a POSITIVE value to refuse -- returned unchanged,
 * nothing written. Every NEGATIVE return belongs to this component, so fn must
 * never return one:
 *   -1  the lock could not be taken
 *   -2  mcfg_commit() failed on storage (NVS or mutex)
 *   -3  mcfg_commit() rejected the config as invalid
 * The -2/-3 split is not decoration: mcfg_commit() distinguishes those two, and
 * the CLI and web surfaces map them to different owner-visible errors
 * (ERR STORAGE vs ERR INVALID).
 *
 * apply runs after a successful commit and STILL HOLDS THE LOCK; pass NULL when
 * there is nothing to apply. That scope is deliberate and predates this
 * component: every writer held the lock across its wifi_mgr_apply() /
 * time_svc_apply_mcfg() / http_auth_sessions_drop(), and GET /api/wifi/scan
 * takes this same lock so a radio reconfigure cannot land underneath a scan.
 * An apply that ran after the release would quietly delete that exclusion.
 * apply must therefore never call back into mcfg_ops_lock()/mcfg_ops_edit() --
 * the lock is not recursive.
 *
 * what is the label the commit path logs, e.g. "SET TZ". */
int  mcfg_ops_edit(int (*fn)(hg_mcfg_t *m, void *ctx), void *ctx,
                   void (*apply)(void *ctx), const char *what);

#ifdef __cplusplus
}
#endif
```

`components/mcfg_ops/mcfg_ops.c`: move the mutex and the sequence out of `net_ops_master.c` verbatim — same 6000 ms internal timeout (longer than `mcfg_commit()`'s own 5000 ms mutex timeout, so a caller that waits gets a real answer rather than a timeout race), same `mcfg_get()` copy semantics. `components/mcfg_ops/CMakeLists.txt`:

```cmake
idf_component_register(SRCS "mcfg_ops.c" INCLUDE_DIRS "." REQUIRES hg_mcfg freertos)
```

- [ ] **Step 4: Rewire both callers**

In `master/main/net_ops_master.c`, delete the local mutex and have `net_set_sta`/`net_set_ap`/`net_set_tz`/`master_web_set_password` call `mcfg_ops_edit()`. Keep `master_net_ops()` — the CLI rows still need the `net_ops_t`. Delete `master_net_ops_try_lock`/`_unlock` from `net_ops_master.h`.

In `components/http_srv/http_api_cfg.c`, delete the `extern int master_net_ops_try_lock(...)` / `extern void master_net_ops_unlock(void)` block at lines 24-25, add `#include "mcfg_ops.h"`, add `mcfg_ops` to `http_srv`'s `REQUIRES`, and replace every `master_net_ops_try_lock(100)` / `master_net_ops_unlock()` with `mcfg_ops_lock(100)` / `mcfg_ops_unlock()`. `GET /api/wifi/scan` keeps holding the lock across the ~2 s blocking radio scan — that behaviour must not change.

Call `mcfg_ops_init()` in `master/main/app_main.c` immediately after `mcfg_store_init()`.

- [ ] **Step 5: Run the tests to verify they pass**

Run: `idf.py -C master build`
Expected: `100% tests passed ... out of 34` (33 + one new test file). Then `idf.py -C zone build` and `idf.py -C rescue build` clean — `http_srv` is master-only, but a new component in `components/` is discovered by every app, so confirm none of them grew a dependency.

- [ ] **Step 6: Bench-verify no behaviour changed**

The refactor touches the live web UI's write paths. Re-run the existing automated suite against the **ESP32** master (this task changes no P4 behaviour), which covers exactly these routes:
```
python tools/flash_app.py --app master --port <master COM>
python tools/web_test.py 192.168.7.7 --password hillgrow1
```
Expected: **web_test 37/37 cases** (a different 37 from the host suite's — do not conflate them), including the Wi-Fi set, TZ set and password-change cases. Report the count.

- [ ] **Step 7: Commit**

```bash
git add components/mcfg_ops tests/host/test_mcfg_ops.c tests/host/CMakeLists.txt \
        master/main/net_ops_master.c master/main/net_ops_master.h \
        master/main/app_main.c components/http_srv/
git commit -m "refactor(mcfg_ops): own the master-config read-modify-write in a component"
```

---

### Task 6: Serve the co-processor image from its own partition

The spike embedded the 1.09 MB C6 image in the app. Task 2 gave it `cp_fw`. Move it, and make the update version-gated so it does not re-flash the radio on every boot.

**Files:**
- Create: `components/cp_ota/cp_ota.c`, `components/cp_ota/cp_ota.h`, `components/cp_ota/CMakeLists.txt`
- Modify: `master/main/app_main.c`
- Modify: `tools/flash_app.py` (add `--app cpfw`)
- Create: `tests/host/test_cp_ota.c`
- Modify: `tests/host/CMakeLists.txt`

**Interfaces:**
- Consumes: `cp_fw` partition (Task 2); `eh_host_cp_ota_begin/write/end/activate` and `eh_host_mcu_transport_get_fw_version` / `_verify_fw_compat` from `esp_hosted`.
- Produces: `int cp_ota_sync(void);` — 0 = already matching, nothing done; 1 = image pushed and activated (the C6 reboots itself); -1 = no image staged in `cp_fw`; -2 = the push failed (the C6 keeps running its old firmware).
- Produces: `int cp_ota_needed(uint32_t cp_ver);` — **pure**, host-tested: 1 when `cp_ver` differs from the host in the major or minor byte, else 0. A patch-only difference returns 0, because esp_hosted's own `verify_fw_compat()` treats it as compatible and re-flashing the radio for it would be churn.

- [ ] **Step 1: Write the failing test**

Only the decision is pure; the RPC push is not. Test the decision — it is where the spike's bug lived (an ungated OTA re-flashed the radio every boot). Create `tests/host/test_cp_ota.c`:

```c
/* The gate, not the transfer. The bring-up spike shipped this ungated and
   re-flashed the C6 on every single boot: pointless flash wear on the radio
   plus ~7 s added to every start-up. It also must not key on a value that is
   structurally constant -- the SP4 final review found exactly that class of bug
   in the HW-plane presence check, where a "generation" was always 0 and the
   guard therefore always took one branch. */
#include "unity.h"
#include "cp_ota.h"

void test_matching_version_needs_no_update(void) {
    TEST_ASSERT_EQUAL_INT(0, cp_ota_needed(CP_OTA_HOST_VERSION));
}

void test_a_zero_version_needs_an_update(void) {
    /* The factory Waveshare co-processor reports 0.0.0. */
    TEST_ASSERT_EQUAL_INT(1, cp_ota_needed(0x00000000u));
}

void test_a_differing_minor_needs_an_update(void) {
    /* 3.1.7 vs 3.0.7. The version word is EH_VERSION_VAL(major, minor, patch)
       == (major << 16) | (minor << 8) | patch, so the MINOR byte is bits 8-15 --
       0x00030006 would be 3.0.6, a patch difference, not a minor one. */
    TEST_ASSERT_EQUAL_INT(1, cp_ota_needed(0x00030107u));
}

void test_a_differing_major_needs_an_update(void) {
    TEST_ASSERT_EQUAL_INT(1, cp_ota_needed(0x00020007u));   /* 2.0.7 vs 3.0.7 */
}

void test_a_differing_patch_alone_does_not(void) {
    /* Read from esp_hosted's own implementation, not inferred: after an exact
       compare, eh_host_mcu_transport_verify_fw_compat() returns +-1 when the
       major or the minor byte differs, and for a patch-only difference it logs
       "patch version differs (compatible)" and returns 0. cp_ota_needed() must
       agree with it, or the host would re-flash the radio for a patch bump that
       esp_hosted itself considers compatible. */
    TEST_ASSERT_EQUAL_INT(0, cp_ota_needed(0x00030008u));
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `idf.py -C master build`
Expected: FAIL — `cp_ota.h` not found.

- [ ] **Step 3: Write the component**

`cp_ota_needed()` compares major.minor against `CP_OTA_HOST_VERSION` (a header constant, `0x00030007` for esp_hosted 3.0.7 — the word is `(major << 16) | (minor << 8) | patch`, matching esp_hosted's `EH_VERSION_VAL`). It must stay a *pure* function so the host suite can test it, which is why it re-implements the comparison instead of calling `eh_host_mcu_transport_verify_fw_compat()` — that lives in a component the host build does not have. Because it is a copy, it must agree with the original: exact match 0, major or minor differing 1, **patch-only differing 0**. `cp_ota_sync()`:

1. `eh_host_mcu_transport_get_fw_version()`, then `cp_ota_needed()`; return 0 if not needed.
2. `esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x41, "cp_fw")`; return -1 if absent or its first bytes are erased (`0xFF`), i.e. nothing staged.
3. `eh_host_cp_ota_begin()`, then `esp_partition_read` + `eh_host_cp_ota_write` in **1536-byte chunks** (`EH_RPC_OTA_CHUNK_MAX`), then `eh_host_cp_ota_end()`.
4. `eh_host_cp_ota_activate()` — **treat ESP_FAIL as expected, not as an error.** The C6 sets its boot partition then reboots on a 2000 ms timer, inside the host's 5000 ms RPC wait, so the reply never arrives and the host logs "no response". Log it at WARN and return 1. Verify by re-reading the version after the link re-handshakes; never trust this return code.
5. Feed the task watchdog on progress, gated on `esp_task_wdt_status()` — the push takes ~7.3 s and the TWDT is 8 s.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `idf.py -C master build`
Expected: `100% tests passed ... out of 35` (34 + one new test file).

- [ ] **Step 5: Wire it in and add the flashing path**

Call `cp_ota_sync()` in `app_main.c` **after** `wifi_mgr_start()` — a working AP proves the RPC path the OTA needs, so do it last, not first. Log the outcome at WARN.

Add `--app cpfw` to `tools/flash_app.py`, mirroring the existing `zonefw` case (which writes `zone/build/hillgrow_zone.bin` into the master's `zone_fw` at `0x570000`): write the CP image into `cp_fw` at `0xBB0000`. Document in its `--help` that the source is the `eh_cp` project's `eh_cp_wifi_softap.bin`, and that building the CP needs `CONFIG_EH_TRANSPORT_CP_SDIO_MODE_STREAM=y` because esp_hosted's default SW_AGGR mode requires an ESP-IDF patch that 6.0.1 lacks — **do not run `eh.py patch-idf` on the shared IDF install.**

- [ ] **Step 6: Commit**

```bash
git add components/cp_ota tests/host/test_cp_ota.c tests/host/CMakeLists.txt \
        master/main/app_main.c tools/flash_app.py
git commit -m "feat(cp_ota): update the C6 from the cp_fw partition, gated on version"
```

---

### Task 7: First full P4 master bring-up on hardware

Everything above is build-time. This is the first time the real master runs on the P4.

**Files:** none — this is a bench task. Its deliverable is a report.

**Interfaces:** consumes Tasks 1-6.

- [ ] **Step 1: Flash the P4**

```
& C:\esp\v6.0.1\esp-idf\export.ps1
idf.py -C master -B build_p4 -D SDKCONFIG=build_p4/sdkconfig build
python tools/flash_all.py --port COM28 --target esp32p4
python tools/flash_app.py --app cpfw --port COM28 --target esp32p4
```
Record the image size and the free percentage of `ota_0`.

- [ ] **Step 2: Verify the boot and the co-processor**

Read the console. Expected: `hg_boot` selects an app; the SDIO transport reports `CLK=18 CMD=19 D0=14 D1=15 D2=16 D3=17 RESET=54`; then **`esp-hosted fw versions: host=3.0.7 coprocessor=3.0.7 (match)`**. If it reports `coprocessor=0.0.0`, `cp_ota_sync()` should push the image once and the C6 should match after the next reset.

- [ ] **Step 3: Verify the CLI over USB-UART**

On COM28: `GET ID` → `OK ID MASTER <mac> 0 master`. Then `GET STATUS` and record **heap-min, which must be ≥ 64 KB**. The P4 has far more RAM than the ESP32, so expect a large margin; report the number either way.

- [ ] **Step 4: Run the existing web suite against the P4**

This is the point of the whole migration — the SP4 web UI, unchanged, on the new platform. Join the PC to the master AP and run the suite that already exists:
```
python tools/uart_test.py --http 192.168.7.7 --password hillgrow1
python tools/web_test.py 192.168.7.7 --password hillgrow1
```
Expected: **uart_test 39/39** and **web_test 37/37**, the same counts SP4 signed off with (again, web_test's 37 is unrelated to the host suite's). Any difference is a migration defect, not a test to adjust.

- [ ] **Step 5: Report**

No commit (no files changed). Report: image size, heap-min, both suite counts, the co-processor version line, and anything that differed from the ESP32 master's behaviour.

---

### Task 8: Ring and fleet OTA on the P4 with three zones

The spike proved enrolment and blame with a cut-down master. This repeats it with the real app, which also owns `fw_srv` and the fleet sequencer.

**Files:** none — bench task.

**Interfaces:** consumes Task 7.

- [ ] **Step 1: Stage the zone firmware and wire the ring**

```
idf.py -C zone build
python tools/flash_app.py --app zonefw --port COM28 --target esp32p4
```
Ring: `P4 IO28 (TX) → first zone RX → ... → last zone TX → P4 IO29 (RX)`, grounds common.

**Read the topology off the running master, never off notes.** `GET NODES` reports `hops`, which is `RING_TTL_INIT - ttl` = the number of forwards a heartbeat took: the **highest** hops is nearest the master's TX, and **0** feeds the master's RX. Two wiring instructions were given wrongly during the spike by trusting a stale note about which zone was the "first hop".

- [ ] **Step 2: Verify enrolment and that config survived**

Expected: `GET NODES` shows all three ONLINE, and **each zone keeps its existing config generation** — the P4 has an empty node table, and §4.4 reconciliation must PULL each zone's config rather than push defaults over it. A zone whose generation resets to a low number is a serious defect; stop and report it.

- [ ] **Step 3: Verify blame on a held zone**

Hold the middle zone in reset ~15 s. Expected: the zones upstream of it go DEGRADED (5 s) then OFFLINE (10 s) while the downstream one stays ONLINE; ring goes OPEN with a generic verdict, then refines to **`Z<n> dead or wire Z<n>->Z<m>`** naming the dead node and the wire to its **downstream** neighbour. A verdict naming the upstream neighbour or `->M` for a middle node means the master has the direction wrong.

- [ ] **Step 4: Verify a fleet OTA end to end**

Trigger a zone update from the web UI. Expected, on the master console: `FW 0 ZONE <n> UPDATING` → the zone joins the AP in rescue (`WIFI 0 AP CLIENTS` rises then falls) → `BOOT <n> ... SW` → `FW 0 ZONE <n> DONE` → `NODE <n> ONLINE` → **`FW <n> TRIAL PASS`**, and the zone's config surviving the reflash.

**Watch the event stream, not a level.** Polling `/api/state` every 10 s never once caught `fleet != IDLE` during the SP4 bench — the whole sequence finished in ~35 s.

- [ ] **Step 5: Report**

Report the `GET NODES` table with hops, the blame verdict verbatim, the fleet `NOTIFY` sequence, and heap-min after the fleet update.

---

### Task 9: Promote the spike's findings into the repo and close it

The bring-up spike lives outside the repo in `C:\Projects\hillgrow-p4-spike\` and is throwaway. Everything durable must be in the repo before it is deleted.

**Files:**
- Modify: `docs/superpowers/specs/2026-08-31-hillgrow-system-design.md` (§11.10)
- Modify: `docs/pin-mapping.md`
- Modify: `docs/what_we_learned.md`
- Modify: `docs/hillgrow-features.drawio`

**Interfaces:** consumes Tasks 1-8 (their results are what gets recorded).

- [ ] **Step 1: Amend spec §11.10**

Record that **migration gate test #1 PASSED**, with the measured evidence (softAP + DHCP + a 246 KB HTTP transfer at 1571 kB/s; a real zone fleet pull). Add the constraint that the board carries **P4 rev v1.3** and that IDF treats rev <3.0 and ≥3.0 as mutually exclusive, so **spare boards must be the same revision** — a rev 3.x board is a separate build, not a config tweak.

- [ ] **Step 2: Tick the bring-up checklist in pin-mapping.md**

The Master v2 table is already current (commit `3567830`). Tick the microSD collision item — settled, SDMMC slot 0 on 39-44, no clash with the IO46-52 header — and add the rescue-button note that the bootloader defines its pin separately from `board.h`.

- [ ] **Step 3: Add the lessons**

Append to `docs/what_we_learned.md`. At minimum, the ones that cost real time:

- **A vendor SDK's default mode may not build against your IDF.** esp_hosted 3.0.7's CP defaults to SW_AGGR, which needs an IDF commit 6.0.1 lacks, and the build offers `eh.py patch-idf`. Patching a shared toolchain that is outside version control and that every other project builds against is a bad trade for throughput nobody had measured. `STREAM` mode needed no patch.
- **An API that reports failure on success.** `eh_host_cp_ota_activate()` returns ESP_FAIL because the co-processor reboots before it can reply. Verify the outcome, not the return code.
- **Trust the running system over your notes.** Two wiring instructions were wrong because they reasoned from a stale ledger note instead of the `hops` the master was reporting live.
- **A pin map that is not target-aware is a trap.** The ring's ESP32 pins 18/19 are the C6 SDIO CLK and CMD on the P4; the rescue button's 15 is SDIO D1. Both would have failed as intermittent, mutually-confusing Wi-Fi and ring faults.

- [ ] **Step 4: Stamp the drawio**

Add `bench-verified <date>` to the "Master v2 (P4) Topology" page in the style the other pages already use (see the SP3 stamp: `— SP3 (bench-verified 2026-09-04, 3-board ring)`).

- [ ] **Step 5: Commit the docs**

```bash
git add docs/
git commit -m "docs: Master v2 migration verified; promote the bring-up findings"
```

- [ ] **Step 6: Delete the spike**

Only after the docs commit is pushed. Confirm nothing in the repo references the spike path, then remove `C:\Projects\hillgrow-p4-spike\`. Report what was deleted so it is recoverable from this plan if ever needed.

---

## Out of scope

- **The panel UI.** Its own plan, from `docs/superpowers/specs/2026-09-18-master-v2-panel-ui-design.md`, once this plan lands.
- **The streamer** (I²S on IO46-50) and the SP5 global-equipment hardware.
- **SP4b** history and plots — still gated on SP2 telemetry.
- **Retiring the DevKitC master.** It stays flashed and available as a fallback until Task 7 passes. Converting it to a third zone is part of Task 7's bench setup, not a separate deliverable.
