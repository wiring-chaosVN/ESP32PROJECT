# ESP32-OS Bootloader v1.1.0

Prototype second-stage bootloader monitor for **ESP32-S3 N16R8** using **ESP-IDF v4.4.7**. The project extends the standard ESP-IDF bootloader through the documented `bootloader_components` hook mechanism; it does not replace ESP-IDF's normal app selection and image-loading path.

> **Build status:** source and CI workflow prepared; it has not yet been built by a real ESP-IDF toolchain or tested on the board in this environment. The first GitHub Actions run for this revision is the compile gate. Do not enable Secure Boot/flash-encryption eFuses during bring-up.

## Boot flow

- At startup the normal ESP-IDF bootloader prints a monitor banner and waits **500 ms**.
- Send any one character during that window to enter the UART monitor. That first character is consumed as the trigger; then send `help`.
- If no key is received, the ordinary ESP-IDF boot flow continues.
- In the monitor, `go`, Ctrl+C, or the 30-minute monitor timeout returns to the regular ESP-IDF boot flow. `reboot` restarts the chip.
- UART is UART0, normally 115200 baud. `baud` changes the live UART speed; change the host terminal to match immediately.

## UART commands

Numeric values accept decimal or `0x`-prefixed hexadecimal. Commands support `help <command>`, `<command> -h`, and `<command> --help`.

| Command | Behavior |
|---|---|
| `help [command]` | List commands or show command help |
| `version` | Print monitor source version |
| `info` | Print target/reset reason, flash JEDEC ID/size, CPU clock and RAM buffer addresses |
| `partitions` / `mtd` | Read, validate and print the ESP-IDF partition table |
| `go` | Continue the standard ESP-IDF boot path |
| `reboot` | Software reset |
| `verify <app-label>` | Verify an application image using the ESP-IDF image verifier |
| `verify <flash_addr> <len>` | Verify an ESP image at a 4-KiB-aligned internal-flash offset |
| `sha1sum -a <flash_addr> <len>` | SHA-1 hash a raw internal-flash range |
| `sha1sum -p <label>` | SHA-1 hash an entire partition |
| `unlock <label>` | Arm one destructive operation for a standard app/data partition (single-use, 30-second idle expiry) |
| `erase <flash_addr> <blocks>` | Erase `<blocks>` sectors of 4096 bytes; requires `unlock` |
| `flash <flash_addr> <ram_addr> <len>` | Program bytes from a monitor RAM buffer into a **previously erased** partition range; requires `unlock` |
| `dump <flash_addr> <len> <ram_addr>` | Copy internal flash into one of the monitor RAM buffers |
| `xmdm <ram_addr> <max_len>` | Receive XMODEM/CRC into a monitor RAM/IRAM buffer |
| `xmdm flash <label> <offset> <max_len>` | Receive XMODEM/CRC straight into a partition, erasing sectors on demand; requires `unlock` |
| `jump <ram_addr>` | Call an entry point inside a successfully loaded executable-IRAM buffer |
| `memrl <count> <ram_addr>` | Read 1–64 32-bit words from a monitor buffer |
| `memwl <value> <ram_addr>` | Write one 32-bit word in a monitor buffer |
| `cpufreq <40\|80\|160\|240>` | Request a CPU clock supported by this target/configuration; rejected if the ESP-IDF clock API cannot configure it |
| `baud <rate>` | Set UART0 baud: 9600, 19200, 38400, 57600, 115200, 230400, 460800, or 921600 |

### RAM payload details

Run `info` first. `dump` and `xmdm` only accept destinations completely inside the two addresses printed by `info`: the 64-KiB transfer buffer in DRAM or 16-KiB executable buffer in IRAM. The bootloader will not accept arbitrary CPU addresses for these commands.

For `jump`, the input must be a **raw machine-code payload linked for the printed IRAM address**, with a valid Xtensa entry point. An ESP-IDF application `.bin` is an image container and is not automatically a raw callable RAM payload. A returning payload should preserve the calling convention and return normally; a payload that changes system state may not safely return.

### Flash write rules

- These commands manage **the ESP32-S3's internal flash and the ESP-IDF partition table**. They do not address the separate external Winbond flash chip; that needs a separate external-SPI-flash driver/API.
- `partitions` must report a valid table before partition-scoped write/erase operations can proceed.
- `unlock <label>` only permits standard `app` or `data` partitions, and grants one matching operation. A new `unlock` replaces any previous token; reset clears it. The monitor never unlocks the bootloader or partition-table sector.
- `erase <addr> <blocks>` requires 4096-byte alignment and interprets the second argument as a **sector count**.
- `flash` intentionally **does not erase first**; erase separately if needed. Partial writes can leave a partition incomplete.
- `xmdm flash` validates the target partition/range before starting and erases sectors as blocks arrive. Interrupted transfers can leave partial contents. XMODEM's last packet can contain padding; verify app images after transfer.
- Encrypted app/partition writes use the ESP-IDF bootloader flash API and enforce the 32-byte alignment requirements documented for ESP-IDF v4.4.7. This has not yet been hardware-tested; use a non-production development board during bring-up.
- `verify` uses ESP-IDF's ESP-image validation (including image checksum and appended SHA-256 when present; signature checks depend on the build's secure-boot configuration). `sha1sum` is only an integrity/compatibility tool, **not authentication**.

## Flash configuration

`sdkconfig.defaults` sets:

- target: ESP32-S3
- configured internal-flash size: 16 MiB
- default single-app partition scheme for bring-up (temporary, **not** the final ESP32-OS layout)
- partition table offset: `0x20000` to reserve up to 128 KiB before the table for the larger customized second-stage bootloader
- RTC watchdog enabled with a 120-second timeout; the monitor feeds it while waiting/processing
- UART0 console, QIO, 80-MHz flash clock

The partition CSV supplied by ESP-IDF's temporary single-app scheme is placed relative to the configured partition-table offset. This layout is for testing the bootloader monitor only; finalize one source of truth for ESP32-OS internal flash partitions before production use.

## GitHub Actions build

The repository workflow is `.github/workflows/build_v1.0.0.yml`. It uses `espressif/esp-idf-ci-action@v1` with the ESP-IDF v4.4.7 container on `ubuntu-latest`. Two caches are used:

1. CCache compiler results, allowing unchanged compile units to be reused after source edits or failed builds.
2. The ESP-IDF `build/` directory plus generated `sdkconfig`, with cache keys separated by build configuration and source hash. A compatible prior build cache can be restored after a source-only change for incremental rebuilding.

On success, Actions uploads bootloader, partition table, application image/ELF, flash arguments, merged image, metadata and SHA-256 sums as an artifact. On failure, it tries to upload available logs. GitHub-hosted runners are ephemeral, so the ESP-IDF Docker image can still be pulled on a fresh runner; caching avoids putting the toolchain on the laptop and reduces recompilation, but is not a guarantee that every CI step is skipped.

### Build and first flash

1. Push this folder to a GitHub repository.
2. Open **Actions → ESP32-OS Bootloader build_v1.0.0** and wait for the first run to finish.
3. If the build fails, download `esp32-os-build-logs...` and use the actual compile/link error to correct the source.
4. Only after a successful build, download the firmware artifact. Use the produced `flash_args.txt`/merged image and follow the ESP-IDF `esptool.py` instructions matching your board's flash mode/size. Do not enable one-way security eFuses for this prototype.
5. Connect UART0 at 115200 baud and reset. The first key inside the 500-ms window enters `bldr>`.

This CI workflow builds firmware only; it never automatically flashes hardware.
