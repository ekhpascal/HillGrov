#!/usr/bin/env python3
"""Full-flash a HillGrow board from a clean chip: bootloader, partition table,
a freshly generated otadata image, rescue slot and the board's own app, all
in one esptool call.

Usage:
    python tools/flash_all.py --board zone|master --port COMx [--baud 460800]

The rescue app (Task 16) is optional here: with no --build-dir, if
rescue/build/hillgrow_rescue.bin does not exist yet, its slot is skipped
with a warning instead of failing. (Note that's <repo>/rescue/build, NOT
<repo>/<board>/build -- rescue is its own app, built out of the top-level
rescue/ directory regardless of which board you're flashing.)

Must run under a python that has esptool installed -- either the IDF venv
python (source C:\\esp\\v6.0.1\\esp-idf\\export.ps1 first) or any python
with `pip install esptool`.

--build-dir overrides the directory every one of the files above (bootloader,
partition table, otadata, rescue, app) is read from (default: bootloader/
partition-table/otadata/app come from <repo>/<board>/build, rescue comes
from <repo>/rescue/build -- today's hard-coded paths, unchanged unless you
pass this). Needed whenever the binaries actually built for --target don't
live in those default directories -- e.g. after building master for esp32p4
into a separate out-of-tree dir, --target esp32 with no --build-dir would
silently read those P4 binaries out of master/build and still write them at
the ESP32 offsets.

IMPORTANT: when --build-dir IS given, it replaces rescue's default
(<repo>/rescue/build) too -- rescue is then read from that SAME --build-dir,
not from rescue/build. So `--target esp32p4 --build-dir master/build_p4`
looks for hillgrow_rescue.bin directly inside master/build_p4, and skips
the rescue slot (with the warning above) unless it's there. That is
CURRENTLY EXPECTED for the P4 bench: no P4 rescue app is built anywhere
today (rescue/sdkconfig.defaults pins CONFIG_IDF_TARGET="esp32" and there is
no rescue/sdkconfig.defaults.esp32p4 override), so a P4 --build-dir flash
always leaves the P4 factory slot unwritten until a P4 rescue build exists.

Every image that will actually RUN on --target (bootloader, the board's app
image, and rescue if present) is checked against --target by reading its
own esp_image_header_t chip_id before it's flashed -- see
tools/hg_image.py. On mismatch this refuses rather than flashing a
right-offset, wrong-architecture image; partition-table.bin and the
generated otadata image aren't app images and aren't checked.
"""
import argparse
import os
import subprocess
import sys

import hg_image
import hg_otadata

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def build_dir(app, override=None):
    """override, when given, replaces the whole default <repo>/<app>/build
    path -- same helper as tools/flash_app.py's. Used for both the board's
    own files (bootloader/table/otadata/app) AND the rescue slot: fix round
    2 found rescue_bin hardcoded straight to REPO_ROOT/rescue/build, bypassing
    --build-dir entirely, so a --target esp32p4 --build-dir <P4 dir> call
    took bootloader/table/app from the P4 directory but rescue from whatever
    ESP32 build happened to be sitting in rescue/build -- and wrote that
    xtensa binary into the P4 factory slot. Routing rescue through this same
    helper makes --build-dir, when given, cover every file this tool
    flashes (as the module docstring already claimed); when --build-dir is
    omitted, rescue's default stays rescue/build exactly as before, because
    override is None for both calls."""
    return override if override else os.path.join(REPO_ROOT, app, "build")

# P4 moves all of these. The app offset especially: the ESP32 value 0x170000
# lands INSIDE the P4 table's factory/rescue partition, so a mis-targeted
# --board master would overwrite the rescue image with nothing to complain
# until rescue was needed. Must match master/partitions.csv (esp32) and
# master/partitions_p4.csv (esp32p4).
#
# The P4 partition table is at 0xF000, NOT IDF's P4 default of 0x8000: the
# custom bootloader is 0x60f0 bytes and 0x8000 would leave it only the 0x6000
# above the bootloader at 0x2000. 0xF000 gives it the same 0xD000 window the
# ESP32 has. Writing the table to 0x8000 here would leave the real table
# unwritten and the chip would boot the stale one, or none.
FLASH_LAYOUT = {
    "esp32": {
        "bootloader": 0x1000, "partition_table": 0xE000,
        "otadata": 0x20000, "rescue": 0x30000, "app": 0x170000,
        "boards": ("zone", "master"),
    },
    "esp32p4": {
        "bootloader": 0x2000, "partition_table": 0xF000,
        "otadata": 0x20000, "rescue": 0x30000, "app": 0x230000,
        "boards": ("master",),
    },
}


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
    all_boards = set()
    for target_layout in FLASH_LAYOUT.values():
        all_boards.update(target_layout["boards"])
    parser.add_argument("--board", required=True, choices=sorted(all_boards))
    parser.add_argument("--port", required=True)
    parser.add_argument("--baud", default="460800")
    parser.add_argument("--target", default="esp32", choices=sorted(FLASH_LAYOUT),
                         help="chip the OFFSETS are for; default esp32")
    parser.add_argument("--build-dir", default=None, metavar="DIR",
                         help="override the build directory everything is read from "
                              "(default: <repo>/<board>/build); see module docstring")
    parser.add_argument("--dry-run", action="store_true",
                         help="print the esptool command without executing it")
    args = parser.parse_args()

    layout = FLASH_LAYOUT[args.target]
    if args.board not in layout["boards"]:
        parser.error(f"--board {args.board} is not a thing on {args.target} "
                     f"(valid: {', '.join(sorted(layout['boards']))})")

    bdir = build_dir(args.board, args.build_dir)
    bootloader_bin = require_file(os.path.join(bdir, "bootloader", "bootloader.bin"), "bootloader.bin")
    part_table_bin = require_file(os.path.join(bdir, "partition_table", "partition-table.bin"), "partition-table.bin")
    app_bin = require_file(os.path.join(bdir, f"hillgrow_{args.board}.bin"), f"hillgrow_{args.board}.bin")

    # Both are real ESP app images that will actually RUN on --target --
    # check them against it by reading their own header (see
    # tools/hg_image.py). partition-table.bin is NOT an app image (no
    # esp_image_header_t, nothing to check) and is left alone.
    hg_image.require_target_chip(bootloader_bin, args.target, "bootloader.bin")
    hg_image.require_target_chip(app_bin, args.target, f"hillgrow_{args.board}.bin")

    # Generate a valid otadata image selecting ota_0, rather than flashing
    # the stock all-0xFF ota_data_initial.bin (see tools/hg_otadata.py for
    # why that would boot factory/rescue instead once Task 16 lands).
    otadata_bin = hg_otadata.write_otadata_file(bdir)

    write_flash_args = [
        hex(layout["bootloader"]), bootloader_bin,
        hex(layout["partition_table"]), part_table_bin,
        hex(layout["otadata"]), otadata_bin,
    ]

    rescue_bin = os.path.join(build_dir("rescue", args.build_dir), "hillgrow_rescue.bin")
    if os.path.isfile(rescue_bin):
        hg_image.require_target_chip(rescue_bin, args.target, "hillgrow_rescue.bin")
        write_flash_args += [hex(layout["rescue"]), rescue_bin]
    else:
        print(f"warning: rescue app not built yet ({rescue_bin} missing) -- skipping rescue slot")

    write_flash_args += [hex(layout["app"]), app_bin]

    cmd = [sys.executable, "-m", "esptool", "--chip", args.target, "-p", args.port,
           "-b", args.baud, "write-flash"] + write_flash_args
    if args.dry_run:
        print(" ".join(cmd))
        return
    run_esptool(cmd)


if __name__ == "__main__":
    main()
