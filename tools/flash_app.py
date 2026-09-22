#!/usr/bin/env python3
"""Flash a single HillGrow application (its OTA app slot + a freshly
generated otadata image selecting it) without touching the bootloader or
partition table.

Usage:
    python tools/flash_app.py --app zone|master|rescue|zonefw|cpfw --port COMx
                               [--baud 460800] [--target esp32|esp32p4]
                               [--build-dir DIR] [--dry-run]

Must run under a python that has esptool installed -- either the IDF venv
python (source C:\\esp\\v6.0.1\\esp-idf\\export.ps1 first) or any python
with `pip install esptool`.

--build-dir overrides the directory the flashed app/rescue/zonefw image is
read from (default: <repo>/<app>/build -- today's hard-coded path, unchanged
unless you pass this). Needed whenever the binaries actually built for
--target don't live in that default directory -- e.g. after building master
for esp32p4 into a separate out-of-tree dir, `--target esp32` with no
--build-dir would silently read those P4 binaries out of master/build and
still write them at the ESP32 offsets. Not used by --app cpfw, which takes
its image from --cp-image instead (see below).

Every image that will actually RUN on --target (the rescue app, and the
master/zone OTA app image) is checked against --target by reading its own
esp_image_header_t chip_id before it's flashed -- see tools/hg_image.py.
On mismatch this refuses rather than flashing a right-offset,
wrong-architecture image. --app zonefw and --app cpfw are deliberately
exempt from *that* check -- see the next paragraph -- but they are not
unchecked: each gets its own inner-header check against the chip its
partition's on-device consumer requires, independent of --target.

--app zonefw and --app cpfw are different in kind from the app-slot cases
(zone/master/rescue): neither flashes an app slot -- no otadata write,
nothing boots from either one directly.

--app zonefw writes zone/build/hillgrow_zone.bin (or --build-dir, if given),
HGFW-header-prefixed, into the MASTER's "zone_fw" data partition (offset
0x570000 on esp32, 0xA30000 on esp32p4; see master's partitions*.csv and
components/fw_srv/fw_srv.h) so the master's fw_srv component can serve it at
GET /fw/zone.bin for a fleet update (Task 15). --port therefore names the
MASTER's serial port here, not a zone's.

--app cpfw (esp32p4 only) stages a raw ESP32-C6 co-processor image,
HGFW-header-prefixed like zonefw's (build_cpfw_image() -- same 16-byte
header, same shared writer as build_zonefw_image()), into the MASTER's
"cp_fw" data partition (offset 0xBB0000, size 0x180000; subtype 0x41 -- a
DATA partition, not an app/ota slot, so this gets its own branch here rather
than falling into the generic app-slot path, which would wrongly write an
otadata selecting ota_0). The header matters here, not just for zone_fw:
cp_fw is a data partition too, so components/cp_ota/cp_ota.c can't recover
the real image length by parsing esp_image segments either, and it validates
the header's crc32 before ever touching the radio over RPC.

--cp-image PATH is REQUIRED with --app cpfw. The co-processor project lives
in this repo at coproc/ and its image is coproc/build/eh_cp_wifi_softap.bin:

    idf.py -C coproc set-target esp32c6     # first time only
    idf.py -C coproc build

There is still no default build directory for it (and --build-dir does not
apply) because coproc/ is a different TARGET with its own partition table, so
it is not one of the <repo>/<app>/build apps this tool flashes -- the path is
passed explicitly on purpose. coproc/README.md gives this same invocation and
points back here for the HGFW_HDR_LEN/HGFW_MAGIC header this tool writes.
Building it needs CONFIG_EH_TRANSPORT_CP_SDIO_MODE_STREAM=y (set in
coproc/sdkconfig.defaults): esp_hosted's default SW_AGGR SDIO mode needs an
ESP-IDF patch that the shared 6.0.1 install lacks. Do NOT run
`eh.py patch-idf` against C:\\esp\\v6.0.1 -- that patches the shared install
for every project that uses it, not just coproc.
"""
import argparse
import binascii
import os
import struct
import subprocess
import sys

import hg_image
import hg_otadata

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

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

# HGFW header shared by every data-type "staged firmware" partition -- today
# zone_fw and cp_fw, both size 0x180000. Neither partition is app-type, so
# neither can have its payload length recovered by parsing esp_image
# segments (that's bootloader territory); the length has to be carried
# explicitly. { magic 'HGFW' u32 LE, len u32 LE, crc32 u32 LE, rsvd u32 },
# checked on the device side by components/fw_srv/fw_srv.c's
# validate_image() (zone_fw) and components/cp_ota/cp_ota.c's
# cp_ota_parse_header() (cp_fw) -- both read the SAME 16-byte layout.
HGFW_HDR_LEN = 16
HGFW_MAGIC = 0x57464748   # 'HGFW' LE -- see components/fw_srv/fw_srv.c's FW_HDR_MAGIC
                          # and components/cp_ota/cp_ota.h's CP_OTA_HDR_MAGIC

ZONE_FW_PART_SIZE = 0x180000
ZONE_FW_MAX_IMAGE = ZONE_FW_PART_SIZE - HGFW_HDR_LEN
CP_FW_PART_SIZE = 0x180000   # master/partitions_p4.csv: cp_fw, same size as zone_fw
CP_FW_MAX_IMAGE = CP_FW_PART_SIZE - HGFW_HDR_LEN

# Back-compat aliases -- nothing outside this module used the old names, but
# keeping them cheap avoids a silent behavior change for anything that does.
ZONE_FW_HDR_LEN = HGFW_HDR_LEN
ZONE_FW_MAGIC = HGFW_MAGIC


def build_hgfw_image(src_bin_path, out_dir, out_name, max_image, what):
    """Prepends the 16-byte HGFW header (see HGFW_HDR_LEN/HGFW_MAGIC above)
    ahead of the raw image at +16. Shared by --app zonefw (build_zonefw_image,
    zone_fw) and --app cpfw (build_cpfw_image, cp_fw) -- same header format,
    same partition size, same device-side check shape (fw_srv.c's
    validate_image() / cp_ota.c's cp_ota_parse_header()), so this is written
    once rather than as two near-identical header writers.

    crc32 is plain binascii.crc32(image) (seed 0 -- the standard zlib/
    CRC-32-ISO-HDLC convention): the SAME check value family hg_blob.c's
    hg_crc32(0, ...) computes, which both device-side validators verify
    against. This is NOT hg_otadata.py's 0xFFFFFFFF-seeded esp_rom_crc32_le
    convention (a different check value family used for a different,
    bootloader-owned structure) -- do not conflate the two.

    `what` names the image kind in the size-limit error message (e.g. "zone"
    or "coprocessor"). Refuses images that wouldn't fit the target partition
    once the header is added."""
    with open(src_bin_path, "rb") as f:
        image = f.read()
    if len(image) > max_image:
        sys.exit(f"error: {what} image too large for its partition ({len(image)} > "
                 f"{max_image} bytes = {max_image + HGFW_HDR_LEN:#x} - {HGFW_HDR_LEN})")
    crc = binascii.crc32(image) & 0xFFFFFFFF
    header = struct.pack("<IIII", HGFW_MAGIC, len(image), crc, 0)
    os.makedirs(out_dir, exist_ok=True)
    out_path = os.path.join(out_dir, out_name)
    with open(out_path, "wb") as f:
        f.write(header + image)
    return out_path


def build_zonefw_image(zone_bin_path, out_dir):
    """--app zonefw: stages a zone app image behind the HGFW header for the
    MASTER's zone_fw partition. See build_hgfw_image() for the format."""
    return build_hgfw_image(zone_bin_path, out_dir, "hg_zonefw.bin", ZONE_FW_MAX_IMAGE, "zone")


def build_cpfw_image(cp_bin_path, out_dir):
    """--app cpfw: stages a raw ESP32-C6 co-processor image (eh_cp's
    eh_cp_wifi_softap.bin) behind the HGFW header for the MASTER's cp_fw
    partition. See build_hgfw_image() for the format."""
    return build_hgfw_image(cp_bin_path, out_dir, "hg_cpfw.bin", CP_FW_MAX_IMAGE, "coprocessor")


def build_dir(app, override=None):
    """override, when given, replaces the whole default <repo>/<app>/build
    path -- see --build-dir in the module docstring."""
    return override if override else os.path.join(REPO_ROOT, app, "build")


def require_file(path, what):
    if not os.path.isfile(path):
        sys.exit(f"error: {what} not found: {path}\n"
                 f"  (build {os.path.dirname(path)} first)")
    if os.path.getsize(path) == 0:
        sys.exit(f"error: {what} is zero bytes (empty/truncated build?): {path}")
    return path


def run_esptool(cmd):
    print(" ".join(cmd))
    try:
        subprocess.run(cmd, check=True)
    except subprocess.CalledProcessError as e:
        sys.exit(f"error: esptool failed (exit {e.returncode}) -- if this is "
                  f"\"No module named esptool\", run this tool under the IDF venv "
                  f"python (source export.ps1 first) or pip install esptool")


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                      formatter_class=argparse.RawDescriptionHelpFormatter)
    all_apps = set()
    for target_offsets in APP_OFFSET.values():
        all_apps.update(target_offsets)
    parser.add_argument("--app", required=True, choices=sorted(all_apps))
    parser.add_argument("--port", required=True)
    parser.add_argument("--baud", default="460800")
    parser.add_argument("--target", default="esp32", choices=sorted(APP_OFFSET),
                         help="chip the OFFSETS are for; default esp32")
    parser.add_argument("--build-dir", default=None, metavar="DIR",
                         help="override the build directory the flashed image is read "
                              "from (default: <repo>/<app>/build); see module docstring. "
                              "Not used by --app cpfw.")
    parser.add_argument("--cp-image", default=None, metavar="PATH",
                         help="path to the coprocessor image (e.g. eh_cp_wifi_softap.bin); "
                              "REQUIRED with --app cpfw. See module docstring for where it "
                              "comes from and how it's built.")
    parser.add_argument("--dry-run", action="store_true",
                         help="print the esptool command without executing it")
    args = parser.parse_args()

    offsets = APP_OFFSET[args.target]
    if args.app not in offsets:
        parser.error(f"--app {args.app} is not a thing on {args.target} "
                     f"(valid: {', '.join(sorted(offsets))})")

    if args.app == "cpfw":
        # cp_fw is a DATA partition (subtype 0x41), not an app/ota slot -- like
        # zonefw, this gets its own branch: no otadata write, nothing boots
        # from it directly. (An earlier build of this tool let --app cpfw fall
        # through to the generic app-slot branch below, which wrote a bogus
        # otadata selecting ota_0 -- see the task-6 brief.) Fix round 1: also
        # stages the raw image behind the HGFW header (build_cpfw_image(),
        # same shape as build_zonefw_image()) rather than writing it raw --
        # cp_fw is a data partition, so cp_ota.c can't recover the real image
        # length by parsing esp_image segments, and the first cut of this
        # branch left it pushing the whole partition, trailing erased bytes
        # and all.
        if not args.cp_image:
            parser.error("--app cpfw requires --cp-image PATH (see --help for where it comes from)")
        cp_bin = require_file(args.cp_image, "coprocessor image")
        # No hg_image.require_target_chip() here on purpose: cp_bin is an
        # ESP32-C6 radio image staged as a PAYLOAD into the master's cp_fw
        # data partition, not something that runs on --target (the master
        # chip). Checking it against --target would reject every legitimate
        # --app cpfw call. See tools/hg_image.py's module docstring.
        cpfw_bin = build_cpfw_image(cp_bin, build_dir("master"))
        # Its own check instead, kept deliberately separate from the --target
        # guard because the two answers are SUPPOSED to differ here: cp_fw's
        # payload must be an esp32c6 image whatever --target is, because what
        # reads it back is components/cp_ota/cp_ota.c, which pushes it
        # straight into the radio over esp_hosted RPC. The only recovery from
        # a half-written radio is the board's C6-UART header (see
        # coproc/README.md), which makes this the most destructive unguarded
        # path this tool had. Checked on the STAGED file -- the exact bytes
        # esptool is about to write -- so the inner esp_image_header_t is at
        # +HGFW_HDR_LEN, behind the header build_cpfw_image() just wrote.
        hg_image.require_payload_chip(cpfw_bin, "esp32c6", "coprocessor image",
                                      offset=HGFW_HDR_LEN, staged_from=cp_bin)
        write_flash_args = [hex(offsets["cpfw"]), cpfw_bin]
    elif args.app == "zonefw":
        # Builds nothing (per the brief): takes the zone app's own build
        # output and re-packages it for the MASTER's zone_fw partition --
        # --port/--baud below address the master board, not a zone.
        zone_bin = require_file(os.path.join(build_dir("zone", args.build_dir), "hillgrow_zone.bin"),
                                 "hillgrow_zone.bin (build zone first)")
        # No hg_image.require_target_chip() here either: zone_bin is a
        # ZONE app image staged as a PAYLOAD into the master's zone_fw data
        # partition -- always ESP32, regardless of the master's --target.
        # Same reasoning as --app cpfw above.
        zonefw_bin = build_zonefw_image(zone_bin, build_dir("master"))
        # And the same separate payload check: zone_fw's consumer is
        # components/fw_srv, which serves these bytes at GET /fw/zone.bin to a
        # zone in rescue, so the payload must be an esp32 image on either
        # --target. A wrong image here bricks every zone in the fleet update
        # that pulls it. Staged file, inner header at +HGFW_HDR_LEN, as above.
        hg_image.require_payload_chip(zonefw_bin, "esp32",
                                      "hillgrow_zone.bin (zone_fw payload)",
                                      offset=HGFW_HDR_LEN, staged_from=zone_bin)
        write_flash_args = [hex(offsets["zonefw"]), zonefw_bin]
    elif args.app == "rescue":
        bdir = build_dir(args.app, args.build_dir)
        app_bin = require_file(os.path.join(bdir, "hillgrow_rescue.bin"), "rescue app binary")
        # This IS an image that runs on --target -- the rescue app boots
        # straight from the factory slot on whichever chip it's flashed to.
        # See tools/hg_image.py: this is the specific hole the guard exists
        # to close (rescue/build/hillgrow_rescue.bin is only ever built for
        # esp32 today; --target esp32p4 must refuse it, not silently write
        # an xtensa image into the P4 factory slot).
        hg_image.require_target_chip(app_bin, args.target, "rescue app binary")
        write_flash_args = [hex(offsets["rescue"]), app_bin]
    else:
        bdir = build_dir(args.app, args.build_dir)
        app_bin = require_file(os.path.join(bdir, f"hillgrow_{args.app}.bin"), f"hillgrow_{args.app}.bin")
        hg_image.require_target_chip(app_bin, args.target, f"hillgrow_{args.app}.bin")
        # Generate a valid otadata image selecting this app's ota_0 slot,
        # rather than flashing the stock all-0xFF ota_data_initial.bin (see
        # tools/hg_otadata.py for why that would boot factory/rescue instead).
        otadata_bin = hg_otadata.write_otadata_file(bdir)
        write_flash_args = [hex(OTADATA_OFFSET), otadata_bin, hex(offsets[args.app]), app_bin]

    cmd = [sys.executable, "-m", "esptool", "--chip", args.target, "-p", args.port,
           "-b", args.baud, "write-flash"] + write_flash_args
    if args.dry_run:
        print(" ".join(cmd))
        return
    run_esptool(cmd)


if __name__ == "__main__":
    main()
