# ESP-OS UART0 Console + RootFS + WCL Build Kit v0.2.0

Target: **ESP32-S3-WROOM-1-N16R8** (16 MiB QSPI Flash + 8 MiB Octal PSRAM)

This is a repository overlay for the pinned NuttX KERNEL bring-up pipeline.
Copy/merge the contents of this ZIP into the root of the ESP-OS Git repository,
then push a branch and run **Actions → ESP-OS NuttX Kernel + RootFS → Run workflow**.

## Included

- Pinned NuttX + nuttx-apps build workflow and script.
- N16R8 KERNEL-mode defconfig (QIO Flash + OCTAL PSRAM).
- UART0 console at 115200 8N1 (GPIO43 TX / GPIO44 RX).
- NuttX NSH as the initial `/system/bin/init` user application.
- WCL enabled, MMU address environments, 64 KiB page size, 64 KiB PSRAM page-0 reservation, and user-fault abort support.
- Linker patch retaining World 1 vectors in internal SRAM.
- Early RTC watchdog write-protection patch.
- External read-only ROMFS payload under `ci/rootfs/system`.
- Build validator checks UART0, NSH, WCL and partition-size limits.

## UART0 console

Connect USB-UART / board USB serial to the board's UART0 console. The expected
serial settings are **115200 baud, 8 data bits, no parity, 1 stop bit**.

The kernel's initial user application is NuttX NSH (`init`). It is scheduled
as a normal user task; UART input does not require a kernel busy-wait loop.
The shell can inspect standard NuttX commands and device nodes. Peripheral
commands are only usable if their actual controller driver and tool are
compiled in and the device is registered under `/dev`.

This package does not claim every I2C/SPI/GPIO peripheral tool is already
enabled. Device Tree / board driver configuration and the exact peripheral
wiring must be completed per device.

## Security boundary terminology

ESP32-S3 does **not** provide ARM TrustZone. This build uses the NuttX KERNEL
mode World 0 / World 1 execution boundary and ESP32-S3 WCL/PMS support:
- World 0: privileged kernel, memory manager, drivers and interrupt handling.
- World 1: restricted user applications such as NSH.
- WCL + MMU address environments + user-fault handling are configured.

This is a development security foundation, **not a completed TEE or a complete
capability/MAC system**. DMA isolation, both-core enforcement, peripheral
ownership, cache/MMU edge cases and fault-injection tests still require
hardware verification. No eFuse is burned. Secure Boot and Flash Encryption
are not enabled by this build.

## RootFS status

The pipeline builds:
1. Embedded bootstrap ROMFS from NuttX `apps/bin`, linked into the kernel.
2. External read-only ROMFS image `out/rootfs.img`, intended for the rootfs MTD
   at `0x730000`, maximum `0x600000` bytes.

The external ROMFS now carries the system configuration and asset directory
layout. **Runtime discovery of `nexus.ptb`/NBIF MTD rows, mounting external
`/system`, and mounting writable `/data` from `rootfs_data` at `0xD30000` are
not implemented by this build kit.** No auto-format is performed. This avoids
writing to the wrong flash offset or masking the bootstrap `/system/bin/init`.

## Build outputs

The GitHub Actions artifact contains:
- `nuttx.bin`, `nuttx.elf`
- `rootfs.img`, `romfs_boot.c`
- `nuttx.config`
- `build-manifest.json`, `build-manifest.txt`, `SHA256SUMS`

## Flash layout used by the project contract

- Kernel: `0x130000`, max `0x600000`
- RootFS: `0x730000`, max `0x600000`
- rootfs_data: `0xD30000`, size `0x2CF000`

This workflow only builds artifacts. It does **not** flash the board, modify
bootloader/PTB/DTB, burn eFuse, or claim bootloader NBIF compatibility.

## Verification performed when preparing this package

- `bash -n ci/scripts/build_kernel_rootfs.sh`
- `python3 -m py_compile ci/scripts/validate_build.py`
- ZIP integrity check
- Config/source cross-check against the supplied ESP-OS N16R8 baseline

A full compiler build must run in GitHub Actions (or a local machine with the
pinned Xtensa toolchain, NuttX Kconfig frontends and genromfs). No such
toolchain is installed in the preparation environment.
