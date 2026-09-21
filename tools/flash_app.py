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

--app zonefw and --app cpfw are different in kind from the app-slot cases
(zone/master/rescue): neither flashes an app slot -- no otadata write,
nothing boots from either one directly.

--app zonefw writes zone/build/hillgrow_zone.bin (or --build-dir, if given),
HGFW-header-prefixed, into the MASTER's "zone_fw" data partition (offset
0x570000 on esp32, 0xA30000 on esp32p4; see master's partitions*.csv and
components/fw_srv/fw_srv.h) so the master's fw_srv component can serve it at
GET /fw/zone.bin for a fleet update (Task 15). --port therefore names the
MASTER's serial port here, not a zone's.

--app cpfw (esp32p4 only) writes a raw ESP32-C6 co-processor image into the
MASTER's "cp_fw" data partition (offset 0xBB0000, size 0x180000; subtype
0x41 -- a DATA partition, not an app/ota slot, so this gets its own branch
here rather than falling into the generic app-slot path, which would wrongly
write an otadata selecting ota_0). --cp-image PATH is REQUIRED with
--app cpfw: there's no default build directory for it because the source
project isn't part of this repo yet (promoting it is Task 9). Today it comes
from the eh_cp project in the throwaway P4 bring-up spike
(C:\\Projects\\hillgrow-p4-spike\\eh_cp) -- `idf.py build` there produces
build/eh_cp_wifi_softap.bin. Building it needs
CONFIG_EH_TRANSPORT_CP_SDIO_MODE_STREAM=y: esp_hosted's default SW_AGGR SDIO
mode needs an ESP-IDF patch that the shared 6.0.1 install lacks. Do NOT run
`eh.py patch-idf` against C:\\esp\\v6.0.1 -- that patches the shared install
for every project that uses it, not just eh_cp.
"""
import argparse
import binascii
import os
import struct
import subprocess
import sys

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

ZONE_FW_HDR_LEN = 16
ZONE_FW_PART_SIZE = 0x180000
ZONE_FW_MAX_IMAGE = ZONE_FW_PART_SIZE - ZONE_FW_HDR_LEN
ZONE_FW_MAGIC = 0x57464748   # 'HGFW' LE -- see components/fw_srv/fw_srv.c's FW_HDR_MAGIC


def build_zonefw_image(zone_bin_path, out_dir):
    """Prepends the 16-byte HGFW header { magic 'HGFW' u32 LE, len u32 LE,
    crc32 u32 LE, rsvd u32 } fw_srv.c validates at zone_fw+0, ahead of the
    raw zone app image at zone_fw+16 (the partition is data-type, so the
    image length can't be recovered by parsing esp_image segments --
    that's bootloader territory -- and has to be carried explicitly).

    crc32 is plain binascii.crc32(image) (seed 0 -- the standard zlib/
    CRC-32-ISO-HDLC convention): the SAME check value family hg_blob.c's
    hg_crc32(0, ...) computes, which fw_srv.c verifies against. This is
    NOT hg_otadata.py's 0xFFFFFFFF-seeded esp_rom_crc32_le convention (a
    different check value family used for a different, bootloader-owned
    structure) -- do not conflate the two.

    Refuses images that wouldn't fit the zone_fw partition once the header
    is added."""
    with open(zone_bin_path, "rb") as f:
        image = f.read()
    if len(image) > ZONE_FW_MAX_IMAGE:
        sys.exit(f"error: zone image too large for zone_fw ({len(image)} > "
                 f"{ZONE_FW_MAX_IMAGE} bytes = 0x180000 - 16)")
    crc = binascii.crc32(image) & 0xFFFFFFFF
    header = struct.pack("<IIII", ZONE_FW_MAGIC, len(image), crc, 0)
    os.makedirs(out_dir, exist_ok=True)
    out_path = os.path.join(out_dir, "hg_zonefw.bin")
    with open(out_path, "wb") as f:
        f.write(header + image)
    return out_path


def build_dir(app, override=None):
    """override, when given, replaces the whole default <repo>/<app>/build
    path -- see --build-dir in the module docstring."""
    return override if override else os.path.join(REPO_ROOT, app, "build")


def require_file(path, what):
    if not os.path.isfile(path):
        sys.exit(f"error: {what} not found: {path}\n"
                 f"  (build {os.path.dirname(path)} first)")
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
        # otadata selecting ota_0 -- see the task-6 brief.)
        if not args.cp_image:
            parser.error("--app cpfw requires --cp-image PATH (see --help for where it comes from)")
        cp_bin = require_file(args.cp_image, "coprocessor image")
        write_flash_args = [hex(offsets["cpfw"]), cp_bin]
    elif args.app == "zonefw":
        # Builds nothing (per the brief): takes the zone app's own build
        # output and re-packages it for the MASTER's zone_fw partition --
        # --port/--baud below address the master board, not a zone.
        zone_bin = require_file(os.path.join(build_dir("zone", args.build_dir), "hillgrow_zone.bin"),
                                 "hillgrow_zone.bin (build zone first)")
        zonefw_bin = build_zonefw_image(zone_bin, build_dir("master"))
        write_flash_args = [hex(offsets["zonefw"]), zonefw_bin]
    elif args.app == "rescue":
        bdir = build_dir(args.app, args.build_dir)
        app_bin = require_file(os.path.join(bdir, "hillgrow_rescue.bin"), "rescue app binary")
        write_flash_args = [hex(offsets["rescue"]), app_bin]
    else:
        bdir = build_dir(args.app, args.build_dir)
        app_bin = require_file(os.path.join(bdir, f"hillgrow_{args.app}.bin"), f"hillgrow_{args.app}.bin")
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
