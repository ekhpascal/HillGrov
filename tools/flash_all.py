#!/usr/bin/env python3
"""Full-flash a HillGrow board from a clean chip: bootloader, partition table,
a freshly generated otadata image, rescue slot and the board's own app, all
in one esptool call.

Usage:
    python tools/flash_all.py --board zone|master --port COMx [--baud 460800]

The rescue app (Task 16) is optional here: if rescue/build/hillgrow_rescue.bin
does not exist yet, its slot is skipped with a warning instead of failing.

Must run under a python that has esptool installed -- either the IDF venv
python (source C:\\esp\\v6.0.1\\esp-idf\\export.ps1 first) or any python
with `pip install esptool`.
"""
import argparse
import os
import subprocess
import sys

import hg_otadata

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# P4 moves all of these. The app offset especially: the ESP32 value 0x170000
# lands INSIDE the P4 table's factory/rescue partition, so a mis-targeted
# --board master would overwrite the rescue image with nothing to complain
# until rescue was needed. Must match master/partitions.csv (esp32) and
# master/partitions_p4.csv (esp32p4).
FLASH_LAYOUT = {
    "esp32": {
        "bootloader": 0x1000, "partition_table": 0xE000,
        "otadata": 0x20000, "rescue": 0x30000, "app": 0x170000,
        "boards": ("zone", "master"),
    },
    "esp32p4": {
        "bootloader": 0x2000, "partition_table": 0x8000,
        "otadata": 0x20000, "rescue": 0x30000, "app": 0x230000,
        "boards": ("master",),
    },
}


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
    all_boards = set()
    for target_layout in FLASH_LAYOUT.values():
        all_boards.update(target_layout["boards"])
    parser.add_argument("--board", required=True, choices=sorted(all_boards))
    parser.add_argument("--port", required=True)
    parser.add_argument("--baud", default="460800")
    parser.add_argument("--target", default="esp32", choices=sorted(FLASH_LAYOUT),
                         help="chip the OFFSETS are for; default esp32")
    parser.add_argument("--dry-run", action="store_true",
                         help="print the esptool command without executing it")
    args = parser.parse_args()

    layout = FLASH_LAYOUT[args.target]
    if args.board not in layout["boards"]:
        parser.error(f"--board {args.board} is not a thing on {args.target} "
                     f"(valid: {', '.join(sorted(layout['boards']))})")

    bdir = os.path.join(REPO_ROOT, args.board, "build")
    bootloader_bin = require_file(os.path.join(bdir, "bootloader", "bootloader.bin"), "bootloader.bin")
    part_table_bin = require_file(os.path.join(bdir, "partition_table", "partition-table.bin"), "partition-table.bin")
    app_bin = require_file(os.path.join(bdir, f"hillgrow_{args.board}.bin"), f"hillgrow_{args.board}.bin")

    # Generate a valid otadata image selecting ota_0, rather than flashing
    # the stock all-0xFF ota_data_initial.bin (see tools/hg_otadata.py for
    # why that would boot factory/rescue instead once Task 16 lands).
    otadata_bin = hg_otadata.write_otadata_file(bdir)

    write_flash_args = [
        hex(layout["bootloader"]), bootloader_bin,
        hex(layout["partition_table"]), part_table_bin,
        hex(layout["otadata"]), otadata_bin,
    ]

    rescue_bin = os.path.join(REPO_ROOT, "rescue", "build", "hillgrow_rescue.bin")
    if os.path.isfile(rescue_bin):
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
