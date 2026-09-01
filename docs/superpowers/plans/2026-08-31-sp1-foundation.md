# HillGrow SP1 — Foundation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Repo skeleton with three buildable ESP-IDF 6.0.1 apps (master/zone/rescue), custom bootloader with rescue/NVS-erase triggers, MSVC host-test gate, the table-driven CLI core on UART0, the versioned config model persisted in NVS, and a working rescue image — ending with both board types answering `HELP` on their consoles and a bench-verified rescue upload.

**Architecture:** Pure-C components (no IDF headers) carry all logic and are Unity-tested on the host as a pre-build gate of every app's `.elf`; thin target components (UART/NVS/Wi-Fi glue) wire them to hardware. One `cmd_task` per node is the sole dispatcher and config writer; a `store` task is the sole flash writer. The rescue image lives in the `factory` partition and is entered via an RTC flag or a GPIO15 hold decided by a custom second-stage bootloader.

**Tech Stack:** ESP-IDF v6.0.1 (`C:\esp\v6.0.1\esp-idf`, activate with its `export.ps1` before any `idf.py`), ESP32 classic (DevKitC-V4), FreeRTOS SMP, C11, Unity 2.6.0 via CMake FetchContent (MSVC VS2019 BuildTools on this machine, multi-config: always `--config Release` / `-C Release`), Python 3 (`C:\Python311\python`) + pyserial for tools.

**Spec:** `docs/superpowers/specs/2026-08-31-hillgrow-system-design.md` (approved 2026-08-31) — section references (§) below point there. Requirements: `docs/superpowers/specs/2026-08-31-hillgrow-requirements.md`.

## Global Constraints

- ESP-IDF v6.0.1 only; builds run in a shell where `C:\esp\v6.0.1\esp-idf\export.ps1` was sourced. The agent MAY run `idf.py build/flash/monitor` (owner-approved); flash/monitor steps need a board and its COM port.
- Partition table offset `0xE000`; zone table tiles 4 MB exactly (`factory 1280K @0x30000, ota_0 1280K @0x170000, ota_1 1344K @0x2B0000`), master 8 MB per spec §1.4.
- `version.txt` at repo root = strict `MAJOR.MINOR.PATCH`, the single `PROJECT_VER` for all three apps; CMake fails the build on any other format (spec §6.3).
- Pure components: **no IDF includes**, no `#ifdef HOST_TEST` in pure files — hardware reaches them only through injected function pointers. Every pure component has a `tests/host/test_<name>.c`.
- Wire/persisted data packed little-endian by explicit byte offset; struct sizes pinned with `_Static_assert`; no pack pragmas (MSVC host tests must agree with Xtensa) (spec §4.1).
- Files ≤ ~300 lines; one `.h`/`.c` pair per module; `static const char *TAG` per target file; no `ESP_ERROR_CHECK` on runtime driver calls after early boot (spec §3.3/§9).
- CLI grammar per spec §5: one `OK …` or `ERR <TOKEN>` first line, `+ ` help lines, two-space continuations, trailing `HELP`, `NOTIFY <TYPE> <node> <payload>`; integer-only wire; ERR vocabulary of §5.1 only.
- NVS namespace `hg`; every blob behind the 16-byte `hg_blob` envelope (spec §4.2/§4.3).
- Host tests gate each app's `.elf` (skippable with `-DHILLGROW_SKIP_HOST_TESTS=ON`); host-test commands: `cmake -S tests/host -B build/host -DCMAKE_BUILD_TYPE=Release`, `cmake --build build/host --config Release --parallel`, `ctest --test-dir build/host -C Release --output-on-failure`.
- Commit at the end of every task: conventional `type(scope): summary` + trailer `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>`; push to `main` after each task.
- SP1 apps call `esp_ota_mark_app_valid_cancel_rollback()` right after successful init (placeholder for SP2's trial criteria) — without it, any OTA-written image rolls back on its second boot.

## File Map

| File | Action — responsibility |
|---|---|
| `tests/host/CMakeLists.txt` | Create — Unity 2.6.0 FetchContent, one executable per test file, MSVC-aware |
| `tests/host/fakes/fake_clock.{h,c}` | Create — settable `now_ms` for pure code |
| `tests/host/fakes/fake_app_if.{h,c}` | Create — recording/injectable `app_if_t` for cmd_common tests |
| `tests/host/test_*.c` | Create — one per pure module (listed per task) |
| `components/app_version/{app_version.h,app_version.c,CMakeLists.txt}` | Create — parse/compare MAJOR.MINOR.PATCH (pure) |
| `components/hg_blob/{hg_blob.h,hg_blob.c,CMakeLists.txt}` | Create — CRC-32 + 16-B envelope wrap/unwrap (pure) |
| `components/hg_cfg/{hg_cfg_types.h,hg_cfg.h,hg_cfg_defaults.c,hg_cfg_fields.c,hg_cfg_validate.c,CMakeLists.txt}` | Create — config structs, defaults, field table, validation (pure) |
| `components/hg_model/{hg_model.h,hg_model.c,CMakeLists.txt}` | Create — zone model: snapshots, validated atomic edits, dirty bits (mutex behind HOST_TEST) |
| `components/hg_store/{hg_store.h,hg_store.c,CMakeLists.txt}` | Create — NVS init/load, store task (sole flash writer), flush (target) |
| `components/cli_line/{cli_line.h,cli_line.c,CMakeLists.txt}` | Create — byte-fed line editor with history (pure) |
| `components/notify/{notify.h,notify.c,CMakeLists.txt}` | Create — NOTIFY formatter, rate limits, sinks (pure) |
| `components/cmd_core/{cmd_core.h,cmd_tok.c,cmd_resp.c,cmd_dispatch.c,cmd_help.c,CMakeLists.txt}` | Create — tokenizer, table match/parse/gates, HELP, table self-check (pure) |
| `components/cmd_common/{app_if.h,cmd_common.h,cmd_common.c,CMakeLists.txt}` | Create — shared rows ID/VERSION/STATUS/ECHO/LOG/NOTIFY/DEBUG/TIME/REBOOT/SAVE/FW via `app_if_t` (pure w/ fakes) |
| `components/zone_cmds/{zone_cmds.h,zone_cmds.c,CMakeLists.txt}` | Create — config field-grammar rows + `SET CAL` + `GET CONFIG` dump over hg_model (pure) |
| `components/rescue_handover/{rescue_handover.h,rescue_handover.c,rescue_handover_nvs.c,CMakeLists.txt}` | Create — 176-B handover codec (pure) + NVS read/erase glue |
| `components/board/{board.h,Kconfig,CMakeLists.txt}` | Create — pins, role, baud constants |
| `components/cli/{cli.h,cli.c,CMakeLists.txt}` | Create — UART0 driver, cli0 task, TX mutex, esp_log vprintf hook (target) |
| `components/cmd_task/{cmd_task.h,cmd_task.c,CMakeLists.txt}` | Create — request pool, queue, sole dispatcher task (target) |
| `cmake/hillgrow.cmake` | Create — version check + host-test gate function shared by the three apps |
| `zone/{CMakeLists.txt,sdkconfig.defaults,partitions.csv,main/…}` | Create — zone app skeleton (boot §3.3 steps 0–3/7/9, CLI up, model+store) |
| `master/{CMakeLists.txt,sdkconfig.defaults,partitions.csv,main/…}` | Create — master app skeleton (CLI up; Wi-Fi/httpd are SP4) |
| `rescue/{CMakeLists.txt,sdkconfig.defaults,partitions.csv,main/…}` | Create — rescue image: handover pull / manual AP + upload page, ring byte repeater |
| `bootloader_components/main/{CMakeLists.txt,bootloader_start.c}` | Create — RTC-flag + GPIO15 boot selection, NVS erase |
| `tools/flash_all.py`, `tools/flash_app.py` | Create — esptool wrappers: full first flash / app-to-ota_0 flash (factory slot holds rescue!) |
| `tools/uart_test.py` | Create — pyserial smoke harness (machine mode, HELP self-check, round-trips) |
| `docs/pin-mapping.md` | Modify — tick bring-up checkboxes as hardware is verified |

Interface note used throughout: **`hg_rc` convention** — pure functions return `0` OK, negative error; CLI handlers return `0`/`-1` and always write exactly one reply into `resp`.

---

### Task 1: Host-test harness + `app_version`

**Files:**
- Create: `tests/host/CMakeLists.txt`, `tests/host/fakes/fake_clock.h`, `tests/host/fakes/fake_clock.c`
- Create: `components/app_version/app_version.h`, `components/app_version/app_version.c`, `components/app_version/CMakeLists.txt`
- Test: `tests/host/test_app_version.c`

**Interfaces:**
- Consumes: nothing (first task).
- Produces: the host harness every later task appends to (`hg_test(<name> <sources…>)` in `tests/host/CMakeLists.txt`); `int app_version_parse(const char *s, app_version_t *out)` (0 ok / −1 bad; strict `M.m.p`, each 0–255, whole string consumed), `int app_version_cmp(const app_version_t *a, const app_version_t *b)` (−1/0/+1); `fake_clock_now/set/add` used by all pure-code tests.

- [ ] **Step 1: Write harness, fakes, header, failing stub and test**

Create `tests/host/CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.16)
project(hillgrow_host_tests C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
include(FetchContent)
FetchContent_Declare(unity
  URL https://github.com/ThrowTheSwitch/Unity/archive/refs/tags/v2.6.0.tar.gz)
FetchContent_MakeAvailable(unity)
if(MSVC)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
endif()
set(COMP ${CMAKE_CURRENT_SOURCE_DIR}/../../components)
include_directories(
  ${COMP}/app_version ${COMP}/hg_blob ${COMP}/hg_cfg ${COMP}/hg_model
  ${COMP}/cli_line ${COMP}/notify ${COMP}/cmd_core ${COMP}/cmd_common
  ${COMP}/zone_cmds ${COMP}/rescue_handover ${CMAKE_CURRENT_SOURCE_DIR}/fakes)
enable_testing()
function(hg_test NAME)
  add_executable(${NAME} ${NAME}.c fakes/fake_clock.c ${ARGN})
  target_link_libraries(${NAME} unity)
  target_compile_definitions(${NAME} PRIVATE HOST_TEST=1)
  add_test(NAME ${NAME} COMMAND ${NAME})
endfunction()

hg_test(test_app_version ${COMP}/app_version/app_version.c)
```

Create `tests/host/fakes/fake_clock.h`:

```c
#pragma once
#include <stdint.h>
void     fake_clock_set(uint32_t ms);
void     fake_clock_add(uint32_t ms);
uint32_t fake_clock_now(void);
```

Create `tests/host/fakes/fake_clock.c`:

```c
#include "fake_clock.h"
static uint32_t s_now;
void fake_clock_set(uint32_t ms) { s_now = ms; }
void fake_clock_add(uint32_t ms) { s_now += ms; }
uint32_t fake_clock_now(void) { return s_now; }
```

Create `components/app_version/app_version.h`:

```c
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct { uint8_t major, minor, patch; } app_version_t;

/* Strict "M.m.p", each 0..255, no leading/trailing junk. 0 ok, -1 bad. */
int app_version_parse(const char *s, app_version_t *out);
/* -1 a<b, 0 equal, +1 a>b */
int app_version_cmp(const app_version_t *a, const app_version_t *b);

#ifdef __cplusplus
}
#endif
```

Create `components/app_version/app_version.c` (failing stub for the TDD cycle):

```c
#include "app_version.h"
int app_version_parse(const char *s, app_version_t *out) { (void)s; (void)out; return -1; }
int app_version_cmp(const app_version_t *a, const app_version_t *b) { (void)a; (void)b; return 0; }
```

Create `components/app_version/CMakeLists.txt`:

```cmake
idf_component_register(SRCS "app_version.c" INCLUDE_DIRS ".")
```

Create `tests/host/test_app_version.c`:

```c
#include "unity.h"
#include "app_version.h"

void setUp(void) {}
void tearDown(void) {}

static void test_parse_valid(void) {
    app_version_t v;
    TEST_ASSERT_EQUAL_INT(0, app_version_parse("0.1.0", &v));
    TEST_ASSERT_EQUAL_UINT8(0, v.major);
    TEST_ASSERT_EQUAL_UINT8(1, v.minor);
    TEST_ASSERT_EQUAL_UINT8(0, v.patch);
    TEST_ASSERT_EQUAL_INT(0, app_version_parse("255.255.255", &v));
    TEST_ASSERT_EQUAL_UINT8(255, v.patch);
}

static void test_parse_invalid(void) {
    app_version_t v;
    TEST_ASSERT_EQUAL_INT(-1, app_version_parse("", &v));
    TEST_ASSERT_EQUAL_INT(-1, app_version_parse("1.2", &v));
    TEST_ASSERT_EQUAL_INT(-1, app_version_parse("1.2.3.4", &v));
    TEST_ASSERT_EQUAL_INT(-1, app_version_parse("1.2.x", &v));
    TEST_ASSERT_EQUAL_INT(-1, app_version_parse("256.0.0", &v));
    TEST_ASSERT_EQUAL_INT(-1, app_version_parse("1.2.3-rc1", &v));
    TEST_ASSERT_EQUAL_INT(-1, app_version_parse(" 1.2.3", &v));
}

static void test_cmp(void) {
    app_version_t a, b;
    app_version_parse("1.2.3", &a);
    app_version_parse("1.2.4", &b);
    TEST_ASSERT_EQUAL_INT(-1, app_version_cmp(&a, &b));
    TEST_ASSERT_EQUAL_INT(1, app_version_cmp(&b, &a));
    TEST_ASSERT_EQUAL_INT(0, app_version_cmp(&a, &a));
    app_version_parse("2.0.0", &b);
    TEST_ASSERT_EQUAL_INT(-1, app_version_cmp(&a, &b));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_parse_valid);
    RUN_TEST(test_parse_invalid);
    RUN_TEST(test_cmp);
    return UNITY_END();
}
```

- [ ] **Step 2: Run — expect FAIL**

Run: `cmake -S tests/host -B build/host -DCMAKE_BUILD_TYPE=Release && cmake --build build/host --config Release --parallel && ctest --test-dir build/host -C Release --output-on-failure`
Expected: `test_app_version` FAILS (stub returns −1/0). First configure downloads Unity (needs network once).

- [ ] **Step 3: Implement**

Replace `components/app_version/app_version.c`:

```c
#include "app_version.h"

static int parse_part(const char **p, uint8_t *out) {
    const char *s = *p;
    if (*s < '0' || *s > '9') return -1;
    unsigned v = 0;
    int n = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10u + (unsigned)(*s - '0');
        if (v > 255u || ++n > 3) return -1;
        s++;
    }
    *out = (uint8_t)v;
    *p = s;
    return 0;
}

int app_version_parse(const char *s, app_version_t *out) {
    if (!s || !out) return -1;
    app_version_t v;
    if (parse_part(&s, &v.major) != 0 || *s++ != '.') return -1;
    if (parse_part(&s, &v.minor) != 0 || *s++ != '.') return -1;
    if (parse_part(&s, &v.patch) != 0 || *s != '\0') return -1;
    *out = v;
    return 0;
}

int app_version_cmp(const app_version_t *a, const app_version_t *b) {
    if (a->major != b->major) return a->major < b->major ? -1 : 1;
    if (a->minor != b->minor) return a->minor < b->minor ? -1 : 1;
    if (a->patch != b->patch) return a->patch < b->patch ? -1 : 1;
    return 0;
}
```

- [ ] **Step 4: Run — expect PASS**

Run: `cmake --build build/host --config Release --parallel && ctest --test-dir build/host -C Release --output-on-failure`
Expected: `100% tests passed, 0 tests failed out of 1`

- [ ] **Step 5: Commit**

```bash
git add tests/host components/app_version
git commit -m "feat(host-tests): Unity harness + app_version parse/compare

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
git push
```

---

### Task 2: `hg_blob` — CRC-32 and the 16-byte envelope

**Files:**
- Create: `components/hg_blob/hg_blob.h`, `components/hg_blob/hg_blob.c`, `components/hg_blob/CMakeLists.txt`
- Modify: `tests/host/CMakeLists.txt` (append one `hg_test` line)
- Test: `tests/host/test_hg_blob.c`

**Interfaces:**
- Consumes: nothing.
- Produces (used by hg_store, hg_model, ring config transfer in SP3):

```c
uint32_t hg_crc32(uint32_t seed, const void *buf, size_t n);   /* CRC-32/ISO-HDLC; seed 0 for one-shot; feed previous result to continue */
typedef enum { HG_BLOB_OK = 0, HG_BLOB_MIGRATED,
               HG_BLOB_E_SHORT, HG_BLOB_E_MAGIC, HG_BLOB_E_LENGTH,
               HG_BLOB_E_CRC, HG_BLOB_E_VERSION_NEWER, HG_BLOB_E_VERSION_OLD } hg_blob_rc_t;
#define HG_BLOB_HDR_LEN 16u
size_t hg_blob_wrap(uint32_t magic, uint16_t version, uint32_t generation,
                    const void *payload, uint16_t len, uint8_t *out, size_t cap); /* bytes written, 0 = cap too small */
hg_blob_rc_t hg_blob_unwrap(uint32_t magic, uint16_t cur_ver, uint16_t min_compat,
                            const uint8_t *in, size_t in_len,
                            void *payload_out, uint16_t payload_cap, uint32_t *gen_out);
```

Envelope layout (LE): `0..3 magic · 4..5 version · 6..7 length · 8..11 generation · 12..15 crc32(bytes 0..11 ++ payload)`. Unwrap check order per spec §4.2: SHORT → MAGIC → LENGTH (> cap or > in_len) → CRC → VERSION_NEWER (defaults, blob kept) → VERSION_OLD (< min_compat) → zero-fill dest, copy, `MIGRATED` when version < cur_ver or length < payload_cap, else `OK`.

- [ ] **Step 1: Write header, failing stub and test**

Create `components/hg_blob/hg_blob.h` with exactly the interface block above (plus `#pragma once`, `<stdint.h>`, `<stddef.h>`, `extern "C"` guards).

Create `components/hg_blob/hg_blob.c` stub:

```c
#include "hg_blob.h"
uint32_t hg_crc32(uint32_t seed, const void *buf, size_t n) { (void)seed; (void)buf; (void)n; return 0; }
size_t hg_blob_wrap(uint32_t m, uint16_t v, uint32_t g, const void *p, uint16_t l, uint8_t *o, size_t c)
{ (void)m; (void)v; (void)g; (void)p; (void)l; (void)o; (void)c; return 0; }
hg_blob_rc_t hg_blob_unwrap(uint32_t m, uint16_t cv, uint16_t mc, const uint8_t *in, size_t n,
                            void *out, uint16_t cap, uint32_t *gen)
{ (void)m; (void)cv; (void)mc; (void)in; (void)n; (void)out; (void)cap; (void)gen; return HG_BLOB_E_SHORT; }
```

Create `components/hg_blob/CMakeLists.txt`:

```cmake
idf_component_register(SRCS "hg_blob.c" INCLUDE_DIRS ".")
```

Append to `tests/host/CMakeLists.txt`:

```cmake
hg_test(test_hg_blob ${COMP}/hg_blob/hg_blob.c)
```

Create `tests/host/test_hg_blob.c`:

```c
#include <string.h>
#include "unity.h"
#include "hg_blob.h"

#define MAGIC 0x46434748u /* 'HGCF' LE */

void setUp(void) {}
void tearDown(void) {}

static void test_crc32_vector(void) {
    TEST_ASSERT_EQUAL_HEX32(0xCBF43926u, hg_crc32(0, "123456789", 9));
    /* incremental == one-shot */
    uint32_t c = hg_crc32(0, "12345", 5);
    TEST_ASSERT_EQUAL_HEX32(0xCBF43926u, hg_crc32(c, "6789", 4));
}

static void test_wrap_unwrap_roundtrip(void) {
    uint8_t payload[32], out[64], back[32];
    for (int i = 0; i < 32; i++) payload[i] = (uint8_t)(i * 7);
    size_t n = hg_blob_wrap(MAGIC, 1, 17, payload, 32, out, sizeof out);
    TEST_ASSERT_EQUAL_size_t(48, n);
    uint32_t gen = 0;
    TEST_ASSERT_EQUAL_INT(HG_BLOB_OK, hg_blob_unwrap(MAGIC, 1, 1, out, n, back, 32, &gen));
    TEST_ASSERT_EQUAL_UINT32(17, gen);
    TEST_ASSERT_EQUAL_MEMORY(payload, back, 32);
}

static void test_unwrap_rejects_in_order(void) {
    uint8_t payload[8] = {1,2,3,4,5,6,7,8}, out[24], back[8];
    uint32_t gen;
    size_t n = hg_blob_wrap(MAGIC, 2, 5, payload, 8, out, sizeof out);
    TEST_ASSERT_EQUAL_size_t(24, n);
    TEST_ASSERT_EQUAL_INT(HG_BLOB_E_SHORT, hg_blob_unwrap(MAGIC, 2, 1, out, 15, back, 8, &gen));
    out[0] ^= 0xFF;
    TEST_ASSERT_EQUAL_INT(HG_BLOB_E_MAGIC, hg_blob_unwrap(MAGIC, 2, 1, out, n, back, 8, &gen));
    out[0] ^= 0xFF;
    TEST_ASSERT_EQUAL_INT(HG_BLOB_E_LENGTH, hg_blob_unwrap(MAGIC, 2, 1, out, n, back, 4, &gen)); /* cap too small */
    out[20] ^= 0x01; /* flip a payload bit */
    TEST_ASSERT_EQUAL_INT(HG_BLOB_E_CRC, hg_blob_unwrap(MAGIC, 2, 1, out, n, back, 8, &gen));
    out[20] ^= 0x01;
    TEST_ASSERT_EQUAL_INT(HG_BLOB_E_VERSION_NEWER, hg_blob_unwrap(MAGIC, 1, 1, out, n, back, 8, &gen)); /* blob v2 > fw v1 */
    TEST_ASSERT_EQUAL_INT(HG_BLOB_E_VERSION_OLD, hg_blob_unwrap(MAGIC, 3, 3, out, n, back, 8, &gen));   /* v2 < min 3 */
    /* version byte is CRC-covered */
    out[4] ^= 0x01;
    TEST_ASSERT_EQUAL_INT(HG_BLOB_E_CRC, hg_blob_unwrap(MAGIC, 2, 1, out, n, back, 8, &gen));
}

static void test_unwrap_migrated_zero_fills(void) {
    uint8_t payload[8] = {9,9,9,9,9,9,9,9}, out[24], back[16];
    uint32_t gen;
    size_t n = hg_blob_wrap(MAGIC, 1, 3, payload, 8, out, sizeof out);
    memset(back, 0xAA, sizeof back);
    /* shorter old blob into a bigger current struct -> MIGRATED, tail zeroed */
    TEST_ASSERT_EQUAL_INT(HG_BLOB_MIGRATED, hg_blob_unwrap(MAGIC, 2, 1, out, n, back, 16, &gen));
    TEST_ASSERT_EQUAL_UINT8(9, back[7]);
    TEST_ASSERT_EQUAL_UINT8(0, back[8]);
    TEST_ASSERT_EQUAL_UINT8(0, back[15]);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_crc32_vector);
    RUN_TEST(test_wrap_unwrap_roundtrip);
    RUN_TEST(test_unwrap_rejects_in_order);
    RUN_TEST(test_unwrap_migrated_zero_fills);
    return UNITY_END();
}
```

- [ ] **Step 2: Run — expect FAIL**

Run: `cmake --build build/host --config Release --parallel && ctest --test-dir build/host -C Release --output-on-failure -R test_hg_blob`
Expected: FAIL (stubs).

- [ ] **Step 3: Implement `components/hg_blob/hg_blob.c`**

```c
#include <string.h>
#include "hg_blob.h"

uint32_t hg_crc32(uint32_t seed, const void *buf, size_t n) {
    uint32_t c = ~seed;
    const uint8_t *p = (const uint8_t *)buf;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(c & 1u)));
    }
    return ~c;
}

static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t *p, uint32_t v) { wr16(p, (uint16_t)v); wr16(p + 2, (uint16_t)(v >> 16)); }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) { return rd16(p) | ((uint32_t)rd16(p + 2) << 16); }

size_t hg_blob_wrap(uint32_t magic, uint16_t version, uint32_t generation,
                    const void *payload, uint16_t len, uint8_t *out, size_t cap) {
    if (cap < (size_t)HG_BLOB_HDR_LEN + len) return 0;
    wr32(out, magic);
    wr16(out + 4, version);
    wr16(out + 6, len);
    wr32(out + 8, generation);
    memcpy(out + HG_BLOB_HDR_LEN, payload, len);
    uint32_t c = hg_crc32(0, out, 12);
    c = hg_crc32(c, payload, len);
    wr32(out + 12, c);
    return (size_t)HG_BLOB_HDR_LEN + len;
}

hg_blob_rc_t hg_blob_unwrap(uint32_t magic, uint16_t cur_ver, uint16_t min_compat,
                            const uint8_t *in, size_t in_len,
                            void *payload_out, uint16_t payload_cap, uint32_t *gen_out) {
    if (in_len < HG_BLOB_HDR_LEN) return HG_BLOB_E_SHORT;
    if (rd32(in) != magic) return HG_BLOB_E_MAGIC;
    uint16_t len = rd16(in + 6);
    if (len > payload_cap || (size_t)HG_BLOB_HDR_LEN + len > in_len) return HG_BLOB_E_LENGTH;
    uint32_t c = hg_crc32(0, in, 12);
    c = hg_crc32(c, in + HG_BLOB_HDR_LEN, len);
    if (c != rd32(in + 12)) return HG_BLOB_E_CRC;
    uint16_t ver = rd16(in + 4);
    if (ver > cur_ver) return HG_BLOB_E_VERSION_NEWER;
    if (ver < min_compat) return HG_BLOB_E_VERSION_OLD;
    memset(payload_out, 0, payload_cap);
    memcpy(payload_out, in + HG_BLOB_HDR_LEN, len);
    if (gen_out) *gen_out = rd32(in + 8);
    return (ver < cur_ver || len < payload_cap) ? HG_BLOB_MIGRATED : HG_BLOB_OK;
}
```

- [ ] **Step 4: Run — expect PASS**

Run: `ctest --test-dir build/host -C Release --output-on-failure`
Expected: 2/2 tests pass.

- [ ] **Step 5: Commit**

```bash
git add components/hg_blob tests/host
git commit -m "feat(hg_blob): CRC-32/ISO-HDLC + versioned 16-byte blob envelope

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
git push
```

---

### Task 3: `hg_cfg` — config structs and defaults

**Files:**
- Create: `components/hg_cfg/hg_cfg_types.h`, `components/hg_cfg/hg_cfg.h`, `components/hg_cfg/hg_cfg_defaults.c`, `components/hg_cfg/CMakeLists.txt`
- Modify: `tests/host/CMakeLists.txt`
- Test: `tests/host/test_hg_cfg_types.c`

**Interfaces:**
- Consumes: nothing.
- Produces: the structs below (byte-layouts are the spec §4.1/§4.2 contract — every later component and SP3's ring transfer depend on them) and `void hg_defaults_hw(hg_zone_hw_t *hw); void hg_defaults_cfg(hg_zone_cfg_t *cfg);`

- [ ] **Step 1: Write the types header, defaults stub, and the size/offset test**

Create `components/hg_cfg/hg_cfg_types.h` (complete file):

```c
#pragma once
#include <stdint.h>
#include <assert.h>
#ifdef __cplusplus
extern "C" {
#endif

#define HG_MAX_SHELVES  4
#define HG_MAX_ZONES    8
#define HG_MAX_AUX      2
#define HG_NONE         0xFF
#define HG_NODE_MASTER      0
#define HG_NODE_UNASSIGNED  254
#define HG_NODE_BCAST       255

#define HG_MAGIC_HW     0x57484748u /* 'HGHW' LE */
#define HG_MAGIC_CFG    0x46434748u /* 'HGCF' LE */
#define HG_MAGIC_DAILY  0x59444748u /* 'HGDY' LE */
#define HG_HW_VER       1
#define HG_HW_VER_MIN   1
#define HG_CFG_VER      1
#define HG_CFG_VER_MIN  1

enum { HG_SRC_LOCAL = 0, HG_SRC_MASTER = 1 };

/* ---------- HARDWARE plane (Zone NVS "hw") ---------- */
typedef struct {                     /* 32 B */
    uint8_t  led_ch[2];              /* 0  PCA9685 ch WHITE, RED; HG_NONE = absent */
    uint8_t  pump_pin, fan_pin;      /* 2  PCF8575 pin; HG_NONE = absent */
    uint8_t  soil_ch[2];             /* 4  logical soil channel A,B -> soil_gpio[]; HG_NONE */
    uint8_t  led_max_pct[2];         /* 6  SAFETY duty cap 0..100 */
    uint16_t soil_dry_mv[2];         /* 8  calibration, mV */
    uint16_t soil_wet_mv[2];         /* 12 */
    uint16_t soil_min_ok_mv;         /* 16 plausibility window */
    uint16_t soil_max_ok_mv;         /* 18 */
    uint16_t pump_max_run_s;         /* 20 SAFETY 1..300 */
    uint16_t pump_max_daily_s;       /* 22 SAFETY 1..3600 */
    uint8_t  vib_ch;                 /* 24 PCA9685 channel of the shelf vibrator; HG_NONE = absent */
    uint8_t  rsvd[7];                /* 25 */
} hg_shelf_hw_t;

typedef struct { uint8_t type, pin, shelf_mask, rsvd; } hg_aux_hw_t; /* 4 B; type: 0 NONE, 1 VIBRATOR, 2 RELAY */

typedef struct {                     /* 160 B */
    uint8_t  shelf_count;            /* 0  1..4 (0 = hw fault, no outputs) */
    uint8_t  pca_addr, pcf_addr;     /* 1  0x40 / 0x20; 0 = absent */
    uint8_t  soil_backend;           /* 3  0 internal ADC, 1 ADS1115 */
    uint16_t pcf_active_low_mask;    /* 4  default 0xFFFF (PCF8575 powers up HIGH) */
    uint16_t pca_freq_hz;            /* 6  default 1000 */
    uint8_t  soil_gpio[8];           /* 8  logical ch -> GPIO {32,33,34,35,36,39,25,26} */
    hg_aux_hw_t aux[HG_MAX_AUX];     /* 16 */
    uint8_t  rsvd[8];                /* 24 */
    hg_shelf_hw_t shelf[HG_MAX_SHELVES]; /* 32..159 */
} hg_zone_hw_t;

/* ---------- LOGICAL plane (Master NVS "zcN", Zone NVS "cfg") ---------- */
typedef struct {                     /* 12 B */
    uint16_t on_min, off_min;        /* minutes since midnight */
    uint8_t  white_pct, red_pct, ramp_min, rsvd;
    uint16_t dli_x10;                /* 0 = off */
    uint8_t  rsvd2[2];
} hg_light_cfg_t;

typedef struct {                     /* 16 B */
    uint8_t  mode;                   /* 0 OFF, 1 AUTO */
    uint8_t  target_pct, hyst_pct, settle_min;
    uint16_t dose_s, min_interval_min;
    uint8_t  max_doses_day, diff_max_pct;
    uint16_t win_start_min, win_end_min; /* equal = always */
    uint8_t  rsvd[2];
} hg_water_cfg_t;

typedef struct { uint8_t mode, on_min; uint16_t period_min; } hg_fan_cfg_t;  /* 4 B; mode: 0 OFF 1 ON 2 LIGHT 3 CYCLE */
typedef struct {                     /* 16 B; per-shelf pollination */
    uint8_t  mode, intensity_pct, pulse_s, rsvd;   /* mode: 0 OFF 1 PULSE; intensity 20..100 */
    uint16_t interval_min, start_min, end_min;
    uint8_t  rsvd2[6];
} hg_vib_cfg_t;
typedef struct { uint8_t mode, pulse_s; uint16_t interval_min, start_min, end_min; } hg_aux_cfg_t; /* 8 B; mode: 0 OFF 1 PULSE */

typedef struct {                     /* 72 B */
    char     crop[16];               /* 0 */
    uint8_t  enabled, profile_id;    /* 16 */
    uint8_t  rsvd[2];                /* 18 */
    uint32_t rsvd_mask;              /* 20 */
    hg_light_cfg_t light;            /* 24 */
    hg_water_cfg_t water;            /* 36 */
    hg_fan_cfg_t   fan;              /* 52 */
    hg_vib_cfg_t   vib;              /* 56 */
} hg_shelf_cfg_t;

typedef struct {                     /* 336 B */
    uint32_t generation;             /* 0  0 = never written */
    uint8_t  source;                 /* 4  HG_SRC_* of last writer */
    uint8_t  rsvd0;                  /* 5 */
    uint16_t link_loss_timeout_s;    /* 6  10..600, default 30 */
    char     name[16];               /* 8 */
    hg_aux_cfg_t aux[HG_MAX_AUX];    /* 24 */
    uint8_t  rsvd[8];                /* 40 */
    hg_shelf_cfg_t shelf[HG_MAX_SHELVES]; /* 48..335 */
} hg_zone_cfg_t;

/* ---------- daily safety counters (RTC_NOINIT + NVS "daily") ---------- */
typedef struct {                     /* 64 B */
    uint32_t magic;                  /* HG_MAGIC_DAILY */
    uint32_t unix_time;
    uint16_t day_index;
    uint8_t  rsvd0[2];
    struct { uint16_t doses_today, pump_today_s, light_today_min, vib_today_s; } shelf[HG_MAX_SHELVES]; /* 12..43 */
    uint16_t dli_today_x10[HG_MAX_SHELVES]; /* 44..51 */
    uint8_t  rsvd[8];                /* 52 */
    uint32_t crc;                    /* 60 */
} hg_daily_t;

_Static_assert(sizeof(hg_shelf_hw_t) == 32,  "hg_shelf_hw_t");
_Static_assert(sizeof(hg_zone_hw_t)  == 160, "hg_zone_hw_t");
_Static_assert(sizeof(hg_light_cfg_t) == 12, "hg_light_cfg_t");
_Static_assert(sizeof(hg_water_cfg_t) == 16, "hg_water_cfg_t");
_Static_assert(sizeof(hg_fan_cfg_t)   == 4,  "hg_fan_cfg_t");
_Static_assert(sizeof(hg_aux_cfg_t)   == 8,  "hg_aux_cfg_t");
_Static_assert(sizeof(hg_vib_cfg_t)   == 16, "hg_vib_cfg_t");
_Static_assert(sizeof(hg_shelf_cfg_t) == 72, "hg_shelf_cfg_t");
_Static_assert(sizeof(hg_zone_cfg_t)  == 336,"hg_zone_cfg_t");
_Static_assert(sizeof(hg_daily_t)     == 64, "hg_daily_t");

#ifdef __cplusplus
}
#endif
```

Create `components/hg_cfg/hg_cfg.h` (grows in Task 4; start with):

```c
#pragma once
#include "hg_cfg_types.h"
#ifdef __cplusplus
extern "C" {
#endif

void hg_defaults_hw(hg_zone_hw_t *hw);
void hg_defaults_cfg(hg_zone_cfg_t *cfg);

#ifdef __cplusplus
}
#endif
```

Create `components/hg_cfg/hg_cfg_defaults.c` stub (both functions `memset(p, 0xEE, sizeof *p);` so the test fails), and `components/hg_cfg/CMakeLists.txt`:

```cmake
idf_component_register(SRCS "hg_cfg_defaults.c" "hg_cfg_fields.c" "hg_cfg_validate.c" INCLUDE_DIRS ".")
```

(Also create empty `hg_cfg_fields.c` / `hg_cfg_validate.c` containing only `/* Task 4 */` so the component compiles.)

Append to `tests/host/CMakeLists.txt`:

```cmake
hg_test(test_hg_cfg_types ${COMP}/hg_cfg/hg_cfg_defaults.c)
```

Create `tests/host/test_hg_cfg_types.c`:

```c
#include <stddef.h>
#include "unity.h"
#include "hg_cfg.h"

void setUp(void) {}
void tearDown(void) {}

static void test_offsets_pinned(void) {
    TEST_ASSERT_EQUAL_size_t(32,  offsetof(hg_zone_hw_t, shelf));
    TEST_ASSERT_EQUAL_size_t(8,   offsetof(hg_shelf_hw_t, soil_dry_mv));
    TEST_ASSERT_EQUAL_size_t(20,  offsetof(hg_shelf_hw_t, pump_max_run_s));
    TEST_ASSERT_EQUAL_size_t(48,  offsetof(hg_zone_cfg_t, shelf));
    TEST_ASSERT_EQUAL_size_t(24,  offsetof(hg_shelf_cfg_t, light));
    TEST_ASSERT_EQUAL_size_t(36,  offsetof(hg_shelf_cfg_t, water));
    TEST_ASSERT_EQUAL_size_t(52,  offsetof(hg_shelf_cfg_t, fan));
    TEST_ASSERT_EQUAL_size_t(56,  offsetof(hg_shelf_cfg_t, vib));
    TEST_ASSERT_EQUAL_size_t(6,   offsetof(hg_zone_cfg_t, link_loss_timeout_s));
    TEST_ASSERT_EQUAL_size_t(60,  offsetof(hg_daily_t, crc));
}

static void test_defaults_hw(void) {
    hg_zone_hw_t hw;
    hg_defaults_hw(&hw);
    TEST_ASSERT_EQUAL_UINT8(4, hw.shelf_count);
    TEST_ASSERT_EQUAL_HEX8(0x40, hw.pca_addr);
    TEST_ASSERT_EQUAL_HEX8(0x20, hw.pcf_addr);
    TEST_ASSERT_EQUAL_HEX16(0xFFFF, hw.pcf_active_low_mask);
    TEST_ASSERT_EQUAL_UINT16(1000, hw.pca_freq_hz);
    TEST_ASSERT_EQUAL_UINT8(32, hw.soil_gpio[0]);
    TEST_ASSERT_EQUAL_UINT8(26, hw.soil_gpio[7]);
    for (int i = 0; i < 4; i++) {
        TEST_ASSERT_EQUAL_UINT8(2 * i,     hw.shelf[i].led_ch[0]);
        TEST_ASSERT_EQUAL_UINT8(2 * i + 1, hw.shelf[i].led_ch[1]);
        TEST_ASSERT_EQUAL_UINT8(i,         hw.shelf[i].pump_pin);
        TEST_ASSERT_EQUAL_UINT8(4 + i,     hw.shelf[i].fan_pin);
        TEST_ASSERT_EQUAL_UINT16(60,       hw.shelf[i].pump_max_run_s);
        TEST_ASSERT_EQUAL_UINT16(600,      hw.shelf[i].pump_max_daily_s);
        TEST_ASSERT_EQUAL_UINT16(2800,     hw.shelf[i].soil_dry_mv[0]);
        TEST_ASSERT_EQUAL_UINT16(1300,     hw.shelf[i].soil_wet_mv[1]);
        TEST_ASSERT_EQUAL_UINT8(100,       hw.shelf[i].led_max_pct[0]);
        TEST_ASSERT_EQUAL_UINT8(8 + i,     hw.shelf[i].vib_ch);
    }
    TEST_ASSERT_EQUAL_UINT8(0, hw.aux[0].type);
}

static void test_defaults_cfg_inert(void) {
    hg_zone_cfg_t c;
    hg_defaults_cfg(&c);
    TEST_ASSERT_EQUAL_UINT32(0, c.generation);
    TEST_ASSERT_EQUAL_UINT8(HG_SRC_LOCAL, c.source);
    TEST_ASSERT_EQUAL_UINT16(30, c.link_loss_timeout_s);
    for (int i = 0; i < 4; i++) {
        TEST_ASSERT_EQUAL_UINT8(0, c.shelf[i].enabled);      /* nothing energises by default */
        TEST_ASSERT_EQUAL_UINT16(360,  c.shelf[i].light.on_min);   /* 06:00 */
        TEST_ASSERT_EQUAL_UINT16(1320, c.shelf[i].light.off_min);  /* 22:00 */
        TEST_ASSERT_EQUAL_UINT8(1,  c.shelf[i].water.mode);        /* AUTO (but shelf disabled) */
        TEST_ASSERT_EQUAL_UINT8(45, c.shelf[i].water.target_pct);
        TEST_ASSERT_EQUAL_UINT16(20, c.shelf[i].water.dose_s);
        TEST_ASSERT_EQUAL_UINT8(3,  c.shelf[i].fan.mode);          /* CYCLE */
        TEST_ASSERT_EQUAL_UINT8(0,  c.shelf[i].vib.mode);           /* OFF */
        TEST_ASSERT_EQUAL_UINT8(60, c.shelf[i].vib.intensity_pct);
    }
    TEST_ASSERT_EQUAL_UINT8(0, c.aux[0].mode);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_offsets_pinned);
    RUN_TEST(test_defaults_hw);
    RUN_TEST(test_defaults_cfg_inert);
    return UNITY_END();
}
```

- [ ] **Step 2: Run — expect FAIL** (stub fills 0xEE)

Run: `cmake --build build/host --config Release --parallel && ctest --test-dir build/host -C Release -R test_hg_cfg_types --output-on-failure`
Expected: FAIL on `test_defaults_hw`.

- [ ] **Step 3: Implement `components/hg_cfg/hg_cfg_defaults.c`**

```c
#include <string.h>
#include "hg_cfg.h"

void hg_defaults_hw(hg_zone_hw_t *hw) {
    static const uint8_t soil_gpio[8] = { 32, 33, 34, 35, 36, 39, 25, 26 };
    memset(hw, 0, sizeof *hw);
    hw->shelf_count = 4;
    hw->pca_addr = 0x40;
    hw->pcf_addr = 0x20;
    hw->soil_backend = 0;
    hw->pcf_active_low_mask = 0xFFFF;
    hw->pca_freq_hz = 1000;
    memcpy(hw->soil_gpio, soil_gpio, 8);
    for (int i = 0; i < HG_MAX_AUX; i++) { hw->aux[i].type = 0; hw->aux[i].pin = HG_NONE; }
    for (int i = 0; i < HG_MAX_SHELVES; i++) {
        hg_shelf_hw_t *s = &hw->shelf[i];
        s->led_ch[0] = (uint8_t)(2 * i);
        s->led_ch[1] = (uint8_t)(2 * i + 1);
        s->pump_pin  = (uint8_t)i;
        s->fan_pin   = (uint8_t)(4 + i);
        s->soil_ch[0] = (uint8_t)(2 * i);
        s->soil_ch[1] = (uint8_t)(2 * i + 1);
        s->led_max_pct[0] = s->led_max_pct[1] = 100;
        s->soil_dry_mv[0] = s->soil_dry_mv[1] = 2800;
        s->soil_wet_mv[0] = s->soil_wet_mv[1] = 1300;
        s->soil_min_ok_mv = 300;
        s->soil_max_ok_mv = 3200;
        s->pump_max_run_s   = 60;
        s->pump_max_daily_s = 600;
        s->vib_ch = (uint8_t)(8 + i);
    }
}

void hg_defaults_cfg(hg_zone_cfg_t *cfg) {
    memset(cfg, 0, sizeof *cfg);
    cfg->source = HG_SRC_LOCAL;
    cfg->link_loss_timeout_s = 30;
    strcpy(cfg->name, "zone");
    for (int i = 0; i < HG_MAX_SHELVES; i++) {
        hg_shelf_cfg_t *s = &cfg->shelf[i];
        s->enabled = 0;
        s->light.on_min = 6 * 60;
        s->light.off_min = 22 * 60;
        s->light.white_pct = 50;
        s->light.red_pct = 30;
        s->water.mode = 1;
        s->water.target_pct = 45;
        s->water.hyst_pct = 5;
        s->water.settle_min = 10;
        s->water.dose_s = 20;
        s->water.min_interval_min = 120;
        s->water.max_doses_day = 6;
        s->water.diff_max_pct = 15;
        s->fan.mode = 3;
        s->fan.on_min = 15;
        s->fan.period_min = 60;
        s->vib.mode = 0;
        s->vib.intensity_pct = 60;
        s->vib.pulse_s = 5;
        s->vib.interval_min = 120;
        s->vib.start_min = 10 * 60;
        s->vib.end_min = 16 * 60;
    }
}
```

- [ ] **Step 4: Run — expect PASS**

Run: `ctest --test-dir build/host -C Release --output-on-failure`
Expected: 3/3 tests pass.

- [ ] **Step 5: Commit**

```bash
git add components/hg_cfg tests/host
git commit -m "feat(hg_cfg): pinned config struct layouts + inert defaults

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
git push
```

---

### Task 4: `hg_cfg` — field table, text conversion, validation

**Files:**
- Create (replace Task-3 placeholders): `components/hg_cfg/hg_cfg_fields.c`, `components/hg_cfg/hg_cfg_validate.c`
- Modify: `components/hg_cfg/hg_cfg.h`, `tests/host/CMakeLists.txt`
- Test: `tests/host/test_hg_cfg_fields.c`, `tests/host/test_hg_cfg_validate.c`

**Interfaces:**
- Consumes: Task 3 structs/defaults.
- Produces (used by zone_cmds Task 10, hg_model Task 9, SP4 JSON):

```c
typedef enum { HG_G_ZONECFG = 0, HG_G_SHELF, HG_G_LIGHT, HG_G_WATER, HG_G_FAN, HG_G_VIB,
               HG_G_AUX, HG_G_HW, HG_G_HWSHELF, HG_G_CAL, HG_G_COUNT } hg_group_t;
typedef enum { HG_T_U8, HG_T_U16, HG_T_BOOL, HG_T_HHMM, HG_T_ENUM, HG_T_STR16, HG_T_PIN } hg_ftype_t;
typedef struct { uint8_t group; const char *key; uint16_t offset; uint8_t type;
                 int32_t min, max; const char *enums; } hg_field_t;
extern const hg_field_t HG_FIELDS[];
extern const int        HG_FIELD_COUNT;
extern const char *const HG_GROUP_NAMES[HG_G_COUNT];   /* "ZONECFG".."CAL" */
int  hg_group_find(const char *name);                  /* case-insensitive; -1 unknown */
int  hg_group_scope(uint8_t group);                    /* 0 zone-scoped, 1 shelf-scoped (idx 0..3), 2 aux-scoped (idx 0..1) */
/* rc: 0 OK, -1 BAD_ARGS, -2 OUT_OF_RANGE, -3 INVALID_FIELD (unknown key), -4 bad index */
int  hg_field_set_text(hg_zone_hw_t *hw, hg_zone_cfg_t *cfg, uint8_t group, int idx, const char *key, const char *val);
int  hg_field_get_text(const hg_zone_hw_t *hw, const hg_zone_cfg_t *cfg, uint8_t group, int idx, const char *key, char *out, size_t cap);
int  hg_hhmm_parse(const char *s);                     /* "HH:MM" or integer minutes; 0..1439, -1 bad */
void hg_hhmm_format(int minutes, char out[6]);
int  hg_hw_validate(const hg_zone_hw_t *hw, char *err, size_t errlen);                       /* 0 ok / -1, err = field path */
int  hg_cfg_validate(const hg_zone_cfg_t *cfg, const hg_zone_hw_t *hw_or_null, char *err, size_t errlen);
```

Row inventory (offsets are `offsetof` into the group's base struct; scope in parentheses): **ZONECFG**(zone,`hg_zone_cfg_t`): NAME STR16@name · LINKLOSS_S U16@6 10..600. **SHELF**(shelf,`hg_shelf_cfg_t`): CROP STR16@0 · ENABLED BOOL@16 · PROFILE U8@17 0..16. **LIGHT**(shelf,`.light`): ON HHMM@0 · OFF HHMM@2 · WHITE U8@4 0..100 · RED U8@5 0..100 · RAMP_MIN U8@6 0..120 · DLI U16@8 0..1000. **WATER**(shelf,`.water`): MODE ENUM@0 `OFF|AUTO` · TARGET U8@1 0..100 · HYST U8@2 1..30 · SETTLE_MIN U8@3 1..60 · DOSE_S U16@4 1..300 · INTERVAL_MIN U16@6 10..1440 · MAX_DOSES U8@8 0..24 · DIFF_MAX U8@9 5..50 · WIN_START HHMM@10 · WIN_END HHMM@12. **FAN**(shelf,`.fan`): MODE ENUM@0 `OFF|ON|LIGHT|CYCLE` · ON_MIN U8@1 0..60 · PERIOD_MIN U16@2 0..1440. **VIB**(shelf,`.vib`): MODE ENUM@0 `OFF|PULSE` · INTENSITY U8@1 20..100 · PULSE_S U8@2 1..30 · INTERVAL_MIN U16@4 5..1440 · START HHMM@6 · END HHMM@8. **VIB**(shelf,`.vib`): MODE ENUM@0 `OFF|PULSE` · INTENSITY U8@1 20..100 · PULSE_S U8@2 1..30 · INTERVAL_MIN U16@4 5..1440 · START HHMM@6 · END HHMM@8. **AUX**(aux,`hg_aux_cfg_t`): MODE ENUM@0 `OFF|PULSE` · PULSE_S U8@1 1..30 · INTERVAL_MIN U16@2 5..1440 · START HHMM@4 · END HHMM@6. **HW**(zone,`hg_zone_hw_t`): SHELVES U8@0 1..4 · PCA_ADDR U8@1 0..127 · PCF_ADDR U8@2 0..127 · SOIL_BACKEND ENUM@3 `INTERNAL|ADS1115` · PCF_ACTLOW U16@4 0..65535 · PCA_HZ U16@6 200..1500. **HWSHELF**(shelf,`hg_shelf_hw_t`): LED_W PIN@0 max15 · LED_R PIN@1 max15 · PUMP PIN@2 max15 · FAN PIN@3 max15 · SOIL_A PIN@4 max7 · SOIL_B PIN@5 max7 · LED_MAX_W U8@6 0..100 · LED_MAX_R U8@7 0..100 · PUMP_MAX_RUN_S U16@20 1..300 · PUMP_MAX_DAILY_S U16@22 1..3600. **CAL**(shelf,`hg_shelf_hw_t`): DRY_A U16@8 0..3300 · DRY_B U16@10 · WET_A U16@12 · WET_B U16@14 · MIN_OK U16@16 · MAX_OK U16@18.

Type semantics: BOOL accepts `0|1|ON|OFF|ENABLE|DISABLE`, prints `0/1`; ENUM accepts a name (case-insensitive) or its index, prints the name; PIN accepts `NONE` (=0xFF) or 0..max, prints `NONE` or the number; STR16 ≤15 chars, printable 0x20–0x7E, **no spaces** (tokenizer), NUL-padded; HHMM accepts `H:MM`/`HH:MM` or bare minutes, prints `HH:MM`. U16 values are written/read with `memcpy` (alignment-safe).

- [ ] **Step 1: Extend `hg_cfg.h` with the interface block above, write failing stubs and the two test files**

`hg_cfg_fields.c` stub: define `HG_FIELDS[] = {{0}}`, `HG_FIELD_COUNT = 0`, all functions return `-3` / format `"?"`. `hg_cfg_validate.c` stub: both return `-1` with `err[0]='\0'`.

Append to `tests/host/CMakeLists.txt`:

```cmake
set(HG_CFG_SRC ${COMP}/hg_cfg/hg_cfg_defaults.c ${COMP}/hg_cfg/hg_cfg_fields.c ${COMP}/hg_cfg/hg_cfg_validate.c)
hg_test(test_hg_cfg_fields   ${HG_CFG_SRC})
hg_test(test_hg_cfg_validate ${HG_CFG_SRC})
```

Create `tests/host/test_hg_cfg_fields.c`:

```c
#include <string.h>
#include "unity.h"
#include "hg_cfg.h"

static hg_zone_hw_t hw;
static hg_zone_cfg_t cfg;
void setUp(void) { hg_defaults_hw(&hw); hg_defaults_cfg(&cfg); }
void tearDown(void) {}

static const size_t GROUP_SIZE[HG_G_COUNT] = {
    sizeof(hg_zone_cfg_t), sizeof(hg_shelf_cfg_t), sizeof(hg_light_cfg_t),
    sizeof(hg_water_cfg_t), sizeof(hg_fan_cfg_t), sizeof(hg_vib_cfg_t), sizeof(hg_aux_cfg_t),
    sizeof(hg_zone_hw_t), sizeof(hg_shelf_hw_t), sizeof(hg_shelf_hw_t) };

static size_t type_width(uint8_t t) {
    switch (t) {
    case HG_T_U16: case HG_T_HHMM: return 2;
    case HG_T_STR16: return 16;
    default: return 1;
    }
}

static void test_table_integrity(void) {
    TEST_ASSERT_GREATER_THAN_INT(40, HG_FIELD_COUNT);
    for (int i = 0; i < HG_FIELD_COUNT; i++) {
        const hg_field_t *f = &HG_FIELDS[i];
        TEST_ASSERT_LESS_THAN_UINT8(HG_G_COUNT, f->group);
        TEST_ASSERT((size_t)f->offset + type_width(f->type) <= GROUP_SIZE[f->group]);
        if (f->type == HG_T_ENUM) TEST_ASSERT_NOT_NULL(f->enums);
        if (f->type == HG_T_HHMM) { /* range implied 0..1439 */ }
        for (int j = i + 1; j < HG_FIELD_COUNT; j++)                     /* unique key per group */
            if (HG_FIELDS[j].group == f->group)
                TEST_ASSERT_NOT_EQUAL(0, strcmp(HG_FIELDS[j].key, f->key));
    }
}

static void test_set_get_roundtrip_all_rows(void) {
    char buf[24], buf2[24];
    for (int i = 0; i < HG_FIELD_COUNT; i++) {
        const hg_field_t *f = &HG_FIELDS[i];
        int idx = 0;
        TEST_ASSERT_EQUAL_INT(0, hg_field_get_text(&hw, &cfg, f->group, idx, f->key, buf, sizeof buf));
        TEST_ASSERT_EQUAL_INT(0, hg_field_set_text(&hw, &cfg, f->group, idx, f->key, buf));
        TEST_ASSERT_EQUAL_INT(0, hg_field_get_text(&hw, &cfg, f->group, idx, f->key, buf2, sizeof buf2));
        TEST_ASSERT_EQUAL_STRING(buf, buf2);
    }
}

static void test_specific_forms(void) {
    char buf[24];
    TEST_ASSERT_EQUAL_INT(0, hg_field_set_text(&hw, &cfg, HG_G_LIGHT, 1, "ON", "06:30"));
    TEST_ASSERT_EQUAL_INT(0, hg_field_get_text(&hw, &cfg, HG_G_LIGHT, 1, "on", buf, sizeof buf));
    TEST_ASSERT_EQUAL_STRING("06:30", buf);
    TEST_ASSERT_EQUAL_INT(0, hg_field_set_text(&hw, &cfg, HG_G_LIGHT, 1, "ON", "390"));   /* integer minutes ok */
    TEST_ASSERT_EQUAL_INT(0, hg_field_set_text(&hw, &cfg, HG_G_WATER, 0, "mode", "off"));
    TEST_ASSERT_EQUAL_INT(0, hg_field_get_text(&hw, &cfg, HG_G_WATER, 0, "MODE", buf, sizeof buf));
    TEST_ASSERT_EQUAL_STRING("OFF", buf);
    TEST_ASSERT_EQUAL_INT(0, hg_field_set_text(&hw, &cfg, HG_G_WATER, 0, "MODE", "1"));   /* index form */
    TEST_ASSERT_EQUAL_INT(0, hg_field_set_text(&hw, &cfg, HG_G_HWSHELF, 2, "PUMP", "NONE"));
    TEST_ASSERT_EQUAL_INT(0, hg_field_get_text(&hw, &cfg, HG_G_HWSHELF, 2, "PUMP", buf, sizeof buf));
    TEST_ASSERT_EQUAL_STRING("NONE", buf);
    TEST_ASSERT_EQUAL_UINT8(HG_NONE, hw.shelf[2].pump_pin);
    TEST_ASSERT_EQUAL_INT(0, hg_field_set_text(&hw, &cfg, HG_G_SHELF, 0, "ENABLED", "ON"));
    TEST_ASSERT_EQUAL_UINT8(1, cfg.shelf[0].enabled);
    TEST_ASSERT_EQUAL_INT(0, hg_field_set_text(&hw, &cfg, HG_G_SHELF, 0, "CROP", "Basil"));
    TEST_ASSERT_EQUAL_STRING("Basil", cfg.shelf[0].crop);
}

static void test_errors(void) {
    TEST_ASSERT_EQUAL_INT(-2, hg_field_set_text(&hw, &cfg, HG_G_WATER, 0, "DOSE_S", "301"));
    TEST_ASSERT_EQUAL_INT(-2, hg_field_set_text(&hw, &cfg, HG_G_WATER, 0, "HYST", "0"));
    TEST_ASSERT_EQUAL_INT(-1, hg_field_set_text(&hw, &cfg, HG_G_WATER, 0, "DOSE_S", "abc"));
    TEST_ASSERT_EQUAL_INT(-1, hg_field_set_text(&hw, &cfg, HG_G_LIGHT, 0, "ON", "25:00"));
    TEST_ASSERT_EQUAL_INT(-1, hg_field_set_text(&hw, &cfg, HG_G_WATER, 0, "MODE", "MAYBE"));
    TEST_ASSERT_EQUAL_INT(-3, hg_field_set_text(&hw, &cfg, HG_G_WATER, 0, "BOGUS", "1"));
    TEST_ASSERT_EQUAL_INT(-4, hg_field_set_text(&hw, &cfg, HG_G_WATER, 4, "TARGET", "50"));
    TEST_ASSERT_EQUAL_INT(-4, hg_field_set_text(&hw, &cfg, HG_G_AUX, 2, "MODE", "OFF"));
    TEST_ASSERT_EQUAL_INT(-1, hg_field_set_text(&hw, &cfg, HG_G_SHELF, 0, "CROP", "name with space"));
    TEST_ASSERT_EQUAL_INT(-1, hg_field_set_text(&hw, &cfg, HG_G_SHELF, 0, "CROP", "sixteencharslong!"));
    TEST_ASSERT_EQUAL_INT(-1, hg_group_find("NOPE"));
    TEST_ASSERT_EQUAL_INT(HG_G_WATER, hg_group_find("water"));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_table_integrity);
    RUN_TEST(test_set_get_roundtrip_all_rows);
    RUN_TEST(test_specific_forms);
    RUN_TEST(test_errors);
    return UNITY_END();
}
```

Create `tests/host/test_hg_cfg_validate.c`:

```c
#include <string.h>
#include "unity.h"
#include "hg_cfg.h"

static hg_zone_hw_t hw;
static hg_zone_cfg_t cfg;
static char err[48];
void setUp(void) { hg_defaults_hw(&hw); hg_defaults_cfg(&cfg); err[0] = '\0'; }
void tearDown(void) {}

static void test_defaults_are_valid(void) {
    TEST_ASSERT_EQUAL_INT(0, hg_hw_validate(&hw, err, sizeof err));
    TEST_ASSERT_EQUAL_INT(0, hg_cfg_validate(&cfg, &hw, err, sizeof err));
    TEST_ASSERT_EQUAL_INT(0, hg_cfg_validate(&cfg, NULL, err, sizeof err));
}

static void test_hw_rejects(void) {
    hw.shelf_count = 5;
    TEST_ASSERT_EQUAL_INT(-1, hg_hw_validate(&hw, err, sizeof err));
    TEST_ASSERT_EQUAL_STRING("hw.shelves", err);
    hg_defaults_hw(&hw);
    hw.shelf[1].led_ch[0] = hw.shelf[0].led_ch[1];             /* duplicate PCA channel */
    TEST_ASSERT_EQUAL_INT(-1, hg_hw_validate(&hw, err, sizeof err));
    TEST_ASSERT_EQUAL_STRING("shelf[1].hwshelf.led_w", err);
    hg_defaults_hw(&hw);
    hw.shelf[3].pump_pin = hw.shelf[0].fan_pin;                /* duplicate PCF pin */
    TEST_ASSERT_EQUAL_INT(-1, hg_hw_validate(&hw, err, sizeof err));
    TEST_ASSERT_EQUAL_STRING("shelf[3].hwshelf.pump", err);
    hg_defaults_hw(&hw);
    hw.shelf[2].soil_ch[1] = hw.shelf[2].soil_ch[0];           /* duplicate soil channel */
    TEST_ASSERT_EQUAL_INT(-1, hg_hw_validate(&hw, err, sizeof err));
    TEST_ASSERT_EQUAL_STRING("shelf[2].hwshelf.soil_b", err);
    hg_defaults_hw(&hw);
    hw.shelf[0].soil_min_ok_mv = 3300; hw.shelf[0].soil_max_ok_mv = 300;
    TEST_ASSERT_EQUAL_INT(-1, hg_hw_validate(&hw, err, sizeof err));
    TEST_ASSERT_EQUAL_STRING("shelf[0].cal.min_ok", err);
    hg_defaults_hw(&hw);
    hw.shelf[0].soil_dry_mv[0] = 1000;                          /* dry <= wet */
    TEST_ASSERT_EQUAL_INT(-1, hg_hw_validate(&hw, err, sizeof err));
    TEST_ASSERT_EQUAL_STRING("shelf[0].cal.dry_a", err);
    hg_defaults_hw(&hw);
    hw.shelf[0].pump_max_run_s = 0;                             /* table range via blob path */
    TEST_ASSERT_EQUAL_INT(-1, hg_hw_validate(&hw, err, sizeof err));
    TEST_ASSERT_EQUAL_STRING("shelf[0].hwshelf.pump_max_run_s", err);
    /* HG_NONE duplicates are allowed */
    hg_defaults_hw(&hw);
    hw.shelf[0].pump_pin = HG_NONE; hw.shelf[1].pump_pin = HG_NONE;
    TEST_ASSERT_EQUAL_INT(0, hg_hw_validate(&hw, err, sizeof err));
}

static void test_cfg_rejects(void) {
    cfg.shelf[1].light.off_min = cfg.shelf[1].light.on_min;
    TEST_ASSERT_EQUAL_INT(-1, hg_cfg_validate(&cfg, &hw, err, sizeof err));
    TEST_ASSERT_EQUAL_STRING("shelf[1].light.off", err);
    hg_defaults_cfg(&cfg);
    hw.shelf_count = 2; cfg.shelf[3].enabled = 1;
    TEST_ASSERT_EQUAL_INT(-1, hg_cfg_validate(&cfg, &hw, err, sizeof err));
    TEST_ASSERT_EQUAL_STRING("shelf[3].enabled", err);
    TEST_ASSERT_EQUAL_INT(0, hg_cfg_validate(&cfg, NULL, err, sizeof err)); /* no hw context -> not checkable */
    hg_defaults_hw(&hw); hg_defaults_cfg(&cfg);
    cfg.shelf[0].water.dose_s = 100; hw.shelf[0].pump_max_run_s = 60;
    TEST_ASSERT_EQUAL_INT(-1, hg_cfg_validate(&cfg, &hw, err, sizeof err));
    TEST_ASSERT_EQUAL_STRING("shelf[0].water.dose_s", err);
    hg_defaults_cfg(&cfg);
    cfg.shelf[0].water.max_doses_day = 24; cfg.shelf[0].water.dose_s = 30; /* 24*30=720 > 600 daily */
    TEST_ASSERT_EQUAL_INT(-1, hg_cfg_validate(&cfg, &hw, err, sizeof err));
    TEST_ASSERT_EQUAL_STRING("shelf[0].water.max_doses", err);
    hg_defaults_cfg(&cfg);
    cfg.shelf[2].water.target_pct = 200;                        /* raw blob out of range */
    TEST_ASSERT_EQUAL_INT(-1, hg_cfg_validate(&cfg, &hw, err, sizeof err));
    TEST_ASSERT_EQUAL_STRING("shelf[2].water.target", err);
    hg_defaults_cfg(&cfg);
    cfg.name[0] = 0x07;
    TEST_ASSERT_EQUAL_INT(-1, hg_cfg_validate(&cfg, &hw, err, sizeof err));
    TEST_ASSERT_EQUAL_STRING("zonecfg.name", err);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_defaults_are_valid);
    RUN_TEST(test_hw_rejects);
    RUN_TEST(test_cfg_rejects);
    return UNITY_END();
}
```

- [ ] **Step 2: Run — expect FAIL**

Run: `cmake --build build/host --config Release --parallel && ctest --test-dir build/host -C Release -R "test_hg_cfg_(fields|validate)" --output-on-failure`
Expected: both FAIL (stubs).

- [ ] **Step 3: Implement `components/hg_cfg/hg_cfg_fields.c`**

```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hg_cfg.h"

const char *const HG_GROUP_NAMES[HG_G_COUNT] =
    { "ZONECFG", "SHELF", "LIGHT", "WATER", "FAN", "VIB", "AUX", "HW", "HWSHELF", "CAL" };

#define F(g, k, off, t, lo, hi, e) { HG_G_##g, k, (uint16_t)(off), HG_T_##t, lo, hi, e }
const hg_field_t HG_FIELDS[] = {
    F(ZONECFG, "NAME",        offsetof(hg_zone_cfg_t, name), STR16, 0, 15, NULL),
    F(ZONECFG, "LINKLOSS_S",  6,  U16, 10, 600, NULL),
    F(SHELF,   "CROP",        0,  STR16, 0, 15, NULL),
    F(SHELF,   "ENABLED",     16, BOOL, 0, 1, NULL),
    F(SHELF,   "PROFILE",     17, U8, 0, 16, NULL),
    F(LIGHT,   "ON",          0,  HHMM, 0, 1439, NULL),
    F(LIGHT,   "OFF",         2,  HHMM, 0, 1439, NULL),
    F(LIGHT,   "WHITE",       4,  U8, 0, 100, NULL),
    F(LIGHT,   "RED",         5,  U8, 0, 100, NULL),
    F(LIGHT,   "RAMP_MIN",    6,  U8, 0, 120, NULL),
    F(LIGHT,   "DLI",         8,  U16, 0, 1000, NULL),
    F(WATER,   "MODE",        0,  ENUM, 0, 1, "OFF|AUTO"),
    F(WATER,   "TARGET",      1,  U8, 0, 100, NULL),
    F(WATER,   "HYST",        2,  U8, 1, 30, NULL),
    F(WATER,   "SETTLE_MIN",  3,  U8, 1, 60, NULL),
    F(WATER,   "DOSE_S",      4,  U16, 1, 300, NULL),
    F(WATER,   "INTERVAL_MIN",6,  U16, 10, 1440, NULL),
    F(WATER,   "MAX_DOSES",   8,  U8, 0, 24, NULL),
    F(WATER,   "DIFF_MAX",    9,  U8, 5, 50, NULL),
    F(WATER,   "WIN_START",   10, HHMM, 0, 1439, NULL),
    F(WATER,   "WIN_END",     12, HHMM, 0, 1439, NULL),
    F(FAN,     "MODE",        0,  ENUM, 0, 3, "OFF|ON|LIGHT|CYCLE"),
    F(FAN,     "ON_MIN",      1,  U8, 0, 60, NULL),
    F(FAN,     "PERIOD_MIN",  2,  U16, 0, 1440, NULL),
    F(VIB,     "MODE",        0,  ENUM, 0, 1, "OFF|PULSE"),
    F(VIB,     "INTENSITY",   1,  U8, 20, 100, NULL),
    F(VIB,     "PULSE_S",     2,  U8, 1, 30, NULL),
    F(VIB,     "INTERVAL_MIN",4,  U16, 5, 1440, NULL),
    F(VIB,     "START",       6,  HHMM, 0, 1439, NULL),
    F(VIB,     "END",         8,  HHMM, 0, 1439, NULL),
    F(AUX,     "MODE",        0,  ENUM, 0, 1, "OFF|PULSE"),
    F(AUX,     "PULSE_S",     1,  U8, 1, 30, NULL),
    F(AUX,     "INTERVAL_MIN",2,  U16, 5, 1440, NULL),
    F(AUX,     "START",       4,  HHMM, 0, 1439, NULL),
    F(AUX,     "END",         6,  HHMM, 0, 1439, NULL),
    F(HW,      "SHELVES",     0,  U8, 1, 4, NULL),
    F(HW,      "PCA_ADDR",    1,  U8, 0, 127, NULL),
    F(HW,      "PCF_ADDR",    2,  U8, 0, 127, NULL),
    F(HW,      "SOIL_BACKEND",3,  ENUM, 0, 1, "INTERNAL|ADS1115"),
    F(HW,      "PCF_ACTLOW",  4,  U16, 0, 65535, NULL),
    F(HW,      "PCA_HZ",      6,  U16, 200, 1500, NULL),
    F(HWSHELF, "LED_W",       0,  PIN, 0, 15, NULL),
    F(HWSHELF, "LED_R",       1,  PIN, 0, 15, NULL),
    F(HWSHELF, "PUMP",        2,  PIN, 0, 15, NULL),
    F(HWSHELF, "FAN",         3,  PIN, 0, 15, NULL),
    F(HWSHELF, "SOIL_A",      4,  PIN, 0, 7, NULL),
    F(HWSHELF, "SOIL_B",      5,  PIN, 0, 7, NULL),
    F(HWSHELF, "VIB",         24, PIN, 0, 15, NULL),
    F(HWSHELF, "LED_MAX_W",   6,  U8, 0, 100, NULL),
    F(HWSHELF, "LED_MAX_R",   7,  U8, 0, 100, NULL),
    F(HWSHELF, "PUMP_MAX_RUN_S",   20, U16, 1, 300, NULL),
    F(HWSHELF, "PUMP_MAX_DAILY_S", 22, U16, 1, 3600, NULL),
    F(CAL,     "DRY_A",       8,  U16, 0, 3300, NULL),
    F(CAL,     "DRY_B",       10, U16, 0, 3300, NULL),
    F(CAL,     "WET_A",       12, U16, 0, 3300, NULL),
    F(CAL,     "WET_B",       14, U16, 0, 3300, NULL),
    F(CAL,     "MIN_OK",      16, U16, 0, 3300, NULL),
    F(CAL,     "MAX_OK",      18, U16, 0, 3300, NULL),
};
#undef F
const int HG_FIELD_COUNT = (int)(sizeof HG_FIELDS / sizeof HG_FIELDS[0]);

static int ci_eq(const char *a, const char *b) {
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'a' && ca <= 'z') ca -= 32;
        if (cb >= 'a' && cb <= 'z') cb -= 32;
        if (ca != cb) return 0;
        a++; b++;
    }
    return *a == *b;
}

int hg_group_find(const char *name) {
    for (int g = 0; g < HG_G_COUNT; g++)
        if (ci_eq(name, HG_GROUP_NAMES[g])) return g;
    return -1;
}

int hg_group_scope(uint8_t group) {
    if (group == HG_G_ZONECFG || group == HG_G_HW) return 0;
    if (group == HG_G_AUX) return 2;
    return 1;
}

static void *group_base(uint8_t group, int idx, const hg_zone_hw_t *hw, const hg_zone_cfg_t *cfg) {
    int scope = hg_group_scope(group);
    if (scope == 1 && (idx < 0 || idx >= HG_MAX_SHELVES)) return NULL;
    if (scope == 2 && (idx < 0 || idx >= HG_MAX_AUX)) return NULL;
    switch (group) {
    case HG_G_ZONECFG: return (void *)cfg;
    case HG_G_SHELF:   return (void *)&cfg->shelf[idx];
    case HG_G_LIGHT:   return (void *)&cfg->shelf[idx].light;
    case HG_G_WATER:   return (void *)&cfg->shelf[idx].water;
    case HG_G_FAN:     return (void *)&cfg->shelf[idx].fan;
    case HG_G_VIB:     return (void *)&cfg->shelf[idx].vib;
    case HG_G_AUX:     return (void *)&cfg->aux[idx];
    case HG_G_HW:      return (void *)hw;
    case HG_G_HWSHELF: return (void *)&hw->shelf[idx];
    case HG_G_CAL:     return (void *)&hw->shelf[idx];
    default:           return NULL;
    }
}

static const hg_field_t *find_row(uint8_t group, const char *key) {
    for (int i = 0; i < HG_FIELD_COUNT; i++)
        if (HG_FIELDS[i].group == group && ci_eq(HG_FIELDS[i].key, key)) return &HG_FIELDS[i];
    return NULL;
}

static int parse_int(const char *s, long *out) {
    char *end;
    long v = strtol(s, &end, 10);
    if (end == s || *end != '\0') return -1;
    *out = v;
    return 0;
}

int hg_hhmm_parse(const char *s) {
    const char *colon = strchr(s, ':');
    long v;
    if (!colon) {
        if (parse_int(s, &v) != 0 || v < 0 || v > 1439) return -1;
        return (int)v;
    }
    char hbuf[4];
    size_t hl = (size_t)(colon - s);
    if (hl < 1 || hl > 2 || strlen(colon + 1) != 2) return -1;
    memcpy(hbuf, s, hl); hbuf[hl] = '\0';
    long h, m;
    if (parse_int(hbuf, &h) != 0 || parse_int(colon + 1, &m) != 0) return -1;
    if (h < 0 || h > 23 || m < 0 || m > 59) return -1;
    return (int)(h * 60 + m);
}

void hg_hhmm_format(int minutes, char out[6]) {
    snprintf(out, 6, "%02d:%02d", minutes / 60, minutes % 60);
}

static int enum_parse(const char *enums, const char *val, long *out) {
    long idx = 0;
    const char *p = enums;
    while (*p) {
        const char *bar = strchr(p, '|');
        size_t n = bar ? (size_t)(bar - p) : strlen(p);
        char name[16];
        if (n < sizeof name) {
            memcpy(name, p, n); name[n] = '\0';
            if (ci_eq(name, val)) { *out = idx; return 0; }
        }
        if (!bar) break;
        p = bar + 1; idx++;
    }
    if (parse_int(val, out) == 0 && *out >= 0 && *out <= idx) return 0; /* numeric form */
    return -1;
}

static const char *enum_name(const char *enums, long idx, char *buf, size_t cap) {
    const char *p = enums;
    while (idx-- > 0) { p = strchr(p, '|'); if (!p) return "?"; p++; }
    const char *bar = strchr(p, '|');
    size_t n = bar ? (size_t)(bar - p) : strlen(p);
    if (n >= cap) n = cap - 1;
    memcpy(buf, p, n); buf[n] = '\0';
    return buf;
}

static int str_ok(const char *s) {
    size_t n = strlen(s);
    if (n > 15) return 0;
    for (size_t i = 0; i < n; i++)
        if (s[i] <= 0x20 || s[i] > 0x7E) return 0;
    return 1;
}

int hg_field_set_text(hg_zone_hw_t *hw, hg_zone_cfg_t *cfg, uint8_t group, int idx,
                      const char *key, const char *val) {
    const hg_field_t *f = find_row(group, key);
    if (!f) return -3;
    uint8_t *base = (uint8_t *)group_base(group, idx, hw, cfg);
    if (!base) return -4;
    uint8_t *dst = base + f->offset;
    long v;
    switch (f->type) {
    case HG_T_STR16:
        if (!str_ok(val)) return -1;
        memset(dst, 0, 16);
        strcpy((char *)dst, val);
        return 0;
    case HG_T_BOOL:
        if (ci_eq(val, "ON") || ci_eq(val, "ENABLE") || ci_eq(val, "1")) v = 1;
        else if (ci_eq(val, "OFF") || ci_eq(val, "DISABLE") || ci_eq(val, "0")) v = 0;
        else return -1;
        *dst = (uint8_t)v;
        return 0;
    case HG_T_ENUM:
        if (enum_parse(f->enums, val, &v) != 0) return -1;
        *dst = (uint8_t)v;
        return 0;
    case HG_T_HHMM: {
        int m = hg_hhmm_parse(val);
        if (m < 0) return -1;
        uint16_t u = (uint16_t)m;
        memcpy(dst, &u, 2);
        return 0;
    }
    case HG_T_PIN:
        if (ci_eq(val, "NONE")) { *dst = HG_NONE; return 0; }
        if (parse_int(val, &v) != 0) return -1;
        if (v < f->min || v > f->max) return -2;
        *dst = (uint8_t)v;
        return 0;
    case HG_T_U8:
        if (parse_int(val, &v) != 0) return -1;
        if (v < f->min || v > f->max) return -2;
        *dst = (uint8_t)v;
        return 0;
    case HG_T_U16: {
        if (parse_int(val, &v) != 0) return -1;
        if (v < f->min || v > f->max) return -2;
        uint16_t u = (uint16_t)v;
        memcpy(dst, &u, 2);
        return 0;
    }
    default:
        return -3;
    }
}

int hg_field_get_text(const hg_zone_hw_t *hw, const hg_zone_cfg_t *cfg, uint8_t group, int idx,
                      const char *key, char *out, size_t cap) {
    const hg_field_t *f = find_row(group, key);
    if (!f) return -3;
    const uint8_t *base = (const uint8_t *)group_base(group, idx, hw, cfg);
    if (!base) return -4;
    const uint8_t *src = base + f->offset;
    uint16_t u16;
    switch (f->type) {
    case HG_T_STR16: snprintf(out, cap, "%s", (const char *)src); return 0;
    case HG_T_BOOL:  snprintf(out, cap, "%u", *src); return 0;
    case HG_T_ENUM: { char b[16]; snprintf(out, cap, "%s", enum_name(f->enums, *src, b, sizeof b)); return 0; }
    case HG_T_HHMM:  memcpy(&u16, src, 2); if (cap >= 6) hg_hhmm_format(u16, out); return 0;
    case HG_T_PIN:
        if (*src == HG_NONE) snprintf(out, cap, "NONE");
        else snprintf(out, cap, "%u", *src);
        return 0;
    case HG_T_U8:    snprintf(out, cap, "%u", *src); return 0;
    case HG_T_U16:   memcpy(&u16, src, 2); snprintf(out, cap, "%u", u16); return 0;
    default:         return -3;
    }
}
```

- [ ] **Step 4: Implement `components/hg_cfg/hg_cfg_validate.c`**

```c
#include <stdio.h>
#include <string.h>
#include "hg_cfg.h"

/* err path: "<scopePrefix><group>.<key>" lowercased, e.g. "shelf[2].water.dose_s" */
static int fail(char *err, size_t n, int shelf, uint8_t group, const char *key) {
    char lg[12], lk[24];
    size_t i;
    for (i = 0; HG_GROUP_NAMES[group][i] && i < 11; i++) {
        char c = HG_GROUP_NAMES[group][i];
        lg[i] = (char)(c >= 'A' && c <= 'Z' ? c + 32 : c);
    }
    lg[i] = '\0';
    for (i = 0; key[i] && i < 23; i++) {
        char c = key[i];
        lk[i] = (char)(c >= 'A' && c <= 'Z' ? c + 32 : c);
    }
    lk[i] = '\0';
    if (shelf >= 0) snprintf(err, n, "shelf[%d].%s.%s", shelf, lg, lk);
    else if (group == HG_G_ZONECFG || group == HG_G_HW) snprintf(err, n, "%s.%s", lg, lk);
    else snprintf(err, n, "%s.%s", lg, lk);
    return -1;
}

static int str16_ok(const char *s) {
    for (int i = 0; i < 16 && s[i]; i++)
        if (s[i] < 0x20 || s[i] > 0x7E) return 0;
    return s[15] == '\0' || 1; /* NUL guaranteed by writer; loaded blobs zero-filled */
}

static long field_raw(const void *base, const hg_field_t *f) {
    const uint8_t *p = (const uint8_t *)base + f->offset;
    if (f->type == HG_T_U16 || f->type == HG_T_HHMM) {
        uint16_t u; memcpy(&u, p, 2); return (long)u;
    }
    return (long)*p;
}

/* range-check every table row of `group` against `base`; PIN allows HG_NONE */
static int check_group_rows(const void *base, uint8_t group, int shelf, char *err, size_t n) {
    for (int i = 0; i < HG_FIELD_COUNT; i++) {
        const hg_field_t *f = &HG_FIELDS[i];
        if (f->group != group || f->type == HG_T_STR16) continue;
        long v = field_raw(base, f);
        if (f->type == HG_T_PIN && v == HG_NONE) continue;
        long hi = (f->type == HG_T_HHMM) ? 1439 : f->max;
        long lo = (f->type == HG_T_HHMM) ? 0 : f->min;
        if (v < lo || v > hi) return fail(err, n, shelf, group, f->key);
    }
    return 0;
}

static int mark_dup(uint8_t used[16], uint8_t pin) {
    if (pin == HG_NONE) return 0;
    if (pin > 15 || used[pin]) return -1;
    used[pin] = 1;
    return 0;
}

int hg_hw_validate(const hg_zone_hw_t *hw, char *err, size_t n) {
    if (check_group_rows(hw, HG_G_HW, -1, err, n) != 0) return -1;
    uint8_t pca[16] = {0}, pcf[16] = {0}, soil[8] = {0};
    for (int s = 0; s < hw->shelf_count && s < HG_MAX_SHELVES; s++) {
        const hg_shelf_hw_t *sh = &hw->shelf[s];
        if (check_group_rows(sh, HG_G_HWSHELF, s, err, n) != 0) return -1;
        if (check_group_rows(sh, HG_G_CAL, s, err, n) != 0) return -1;
        if (mark_dup(pca, sh->led_ch[0]) != 0) return fail(err, n, s, HG_G_HWSHELF, "LED_W");
        if (mark_dup(pca, sh->led_ch[1]) != 0) return fail(err, n, s, HG_G_HWSHELF, "LED_R");
        if (mark_dup(pca, sh->vib_ch) != 0) return fail(err, n, s, HG_G_HWSHELF, "VIB");
        if (mark_dup(pcf, sh->pump_pin) != 0) return fail(err, n, s, HG_G_HWSHELF, "PUMP");
        if (mark_dup(pcf, sh->fan_pin) != 0) return fail(err, n, s, HG_G_HWSHELF, "FAN");
        if (sh->soil_ch[0] != HG_NONE && (sh->soil_ch[0] > 7 || soil[sh->soil_ch[0]]++))
            return fail(err, n, s, HG_G_HWSHELF, "SOIL_A");
        if (sh->soil_ch[1] != HG_NONE && (sh->soil_ch[1] > 7 || soil[sh->soil_ch[1]]++))
            return fail(err, n, s, HG_G_HWSHELF, "SOIL_B");
        if (sh->soil_min_ok_mv >= sh->soil_max_ok_mv) return fail(err, n, s, HG_G_CAL, "MIN_OK");
        for (int p = 0; p < 2; p++)
            if (sh->soil_dry_mv[p] <= sh->soil_wet_mv[p])
                return fail(err, n, s, HG_G_CAL, p == 0 ? "DRY_A" : "DRY_B");
    }
    for (int a = 0; a < HG_MAX_AUX; a++)
        if (hw->aux[a].type != 0 && mark_dup(pcf, hw->aux[a].pin) != 0)
            return fail(err, n, -1, HG_G_HW, "AUX_PIN");
    return 0;
}

int hg_cfg_validate(const hg_zone_cfg_t *cfg, const hg_zone_hw_t *hw, char *err, size_t n) {
    if (!str16_ok(cfg->name) || cfg->name[0] == '\0') return fail(err, n, -1, HG_G_ZONECFG, "NAME");
    if (check_group_rows(cfg, HG_G_ZONECFG, -1, err, n) != 0) return -1;
    for (int a = 0; a < HG_MAX_AUX; a++)
        if (cfg->aux[a].mode != 0 && check_group_rows(&cfg->aux[a], HG_G_AUX, -1, err, n) != 0) return -1;
    for (int s = 0; s < HG_MAX_SHELVES; s++) {
        const hg_shelf_cfg_t *sc = &cfg->shelf[s];
        if (!str16_ok(sc->crop)) return fail(err, n, s, HG_G_SHELF, "CROP");
        if (check_group_rows(sc, HG_G_SHELF, s, err, n) != 0) return -1;
        if (check_group_rows(&sc->light, HG_G_LIGHT, s, err, n) != 0) return -1;
        if (check_group_rows(&sc->water, HG_G_WATER, s, err, n) != 0) return -1;
        if (check_group_rows(&sc->fan, HG_G_FAN, s, err, n) != 0) return -1;
        if (check_group_rows(&sc->vib, HG_G_VIB, s, err, n) != 0) return -1;
        if (sc->light.on_min == sc->light.off_min) return fail(err, n, s, HG_G_LIGHT, "OFF");
        if (hw) {
            if (sc->enabled && s >= hw->shelf_count) return fail(err, n, s, HG_G_SHELF, "ENABLED");
            const hg_shelf_hw_t *sh = &hw->shelf[s];
            if (sc->water.dose_s > sh->pump_max_run_s) return fail(err, n, s, HG_G_WATER, "DOSE_S");
            if ((uint32_t)sc->water.max_doses_day * sc->water.dose_s > sh->pump_max_daily_s)
                return fail(err, n, s, HG_G_WATER, "MAX_DOSES");
        }
    }
    return 0;
}
```

Note the shelf-scoped `fail()` path for SHELF/ENABLED prints `shelf[3].shelf.enabled` — the test expects `shelf[3].enabled`. Add this special case to `fail()`: when `group == HG_G_SHELF`, print `shelf[%d].%s` (key only, no group segment).

- [ ] **Step 5: Run — expect PASS**

Run: `cmake --build build/host --config Release --parallel && ctest --test-dir build/host -C Release --output-on-failure`
Expected: 5/5 tests pass.

- [ ] **Step 6: Commit**

```bash
git add components/hg_cfg tests/host
git commit -m "feat(hg_cfg): field table, text conversion, hw+cfg validation with paths

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
git push
```

---

### Task 5: `cli_line` — byte-fed terminal line editor

**Files:**
- Create: `components/cli_line/cli_line.h`, `components/cli_line/cli_line.c`, `components/cli_line/CMakeLists.txt` (`idf_component_register(SRCS "cli_line.c" INCLUDE_DIRS ".")`)
- Modify: `tests/host/CMakeLists.txt` (`hg_test(test_cli_line ${COMP}/cli_line/cli_line.c)`)
- Test: `tests/host/test_cli_line.c`

**Interfaces:**
- Consumes: `fake_clock_now` in tests.
- Produces (used by the `cli` task, Task 12):

```c
#define CLI_LINE_MAX 192          /* 191 chars + NUL */
#define CLI_HIST_N   10
#define CLI_HIST_LEN 96
typedef enum { CLI_EVT_NONE = 0, CLI_EVT_LINE, CLI_EVT_TOO_LONG } cli_evt_t;
typedef struct { /* all state internal; ~1.2 KB */ } cli_line_t;
void        cli_line_init(cli_line_t *l, int echo);
void        cli_line_set_echo(cli_line_t *l, int echo);          /* echo=1 human mode, 0 machine mode */
cli_evt_t   cli_line_feed(cli_line_t *l, uint8_t b, uint32_t now_ms,
                          char *echo_out, size_t echo_cap, size_t *echo_len);
const char *cli_line_take(cli_line_t *l);                        /* valid after CLI_EVT_LINE; clears the buffer */
uint32_t    cli_line_noise(const cli_line_t *l);
uint32_t    cli_line_stale(const cli_line_t *l);
```

Behaviour (spec §5.7): printable 0x20–0x7E appends (echoes in human mode); at 191 chars sets overflow — the terminating CR/LF then returns `CLI_EVT_TOO_LONG` with the buffer discarded. `\r` or `\n` ends a line (an LF directly after a CR is swallowed); empty line = `CLI_EVT_LINE` with `""` (caller answers `ERR EMPTY`). 0x08/0x7F erases one char, echoing `\b \b`. Ctrl-C (0x03) clears the line, echoing `^C\r\n`. Human mode only: `ESC [ A/B` walks a 10×96 duplicate-suppressed history — the echo output is `\r\x1b[2K` followed by the replacement line; the in-progress line is saved and restored past the newest entry. Machine mode: no echo, ESC bytes count as noise, a partial line idle > 30 000 ms is discarded (`stale++`) when the next byte arrives. 0x00/0xFF/other control bytes are dropped and counted (`noise++`).

- [ ] **Step 1: Header + stub (`cli_line_feed` returns `CLI_EVT_NONE` always) + test**

Create `tests/host/test_cli_line.c`:

```c
#include <string.h>
#include "unity.h"
#include "cli_line.h"
#include "fake_clock.h"

static cli_line_t l;
static char eo[256];
static size_t eol;
void setUp(void) { cli_line_init(&l, 1); fake_clock_set(1000); }
void tearDown(void) {}

static cli_evt_t feed(const char *s) {
    cli_evt_t e = CLI_EVT_NONE;
    while (*s) e = cli_line_feed(&l, (uint8_t)*s++, fake_clock_now(), eo, sizeof eo, &eol);
    return e;
}

static void test_basic_line_and_backspace(void) {
    feed("ab");
    cli_line_feed(&l, 0x7F, fake_clock_now(), eo, sizeof eo, &eol);
    TEST_ASSERT_EQUAL_size_t(3, eol);                       /* "\b \b" */
    TEST_ASSERT_EQUAL_MEMORY("\b \b", eo, 3);
    TEST_ASSERT_EQUAL_INT(CLI_EVT_LINE, feed("c\r"));
    TEST_ASSERT_EQUAL_STRING("ac", cli_line_take(&l));
}

static void test_crlf_swallow_and_lf_alone(void) {
    TEST_ASSERT_EQUAL_INT(CLI_EVT_LINE, feed("X\r"));
    TEST_ASSERT_EQUAL_STRING("X", cli_line_take(&l));
    TEST_ASSERT_EQUAL_INT(CLI_EVT_NONE, feed("\n"));        /* LF after CR swallowed */
    TEST_ASSERT_EQUAL_INT(CLI_EVT_LINE, feed("Y\n"));
    TEST_ASSERT_EQUAL_STRING("Y", cli_line_take(&l));
    TEST_ASSERT_EQUAL_INT(CLI_EVT_LINE, feed("\r"));        /* empty line */
    TEST_ASSERT_EQUAL_STRING("", cli_line_take(&l));
}

static void test_history_up_down(void) {
    feed("AAA\r"); cli_line_take(&l);
    feed("BBB\r"); cli_line_take(&l);
    feed("BBB\r"); cli_line_take(&l);                        /* duplicate not stored twice */
    feed("cc");                                              /* partial, then browse */
    feed("\x1b[A");
    TEST_ASSERT_EQUAL_STRING("BBB", l_buf_peek());          /* helper below */
    feed("\x1b[A");
    TEST_ASSERT_EQUAL_STRING("AAA", l_buf_peek());
    feed("\x1b[A");                                          /* at oldest, stays */
    TEST_ASSERT_EQUAL_STRING("AAA", l_buf_peek());
    feed("\x1b[B");
    TEST_ASSERT_EQUAL_STRING("BBB", l_buf_peek());
    feed("\x1b[B");                                          /* past newest -> saved partial */
    TEST_ASSERT_EQUAL_STRING("cc", l_buf_peek());
}

static void test_too_long_and_noise(void) {
    for (int i = 0; i < 300; i++) cli_line_feed(&l, 'z', fake_clock_now(), eo, sizeof eo, &eol);
    TEST_ASSERT_EQUAL_INT(CLI_EVT_TOO_LONG, feed("\r"));
    TEST_ASSERT_EQUAL_INT(CLI_EVT_LINE, feed("Q\r"));
    TEST_ASSERT_EQUAL_STRING("Q", cli_line_take(&l));
    uint8_t junk[3] = { 0x00, 0xFF, 0x07 };
    for (int i = 0; i < 3; i++) cli_line_feed(&l, junk[i], fake_clock_now(), eo, sizeof eo, &eol);
    TEST_ASSERT_EQUAL_UINT32(3, cli_line_noise(&l));
}

static void test_machine_mode_stale_and_esc_noise(void) {
    cli_line_init(&l, 0);
    feed("par");
    fake_clock_add(30001);
    TEST_ASSERT_EQUAL_INT(CLI_EVT_LINE, feed("Z\r"));        /* stale partial dropped first */
    TEST_ASSERT_EQUAL_STRING("Z", cli_line_take(&l));
    TEST_ASSERT_EQUAL_UINT32(1, cli_line_stale(&l));
    feed("\x1b");
    TEST_ASSERT_EQUAL_UINT32(1, cli_line_noise(&l));
}

static void test_ctrl_c(void) {
    feed("junk");
    cli_line_feed(&l, 0x03, fake_clock_now(), eo, sizeof eo, &eol);
    TEST_ASSERT_EQUAL_MEMORY("^C\r\n", eo, 4);
    TEST_ASSERT_EQUAL_INT(CLI_EVT_LINE, feed("k\r"));
    TEST_ASSERT_EQUAL_STRING("k", cli_line_take(&l));
}

int main(void) { UNITY_BEGIN();
    RUN_TEST(test_basic_line_and_backspace);
    RUN_TEST(test_crlf_swallow_and_lf_alone);
    RUN_TEST(test_history_up_down);
    RUN_TEST(test_too_long_and_noise);
    RUN_TEST(test_machine_mode_stale_and_esc_noise);
    RUN_TEST(test_ctrl_c);
    return UNITY_END(); }
```

`l_buf_peek()` is a 3-line test helper: add `const char *cli_line_peek(const cli_line_t *l);` to the public header (returns the current edit buffer, NUL-terminated) and `#define l_buf_peek() cli_line_peek(&l)` in the test.

- [ ] **Step 2: Run — expect FAIL** (`ctest … -R test_cli_line`).

- [ ] **Step 3: Implement `components/cli_line/cli_line.c`**

Complete implementation guidance (write it out fully — target ≤ 220 lines): struct fields `char buf[CLI_LINE_MAX]; uint16_t len; uint8_t echo, esc, overflow, last_cr, browsing; char hist[CLI_HIST_N][CLI_HIST_LEN]; uint8_t hist_count, hist_next; int hist_pos; char saved[CLI_LINE_MAX]; uint16_t saved_len; uint32_t last_ms, noise, stale;`. `feed()` order: (machine mode) stale check first — `len>0 && now−last_ms>30000 → len=0, stale++`; update `last_ms`; handle `esc` state machine (`0x1B→esc=1; esc==1 && '['→esc=2 else esc=0+noise; esc==2 && 'A'/'B'→history, else esc=0`); then CR/LF (set `last_cr`, `overflow→TOO_LONG reset`, else terminate + push history + `CLI_EVT_LINE`; echo `"\r\n"`); LF-after-CR swallow; 0x03; 0x08/0x7F; printable append (if `len==CLI_LINE_MAX−1 → overflow=1` else append + echo); everything else `noise++`. History push: skip empty, skip if equal to newest entry, copy first `CLI_HIST_LEN−1` chars; ring via `hist_next`. Browsing: first `ESC[A` saves `buf/len` into `saved`; `hist_pos` walks newest→oldest, clamped; `ESC[B` walks back and past newest restores `saved`; any printable/CR resets `browsing=0`. History replacement echo (human mode only): `"\r\x1b[2K"` + buffer content, bounded by `echo_cap`. `cli_line_take` NUL-terminates, resets `len/browsing/overflow`, returns `buf`.

- [ ] **Step 4: Run — expect PASS** (all 6 tests).

- [ ] **Step 5: Commit**

```bash
git add components/cli_line tests/host
git commit -m "feat(cli_line): pure byte-fed line editor with history and machine mode

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
git push
```

---

### Task 6: `notify` — formatter, rate limits, sinks

**Files:**
- Create: `components/notify/notify.h`, `components/notify/notify.c`, `components/notify/CMakeLists.txt`
- Modify: `tests/host/CMakeLists.txt` (`hg_test(test_notify ${COMP}/notify/notify.c)`)
- Test: `tests/host/test_notify.c`

**Interfaces:**
- Consumes: injected `now_ms` function pointer (tests: `fake_clock_now`).
- Produces (used by cmd_common, cli, and every later controller):

```c
typedef enum { NTF_BOOT, NTF_ALARM, NTF_SAFE, NTF_NODE, NTF_RING,
               NTF_WATER, NTF_LIGHT, NTF_SOIL, NTF_FW, NTF_CMD, NTF_COUNT } ntf_type_t;
#define NTF_LINE_MAX  128
#define NTF_MAX_SINKS 3
#define NTF_MASK(t)   (uint16_t)(1u << (t))
#define NTF_MASK_ALL  ((uint16_t)((1u << NTF_COUNT) - 1))
/* CLI default per spec §5.5: everything ON except LIGHT and SOIL */
#define NTF_DEFAULT_CLI_MASK (uint16_t)(NTF_MASK_ALL & ~(NTF_MASK(NTF_LIGHT) | NTF_MASK(NTF_SOIL)))
typedef void (*ntf_sink_fn)(void *ctx, const char *line);   /* line ends with '\n' */
void        notify_init(uint32_t (*now_ms)(void), uint8_t node_id);
void        notify_set_node_id(uint8_t id);
int         notify_add_sink(ntf_sink_fn fn, void *ctx, uint16_t mask);  /* index or -1 */
void        notify_set_sink_mask(int sink, uint16_t mask);
uint16_t    notify_sink_mask(int sink);
int         notify_parse(const char *name);   /* type index; NTF_COUNT for "ALL"; -1 unknown */
const char *notify_type_name(int t);
void        notify_emit(ntf_type_t t, uint8_t idx, const char *fmt, ...);
```

Line format: `NOTIFY <TYPE> <node> <payload>\n`, truncated to `NTF_LINE_MAX` (newline preserved). Minimum interval per `(type, idx≤7)`: BOOT once per boot; ALARM 1000 ms per idx; RING 2000 ms; LIGHT 1000 ms; SOIL 60 000 ms; CMD 200 ms; SAFE/NODE/WATER/FW 0 (edge events, the emitter is edge-triggered).

- [ ] **Step 1: Header + stub + test.** Test asserts (using a capture sink appending lines to an array): exact line `NOTIFY WATER 2 1 START 15 MANUAL\n` when node_id 2 and `notify_emit(NTF_WATER, 1, "%d START %d MANUAL", 1, 15)`; SOIL emit for shelf 1 twice within 60 s → second suppressed while shelf 2 passes; ALARM passes again only after `fake_clock_add(1000)`; a sink whose mask lacks WATER gets nothing while a second sink gets the line; `notify_parse("all")==NTF_COUNT`, `notify_parse("soil")==NTF_SOIL`, `notify_parse("nope")==-1`; a 200-char payload arrives truncated to `NTF_LINE_MAX` ending in `\n`; BOOT emits once, a second BOOT is suppressed.

- [ ] **Step 2: Run — expect FAIL.**

- [ ] **Step 3: Implement `notify.c`** (~110 lines): static `now_fn`, `node_id`, `sinks[NTF_MAX_SINKS]`, `last_ms[NTF_COUNT][8]` initialised to a sentinel meaning "never"; interval table `{UINT32_MAX,1000,0,0,2000,0,1000,60000,0,200}` indexed by type (BOOT uses the sentinel check to mean once); `notify_emit` formats the payload with `vsnprintf` into a local `NTF_LINE_MAX` buffer prefixed by `NOTIFY <name> <node> `, enforces the interval, then calls every sink whose mask has the bit. Name table `{"BOOT","ALARM","SAFE","NODE","RING","WATER","LIGHT","SOIL","FW","CMD"}`; `notify_parse` case-insensitive + `"ALL"`.

- [ ] **Step 4: Run — expect PASS.**

- [ ] **Step 5: Commit**

```bash
git add components/notify tests/host
git commit -m "feat(notify): NOTIFY formatter with per-(type,idx) rate limits and masked sinks

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
git push
```

---

### Task 7: `cmd_core` — tokenizer, response helpers, table dispatch

**Files:**
- Create: `components/cmd_core/cmd_core.h`, `components/cmd_core/cmd_tok.c`, `components/cmd_core/cmd_resp.c`, `components/cmd_core/cmd_dispatch.c`, `components/cmd_core/CMakeLists.txt` (`SRCS "cmd_tok.c" "cmd_resp.c" "cmd_dispatch.c" "cmd_help.c"` — create an empty `cmd_help.c` placeholder for Task 8)
- Modify: `tests/host/CMakeLists.txt` (`set(CMD_CORE_SRC ${COMP}/cmd_core/cmd_tok.c ${COMP}/cmd_core/cmd_resp.c ${COMP}/cmd_core/cmd_dispatch.c ${COMP}/cmd_core/cmd_help.c)` + `hg_test(test_cmd_core ${CMD_CORE_SRC})`)
- Test: `tests/host/test_cmd_core.c`

**Interfaces:**
- Consumes: nothing (self-contained pure C).
- Produces — `components/cmd_core/cmd_core.h`, the exact contract every command table in the repo compiles against. Create it verbatim:

```c
#pragma once
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

#define CMD_LINE_MAX   192
#define CMD_MAX_TOKENS 12
#define CMD_MAX_ARGS   8
#define CMD_RESP_MAX   4096
#define CMD_UNLOCK_MS  600000u

typedef enum { CMD_SRC_CLI = 0, CMD_SRC_HTTP, CMD_SRC_RING, CMD_SRC_INTERNAL } cmd_src_t;
typedef enum { ARG_INT = 0, ARG_ENUM, ARG_STR, ARG_TIME, ARG_MAC } cmd_arg_type_t;
typedef struct { const char *name; uint8_t type; int32_t min, max; const char *enums; } cmd_arg_t;

#define CMDV_SET  0x01
#define CMDV_GET  0x02
#define CMDV_BARE 0x04

#define CMDF_ACTUATOR 0x0001  /* audited; must carry a bounded duration arg */
#define CMDF_UNLOCK   0x0002  /* needs DEBUG ENABLE on this session */
#define CMDF_MASTER   0x0004
#define CMDF_ZONE     0x0008
#define CMDF_SESSION  0x0010  /* acts on the calling session; ERR NOT_LOCAL from HTTP */
#define CMDF_SLOW     0x0020  /* handler budget 1000 ms instead of 50 ms */

enum { CMD_AREA_SYSTEM = 0, CMD_AREA_CONFIG, CMD_AREA_SESSION, CMD_AREA_TIME, CMD_AREA_FW,
       CMD_AREA_RING, CMD_AREA_SHELF, CMD_AREA_SENSOR, CMD_AREA_FAULT, CMD_AREA_NET,
       CMD_AREA_DEBUG, CMD_AREA_COUNT };

#define CMD_ROLE_MASTER 0
#define CMD_ROLE_ZONE   1

typedef struct { cmd_src_t source; uint8_t echo; uint16_t notify_mask; uint32_t unlock_until_ms; } cmd_session_t;

typedef struct cmd_core cmd_core_t;
typedef struct cmd_entry cmd_entry_t;

typedef struct {
    const cmd_core_t  *core;
    cmd_session_t     *ses;
    const cmd_entry_t *e;
    uint8_t  verb;                       /* the CMDV_* bit actually used */
    uint8_t  n;                          /* args given */
    const char *tok[CMD_MAX_ARGS];       /* raw arg tokens, case preserved */
    int32_t  val[CMD_MAX_ARGS];          /* INT value / ENUM index / TIME minutes; 0 for STR/MAC */
    uint8_t  mac[6];                     /* last ARG_MAC parsed */
} cmd_req_t;

typedef int (*cmd_handler_fn)(cmd_req_t *q, char *resp, int len);   /* 0 OK / -1 ERR; must write one reply */

struct cmd_entry {
    uint8_t         verbs, area;
    const char     *noun1, *noun2;       /* noun2 NULL for one-word nouns */
    const cmd_arg_t *args;               /* max_args entries */
    uint8_t         n_key;               /* leading args GET also takes */
    uint8_t         min_args, max_args;  /* for SET / BARE */
    uint16_t        flags;
    cmd_handler_fn  handler;
    const char     *desc;                /* <= 40 chars or NULL */
};

/* 0 = forwarded OK (resp holds the zone's reply line); -1 = resp holds an ERR line */
typedef int (*cmd_forward_fn)(void *ctx, uint8_t zone, const char *line, char *resp, int len);

struct cmd_core {
    const cmd_entry_t *table;
    int                table_len;
    uint8_t            role;             /* CMD_ROLE_* */
    uint8_t            zone_id;          /* own ring id; 0 on master/unassigned */
    uint32_t         (*now_ms)(void);
    cmd_forward_fn     forward;          /* master only; NULL elsewhere */
    void              *forward_ctx;
    void             (*audit)(void *ctx, cmd_src_t src, const char *line);
    void              *audit_ctx;
    const char        *debug_key;        /* for cmd_common's DEBUG ENABLE row */
};

int cmd_dispatch(const cmd_core_t *core, cmd_session_t *ses, const char *line, char *resp, int resp_len);
int cmd_help(const cmd_core_t *core, cmd_session_t *ses, const char *const *tok, int ntok, char *resp, int len);
int cmd_table_check(const cmd_entry_t *t, int n);   /* -1 ok, else index of first bad row */

int cmd_err(char *resp, int len, const char *token);            /* writes "ERR <token>\n", returns -1 */
int cmd_okf(char *resp, int len, const char *fmt, ...);         /* writes "OK " + fmt + "\n", returns 0 */
int cmd_linef(char *resp, int len, const char *fmt, ...);       /* appends a line (help/continuations), 0/-1 full */
int cmd_ci_eq(const char *a, const char *b);
int cmd_tokenize(char *line, const char *tok[], int max);       /* in place; count or -1 if > max */
int cmd_parse_time(const char *s);                              /* "H:MM"/"HH:MM"/minutes -> 0..1439, -1 bad */

#ifdef __cplusplus
}
#endif
```

- [ ] **Step 1: Write `cmd_tok.c`, `cmd_resp.c` (final code below), a `cmd_dispatch.c` stub returning `cmd_err(resp,len,"NOT_IMPLEMENTED")`, an empty `cmd_help.c` with a stub `cmd_help`/`cmd_table_check`, and the test**

`components/cmd_core/cmd_tok.c` (final):

```c
#include <stdlib.h>
#include <string.h>
#include "cmd_core.h"

int cmd_ci_eq(const char *a, const char *b) {
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'a' && ca <= 'z') ca -= 32;
        if (cb >= 'a' && cb <= 'z') cb -= 32;
        if (ca != cb) return 0;
        a++; b++;
    }
    return *a == *b;
}

int cmd_tokenize(char *line, const char *tok[], int max) {
    int n = 0;
    char *p = line;
    for (;;) {
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0') return n;
        if (n == max) return -1;
        tok[n++] = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) *p++ = '\0';
    }
}

int cmd_parse_time(const char *s) {
    const char *colon = strchr(s, ':');
    char *end;
    if (!colon) {
        long v = strtol(s, &end, 10);
        if (end == s || *end || v < 0 || v > 1439) return -1;
        return (int)v;
    }
    if (colon == s || strlen(colon + 1) != 2) return -1;
    long h = strtol(s, &end, 10);
    if (end != colon || h < 0 || h > 23) return -1;
    long m = strtol(colon + 1, &end, 10);
    if (*end || m < 0 || m > 59) return -1;
    return (int)(h * 60 + m);
}
```

`components/cmd_core/cmd_resp.c` (final):

```c
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "cmd_core.h"

int cmd_err(char *resp, int len, const char *token) {
    snprintf(resp, (size_t)len, "ERR %s\n", token);
    return -1;
}

int cmd_okf(char *resp, int len, const char *fmt, ...) {
    va_list ap;
    int off = snprintf(resp, (size_t)len, "OK ");
    va_start(ap, fmt);
    off += vsnprintf(resp + off, (size_t)(len - off), fmt, ap);
    va_end(ap);
    if (off < len - 1) { resp[off] = '\n'; resp[off + 1] = '\0'; }
    else { resp[len - 2] = '\n'; resp[len - 1] = '\0'; }
    return 0;
}

int cmd_linef(char *resp, int len, const char *fmt, ...) {
    va_list ap;
    size_t off = strlen(resp);
    if ((int)off >= len - 2) return -1;
    va_start(ap, fmt);
    int w = vsnprintf(resp + off, (size_t)(len - (int)off), fmt, ap);
    va_end(ap);
    off += (size_t)(w > 0 ? w : 0);
    if ((int)off < len - 1) { resp[off] = '\n'; resp[off + 1] = '\0'; return 0; }
    resp[len - 2] = '\n'; resp[len - 1] = '\0';
    return -1;
}
```

`tests/host/test_cmd_core.c` — complete fixture and tests:

```c
#include <string.h>
#include "unity.h"
#include "cmd_core.h"
#include "fake_clock.h"

/* ---- fixture table ---- */
static int h_calls, audit_calls, fwd_calls;
static char fwd_line[CMD_LINE_MAX]; static uint8_t fwd_zone;

static int h_light(cmd_req_t *q, char *r, int l) {
    h_calls++;
    if (q->verb == CMDV_GET) return cmd_okf(r, l, "LIGHT %d 70 45 MANUAL 60", (int)q->val[0]);
    return cmd_okf(r, l, "LIGHT %d %d %d MANUAL %d", (int)q->val[0], (int)q->val[1], (int)q->val[2], (int)q->val[3]);
}
static int h_name(cmd_req_t *q, char *r, int l) { return cmd_okf(r, l, "NODE %d %s", (int)q->val[0], q->tok[1]); }
static int h_mac(cmd_req_t *q, char *r, int l)  { return cmd_okf(r, l, "MAC %02X%02X", q->mac[0], q->mac[5]); }
static int h_when(cmd_req_t *q, char *r, int l) { return cmd_okf(r, l, "WHEN %d", (int)q->val[0]); }
static int h_reboot(cmd_req_t *q, char *r, int l){ (void)q; return cmd_okf(r, l, "REBOOT"); }
static int h_poke(cmd_req_t *q, char *r, int l) { (void)q; return cmd_okf(r, l, "POKE"); }
static int h_wifi(cmd_req_t *q, char *r, int l) { (void)q; return cmd_okf(r, l, "WIFI"); }
static int h_echo(cmd_req_t *q, char *r, int l) { (void)q; return cmd_okf(r, l, "ECHO"); }
static int h_silent(cmd_req_t *q, char *r, int l){ (void)q; (void)r; (void)l; return 0; }  /* writes nothing */

static const cmd_arg_t A_LIGHT[] = {{"shelf",ARG_INT,1,4,NULL},{"white",ARG_INT,0,100,NULL},
                                    {"red",ARG_INT,0,100,NULL},{"minutes",ARG_INT,1,720,NULL}};
static const cmd_arg_t A_NAME[]  = {{"zone",ARG_INT,1,8,NULL},{"name",ARG_STR,0,15,NULL}};
static const cmd_arg_t A_MAC[]   = {{"zone",ARG_INT,1,8,NULL},{"mac",ARG_MAC,0,0,NULL}};
static const cmd_arg_t A_WHEN[]  = {{"at",ARG_TIME,0,1439,NULL}};
static const cmd_arg_t A_CONF[]  = {{"confirm",ARG_ENUM,0,0,"CONFIRM"}};
static const cmd_arg_t A_MODE[]  = {{"mode",ARG_ENUM,0,1,"OFF|ON"}};

static const cmd_entry_t TBL[] = {
  { CMDV_SET|CMDV_GET, CMD_AREA_SHELF, "LIGHT", NULL, A_LIGHT, 1, 4, 4, CMDF_ACTUATOR|CMDF_ZONE, h_light, "manual override, then AUTO" },
  { CMDV_SET,          CMD_AREA_RING,  "NODE", "NAME", A_NAME, 1, 2, 2, CMDF_MASTER, h_name, NULL },
  { CMDV_SET,          CMD_AREA_RING,  "NODE", "MAC",  A_MAC,  1, 2, 2, CMDF_MASTER, h_mac,  NULL },
  { CMDV_SET,          CMD_AREA_TIME,  "WHEN", NULL,   A_WHEN, 0, 1, 1, 0, h_when, NULL },
  { CMDV_BARE,         CMD_AREA_SYSTEM,"REBOOT", NULL, A_CONF, 0, 1, 1, 0, h_reboot, "restart the node" },
  { CMDV_SET,          CMD_AREA_DEBUG, "POKE", NULL,   A_MODE, 0, 1, 1, CMDF_UNLOCK, h_poke, NULL },
  { CMDV_SET,          CMD_AREA_NET,   "WIFI", NULL,   A_MODE, 0, 1, 1, CMDF_MASTER, h_wifi, NULL },
  { CMDV_SET|CMDV_GET, CMD_AREA_SESSION,"ECHO", NULL,  A_MODE, 0, 1, 1, CMDF_SESSION, h_echo, NULL },
  { CMDV_GET,          CMD_AREA_SYSTEM,"SILENT", NULL, NULL,   0, 0, 0, 0, h_silent, NULL },
};
#define TBL_N (int)(sizeof TBL / sizeof TBL[0])

static int fwd(void *ctx, uint8_t zone, const char *line, char *resp, int len) {
    (void)ctx; fwd_calls++; fwd_zone = zone;
    snprintf(fwd_line, sizeof fwd_line, "%s", line);
    return cmd_okf(resp, len, "FORWARDED");
}
static void audit(void *ctx, cmd_src_t src, const char *line) { (void)ctx; (void)src; (void)line; audit_calls++; }

static cmd_core_t core;
static cmd_session_t ses;
static char resp[CMD_RESP_MAX];

void setUp(void) {
    memset(&core, 0, sizeof core);
    core.table = TBL; core.table_len = TBL_N;
    core.role = CMD_ROLE_ZONE; core.zone_id = 2;
    core.now_ms = fake_clock_now;
    core.audit = audit; core.debug_key = "hill";
    memset(&ses, 0, sizeof ses);
    ses.source = CMD_SRC_CLI;
    h_calls = audit_calls = fwd_calls = 0;
    fake_clock_set(100000);
    resp[0] = '\0';
}
void tearDown(void) {}

static int run(const char *line) { return cmd_dispatch(&core, &ses, line, resp, sizeof resp); }

static void test_syntax_errors(void) {
    TEST_ASSERT_EQUAL_INT(-1, run(""));                TEST_ASSERT_EQUAL_STRING("ERR EMPTY\n", resp);
    TEST_ASSERT_EQUAL_INT(-1, run("   "));             TEST_ASSERT_EQUAL_STRING("ERR EMPTY\n", resp);
    char long_line[300]; memset(long_line, 'a', 299); long_line[299] = '\0';
    TEST_ASSERT_EQUAL_INT(-1, run(long_line));         TEST_ASSERT_EQUAL_STRING("ERR TOO_LONG\n", resp);
    TEST_ASSERT_EQUAL_INT(-1, run("a b c d e f g h i j k l m")); TEST_ASSERT_EQUAL_STRING("ERR BAD_ARGS\n", resp);
    TEST_ASSERT_EQUAL_INT(-1, run("FOO BAR"));         TEST_ASSERT_EQUAL_STRING("ERR UNKNOWN_CMD\n", resp);
}

static void test_match_case_and_two_word_precedence(void) {
    core.role = CMD_ROLE_MASTER;
    TEST_ASSERT_EQUAL_INT(0, run("set node 3 name Basil"));
    TEST_ASSERT_EQUAL_STRING("OK NODE 3 Basil\n", resp);        /* STR case preserved */
    TEST_ASSERT_EQUAL_INT(0, run("SET NODE 3 MAC 24:6F:28:AA:BB:02"));
    TEST_ASSERT_EQUAL_STRING("OK MAC 2402\n", resp);
}

static void test_arity_and_ranges(void) {
    TEST_ASSERT_EQUAL_INT(-1, run("SET LIGHT 1 70 45"));   TEST_ASSERT_EQUAL_STRING("ERR BAD_ARGS\n", resp);
    TEST_ASSERT_EQUAL_INT(-1, run("GET LIGHT 1 2"));       TEST_ASSERT_EQUAL_STRING("ERR BAD_ARGS\n", resp);
    TEST_ASSERT_EQUAL_INT(-1, run("SET LIGHT 5 1 1 1"));   TEST_ASSERT_EQUAL_STRING("ERR OUT_OF_RANGE\n", resp);
    TEST_ASSERT_EQUAL_INT(-1, run("SET LIGHT x 1 1 1"));   TEST_ASSERT_EQUAL_STRING("ERR BAD_ARGS\n", resp);
    TEST_ASSERT_EQUAL_INT(0,  run("SET LIGHT 3 70 45 60"));
    TEST_ASSERT_EQUAL_STRING("OK LIGHT 3 70 45 MANUAL 60\n", resp);
    TEST_ASSERT_EQUAL_INT(0,  run("GET LIGHT 3"));
    TEST_ASSERT_EQUAL_INT(-1, run("SET WHEN 25:00"));      TEST_ASSERT_EQUAL_STRING("ERR BAD_ARGS\n", resp);
    TEST_ASSERT_EQUAL_INT(0,  run("SET WHEN 06:30"));      TEST_ASSERT_EQUAL_STRING("OK WHEN 390\n", resp);
    TEST_ASSERT_EQUAL_INT(0,  run("SET WHEN 390"));
    TEST_ASSERT_EQUAL_INT(-1, run("SET NODE 3 MAC 24:6F:28:AA:BB")); /* 5 groups */
    TEST_ASSERT_EQUAL_STRING("ERR MASTER_ONLY\n", resp);   /* role gate wins before parse on zone */
    TEST_ASSERT_EQUAL_INT(0,  run("REBOOT CONFIRM"));      TEST_ASSERT_EQUAL_STRING("OK REBOOT\n", resp);
    TEST_ASSERT_EQUAL_INT(-1, run("REBOOT"));              TEST_ASSERT_EQUAL_STRING("ERR BAD_ARGS\n", resp);
    TEST_ASSERT_EQUAL_INT(-1, run("REBOOT YES"));          TEST_ASSERT_EQUAL_STRING("ERR BAD_ARGS\n", resp);
}

static void test_gates(void) {
    TEST_ASSERT_EQUAL_INT(-1, run("SET WIFI ON"));         TEST_ASSERT_EQUAL_STRING("ERR MASTER_ONLY\n", resp);
    core.role = CMD_ROLE_MASTER;
    TEST_ASSERT_EQUAL_INT(-1, run("SET LIGHT 1 1 1 1"));   TEST_ASSERT_EQUAL_STRING("ERR ZONE_ONLY\n", resp);
    core.role = CMD_ROLE_ZONE;
    ses.source = CMD_SRC_HTTP;
    TEST_ASSERT_EQUAL_INT(-1, run("SET ECHO ON"));         TEST_ASSERT_EQUAL_STRING("ERR NOT_LOCAL\n", resp);
    ses.source = CMD_SRC_CLI;
    TEST_ASSERT_EQUAL_INT(-1, run("SET POKE ON"));         TEST_ASSERT_EQUAL_STRING("ERR LOCKED\n", resp);
    ses.unlock_until_ms = fake_clock_now() + 1000;
    TEST_ASSERT_EQUAL_INT(0,  run("SET POKE ON"));
    TEST_ASSERT_EQUAL_UINT32(fake_clock_now() + CMD_UNLOCK_MS, ses.unlock_until_ms); /* refreshed */
    fake_clock_add(CMD_UNLOCK_MS + 1);
    TEST_ASSERT_EQUAL_INT(-1, run("SET POKE ON"));         TEST_ASSERT_EQUAL_STRING("ERR LOCKED\n", resp);
}

static void test_zone_prefix(void) {
    /* zone role, own id 2 */
    TEST_ASSERT_EQUAL_INT(0,  run("SET ZONE 2 LIGHT 1 10 10 5"));
    TEST_ASSERT_EQUAL_INT(0,  run("SET ZONE 0 LIGHT 1 10 10 5"));
    TEST_ASSERT_EQUAL_INT(-1, run("SET ZONE 3 LIGHT 1 10 10 5"));
    TEST_ASSERT_EQUAL_STRING("ERR WRONG_ZONE\n", resp);
    TEST_ASSERT_EQUAL_INT(-1, run("SET ZONE 9 LIGHT 1 1 1 1"));
    TEST_ASSERT_EQUAL_STRING("ERR OUT_OF_RANGE\n", resp);
    TEST_ASSERT_EQUAL_INT(-1, run("SET ZONE 2"));
    TEST_ASSERT_EQUAL_STRING("ERR BAD_ARGS\n", resp);
    /* master role: 0 local, others forwarded verbatim, case preserved */
    core.role = CMD_ROLE_MASTER; core.zone_id = 0;
    core.forward = fwd;
    TEST_ASSERT_EQUAL_INT(0, run("set ZONE 2 light 3 70 45 60"));
    TEST_ASSERT_EQUAL_INT(1, fwd_calls);
    TEST_ASSERT_EQUAL_UINT8(2, fwd_zone);
    TEST_ASSERT_EQUAL_STRING("set light 3 70 45 60", fwd_line);
    TEST_ASSERT_EQUAL_STRING("OK FORWARDED\n", resp);
    core.forward = NULL;                                    /* SP1 master without ring */
    TEST_ASSERT_EQUAL_INT(-1, run("SET ZONE 2 LIGHT 1 1 1 1"));
    TEST_ASSERT_EQUAL_STRING("ERR ZONE_UNKNOWN\n", resp);
}

static void test_internal_and_audit(void) {
    TEST_ASSERT_EQUAL_INT(-1, run("GET SILENT"));
    TEST_ASSERT_EQUAL_STRING("ERR INTERNAL\n", resp);
    audit_calls = 0;
    TEST_ASSERT_EQUAL_INT(0, run("SET LIGHT 1 10 10 5"));
    TEST_ASSERT_EQUAL_INT(1, audit_calls);                  /* actuator + success */
    TEST_ASSERT_EQUAL_INT(-1, run("SET LIGHT 9 10 10 5"));
    TEST_ASSERT_EQUAL_INT(1, audit_calls);                  /* refused -> no audit */
    TEST_ASSERT_EQUAL_INT(0, run("GET LIGHT 1"));
    TEST_ASSERT_EQUAL_INT(1, audit_calls);                  /* GET is not audited */
}

int main(void) { UNITY_BEGIN();
    RUN_TEST(test_syntax_errors);
    RUN_TEST(test_match_case_and_two_word_precedence);
    RUN_TEST(test_arity_and_ranges);
    RUN_TEST(test_gates);
    RUN_TEST(test_zone_prefix);
    RUN_TEST(test_internal_and_audit);
    return UNITY_END(); }
```

- [ ] **Step 2: Run — expect FAIL** (`ctest … -R test_cmd_core`).

- [ ] **Step 3: Implement `components/cmd_core/cmd_dispatch.c`**

```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cmd_core.h"

static const cmd_entry_t *match(const cmd_core_t *c, uint8_t vbit, const char *n1, const char *n2, int two) {
    for (int i = 0; i < c->table_len; i++) {
        const cmd_entry_t *e = &c->table[i];
        if (!(e->verbs & vbit)) continue;
        if (two) {
            if (e->noun2 && cmd_ci_eq(e->noun1, n1) && cmd_ci_eq(e->noun2, n2)) return e;
        } else {
            if (!e->noun2 && cmd_ci_eq(e->noun1, n1)) return e;
        }
    }
    return NULL;
}

static int parse_int_arg(const char *s, long *out) {
    char *end;
    long v = strtol(s, &end, 10);
    if (end == s || *end) return -1;
    *out = v;
    return 0;
}

static int parse_mac(const char *s, uint8_t mac[6]) {
    for (int i = 0; i < 6; i++) {
        char *end;
        long v = strtol(s, &end, 16);
        if (end != s + 2 || v < 0 || v > 255) return -1;
        mac[i] = (uint8_t)v;
        s = end;
        if (i < 5 && *s++ != ':') return -1;
    }
    return *s == '\0' ? 0 : -1;
}

static int enum_index(const char *enums, const char *val, long *out) {
    long idx = 0;
    const char *p = enums;
    while (*p) {
        const char *bar = strchr(p, '|');
        size_t n = bar ? (size_t)(bar - p) : strlen(p);
        char name[20];
        if (n < sizeof name) {
            memcpy(name, p, n); name[n] = '\0';
            if (cmd_ci_eq(name, val)) { *out = idx; return 0; }
        }
        if (!bar) break;
        p = bar + 1; idx++;
    }
    long v;
    if (parse_int_arg(val, &v) == 0 && v >= 0 && v <= idx) { *out = v; return 0; }
    return -1;
}

int cmd_dispatch(const cmd_core_t *core, cmd_session_t *ses, const char *line, char *resp, int resp_len) {
    resp[0] = '\0';
    if (strlen(line) >= CMD_LINE_MAX) return cmd_err(resp, resp_len, "TOO_LONG");
    char buf[CMD_LINE_MAX];
    strcpy(buf, line);
    const char *tok[CMD_MAX_TOKENS];
    int ntok = cmd_tokenize(buf, tok, CMD_MAX_TOKENS);
    if (ntok < 0) return cmd_err(resp, resp_len, "BAD_ARGS");
    if (ntok == 0) return cmd_err(resp, resp_len, "EMPTY");

    /* ZONE <z> address prefix: <VERB> ZONE <z> <tail...> */
    char tail[CMD_LINE_MAX];
    if (ntok >= 3 && cmd_ci_eq(tok[1], "ZONE")) {
        long z;
        if (parse_int_arg(tok[2], &z) != 0) return cmd_err(resp, resp_len, "BAD_ARGS");
        if (z < 0 || z > 8) return cmd_err(resp, resp_len, "OUT_OF_RANGE");
        if (ntok < 4) return cmd_err(resp, resp_len, "BAD_ARGS");
        size_t off = (size_t)snprintf(tail, sizeof tail, "%s", tok[0]);
        for (int i = 3; i < ntok; i++)
            off += (size_t)snprintf(tail + off, sizeof tail - off, " %s", tok[i]);
        if (core->role == CMD_ROLE_MASTER && z != 0) {
            if (!core->forward) return cmd_err(resp, resp_len, "ZONE_UNKNOWN");
            return core->forward(core->forward_ctx, (uint8_t)z, tail, resp, resp_len);
        }
        if (core->role == CMD_ROLE_ZONE && z != 0 && z != core->zone_id)
            return cmd_err(resp, resp_len, "WRONG_ZONE");
        return cmd_dispatch(core, ses, tail, resp, resp_len);   /* re-enter with the tail */
    }

    if (cmd_ci_eq(tok[ntok - 1], "HELP"))
        return cmd_help(core, ses, tok, ntok - 1, resp, resp_len);

    uint8_t vbit;
    int noun0;                                   /* index of noun1 */
    if (cmd_ci_eq(tok[0], "SET")) { vbit = CMDV_SET; noun0 = 1; }
    else if (cmd_ci_eq(tok[0], "GET")) { vbit = CMDV_GET; noun0 = 1; }
    else { vbit = CMDV_BARE; noun0 = 0; }
    if (vbit != CMDV_BARE && ntok < 2) return cmd_err(resp, resp_len, "UNKNOWN_CMD");

    const cmd_entry_t *e = NULL;
    int args0 = 0;
    if (ntok > noun0 + 1) {
        e = match(core, vbit, tok[noun0], tok[noun0 + 1], 1);
        if (e) args0 = noun0 + 2;
    }
    if (!e) {
        e = match(core, vbit, tok[noun0], NULL, 0);
        if (e) args0 = noun0 + 1;
    }
    if (!e) return cmd_err(resp, resp_len, "UNKNOWN_CMD");

    if ((e->flags & CMDF_MASTER) && core->role != CMD_ROLE_MASTER) return cmd_err(resp, resp_len, "MASTER_ONLY");
    if ((e->flags & CMDF_ZONE) && core->role != CMD_ROLE_ZONE) return cmd_err(resp, resp_len, "ZONE_ONLY");
    if ((e->flags & CMDF_SESSION) && ses->source == CMD_SRC_HTTP) return cmd_err(resp, resp_len, "NOT_LOCAL");
    if (e->flags & CMDF_UNLOCK) {
        uint32_t now = core->now_ms();
        if (!(ses->unlock_until_ms > now)) return cmd_err(resp, resp_len, "LOCKED");
    }

    cmd_req_t q = { .core = core, .ses = ses, .e = e, .verb = vbit };
    int n = ntok - args0;
    int want_min = (vbit == CMDV_GET) ? e->n_key : e->min_args;
    int want_max = (vbit == CMDV_GET) ? e->n_key : e->max_args;
    if (n < want_min || n > want_max) return cmd_err(resp, resp_len, "BAD_ARGS");
    q.n = (uint8_t)n;
    for (int i = 0; i < n; i++) {
        const cmd_arg_t *a = &e->args[i];
        q.tok[i] = tok[args0 + i];
        q.val[i] = 0;
        long v;
        switch (a->type) {
        case ARG_INT:
            if (parse_int_arg(q.tok[i], &v) != 0) return cmd_err(resp, resp_len, "BAD_ARGS");
            if (v < a->min || v > a->max) return cmd_err(resp, resp_len, "OUT_OF_RANGE");
            q.val[i] = (int32_t)v;
            break;
        case ARG_ENUM:
            if (enum_index(a->enums, q.tok[i], &v) != 0) return cmd_err(resp, resp_len, "BAD_ARGS");
            q.val[i] = (int32_t)v;
            break;
        case ARG_STR:
            if ((int)strlen(q.tok[i]) > a->max) return cmd_err(resp, resp_len, "BAD_ARGS");
            break;
        case ARG_TIME: {
            int m = cmd_parse_time(q.tok[i]);
            if (m < 0) return cmd_err(resp, resp_len, "BAD_ARGS");
            q.val[i] = m;
            break;
        }
        case ARG_MAC:
            if (parse_mac(q.tok[i], q.mac) != 0) return cmd_err(resp, resp_len, "BAD_ARGS");
            break;
        default:
            return cmd_err(resp, resp_len, "INTERNAL");
        }
    }

    int rc = e->handler(&q, resp, resp_len);
    if (resp[0] == '\0') return cmd_err(resp, resp_len, "INTERNAL");
    if (rc == 0 && (e->flags & CMDF_ACTUATOR) && vbit == CMDV_SET && core->audit)
        core->audit(core->audit_ctx, ses->source, line);
    if (ses->unlock_until_ms > core->now_ms())
        ses->unlock_until_ms = core->now_ms() + CMD_UNLOCK_MS;
    return rc;
}
```

Note the MASTER_ONLY gate deliberately runs before argument parsing (the test `SET NODE 3 MAC 24:6F:28:AA:BB` on a zone expects `ERR MASTER_ONLY`).

- [ ] **Step 4: Run — expect PASS** (`test_cmd_core` green; `test_cmd_help` comes next task).

- [ ] **Step 5: Commit**

```bash
git add components/cmd_core tests/host
git commit -m "feat(cmd_core): tokenizer, table-driven dispatch, gates, ZONE prefix

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
git push
```

---

### Task 8: `cmd_core` — HELP rendering and table self-check

**Files:**
- Modify: `components/cmd_core/cmd_help.c` (replace stub)
- Modify: `tests/host/CMakeLists.txt` (`hg_test(test_cmd_help ${CMD_CORE_SRC})`)
- Test: `tests/host/test_cmd_help.c`

**Interfaces:**
- Consumes: Task 7 table/dispatch (HELP interception already calls `cmd_help`).
- Produces: `cmd_help()` (bare `HELP` = grammar header + one `+ <AREA>: <nouns> -- <NOUN> HELP for details` index line per area that has visible rows; a prefix = full `+ ` usage lines for matching rows) and `cmd_table_check()` per spec §5.2.

Rendering rules (host-tested exactly): SET line = `+ SET <NOUN[ NOUN2]> <arg1 …> [optional …]` where INT renders `<name lo-hi>`, ENUM `<A|B>`, STR `<name>`, TIME `<name HH:MM>`, MAC `<name xx:xx:xx:xx:xx:xx>`; args with index ≥ `min_args` use `[…]`; GET line shows the first `n_key` args only; ` [unlock]` appended for `CMDF_UNLOCK`; `  -- <desc>` appended when `desc` non-NULL (two spaces before `--`). Rows carrying the other role's flag are omitted entirely. No match → `ERR UNKNOWN_CMD`.

`cmd_table_check(t, n)` returns the index of the first row violating: duplicate `(verb-overlap, noun1, noun2)`; `n_key > max_args`; `min_args > max_args`; `max_args > CMD_MAX_ARGS`; NULL `handler`; ENUM arg with NULL `enums`; `CMDV_BARE` combined with SET/GET bits; `CMDF_ACTUATOR` row with no INT arg named `seconds` or `minutes` (case-insensitive); rendered help line > 96 chars; else −1.

- [ ] **Step 1: Test** `tests/host/test_cmd_help.c` (reuses the Task-7 fixture table via `#include "test_cmd_core_fixture.h"` — move the table + handlers of Task 7's test into that shared header, keeping both test files compiling): asserts — `HELP` output starts with `+ SET|GET <NOUN> [args] | <VERB> ZONE <z 0-8> <command> | <prefix> HELP\n`, contains one line per non-empty area, contains no `<shelf` text; `SET LIGHT HELP` (zone role) returns exactly `+ SET LIGHT <shelf 1-4> <white 0-100> <red 0-100> <minutes 1-720> [A]  -- manual override, then AUTO\n+ GET LIGHT <shelf 1-4>\n` — where `[A]` is NOT rendered (actuator marking is not part of HELP; assert the literal without it); `SET POKE HELP` contains ` [unlock]`; `SET WIFI HELP` on zone → `ERR UNKNOWN_CMD` (master row hidden); `SET NOPE HELP` → `ERR UNKNOWN_CMD`; full `SET HELP` and `GET HELP` outputs are < 4000 bytes. Table-check: the fixture table returns −1; a copy with a duplicated LIGHT row returns its index; a copy whose actuator row's duration arg is renamed `"level"` fails; a BARE|SET row fails; `n_key 5 > max_args 4` fails.

- [ ] **Step 2: Run — expect FAIL.**

- [ ] **Step 3: Implement `cmd_help.c`** (~160 lines): `render_usage(e, vbit, out, cap)` builds the argument text per the rules above; `cmd_help` walks the table with role filtering; area names `{"SYSTEM","CONFIG","SESSION","TIME","FIRMWARE","RING","SHELF","SENSORS","FAULTS","NETWORK","DEBUG"}`; prefix matching: if `tok[0]` is SET/GET the verb bit filters and `tok[1..]` filter noun1/noun2; otherwise `tok[0..]` filter nouns for BARE rows too; emit with `cmd_linef`. `cmd_table_check` implements the listed violations in order, using `render_usage` for the length rule.

- [ ] **Step 4: Run — expect PASS** (all host tests green).

- [ ] **Step 5: Commit**

```bash
git add components/cmd_core tests/host
git commit -m "feat(cmd_core): generated HELP and boot-time table self-check

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
git push
```

---

### Task 9: `hg_model` — snapshots, validated atomic edits, dirty bits

**Files:**
- Create: `components/hg_model/hg_model.h`, `components/hg_model/hg_model.c`, `components/hg_model/CMakeLists.txt` (`SRCS "hg_model.c" INCLUDE_DIRS "." REQUIRES hg_cfg hg_blob` — plus `freertos` guarded inside the file)
- Modify: `tests/host/CMakeLists.txt` (`hg_test(test_hg_model ${COMP}/hg_model/hg_model.c ${HG_CFG_SRC})`)
- Test: `tests/host/test_hg_model.c`

**Interfaces:**
- Consumes: Task 3/4 (`hg_defaults_*`, `hg_*_validate`), Task 2 (nothing directly).
- Produces (used by zone_cmds, hg_store; SP3's `apply_remote` extends this file):

```c
#define HG_CH_HW      0x01   /* hardware map changed -> restart pending */
#define HG_CH_CFG     0x02   /* logical config changed */
#define HG_CH_HW_LIVE 0x04   /* calibration / safety-limit fields: persist as hw, no restart flag */
void     hg_model_init(void);                                   /* defaults, generation 0, seq 0 */
void     hg_model_boot_load(const hg_zone_hw_t *hw_or_null, const hg_zone_cfg_t *cfg_or_null);
void     hg_model_snapshot_hw(hg_zone_hw_t *out);
void     hg_model_snapshot_cfg(hg_zone_cfg_t *out, uint32_t *seq_or_null);
uint32_t hg_model_cfg_seq(void);
typedef uint32_t (*hg_edit_fn)(hg_zone_hw_t *hw, hg_zone_cfg_t *cfg, void *arg); /* returns HG_CH_* mask, 0 = no change */
int      hg_model_edit(hg_edit_fn fn, void *arg, char *err, size_t errlen);      /* 0 ok, -1 busy, -2 invalid (err = path) */
uint32_t hg_model_take_dirty(uint8_t kind, void *staging, uint32_t *gen_out);    /* HG_CH_HW|HG_CH_CFG: copies struct, clears bit, returns size or 0 */
uint8_t  hg_model_dirty_mask(void);
int      hg_model_restart_pending(void);
```

Semantics: `hg_model_edit` copies the live `hw`+`cfg` (496 B) to stack scratch → runs `fn` → mask & (HW|HW_LIVE) → `hg_hw_validate(scratch_hw)`; mask & CFG or any hw change → `hg_cfg_validate(scratch_cfg, scratch_hw)` → on any failure return −2 with the path, live structs untouched → commit: memcpy back; if mask & CFG: `generation = generation + 1` (skip 0 on wrap), `source = HG_SRC_LOCAL`; `seq++`; `dirty |= (mask & HW_LIVE ? HG_CH_HW : 0) | (mask & (HG_CH_HW|HG_CH_CFG))`; `restart_pending = 1` when mask & HG_CH_HW. Mutex is a real FreeRTOS mutex on target, a no-op under `HOST_TEST` (`#ifndef HOST_TEST` around the three lock lines only — the header stays pure).

- [ ] **Step 1: Header + stubs + test.** `tests/host/test_hg_model.c` (complete):

```c
#include <string.h>
#include "unity.h"
#include "hg_model.h"

void setUp(void) { hg_model_init(); }
void tearDown(void) {}
static char err[48];

static uint32_t ed_set_target(hg_zone_hw_t *hw, hg_zone_cfg_t *cfg, void *arg) {
    (void)hw; cfg->shelf[0].water.target_pct = *(uint8_t *)arg; return HG_CH_CFG;
}
static uint32_t ed_bad_dose(hg_zone_hw_t *hw, hg_zone_cfg_t *cfg, void *arg) {
    (void)hw; (void)arg; cfg->shelf[1].water.dose_s = 299; return HG_CH_CFG; /* > pump_max_run_s 60 */
}
static uint32_t ed_pump_pin(hg_zone_hw_t *hw, hg_zone_cfg_t *cfg, void *arg) {
    (void)cfg; hw->shelf[0].pump_pin = *(uint8_t *)arg; return HG_CH_HW;
}
static uint32_t ed_cal(hg_zone_hw_t *hw, hg_zone_cfg_t *cfg, void *arg) {
    (void)cfg; (void)arg; hw->shelf[0].soil_dry_mv[0] = 2900; return HG_CH_HW_LIVE;
}

static void test_edit_commit_and_gen(void) {
    uint8_t v = 60;
    TEST_ASSERT_EQUAL_INT(0, hg_model_edit(ed_set_target, &v, err, sizeof err));
    hg_zone_cfg_t c; uint32_t seq;
    hg_model_snapshot_cfg(&c, &seq);
    TEST_ASSERT_EQUAL_UINT8(60, c.shelf[0].water.target_pct);
    TEST_ASSERT_EQUAL_UINT32(1, c.generation);
    TEST_ASSERT_EQUAL_UINT8(HG_SRC_LOCAL, c.source);
    TEST_ASSERT_EQUAL_UINT32(1, seq);
    TEST_ASSERT_EQUAL_UINT8(HG_CH_CFG, hg_model_dirty_mask());
}

static void test_invalid_edit_changes_nothing(void) {
    TEST_ASSERT_EQUAL_INT(-2, hg_model_edit(ed_bad_dose, NULL, err, sizeof err));
    TEST_ASSERT_EQUAL_STRING("shelf[1].water.dose_s", err);
    hg_zone_cfg_t c;
    hg_model_snapshot_cfg(&c, NULL);
    TEST_ASSERT_EQUAL_UINT16(20, c.shelf[1].water.dose_s);
    TEST_ASSERT_EQUAL_UINT32(0, c.generation);
    TEST_ASSERT_EQUAL_UINT8(0, hg_model_dirty_mask());
}

static void test_hw_edit_restart_pending_cal_not(void) {
    uint8_t pin = 9;
    TEST_ASSERT_EQUAL_INT(0, hg_model_edit(ed_pump_pin, &pin, err, sizeof err));
    TEST_ASSERT_EQUAL_INT(1, hg_model_restart_pending());
    TEST_ASSERT_EQUAL_UINT8(HG_CH_HW, hg_model_dirty_mask() & HG_CH_HW);
    hg_model_init();
    TEST_ASSERT_EQUAL_INT(0, hg_model_edit(ed_cal, NULL, err, sizeof err));
    TEST_ASSERT_EQUAL_INT(0, hg_model_restart_pending());
    TEST_ASSERT_EQUAL_UINT8(HG_CH_HW, hg_model_dirty_mask()); /* still persisted as hw */
}

static void test_take_dirty(void) {
    uint8_t v = 50;
    hg_model_edit(ed_set_target, &v, err, sizeof err);
    hg_zone_cfg_t staged; uint32_t gen = 0;
    TEST_ASSERT_EQUAL_UINT32(sizeof(hg_zone_cfg_t), hg_model_take_dirty(HG_CH_CFG, &staged, &gen));
    TEST_ASSERT_EQUAL_UINT32(1, gen);
    TEST_ASSERT_EQUAL_UINT8(50, staged.shelf[0].water.target_pct);
    TEST_ASSERT_EQUAL_UINT32(0, hg_model_take_dirty(HG_CH_CFG, &staged, &gen)); /* cleared */
}

static void test_boot_load(void) {
    hg_zone_cfg_t c; hg_zone_hw_t h;
    hg_defaults_cfg(&c); hg_defaults_hw(&h);
    c.generation = 7; c.shelf[2].water.target_pct = 33; h.shelf[0].pump_pin = 11;
    hg_model_boot_load(&h, &c);
    hg_zone_cfg_t out; hg_model_snapshot_cfg(&out, NULL);
    TEST_ASSERT_EQUAL_UINT32(7, out.generation);
    TEST_ASSERT_EQUAL_UINT8(33, out.shelf[2].water.target_pct);
    TEST_ASSERT_EQUAL_UINT8(0, hg_model_dirty_mask());        /* boot load is clean */
}

int main(void) { UNITY_BEGIN();
    RUN_TEST(test_edit_commit_and_gen);
    RUN_TEST(test_invalid_edit_changes_nothing);
    RUN_TEST(test_hw_edit_restart_pending_cal_not);
    RUN_TEST(test_take_dirty);
    RUN_TEST(test_boot_load);
    return UNITY_END(); }
```

- [ ] **Step 2: Run — expect FAIL.**

- [ ] **Step 3: Implement `hg_model.c`** (~120 lines): statics `s_hw, s_cfg, s_seq, s_dirty, s_restart_pending` (+ `SemaphoreHandle_t s_mux` under `#ifndef HOST_TEST` with `LOCK()/UNLOCK()` macros expanding to nothing on host, `xSemaphoreTake(s_mux, pdMS_TO_TICKS(100))` returning −1 busy on target). Follow the semantics block above exactly; `gen_next(g)` = `g + 1 ? g + 1 : 1`.

- [ ] **Step 4: Run — expect PASS.** • **Step 5: Commit** `feat(hg_model): validated atomic config edits with dirty tracking`.

---

### Task 10: `zone_cmds` — config field grammar over the model

**Files:**
- Create: `components/zone_cmds/zone_cmds.h`, `components/zone_cmds/zone_cmds.c`, `components/zone_cmds/CMakeLists.txt` (`REQUIRES cmd_core hg_cfg hg_model`)
- Modify: `tests/host/CMakeLists.txt` (`hg_test(test_zone_cmds ${COMP}/zone_cmds/zone_cmds.c ${CMD_CORE_SRC} ${HG_CFG_SRC} ${COMP}/hg_model/hg_model.c)`)
- Test: `tests/host/test_zone_cmds.c`

**Interfaces:**
- Consumes: `cmd_core` row/req types, `hg_field_*`, `hg_model_edit/snapshot`.
- Produces: `extern const cmd_entry_t ZONE_CMD_ROWS[]; extern const int ZONE_CMD_ROWS_N;` — rows (all `CMDF_ZONE`):

| Row | args | flags | behaviour |
|---|---|---|---|
| `SET\|GET ZONECFG` | SET: `<key> <value>` (STR,STR); GET: none (`n_key 0`) | – | field set / group print |
| `SET\|GET SHELF <s>` / `LIGHT` / `WATER` / `FAN` / `VIB` | SET: `<shelf 1-4> <key> <value>`; GET: `<shelf 1-4>` (`n_key 1`) | – | shelf-scoped field set / group print |
| `SET\|GET AUX <a 1-2>` | as shelf rows with idx 1–2 | – | aux-scoped |
| `SET\|GET HW` | zone-scoped | `CMDF_UNLOCK` on SET | installer plane |
| `SET\|GET HWSHELF <s>` | shelf-scoped | `CMDF_UNLOCK` on SET (single row: gate checked in handler for the SET verb only) | installer plane |
| `SET SHELVES <n 1-4>` | INT | `CMDF_UNLOCK` | alias for `SET HW SHELVES` |
| `SET CAL <s 1-4> <A\|B> <DRY\|WET> <mv 0-3300>` / `GET CAL <s>` | – | maps to CAL keys `DRY_A/WET_A/DRY_B/WET_B` |
| `GET CONFIG` (GET-verb row, noun `CONFIG`) | – | – | `OK CONFIG <n>` + one `  SET …` line per **non-default** field (replayable dump) |

Replies: `SET <GROUP> [idx] <KEY> <canonical-value>` echoed via `hg_field_get_text` after the edit; GET group prints `OK <GROUP> [idx]` + `  <Key> : <value>` per row of that group. Error mapping from `hg_field_set_text` / `hg_model_edit`: −1→`BAD_ARGS`, −2→`OUT_OF_RANGE`, −3→`INVALID_FIELD`, −4→`OUT_OF_RANGE`, model −1→`BUSY`, −2→`ERR INVALID_FIELD` with `  Field : <path>` continuation. Because `cmd_entry_t` has one flags word, HWSHELF/HW use `CMDF_UNLOCK` only on their SET rows by registering **separate SET and GET rows** for those groups (GET rows without the flag).

- [ ] **Step 1: Test (complete `tests/host/test_zone_cmds.c`)** — fixture: zone-role core with `ZONE_CMD_ROWS`, fake clock, fresh `hg_model_init()` in `setUp`. Asserts: `SET WATER 1 TARGET 60` → `OK WATER 1 TARGET 60\n` and model shows 60; `GET WATER 1` contains `  Target : 60`; `SET WATER 1 TARGET 101` → `ERR OUT_OF_RANGE`; `SET WATER 1 BOGUS 1` → `ERR INVALID_FIELD`; `SET WATER 5 TARGET 60` → `ERR OUT_OF_RANGE` (table range); `SET LIGHT 1 ON 06:30` → `OK LIGHT 1 ON 06:30`; `SET SHELF 1 CROP Basil` preserves case; `SET HW SHELVES 3` locked → `ERR LOCKED`, after `ses.unlock_until_ms` set → OK and `hg_model_restart_pending()==1`; `SET CAL 1 A DRY 2850` → `OK CAL 1 A DRY 2850` and hw field updated, no restart flag; `SET ZONECFG NAME Herbs` → `OK ZONECFG NAME Herbs`; dump: after three non-default edits `GET CONFIG` starts `OK CONFIG 3` and replaying each `  SET` line into a fresh model reproduces the three values; invalid cross-field via field grammar (`SET WATER 1 DOSE_S 100` with default pump cap 60) → `ERR INVALID_FIELD` + `  Field : shelf[0].water.dose_s`.

- [ ] **Step 2: Run — expect FAIL.**

- [ ] **Step 3: Implement `zone_cmds.c`** (~230 lines): generic `h_field_set`/`h_field_get` reading the group from `q->e->noun1` via `hg_group_find`, idx = `q->val[0]` −1 for scoped groups; edit callback struct `{group, idx, key, value, rc, path}` calling `hg_field_set_text` on the scratch structs inside the `hg_edit_fn` and returning the right `HG_CH_*` mask (HW/HWSHELF map keys → `HG_CH_HW`; HWSHELF `LED_MAX_*`/`PUMP_MAX_*` and all CAL keys → `HG_CH_HW_LIVE`; else `HG_CH_CFG`); CAL row handler builds the key from the two enums; dump compares live snapshot to freshly built defaults using `hg_field_get_text` on both.

- [ ] **Step 4: Run — expect PASS.** • **Step 5: Commit** `feat(zone_cmds): config field grammar, CAL commands, replayable dump`.

---

### Task 11: `cmd_common` — shared rows over `app_if`

**Files:**
- Create: `components/cmd_common/app_if.h`, `components/cmd_common/cmd_common.h`, `components/cmd_common/cmd_common.c`, `components/cmd_common/CMakeLists.txt` (`REQUIRES cmd_core notify`)
- Create: `tests/host/fakes/fake_app_if.h`, `tests/host/fakes/fake_app_if.c`
- Modify: `tests/host/CMakeLists.txt` (`hg_test(test_cmd_common ${COMP}/cmd_common/cmd_common.c ${CMD_CORE_SRC} ${COMP}/notify/notify.c fakes/fake_app_if.c)`)
- Test: `tests/host/test_cmd_common.c`

**Interfaces:**
- Consumes: cmd_core, notify.
- Produces — `components/cmd_common/app_if.h` (create verbatim; each app implements it once, `fake_app_if.c` fakes it):

```c
#pragma once
#include <stdint.h>
#include <stddef.h>
typedef struct {
    const char *role_name;                          /* "MASTER" / "ZONE" */
    uint8_t   (*zone_id)(void);
    void      (*get_mac)(uint8_t mac[6]);
    const char *(*node_name)(void);                 /* "-" when unnamed */
    uint32_t  (*uptime_s)(void);
    int       (*status_lines)(char *resp, int len); /* append "  Label : value" lines; 0 ok */
    int       (*log_set)(const char *level, const char *tag, char *eff, size_t n);
    int       (*time_get)(char *buf, size_t n);     /* "YYYY-MM-DD HH:MM:SS <RTC|NTP|RING|NONE> <age_s>" */
    int       (*time_set)(int y, int mo, int d, int h, int mi, int s);
    int       (*save_flush)(uint32_t timeout_ms);   /* 0 ok / -1 */
    int       (*fw_info)(char *buf, size_t n);      /* "<ver> <ota_0|ota_1> <VALID|PENDING> <other|NONE>" */
    int       (*fw_rollback)(void);                 /* reboots on success; returns -1 on failure */
    int       (*fw_update)(const char *ssid, const char *pass, const char *url); /* writes handover + RTC flag + reboots */
    void      (*reboot)(void);
    int       (*factory_reset)(void);               /* erases NVS then reboots; returns -1 on failure */
} app_if_t;
```

and `void cmd_common_init(const app_if_t *app); extern const cmd_entry_t CMD_COMMON_ROWS[]; extern const int CMD_COMMON_ROWS_N;` with rows: `GET ID` → `OK ID <ROLE> <aa:bb:cc:dd:ee:ff> <zone> <name>`; `GET VERSION` → `OK VERSION <fw_info>`; `GET STATUS` → `OK STATUS` + app lines; `SET|GET ECHO <OFF|ON>` (SESSION; writes `ses->echo`); `SET NOTIFY <type|ALL> <OFF|ON>` + `GET NOTIFY` (SESSION; edits `ses->notify_mask`, reply `OK NOTIFY BOOT=1 ALARM=1 …`); `SET LOG <NONE|ERROR|WARN|INFO|DEBUG|VERBOSE> [tag]` + `GET LOG [tag]` (SESSION); `DEBUG ENABLE <key>` (BARE two-word, SESSION; `strcmp` against `core->debug_key`; ok → `unlock_until_ms = now + CMD_UNLOCK_MS`, `OK DEBUG ENABLE 600`, else `ERR AUTH_FAILED`) / `DEBUG DISABLE` / `GET DEBUG`; `GET TIME` / `SET TIME <date> <time>` (two ARG_STR parsed `%d-%d-%d` `%d:%d:%d` in the handler); `SAVE` (BARE, `CMDF_SLOW`) → `OK SAVE` or `ERR NVS_WRITE`; `REBOOT <CONFIRM>`; `FACTORY RESET <CONFIRM>` (BARE two-word, SLOW); `GET FW`; `SET FW ROLLBACK <CONFIRM>` (`CMDF_UNLOCK`); `SET FW UPDATE <ssid> <pass> <url>` (`CMDF_ZONE|CMDF_UNLOCK|CMDF_SLOW`; handler additionally requires `ses->source == CMD_SRC_CLI` else `ERR NOT_LOCAL` — local console only, spec §5.4).

- [ ] **Step 1: fake + test.** `fake_app_if.c` returns canned values (`role ZONE`, mac `24:6F:28:AA:BB:02`, zone 2, name `-`, uptime 812, fw_info `0.1.0 ota_0 VALID NONE`), records last call name + args, injectable failure flags. Test asserts (complete file, same fixture pattern as Task 7 with `CMD_COMMON_ROWS`): `GET ID` → `OK ID ZONE 24:6f:28:aa:bb:02 2 -\n`; `GET VERSION`; `GET STATUS` first line + at least one continuation from the fake; ECHO/NOTIFY/LOG session behaviour incl. `SET NOTIFY ALL OFF` → mask 0 and `GET NOTIFY` shows `BOOT=0`; DEBUG ENABLE wrong key → `ERR AUTH_FAILED`, right key unlocks a `CMDF_UNLOCK` fixture row, `GET DEBUG` shows remaining, expiry after `fake_clock_add(600001)`; `SET TIME 2026-08-31 14:03:22` calls the fake with parsed ints, malformed date → `ERR BAD_ARGS`; `SAVE` ok + injected failure → `ERR NVS_WRITE`; `REBOOT` without CONFIRM → `ERR BAD_ARGS`; `SET FW UPDATE a b c` from an HTTP session → `ERR NOT_LOCAL`, from CLI locked → `ERR LOCKED`, unlocked → fake records ssid/pass/url.

- [ ] **Step 2: FAIL. Step 3: implement `cmd_common.c` (~260 lines). Step 4: PASS. Step 5: Commit** `feat(cmd_common): shared command rows over app_if`.

---

### Task 12: `rescue_handover` — the app↔rescue contract

**Files:**
- Create: `components/rescue_handover/rescue_handover.h`, `components/rescue_handover/rescue_handover.c` (pure codec), `components/rescue_handover/rescue_handover_nvs.c` (target glue), `components/rescue_handover/CMakeLists.txt` (`SRCS both .c INCLUDE_DIRS "." REQUIRES hg_blob nvs_flash` — the NVS file is excluded from host builds by the test CMake, which links only the codec)
- Modify: `tests/host/CMakeLists.txt` (`hg_test(test_handover ${COMP}/rescue_handover/rescue_handover.c ${COMP}/hg_blob/hg_blob.c)`)
- Test: `tests/host/test_handover.c`

**Interfaces (spec §2.9):**

```c
#define HG_HANDOVER_MAGIC 0x48524748u  /* 'HGRH' LE */
#define HG_HANDOVER_LEN   176
typedef struct {
    uint8_t expect_link;               /* 1 = fleet path: trial requires a Master frame (SP2) */
    char ssid[33], pass[65], url[64];
} hg_handover_t;
int hg_handover_pack(const hg_handover_t *h, uint8_t out[HG_HANDOVER_LEN]);            /* 0 / -1 bad strings */
int hg_handover_unpack(const uint8_t in[HG_HANDOVER_LEN], hg_handover_t *out);         /* 0 / -1 invalid */
/* target glue (NVS namespace "hg", key "hando"): */
int hg_handover_write(const hg_handover_t *h);      /* wraps pack + nvs_set_blob + commit */
int hg_handover_take(hg_handover_t *out);           /* read + erase (one-shot); -1 absent/corrupt */
```

Byte layout: `0 magic u32 · 4 ver u8 (=1) · 5 expect_link · 6..7 rsvd · 8 ssid[33] · 41 pass[65] · 106 url[64] · 170..171 rsvd · 172 crc32(bytes 0..171)`. Unpack rejects wrong magic/ver/CRC and non-NUL-terminated fields.

- [ ] **Step 1: codec test** — pack/unpack roundtrip with `expect_link 1`, ssid `HillGrow`, pass `hillgrow1`, url `http://192.168.7.7/fw/zone.bin`; flipped byte → −1; wrong magic → −1; 33-char ssid → pack −1.
- [ ] **Step 2: FAIL. Step 3: implement codec (~70 lines, reuse `hg_crc32`); write the NVS glue file (open `hg`, `nvs_set_blob`/`nvs_get_blob` + `nvs_erase_key` + commit; `#include "nvs_flash.h"`, `static const char *TAG = "handover"`). Step 4: PASS. Step 5: Commit** `feat(rescue_handover): one-shot app-to-rescue handover record`.

---

### Task 13: Zone app — target glue (`board`, `cli`, `cmd_task`, `hg_store`) + first firmware build

No host TDD cycle here (target-only glue); the test cycle is `idf.py build` + the Task-17 hardware smoke. Keep every file under ~300 lines.

**Files:**
- Create: `components/board/board.h`, `components/board/Kconfig`, `components/board/CMakeLists.txt`
- Create: `components/cmd_task/cmd_task.h`, `components/cmd_task/cmd_task.c`, `components/cmd_task/CMakeLists.txt` (`REQUIRES cmd_core freertos esp_system` — log via `esp_log`)
- Create: `components/cli/cli.h`, `components/cli/cli.c`, `components/cli/CMakeLists.txt` (`REQUIRES esp_driver_uart log freertos cli_line cmd_task notify esp_app_format board`)
- Create: `components/hg_store/hg_store.h`, `components/hg_store/hg_store.c`, `components/hg_store/CMakeLists.txt` (`REQUIRES nvs_flash hg_blob hg_cfg hg_model freertos esp_system`)
- Create: `cmake/hillgrow.cmake`, `zone/CMakeLists.txt`, `zone/sdkconfig.defaults`, `zone/partitions.csv`, `zone/main/CMakeLists.txt`, `zone/main/app_main.c`, `zone/main/app_if_zone.c`, `zone/main/cmd_table_zone.c`

**Interfaces:**
- Consumes: everything from Tasks 1–12.
- Produces: `cmd_task_start(core)`, `int cmd_task_execute(cmd_session_t*, const char *line, char *resp, int len, uint32_t timeout_ms)` (0/-1 as dispatch; `ERR BUSY` on pool/queue exhaustion, `ERR INTERNAL` on 3500 ms timeout); `cli_init/cli_start` (UART0 115200, echo human mode, registers a notify sink with `NTF_DEFAULT_CLI_MASK`); `hg_store_init` (NVS once + boot-load into model) / `hg_store_start` / `hg_store_flush(ms)` / `hg_store_set_zid(id)` / `hg_store_factory_reset()`; `board.h` pin constants; the zone command table.

Key content (write these files exactly):

`components/board/board.h`:

```c
#pragma once
#define HG_GPIO_RING_RX    18
#define HG_GPIO_RING_TX    19
#define HG_GPIO_I2C_SDA    21
#define HG_GPIO_I2C_SCL    22
#define HG_GPIO_RESCUE_BTN 15
#define HG_GPIO_STATUS_LED 2
#define HG_GPIO_PCA_OE     23
#define HG_CONSOLE_BAUD    115200
#define HG_RING_BAUD       115200
#if CONFIG_HG_ROLE_MASTER
#define HG_ROLE_NAME "MASTER"
#else
#define HG_ROLE_NAME "ZONE"
#endif
```

`components/board/Kconfig`:

```
menu "HillGrow"
    choice HG_ROLE
        prompt "Node role"
        default HG_ROLE_ZONE
        config HG_ROLE_MASTER
            bool "Master"
        config HG_ROLE_ZONE
            bool "Zone"
    endchoice
    config HILLGROW_DEBUG_KEY
        string "Debug unlock key"
        default "hill"
endmenu
```

`components/cmd_task/cmd_task.h`:

```c
#pragma once
#include "cmd_core.h"
void cmd_task_start(const cmd_core_t *core);   /* creates the task: core 0, prio 5, 6144, TWDT-subscribed */
int  cmd_task_execute(cmd_session_t *ses, const char *line, char *resp, int resp_len, uint32_t timeout_ms);
```

`components/cmd_task/cmd_task.c` (complete):

```c
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "cmd_task.h"

static const char *TAG = "cmd_task";
#define POOL_N 4

typedef struct {
    volatile int  used;
    cmd_session_t *ses;
    char           line[CMD_LINE_MAX];
    char          *resp;
    int            resp_len, rc;
    TaskHandle_t   waiter;
} slot_t;

static slot_t s_pool[POOL_N];
static QueueHandle_t s_q;                  /* items: slot_t* */
static const cmd_core_t *s_core;

static void cmd_task_main(void *arg) {
    (void)arg;
    esp_task_wdt_add(NULL);
    slot_t *s;
    for (;;) {
        esp_task_wdt_reset();
        if (xQueueReceive(s_q, &s, pdMS_TO_TICKS(100)) != pdTRUE) continue;
        s->rc = cmd_dispatch(s_core, s->ses, s->line, s->resp, s->resp_len);
        TaskHandle_t w = s->waiter;
        if (w) xTaskNotifyGive(w);
        else s->used = 0;                  /* orphaned: free here */
    }
}

void cmd_task_start(const cmd_core_t *core) {
    s_core = core;
    s_q = xQueueCreate(8, sizeof(slot_t *));
    int rc = cmd_table_check(core->table, core->table_len);
    if (rc >= 0) ESP_LOGE(TAG, "command table row %d invalid", rc);
    xTaskCreatePinnedToCore(cmd_task_main, "cmd_task", 6144, NULL, 5, NULL, 0);
}

int cmd_task_execute(cmd_session_t *ses, const char *line, char *resp, int resp_len, uint32_t timeout_ms) {
    slot_t *s = NULL;
    for (int tries = 0; tries < 10 && !s; tries++) {          /* ~100 ms pool wait */
        for (int i = 0; i < POOL_N; i++)
            if (!s_pool[i].used) { s_pool[i].used = 1; s = &s_pool[i]; break; }
        if (!s) vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (!s) return cmd_err(resp, resp_len, "BUSY");
    s->ses = ses;
    snprintf(s->line, sizeof s->line, "%s", line);
    s->resp = resp; s->resp_len = resp_len;
    s->waiter = xTaskGetCurrentTaskHandle();
    if (xQueueSend(s_q, &s, pdMS_TO_TICKS(50)) != pdTRUE) {
        s->used = 0;
        return cmd_err(resp, resp_len, "BUSY");
    }
    if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(timeout_ms)) == 0) {
        s->waiter = NULL;                  /* orphan; cmd_task frees it on completion */
        return cmd_err(resp, resp_len, "INTERNAL");
    }
    int rc = s->rc;
    s->used = 0;
    return rc;
}
```

`components/cli/cli.c` — complete file implementing: `cli_init()` = `uart_driver_install(UART_NUM_0, 512, 2048, 0, NULL, 0)` + `uart_param_config` 115200 8N1 + `uart_vfs_dev_use_driver(0)` + recursive mutex + `esp_log_set_vprintf(cli_vprintf)`; `cli_vprintf` formats into a 256-B static buffer (truncate with `~`), takes the mutex ≤ 50 ms (else `s_log_drops++`), in human mode with a non-empty edit line writes `"\r\x1b[2K"` first, writes the log line (converting `\n`→`\r\n`), redraws the partial line, releases; `cli_write(const char *s)` under the same mutex converting `\n`→`\r\n`; a notify sink `void cli_notify_sink(void *ctx, const char *line)` = `cli_write(line)` registered with `NTF_DEFAULT_CLI_MASK`; `cli_start()` creates `cli0` (core 0, prio 4, 4096, TWDT): loop `uart_read_bytes(UART_NUM_0, buf, 64, pdMS_TO_TICKS(100))`, feed bytes through `cli_line_feed` (echo bytes written via `cli_write` raw, no LF conversion), on `CLI_EVT_LINE` → `""` → `cli_write("ERR EMPTY\r\n")` shortcut? No — submit every line (including empty) via `cmd_task_execute(&s_cli_ses, line, s_resp, sizeof s_resp, 3500)` and `cli_write(s_resp)`; on `CLI_EVT_TOO_LONG` → `cli_write("ERR TOO_LONG\r\n")`. Session: `static cmd_session_t s_cli_ses = { .source = CMD_SRC_CLI, .echo = 1, .notify_mask = NTF_DEFAULT_CLI_MASK };` `SET ECHO` toggles both `ses.echo` and `cli_line_set_echo` (the cli task checks `s_cli_ses.echo` each loop). Static buffers: `cli_line_t` (~1.2 KB), `s_resp[CMD_RESP_MAX]`. Print the boot banner (`HillGrow <role> <ver> — HELP for commands`).

`components/hg_store/hg_store.h`:

```c
#pragma once
#include <stdint.h>
#include "hg_cfg_types.h"
int  hg_store_init(void);          /* nvs_flash_init (erase-retry once), load zid/hw/cfg -> hg_model_boot_load */
void hg_store_start(void);         /* store task: core 0, prio 2, 3072, TWDT; wakes on flush sem or 1000 ms */
int  hg_store_flush(uint32_t timeout_ms);   /* force a pass; 0 when all dirty bits written */
uint8_t hg_store_zid(void);
int  hg_store_set_zid(uint8_t id);
int  hg_store_factory_reset(void); /* erase namespace "hg", esp_restart(); returns only on failure */
```

`components/hg_store/hg_store.c` — complete: keys `zid` (u8), `hw`, `cfg`, throttle table `{hw: 0 ms, cfg: 5000 ms}`; pass logic: for each kind with `hg_model_dirty_mask()` bit set and `now - last_write >= throttle` → `hg_model_take_dirty(kind, staging, &gen)` → `hg_blob_wrap(HG_MAGIC_*, HG_*_VER, gen, …)` into a static 368-B buffer (16 + 336, rounded) → `nvs_set_blob` + `nvs_commit` (failure: `ESP_LOGE`, re-set dirty via a retry flag, back off 30 s; 3 consecutive → log `F_NVS placeholder` — the fault store arrives in SP2); boot load: `nvs_get_blob` each key, `hg_blob_unwrap` (+ on `MIGRATED` mark dirty so it rewrites), `hg_hw_validate`/`hg_cfg_validate` → on any failure log W and pass NULL for that plane to `hg_model_boot_load` (defaults); `hg_store_flush` gives the semaphore and polls the dirty mask ≤ timeout. Single `nvs_open("hg", NVS_READWRITE, …)` kept open.

`zone/main/app_if_zone.c` — complete `app_if_t` implementation exported as `const app_if_t APP_IF_ZONE`: `zone_id` = `hg_store_zid`; `get_mac` = `esp_efuse_mac_get_default`; `node_name` = model `cfg.name`; `uptime_s` = `esp_timer_get_time()/1000000`; `status_lines` appends `  Uptime : %u s`, `  Heap min : %u`, `  Log drops : %u`, `  Cfg gen : %u`, `  Restart pending : %d`; `log_set` = `esp_log_level_set(tag ? tag : "*", lvl)` + echo `esp_log_level_get`; `time_get/_set` via `time()/localtime_r/settimeofday` (source string fixed `NONE`/`SET` in SP1); `save_flush` = `hg_store_flush`; `fw_info` via `esp_ota_get_running_partition()` + `esp_app_get_description()->version` + `esp_ota_get_state_partition`; `fw_rollback` = `esp_ota_mark_app_invalid_rollback_and_reboot()`; `fw_update` = `hg_handover_write` + `hg_reboot_to_rescue()`; `reboot` = `esp_restart`; `factory_reset` = `hg_store_factory_reset`. Also implement `void hg_reboot_to_rescue(void)` here for now: `bootloader_common_get_rtc_retain_mem()->custom[0] = 0xB0FAAF0B; bootloader_common_update_rtc_retain_mem(NULL, false); esp_restart();` (requires `bootloader_support`).

`zone/main/cmd_table_zone.c` — concatenates `CMD_COMMON_ROWS` + `ZONE_CMD_ROWS` into one static array at init (`memcpy` into `static cmd_entry_t s_table[64]`), exposes `const cmd_entry_t *zone_table(int *n)`.

`zone/main/app_main.c` (complete):

```c
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "board.h"
#include "hg_store.h"
#include "hg_model.h"
#include "notify.h"
#include "cmd_task.h"
#include "cmd_common.h"
#include "cli.h"
#include "esp_timer.h"

static const char *TAG = "hg_main";
extern const app_if_t APP_IF_ZONE;
extern const cmd_entry_t *zone_table(int *n);
static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

void app_main(void) {
    /* step 1 (spec 3.3): outputs stay hardware-safe before anything else */
    gpio_config_t oe = { .pin_bit_mask = 1ULL << HG_GPIO_PCA_OE, .mode = GPIO_MODE_OUTPUT,
                         .pull_up_en = GPIO_PULLUP_ENABLE };
    gpio_config(&oe);
    gpio_set_level(HG_GPIO_PCA_OE, 1);

    ESP_LOGI(TAG, "HillGrow zone %s boot", esp_app_get_description()->version);
    if (hg_store_init() != 0) ESP_LOGE(TAG, "store init failed (defaults active)");
    hg_store_start();

    notify_init(now_ms, hg_store_zid());
    cmd_common_init(&APP_IF_ZONE);
    int n;
    static cmd_core_t core;
    core.table = zone_table(&n); core.table_len = n;
    core.role = CMD_ROLE_ZONE; core.zone_id = hg_store_zid();
    core.now_ms = now_ms;
    core.debug_key = CONFIG_HILLGROW_DEBUG_KEY;
    cmd_task_start(&core);
    cli_init();
    cli_start();

    /* SP1 placeholder for the SP2 OTA trial (spec 3.10) */
    esp_ota_mark_app_valid_cancel_rollback();
    notify_emit(NTF_BOOT, 0, "%s POWERON", esp_app_get_description()->version);
    vTaskDelete(NULL);
}
```

`cmake/hillgrow.cmake`:

```cmake
# Read + enforce version.txt as PROJECT_VER for every app (spec 6.3)
file(READ "${CMAKE_CURRENT_LIST_DIR}/../version.txt" HG_VERSION)
string(STRIP "${HG_VERSION}" HG_VERSION)
if(NOT HG_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR "version.txt must be MAJOR.MINOR.PATCH, got '${HG_VERSION}'")
endif()
set(PROJECT_VER "${HG_VERSION}")

# Host-test gate: <app>.elf depends on a green ctest run (spec 8)
function(hillgrow_host_test_gate ELF)
    option(HILLGROW_SKIP_HOST_TESTS "Skip host unit tests" OFF)
    if(HILLGROW_SKIP_HOST_TESTS)
        return()
    endif()
    set(REPO ${CMAKE_CURRENT_LIST_DIR}/..)
    file(GLOB_RECURSE HG_TEST_DEPS ${REPO}/components/*.c ${REPO}/components/*.h ${REPO}/tests/host/*.c ${REPO}/tests/host/*.h)
    add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/host_tests_passed.stamp
        COMMAND ${CMAKE_COMMAND} -S ${REPO}/tests/host -B ${CMAKE_BINARY_DIR}/host_tests -DCMAKE_BUILD_TYPE=Release
        COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR}/host_tests --config Release --parallel
        COMMAND ctest --test-dir ${CMAKE_BINARY_DIR}/host_tests -C Release --output-on-failure
        COMMAND ${CMAKE_COMMAND} -E touch ${CMAKE_BINARY_DIR}/host_tests_passed.stamp
        DEPENDS ${HG_TEST_DEPS}
        COMMENT "HillGrow host tests")
    add_custom_target(hillgrow_host_tests DEPENDS ${CMAKE_BINARY_DIR}/host_tests_passed.stamp)
    add_dependencies(${ELF} hillgrow_host_tests)
endfunction()
```

Caveat for the executor: the host-test build inside the gate runs with the **host** toolchain even though the outer environment exports the Xtensa one — pass `-DCMAKE_C_COMPILER=cl` if the outer `CC` leaks in (verify on first build; on this machine `cmake` picks VS2019 by generator default).

`zone/CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.16)
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/hillgrow.cmake)
set(SDKCONFIG_DEFAULTS "sdkconfig.defaults")
list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../components")
set(BOOTLOADER_EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../bootloader_components/main")  # consumed in Task 15
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(hillgrow_zone)
hillgrow_host_test_gate(hillgrow_zone.elf)
```

`zone/sdkconfig.defaults`:

```
CONFIG_IDF_TARGET="esp32"
CONFIG_HG_ROLE_ZONE=y
CONFIG_FREERTOS_HZ=1000
CONFIG_BT_ENABLED=n
CONFIG_COMPILER_OPTIMIZATION_SIZE=y
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_OFFSET=0xE000
CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y
CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC=y
CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC_SIZE=0x100
CONFIG_ESP_TASK_WDT_TIMEOUT_S=8
CONFIG_ESP_TASK_WDT_PANIC=y
CONFIG_ESP_INT_WDT_TIMEOUT_MS=300
CONFIG_ESP_BROWNOUT_DET_LVL_SEL_7=y
CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192
CONFIG_LOG_DEFAULT_LEVEL_INFO=y
CONFIG_LOG_MAXIMUM_LEVEL_DEBUG=y
```

(Verify during the build that the log-v1 pin symbol exists in IDF 6.0.1 — `grep -r "LOG_VERSION" $IDF_PATH/components/log/Kconfig` — and add `CONFIG_LOG_VERSION_1=y` if the choice symbol is present; add the `_Static_assert(CONFIG_LOG_VERSION == 1, "pin log v1")` in `cli.c` guarded by `#ifdef CONFIG_LOG_VERSION`.)

`zone/partitions.csv`:

```
# Name,    Type, SubType, Offset,   Size
nvs,       data, nvs,     0x10000,  0x10000
otadata,   data, ota,     0x20000,  0x2000
phy_init,  data, phy,     0x22000,  0x1000
factory,   app,  factory, 0x30000,  0x140000
ota_0,     app,  ota_0,   0x170000, 0x140000
ota_1,     app,  ota_1,   0x2B0000, 0x150000
```

`zone/main/CMakeLists.txt`: `idf_component_register(SRCS "app_main.c" "app_if_zone.c" "cmd_table_zone.c" INCLUDE_DIRS "." REQUIRES board cli cmd_task cmd_common zone_cmds hg_store hg_model hg_cfg notify rescue_handover app_update esp_timer bootloader_support esp_driver_gpio)`.

Steps:

- [ ] **Step 1:** Write all files above (target components with the complete code / specified content).
- [ ] **Step 2:** Build. Run (PowerShell): `& C:\esp\v6.0.1\esp-idf\export.ps1; idf.py -C zone build`
  Expected: host tests run first (gate), then `hillgrow_zone.bin` links; note the binary size — must be < 1 280 KB (the smaller OTA slot). If `BOOTLOADER_EXTRA_COMPONENT_DIRS` errors before Task 15 lands, comment that line for now (the default bootloader is fine until Task 15).
- [ ] **Step 3:** Fix compile errors until clean (IDF 6 driver names: `esp_driver_uart`, `esp_driver_gpio`; `uart_vfs_dev_use_driver` lives in `esp_driver_uart`'s `uart_vfs.h`).
- [ ] **Step 4: Commit** `feat(zone): first bootable zone app — CLI + config model + store` (+ push).

---

### Task 14: Master app skeleton

**Files:**
- Create: `master/CMakeLists.txt`, `master/sdkconfig.defaults`, `master/partitions.csv`, `master/main/CMakeLists.txt`, `master/main/app_main.c`, `master/main/app_if_master.c`, `master/main/cmd_table_master.c`

**Interfaces:** Consumes everything the zone uses except `zone_cmds`/`hg_model`/`hg_store` config rows. Produces `master.bin` — CLI on UART0, role MASTER (`GET ID` → `OK ID MASTER …`), config commands answer `ERR NOT_IMPLEMENTED` via app_if stubs (the master model arrives in SP3/SP4); Wi-Fi/httpd deliberately absent.

- [ ] **Step 1:** Copy the zone app structure with: `CONFIG_HG_ROLE_MASTER=y`; 8 MB partitions (`master/partitions.csv`):

```
# Name,    Type, SubType, Offset,   Size
nvs,       data, nvs,     0x10000,  0x10000
otadata,   data, ota,     0x20000,  0x2000
phy_init,  data, phy,     0x22000,  0x1000
factory,   app,  factory, 0x30000,  0x140000
ota_0,     app,  ota_0,   0x170000, 0x200000
ota_1,     app,  ota_1,   0x370000, 0x200000
zone_fw,   data, 0x40,    0x570000, 0x180000
data,      data, spiffs,  0x6F0000, 0x110000
```

plus `CONFIG_ESPTOOLPY_FLASHSIZE_8MB=y`; `app_if_master.c` (exports `const app_if_t APP_IF_MASTER`): same as zone but `save_flush`/`factory_reset` operate on a master NVS holding nothing yet (return 0), `fw_update` returns −1 (`ERR NOT_IMPLEMENTED` comes from the zone-only gate anyway), `zone_id` returns 0, `node_name` "master"; `cmd_table_master.c` = `CMD_COMMON_ROWS` only; `app_main.c` = zone's minus the OE GPIO block (master has no PCA9685) and with `project(hillgrow_master)`.

- [ ] **Step 2:** `idf.py -C master build` — clean, size < 2 048 KB.
- [ ] **Step 3:** Flash + smoke on a board if one is connected (`C:\Python311\python tools/flash_all.py --app master --port COMx` after Task 15; before Task 15, `idf.py -C master flash` is acceptable ONCE — it writes the app into the `factory` slot, which Task 15's full flash overwrites with rescue).
- [ ] **Step 4: Commit** `feat(master): master app skeleton with shared CLI` (+ push).

---

### Task 15: Custom bootloader + flash tooling

**Files:**
- Create: `bootloader_components/main/CMakeLists.txt`, `bootloader_components/main/bootloader_start.c`
- Create: `tools/flash_all.py`, `tools/flash_app.py`
- Modify: `zone/CMakeLists.txt`, `master/CMakeLists.txt` (the `BOOTLOADER_EXTRA_COMPONENT_DIRS` line becomes active)

**Interfaces:**
- Consumes: partition tables from Tasks 13/14; the RTC flag `0xB0FAAF0B` written by `hg_reboot_to_rescue()`.
- Produces: boot selection per spec §1.4 — RTC flag → factory once; GPIO15 low ≥ 10 s at power-on → factory; GPIO15 low 1–9 s → erase the `nvs` partition then boot normally; < 1 s → ignored. Plus the two flash helpers every later task uses.

`bootloader_components/main/CMakeLists.txt`:

```cmake
idf_component_register(SRCS "bootloader_start.c" REQUIRES bootloader bootloader_support)
idf_build_get_property(target IDF_TARGET)
set(scripts "${IDF_PATH}/components/bootloader/subproject/main/ld/${target}/bootloader.ld"
            "${IDF_PATH}/components/bootloader/subproject/main/ld/${target}/bootloader.rom.ld")
target_linker_script(${COMPONENT_LIB} INTERFACE "${scripts}")
```

`bootloader_components/main/bootloader_start.c` (complete — modelled on IDF `examples/custom_bootloader/bootloader_override` and the kw-bootloader pattern, inverted for an idle-high button):

```c
#include <stdbool.h>
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "bootloader_init.h"
#include "bootloader_utility.h"
#include "bootloader_common.h"
#include "esp32/rom/gpio.h"
#include "soc/gpio_periph.h"

static const char *TAG = "hg_boot";
#define HG_RESCUE_FLAG   0xB0FAAF0Bu
#define HG_RESCUE_GPIO   15
#define HG_HOLD_FACTORY_MS 10000u
#define HG_HOLD_ERASE_MS   1000u

void __attribute__((noreturn)) call_start_cpu0(void) {
    if (bootloader_init() != ESP_OK) bootloader_reset();
    bootloader_state_t bs = {0};
    if (!bootloader_utility_load_partition_table(&bs)) {
        ESP_LOGE(TAG, "partition table load failed");
        bootloader_reset();
    }
    int boot_index = bootloader_utility_get_selected_boot_partition(&bs);
    if (boot_index == INVALID_INDEX) bootloader_reset();

    if (esp_rom_get_reset_reason(0) != RESET_REASON_CORE_DEEP_SLEEP) {
        /* 1. software rescue request via RTC retain memory */
        bootloader_common_update_rtc_retain_mem(NULL, true);
        rtc_retain_mem_t *rtc = bootloader_common_get_rtc_retain_mem();
        uint32_t *custom = (uint32_t *)rtc->custom;
        bool to_rescue = (custom[0] == HG_RESCUE_FLAG);
        custom[0] = 0;
        bootloader_common_update_rtc_retain_mem(NULL, false);
        if (to_rescue) {
            ESP_LOGI(TAG, "rescue flag set -> factory");
            bootloader_utility_load_boot_image(&bs, FACTORY_INDEX);
        }

        /* 2. rescue button: GPIO15, idle high (pull-up), pressed = low */
        gpio_pad_select_gpio(HG_RESCUE_GPIO);
        PIN_INPUT_ENABLE(GPIO_PIN_MUX_REG[HG_RESCUE_GPIO]);
        gpio_pad_pullup(HG_RESCUE_GPIO);
        if (GPIO_INPUT_GET(HG_RESCUE_GPIO) == 0) {
            uint32_t t0 = esp_log_early_timestamp(), held;
            do {
                held = esp_log_early_timestamp() - t0;
                if (held >= HG_HOLD_FACTORY_MS) {
                    ESP_LOGI(TAG, "button held %u ms -> factory", (unsigned)held);
                    bootloader_utility_load_boot_image(&bs, FACTORY_INDEX);
                }
            } while (GPIO_INPUT_GET(HG_RESCUE_GPIO) == 0);
            if (held >= HG_HOLD_ERASE_MS) {
                ESP_LOGW(TAG, "button held %u ms -> erase nvs", (unsigned)held);
                if (!bootloader_common_erase_part_type_data("nvs", false))
                    ESP_LOGE(TAG, "nvs erase failed");
            }
        }
    }
    bootloader_utility_load_boot_image(&bs, boot_index);
}

struct _reent *__getreent(void) { return _GLOBAL_REENT; }
```

`tools/flash_app.py` (complete): argparse `--app zone|master|rescue --port COMx [--baud 460800]`; offsets table `APP_OFFSET = {"zone": 0x170000, "master": 0x170000, "rescue": 0x30000}`; runs the IDF venv esptool: for zone/master → `write-flash 0x20000 <app>/build/ota_data_initial.bin 0x170000 <app>/build/hillgrow_<app>.bin`; for rescue → `write-flash 0x30000 rescue/build/hillgrow_rescue.bin`. `tools/flash_all.py`: `--board zone|master --port COMx` → bootloader `0x1000` (from that app's build), partition table `0xE000`, `ota_data_initial.bin 0x20000`, rescue `0x30000`, app `0x170000`. Both wrap `subprocess.run([sys.executable, "-m", "esptool", "--chip", "esp32", "-p", port, "-b", "460800", "write-flash", ...])` and print the exact command first.

Steps:

- [ ] **Step 1:** Verify the override hook mechanism in this IDF: `grep -rn "BOOTLOADER_EXTRA_COMPONENT_DIRS" C:\esp\v6.0.1\esp-idf\components\bootloader\project_include.cmake` and `dir C:\esp\v6.0.1\esp-idf\examples\custom_bootloader\bootloader_override`. If the variable is not consumed there, use the example's mechanism instead (its README documents the supported way — likely a project-adjacent `bootloader_components/` directory; then create thin per-app wrapper dirs `zone/bootloader_components/main/CMakeLists.txt` that `include()` the shared sources by absolute path). Also verify `bootloader_common_erase_part_type_data` exists (`grep -rn erase_part_type_data C:\esp\v6.0.1\esp-idf\components\bootloader_support\include`).
- [ ] **Step 2:** Write the files above; rebuild both apps: `idf.py -C zone build; idf.py -C master build`.
  Expected: build log shows the custom bootloader compiling (`hg_boot`); `zone/build/bootloader/bootloader.bin` ≤ 53 248 bytes (fits below the 0xE000 table).
- [ ] **Step 3 (board on COMx):** `C:\Python311\python tools/flash_all.py --board zone --port COMx` (rescue slot still empty — flash_all skips a missing rescue bin with a warning until Task 16). Open the monitor (`idf.py -C zone monitor -p COMx`):
  - normal boot → `hg_boot` banner then the zone app;
  - hold the GPIO15 button 1–9 s during reset → `erase nvs` log, app boots with defaults (`GET ID` shows zone 254-behaviour: id 0);
  - hold ≥ 10 s → boot falls into the factory slot (garbage until Task 16 — expect `invalid image` log, which proves the path).
- [ ] **Step 4: Commit** `feat(bootloader): rescue/NVS-erase boot selection + flash helpers` (+ push).

---

### Task 16: Rescue image

**Files:**
- Create: `rescue/CMakeLists.txt` (same skeleton as zone, `project(hillgrow_rescue)`, host-test gate NOT applied — rescue links no pure components except `rescue_handover`/`hg_blob`/`app_version`/`board`), `rescue/sdkconfig.defaults`, `rescue/partitions.csv` (copy of `zone/partitions.csv` — the build needs a table; the image is flashed into whatever table is on the board), `rescue/main/CMakeLists.txt` (`EMBED_TXTFILES "upload.html"`), `rescue/main/rescue.h` (declares `rescue_wifi_sta/ap`, `rescue_pull`, `rescue_http_start`, `rescue_target_slot`), `rescue/main/app_main.c`, `rescue/main/rescue_wifi.c`, `rescue/main/rescue_http.c`, `rescue/main/rescue_pull.c`, `rescue/main/ring_fwd.c`, `rescue/main/upload.html`

**Interfaces:**
- Consumes: `hg_handover_take()` (reads AND erases the record), `board.h`, the partition layout.
- Produces: `rescue.bin` per spec §1.4/§6.1 — pull mode then manual AP fallback; never touches GPIO23/I²C.

`rescue/sdkconfig.defaults` (beyond the zone's baseline lines): `CONFIG_ESP_TASK_WDT_TIMEOUT_S=30`, `CONFIG_LOG_DEFAULT_LEVEL_WARN=y`, `CONFIG_LWIP_IPV6=n`, `CONFIG_ESP_WIFI_ENTERPRISE_SUPPORT=n`, `CONFIG_ESP_HTTP_CLIENT_ENABLE_HTTPS=n`, `CONFIG_HTTPD_MAX_REQ_HDR_LEN=1024`; keep `CONFIG_COMPILER_OPTIMIZATION_SIZE=y`.

Core logic (write in full):

`rescue/main/app_main.c` — LED helper (esp_timer periodic toggling GPIO2: 5 Hz pull, 1 Hz manual, 3-blink burst on error); `nvs_flash_init` (erase-retry); start `ring_fwd`; `hg_handover_t h; if (hg_handover_take(&h) == 0) { for (int a = 0; a < 3; a++) { if (rescue_wifi_sta(h.ssid, h.pass, 20000) == 0 && rescue_pull(h.url) == 0) { esp_restart(); } vTaskDelay(pdMS_TO_TICKS(5000)); } }` then `rescue_wifi_ap()` + `rescue_http_start()` and idle loop feeding the TWDT.

`rescue/main/rescue_pull.c` (complete):

```c
#include <string.h>
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_ota_ops.h"
#include "esp_task_wdt.h"
#include "rescue.h"

static const char *TAG = "pull";

const esp_partition_t *rescue_target_slot(void) {
    const esp_partition_t *boot = esp_ota_get_boot_partition();   /* the app that requested rescue */
    const esp_partition_t *ota0 = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
    const esp_partition_t *ota1 = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, NULL);
    if (!ota0 || !ota1) return NULL;
    if (boot && boot->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0) return ota1;
    return ota0;   /* boot==ota_1, factory, or undefined -> ota_0 */
}

int rescue_pull(const char *url) {
    const esp_partition_t *dst = rescue_target_slot();
    if (!dst) return -1;
    esp_http_client_config_t cfg = { .url = url, .timeout_ms = 10000 };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) return -1;
    int rc = -1;
    esp_ota_handle_t ota = 0;
    if (esp_http_client_open(cli, 0) == ESP_OK) {
        int64_t len = esp_http_client_fetch_headers(cli);
        int status = esp_http_client_get_status_code(cli);
        ESP_LOGW(TAG, "GET %s -> %d, %lld bytes -> %s", url, status, (long long)len, dst->label);
        if (status == 200 && len > 0 && len <= (int64_t)dst->size &&
            esp_ota_begin(dst, (size_t)len, &ota) == ESP_OK) {
            static char buf[4096];
            int n;
            rc = 0;
            while ((n = esp_http_client_read(cli, buf, sizeof buf)) > 0) {
                esp_task_wdt_reset();
                if (esp_ota_write(ota, buf, (size_t)n) != ESP_OK) { rc = -1; break; }
            }
            if (n < 0) rc = -1;
            if (rc == 0 && esp_ota_end(ota) != ESP_OK) rc = -1;
            else if (rc != 0) esp_ota_abort(ota);
            if (rc == 0 && esp_ota_set_boot_partition(dst) != ESP_OK) rc = -1;
        }
    }
    esp_http_client_cleanup(cli);
    return rc;
}
```

`rescue/main/rescue_http.c` — httpd (stack 8192): `GET /` serves the embedded `upload.html` after substituting `%RUNNING%`/`%SLOTS%` (running rescue version via `esp_app_get_description()`, per-slot `esp_ota_get_partition_description` version or `empty`); `POST /upload` streams `req->content_len` through a 4 KB buffer into `rescue_target_slot()` with the same begin/write/end/set_boot sequence (reject `content_len <= 0` or > slot size with HTTP 400, `esp_task_wdt_reset` per chunk), replies `OK <ver> — rebooting` and schedules `esp_restart()` via a 1 s esp_timer; `POST /reboot` → restart. `upload.html`: one form (`<input type=file>` + XHR POST of the raw file body with a progress bar, ~60 lines, no framework).

`rescue/main/rescue_wifi.c` — `rescue_wifi_sta(ssid, pass, timeout_ms)`: netif/event init, `esp_netif_create_default_wifi_sta`, connect with an event-group wait for IP; `rescue_wifi_ap()`: `esp_netif_create_default_wifi_ap`, **fixed IP**: `esp_netif_dhcps_stop → esp_netif_set_ip_info(192.168.7.7/24, gw 192.168.7.7) → esp_netif_dhcps_start`; SSID `HillGrow-Rescue-%02X%02X%02X` from the last 3 MAC bytes, WPA2 pass `hillgrow1`, max 2 stations.

`rescue/main/ring_fwd.c` (complete): install UART2 (rx 1024/tx 1024, pins from `board.h`), task prio 5/2048, `n = uart_read_bytes(UART_NUM_2, buf, 128, pdMS_TO_TICKS(100)); if (n > 0) uart_write_bytes(UART_NUM_2, buf, n);` + TWDT.

Steps:

- [ ] **Step 1:** Write all files. **Step 2:** `idf.py -C rescue build` — clean; `hillgrow_rescue.bin` **≤ 1 280 KB** (record the size in the commit message; if over, first levers: `CONFIG_LWIP_TCP_*` buffers, WPA3 off, `CONFIG_COMPILER_OPTIMIZATION_CHECKS_SILENT=y`).
- [ ] **Step 3 (bench, board on COMx):** `python tools/flash_all.py --board zone --port COMx` (now includes rescue). Verify: hold GPIO15 ≥ 10 s at reset → LED 1 Hz, AP `HillGrow-Rescue-xxxxxx` appears; join it (pass `hillgrow1`), open `http://192.168.7.7` → upload page shows slot versions; upload `zone/build/hillgrow_zone.bin` → reboots into the zone app from an OTA slot (`GET FW` shows the slot + `VALID` after the SP1 auto-mark).
- [ ] **Step 4 (bench pull mode):** on a PC on any Wi-Fi AP the board can reach (e.g. phone hotspot, or the master's AP in SP4 — for now `python -m http.server 80` in `zone/build` on a laptop hotspot): on the zone console `DEBUG ENABLE hill` then `SET FW UPDATE <ssid> <pass> http://<pc-ip>/hillgrow_zone.bin` → device reboots to rescue (LED 5 Hz), pulls, reboots into the new image. `GET VERSION` confirms.
- [ ] **Step 5: Commit** `feat(rescue): pull + manual-AP recovery image with ring repeater` (+ push).

---

### Task 17: `tools/uart_test.py` + hardware smoke suite

**Files:**
- Create: `tools/uart_test.py`, `tools/requirements.txt` (`pyserial>=3.5`)
- Modify: `docs/pin-mapping.md` (tick the bring-up boxes exercised here), `docs/what_we_learned.md` (any surprises)

**Interfaces:** Consumes the live CLI. Produces the regression harness used from SP2 on.

- [ ] **Step 1:** Write `tools/uart_test.py` (complete, ~200 lines): class `Node(port, baud=115200)` with `send(line, timeout=4.0)` collecting the first `OK|ERR` line + two-space continuations, dropping `^[IWEDV] \(\d+\)` log lines, side-collecting `NOTIFY`; prologue `SET ECHO OFF`, `SET LOG WARN`, `SET NOTIFY ALL OFF`, `GET ID` (parses role). Suites (`--suite smoke` default): **ID/VERSION** (`GET ID` role matches `--role` if given; `GET VERSION` parses `M.m.p`); **HELP self-check** (bare `HELP` non-empty; every `+ SET|GET <NOUN…>` first line re-queried as `<prefix> HELP` returns itself); **ERRORS** (`FOO` → `ERR UNKNOWN_CMD`, `SET WATER 1 TARGET 101` → `OUT_OF_RANGE` zone-only, `SET POKE ON`-class unlock row → `LOCKED`); **CONFIG round-trip** (zone: `SET WATER 1 TARGET 61` → `GET WATER 1` shows `Target : 61`; `SET SHELF 1 CROP Basil` → echo preserved); **PERSIST** (`SAVE`, then with `--allow-reboot`: `REBOOT CONFIRM`, reconnect, `GET WATER 1` still 61); **SESSION** (`SET LOG DEBUG` echoes `DEBUG`; `GET NOTIFY` mask lines). Exit code = number of failures; `--json out.json` dump.
- [ ] **Step 2 (zone board):** `C:\Python311\python tools\uart_test.py COMx --role ZONE --allow-reboot` → 0 failures. **Step 3 (master board):** same with `--role MASTER` (config suites auto-skip on master). 
- [ ] **Step 4:** Tick in `docs/pin-mapping.md`: rescue button verified; note in `docs/what_we_learned.md` anything the bring-up taught. **Step 5: Commit** `feat(tools): uart_test.py smoke harness; SP1 hardware verified` (+ push).

---

## Troubleshooting

| Symptom | Likely cause | Action |
|---|---|---|
| First host-test configure fails downloading Unity | no network / proxy | retry once; or pre-place the tarball per FetchContent docs |
| `ctest` reports 0 tests | multi-config generator without `-C Release` | always pass `--config Release` / `-C Release` |
| Host build picks the Xtensa compiler | IDF export leaked `CC` into the gate | configure host tests from a clean shell or pass `-DCMAKE_C_COMPILER=cl` |
| `idf.py build` rebuilds host tests every time | glob DEPENDS touched (expected) or clock skew | harmless; `-DHILLGROW_SKIP_HOST_TESTS=ON` for quick iterations |
| Custom bootloader ignored | `BOOTLOADER_EXTRA_COMPONENT_DIRS` name wrong for this IDF | Task 15 Step 1 fallback: per-app `bootloader_components/` wrapper dir |
| Bootloader too big (> 0xD000) | log level | `CONFIG_BOOTLOADER_LOG_LEVEL_ERROR=y` |
| App boots into rescue after `idf.py flash` | plain `flash` wrote the app to the factory slot | use `tools/flash_app.py` / `flash_all.py`; re-flash rescue |
| OTA-uploaded image boots once then reverts | `mark_app_valid` call missing/crashed before running | check the SP1 auto-mark in `app_main`; `GET FW` shows `PENDING` |
| `ERR LOCKED` on `SET HW …` | by design | `DEBUG ENABLE hill` first (Kconfig key) |
| Garbage on the console while typing | log burst redraw on a non-ANSI terminal | use PuTTY/idf monitor, or `SET LOG NONE` during typing |
| `SET FW UPDATE` says `ERR NOT_LOCAL` | sent over HTTP/ring | local console only, by design |

## Integration Verification (end of SP1)

1. `idf.py -C zone build && idf.py -C master build && idf.py -C rescue build` — all clean; host tests ran as gates; sizes: zone < 1280 K, master < 2048 K, rescue < 1280 K.
2. Zone board: full flash, console answers `HELP`, `GET ID`, config edit + `SAVE` + power-cycle persists, NVS-erase hold resets to defaults, rescue hold + manual upload works, bench `SET FW UPDATE` pull works.
3. Master board: full flash, `GET ID` → `MASTER`, `SET ZONE 2 …` → `ERR ZONE_UNKNOWN` (ring lands in SP3).
4. `uart_test.py` green on both roles.
5. TWDT framework active: `CONFIG_ESP_TASK_WDT_PANIC=y`, idle tasks watched, every app task subscribed — verify once by inserting a temporary `while(1);` into the cli0 loop and observing a panic+reboot within 8 s (then remove it).

## Post-implementation checklist

- [ ] All host tests pass and the gate is active (`idf.py build` fails when a test is red — verify once by breaking a test deliberately)
- [ ] No file > ~300 lines (`git ls-files "components/*.c" | xargs wc -l | sort -n`)
- [ ] `cmd_table_check` returns −1 at boot on both roles (check the boot log)
- [ ] Malformed console input (binary noise, 300-char lines, Ctrl-C) never crashes a node
- [ ] `docs/pin-mapping.md` bring-up boxes for SP1 items ticked; `what_we_learned.md` updated
- [ ] `version.txt` still `0.1.0`; every commit pushed to `main`
