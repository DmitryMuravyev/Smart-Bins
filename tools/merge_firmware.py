#!/usr/bin/env python3
"""Builds the release files for one PlatformIO environment.

Usage: merge_firmware.py ENV VERSION OUTDIR

Writes two files to OUTDIR:

  smartbins-VERSION-ENV-full.bin    bootloader, partition table, OTA data and
                                    firmware in one image, to be written at
                                    address 0x0 over USB. For a new board, or
                                    one whose partition table must change.
  smartbins-VERSION-ENV-update.bin  the firmware alone, for the System page's
                                    "Install firmware" or an OTA upload.

The offsets come from the environment's partition table (partitions/*.csv):
the app goes to its first app partition, the boot_app0 image (which tells the
bootloader to start app0) to the "ota" data partition when there is one.
Needs esptool (pip install esptool) and a finished `pio run -e ENV`.
"""

import csv
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BOOTLOADER_OFFSET = 0x1000  # classic ESP32
PARTITION_TABLE_OFFSET = 0x8000

# Partition table per environment (the -ota and -psram variants use their
# base's table).
TABLES = {"esp32-4mb": "4mb.csv", "esp32-8mb": "8mb.csv", "esp32-16mb": "16mb.csv", "esp32-16mb-psram": "16mb.csv"}


def read_table(path):
    rows = []
    with open(path, encoding="utf-8") as f:
        for row in csv.reader(line for line in f if line.strip() and not line.lstrip().startswith("#")):
            rows.append([c.strip() for c in row])
    return rows


def boot_app0():
    # Shipped with the Arduino core.
    path = os.path.expanduser(
        "~/.platformio/packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin")
    if not os.path.exists(path):
        sys.exit(f"boot_app0.bin not found at {path}: build the environment first")
    return path


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    env, version, outdir = sys.argv[1:]
    if env not in TABLES:
        sys.exit(f"unknown environment {env}; known: {', '.join(TABLES)}")
    build = os.path.join(ROOT, ".pio", "build", env)
    table = read_table(os.path.join(ROOT, "partitions", TABLES[env]))
    app = next(int(r[3], 0) for r in table if r[1] == "app")
    otadata = next((int(r[3], 0) for r in table if r[1] == "data" and r[2] == "ota"), None)

    parts = [(BOOTLOADER_OFFSET, os.path.join(build, "bootloader.bin")),
             (PARTITION_TABLE_OFFSET, os.path.join(build, "partitions.bin"))]
    if otadata is not None:
        parts.append((otadata, boot_app0()))
    parts.append((app, os.path.join(build, "firmware.bin")))
    for _, path in parts:
        if not os.path.exists(path):
            sys.exit(f"missing {path}: run `pio run -e {env}` first")

    os.makedirs(outdir, exist_ok=True)
    stem = os.path.join(outdir, f"smartbins-{version}-{env}")
    args = [sys.executable, "-m", "esptool", "--chip", "esp32", "merge_bin", "-o", stem + "-full.bin",
            "--flash_mode", "keep", "--flash_freq", "keep", "--flash_size", "keep"]
    for offset, path in parts:
        args += [hex(offset), path]
    subprocess.run(args, check=True)
    shutil.copyfile(os.path.join(build, "firmware.bin"), stem + "-update.bin")
    print(f"{stem}-full.bin  (write at 0x0)")
    print(f"{stem}-update.bin")


if __name__ == "__main__":
    main()
