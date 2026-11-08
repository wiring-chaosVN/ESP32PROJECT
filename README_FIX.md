# ESP-OS CI linker fix + cache

Replace/add these paths in the ESP32PROJECT repository:

- Replace `.github/workflows/esp-os-kernel-rootfs.yml`.
- Replace `ci/scripts/build_kernel_rootfs.sh`.
- Add `ci/patches/nuttx/0001-esp32s3-place-world1-vectors-in-iram.patch`.

## Root cause

The N16R8 defconfig selects `CONFIG_ESP32S3_APP_FORMAT_LEGACY=y`. The build log confirms that this selects/preprocesses `boards/xtensa/esp32s3/common/scripts/legacy_sections.ld`. That linker script retained the privileged Xtensa vectors in `.iram0.vectors` but did not define an output section for `.world1_vectors.text`.

The World1 vector assembly contains direct `j` instructions to the privileged vector handlers. Without an explicit placement rule, the World1 input section becomes an orphan section and is not placed beside the privileged vectors in IRAM. The new patch adds an aligned `.world1.vectors` output section in `iram0_0_seg`, immediately after `.iram0.text`, matching the intended ESP32-S3 World1 vector placement while respecting the legacy linker script's memory layout.

The patch deliberately does not remove the World1 vectors, disable WCL, or replace the direct jumps with register-clobbering instructions.

## Cache behavior

The workflow caches:

- Espressif Xtensa 14.2.0 toolchain.
- Kconfig tools source and installed frontends.
- pip download cache.
- NuttX and nuttx-apps Git object stores only.

Generated kernel objects and output directories are not cached, preventing stale objects from being reused after config or patch changes. The build script checks out the exact pinned commits and cleans generated/untracked files before each build, then reapplies the local linker patch.

## Verification performed here

- YAML parsed successfully.
- Workflow remains `workflow_dispatch` only; there is no `push` trigger.
- `bash -n ci/scripts/build_kernel_rootfs.sh` passed.
- Linker patch format/apply check passed against a fixture containing the exact upstream hunk context.

A full GitHub Actions build has not been run after this correction. The first rerun must confirm the linker now places `_world1_vectors` and resolves the `j` relocations. This package does not claim hardware boot validation.
