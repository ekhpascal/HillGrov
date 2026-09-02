#!/usr/bin/env python3
"""Flash a single HillGrow application (its OTA app slot + ota_data_initial.bin)
without touching the bootloader or partition table.

Usage:
    python tools/flash_app.py --app zone|master|rescue --port COMx [--baud 460800]
"""
import argparse
import os
import subprocess
import sys

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Offset of each app's OTA slot / rescue slot in flash (must match the
# partitions.csv of the corresponding board -- Task 13/14).
APP_OFFSET = {"zone": 0x170000, "master": 0x170000, "rescue": 0x30000}


def build_dir(app):
    return os.path.join(REPO_ROOT, app, "build")


def require_file(path, what):
    if not os.path.isfile(path):
        sys.exit(f"error: {what} not found: {path}\n"
                 f"  (build {os.path.dirname(path)} first)")
    return path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app", required=True, choices=sorted(APP_OFFSET))
    parser.add_argument("--port", required=True)
    parser.add_argument("--baud", default="460800")
    args = parser.parse_args()

    bdir = build_dir(args.app)
    if args.app == "rescue":
        app_bin = require_file(os.path.join(bdir, "hillgrow_rescue.bin"), "rescue app binary")
        write_flash_args = [hex(APP_OFFSET["rescue"]), app_bin]
    else:
        ota_data = require_file(os.path.join(bdir, "ota_data_initial.bin"), "ota_data_initial.bin")
        app_bin = require_file(os.path.join(bdir, f"hillgrow_{args.app}.bin"), f"hillgrow_{args.app}.bin")
        write_flash_args = [hex(0x20000), ota_data, hex(APP_OFFSET[args.app]), app_bin]

    cmd = [sys.executable, "-m", "esptool", "--chip", "esp32", "-p", args.port,
           "-b", args.baud, "write-flash"] + write_flash_args
    print(" ".join(cmd))
    subprocess.run(cmd, check=True)


if __name__ == "__main__":
    main()
