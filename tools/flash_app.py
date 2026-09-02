#!/usr/bin/env python3
"""Flash a single HillGrow application (its OTA app slot + a freshly
generated otadata image selecting it) without touching the bootloader or
partition table.

Usage:
    python tools/flash_app.py --app zone|master|rescue --port COMx [--baud 460800]

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
    parser.add_argument("--app", required=True, choices=sorted(APP_OFFSET))
    parser.add_argument("--port", required=True)
    parser.add_argument("--baud", default="460800")
    args = parser.parse_args()

    bdir = build_dir(args.app)
    if args.app == "rescue":
        app_bin = require_file(os.path.join(bdir, "hillgrow_rescue.bin"), "rescue app binary")
        write_flash_args = [hex(APP_OFFSET["rescue"]), app_bin]
    else:
        app_bin = require_file(os.path.join(bdir, f"hillgrow_{args.app}.bin"), f"hillgrow_{args.app}.bin")
        # Generate a valid otadata image selecting this app's ota_0 slot,
        # rather than flashing the stock all-0xFF ota_data_initial.bin (see
        # tools/hg_otadata.py for why that would boot factory/rescue instead).
        otadata_bin = hg_otadata.write_otadata_file(bdir)
        write_flash_args = [hex(0x20000), otadata_bin, hex(APP_OFFSET[args.app]), app_bin]

    cmd = [sys.executable, "-m", "esptool", "--chip", "esp32", "-p", args.port,
           "-b", args.baud, "write-flash"] + write_flash_args
    run_esptool(cmd)


if __name__ == "__main__":
    main()
