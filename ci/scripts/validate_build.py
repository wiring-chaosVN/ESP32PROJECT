#!/usr/bin/env python3
"""Validate ESP-OS N16R8 kernel/rootfs build outputs without flashing hardware."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

KERNEL_LIMIT = 0x600000
ROOTFS_LIMIT = 0x600000
ROMFS_MAGIC = b"-rom1fs-"


def fail(message: str) -> None:
    print(f"FAIL: {message}", file=sys.stderr)
    raise SystemExit(1)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--kernel", required=True, type=Path)
    parser.add_argument("--rootfs", required=True, type=Path)
    parser.add_argument("--config", required=True, type=Path)
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--romfs-symbol-size", required=True, type=int)
    args = parser.parse_args()

    for path in (args.kernel, args.rootfs, args.config, args.manifest):
        if not path.is_file() or path.stat().st_size == 0:
            fail(f"missing or empty artifact: {path}")

    kernel_size = args.kernel.stat().st_size
    rootfs_size = args.rootfs.stat().st_size
    if kernel_size > KERNEL_LIMIT:
        fail(f"kernel is {kernel_size} bytes; partition capacity is {KERNEL_LIMIT}")
    if rootfs_size > ROOTFS_LIMIT:
        fail(f"rootfs is {rootfs_size} bytes; partition capacity is {ROOTFS_LIMIT}")

    rootfs_prefix = args.rootfs.read_bytes()[:8]
    if rootfs_prefix != ROMFS_MAGIC:
        fail(f"rootfs does not begin with ROMFS magic: {rootfs_prefix!r}")

    config = args.config.read_text(encoding="utf-8")
    required = (
        "CONFIG_ARCH_CHIP_ESP32S3WROOM1N16R8=y",
        "CONFIG_BUILD_KERNEL=y",
        "CONFIG_ARCH_USE_MMU=y",
        "CONFIG_ESP32S3_FLASH_MODE_QIO=y",
        "CONFIG_ESP32S3_SPIRAM_MODE_OCT=y",
        "CONFIG_ESP32S3_FLASH_16M=y",
        "CONFIG_ESP32S3_PSRAM_8M=y",
        "CONFIG_ESP32S3_FLASH_FREQ_40M=y",
        "CONFIG_ENABLE_ALL_SIGNALS=y",
        "CONFIG_ESP32S3_APP_FORMAT_LEGACY=y",
        "CONFIG_ESP32S3_USERFAULT_ABORT=y",
        "CONFIG_XTENSA_TOOLCHAIN_ESP=y",
        "CONFIG_MM_PGSIZE=65536",
        "CONFIG_ARCH_PGPOOL_PBASE=0x10000",
        "CONFIG_ARCH_PGPOOL_SIZE=8323072",
    )
    for line in required:
        if line not in config.splitlines():
            fail(f"effective NuttX config does not contain {line}")
    if "CONFIG_ESPRESSIF_SIMPLE_BOOT=y" in config.splitlines():
        fail("NuttX Simple Boot is enabled; ESP-OS external bootloader path requires a standard ESP image header")

    manifest = json.loads(args.manifest.read_text(encoding="utf-8"))
    if manifest.get("target") != "ESP32-S3-WROOM-1-N16R8":
        fail("manifest target mismatch")
    if manifest.get("flash_mode") != "QIO" or manifest.get("psram_mode") != "OCTAL":
        fail("manifest memory bus modes mismatch")
    if manifest.get("romfile_built") is not False:
        fail("ROMF must remain deferred")
    if manifest.get("flash_programmed") is not False:
        fail("CI must not flash hardware")
    if args.romfs_symbol_size <= 1:
        fail("linked kernel contains the one-byte ROMFS stub")

    print("PASS: kernel image exists and fits the 6 MiB kernel MTD")
    print("PASS: external rootfs is ROMFS and fits the 6 MiB rootfs MTD")
    print("PASS: N16R8 16 MiB QIO Flash + 8 MiB Octal PSRAM KERNEL configuration")
    print("PASS: 64 KiB page pool baseline and first 64 KiB PSRAM reservation")
    print("PASS: embedded boot ROMFS is linked (non-stub symbol)")
    print("PASS: ROMF deferred; no flash operation requested")
    print("NOTE: this is a build-artifact check, not a bootloader/hardware validation")


if __name__ == "__main__":
    main()
