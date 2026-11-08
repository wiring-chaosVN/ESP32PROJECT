#!/usr/bin/env bash
set -Eeuo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
WORK_DIR="${RUNNER_TEMP:-${ROOT_DIR}/.ci}/esp-os-n16r8-build"
NUTTX_DIR="$WORK_DIR/nuttx"
APPS_DIR="$WORK_DIR/apps"
OUT_DIR="$ROOT_DIR/out"
ROOTFS_STAGE="$WORK_DIR/rootfs-stage"

NUTTX_COMMIT="${NUTTX_COMMIT:-3e04c33049ef28ffdcae351f5b08d10053a5df9a}"
NUTTX_APPS_COMMIT="${NUTTX_APPS_COMMIT:-d14111ebeff7e8eb0512e63238883dece6842c05}"
TOOLCHAIN_VERSION="${XTENSA_TOOLCHAIN:-xtensa-esp-elf-14.2.0_20241119}"
JOBS="${JOBS:-$(nproc)}"
KERNEL_PARTITION_MAX=$((0x600000))
ROOTFS_PARTITION_MAX=$((0x600000))
NUTTX_LINKER_PATCH="$ROOT_DIR/ci/patches/nuttx/0001-esp32s3-place-world1-vectors-in-iram.patch"

require_command() {
  command -v "$1" >/dev/null 2>&1 || {
    echo "ERROR: required command not found: $1" >&2
    exit 1
  }
}

clone_at_commit() {
  local url="$1" dest="$2" commit="$3"
  mkdir -p "$(dirname "$dest")"

  # The CI cache stores only each repository's .git directory.  Reuse its
  # pinned commit objects, but always recreate a clean, exact source tree.
  if [[ ! -d "$dest/.git" ]]; then
    rm -rf "$dest"
    mkdir -p "$dest"
    git -C "$dest" init -q
  fi

  if git -C "$dest" remote get-url origin >/dev/null 2>&1; then
    git -C "$dest" remote set-url origin "$url"
  else
    git -C "$dest" remote add origin "$url"
  fi

  if ! git -C "$dest" cat-file -e "${commit}^{commit}" 2>/dev/null; then
    git -C "$dest" fetch --depth=1 origin "$commit"
  fi

  git -C "$dest" checkout --quiet --force --detach "$commit"
  git -C "$dest" reset --hard "$commit"
  git -C "$dest" clean -ffdx

  local actual
  actual="$(git -C "$dest" rev-parse HEAD)"
  [[ "$actual" == "$commit" ]] || {
    echo "ERROR: commit pin mismatch for $dest: expected $commit, got $actual" >&2
    exit 1
  }
}

report_kernel_link_failure() {
  echo '== Kernel link diagnostics ==' >&2

  if [[ -f "$NUTTX_DIR/nuttx" ]]; then
    echo '-- Vector symbol addresses in partial nuttx ELF (if available) --' >&2
    xtensa-esp32s3-elf-nm -n "$NUTTX_DIR/nuttx" 2>/dev/null \
      | grep -E '(_world1_vectors|_xtensa_level[23]_vector|_debug_exception_vector|_xtensa_nmi_vector|_kernel_exception_vector|_user_exception_vector|_double_exception_vector)' \
      || true
  fi

  local linker_script="$NUTTX_DIR/boards/xtensa/esp32s3/common/scripts/legacy_sections.ld.tmp"
  if [[ -f "$linker_script" ]]; then
    echo '-- Effective legacy linker script: World1 placement --' >&2
    grep -n -B 5 -A 9 'world1\.vectors\|world1_vectors\.text' "$linker_script" >&2 || true
  fi

  echo '-- Linker-related output files --' >&2
  find "$NUTTX_DIR" -maxdepth 3 -type f \
    \( -name '*.map' -o -name 'nuttx' -o -name '*.ld.tmp' \) \
    -printf '%p\n' 2>/dev/null | head -80 >&2 || true
}

for cmd in git make python3 genromfs xxd xtensa-esp32s3-elf-gcc xtensa-esp32s3-elf-nm esptool.py; do
  require_command "$cmd"
done

# Keep the .git object stores under WORK_DIR so actions/cache can reuse them.
# Only remove generated outputs; clone_at_commit resets each source tree itself.
rm -rf "$OUT_DIR" "$ROOTFS_STAGE"
mkdir -p "$WORK_DIR" "$OUT_DIR"

printf '%s\n' '== Clone/reuse pinned Apache NuttX sources =='
clone_at_commit https://github.com/apache/nuttx.git "$NUTTX_DIR" "$NUTTX_COMMIT"
clone_at_commit https://github.com/apache/nuttx-apps.git "$APPS_DIR" "$NUTTX_APPS_COMMIT"

printf '%s\n' '== Patch legacy ESP32-S3 linker script for World1 vectors =='
LEGACY_LINKER_SCRIPT="$NUTTX_DIR/boards/xtensa/esp32s3/common/scripts/legacy_sections.ld"
[[ -f "$LEGACY_LINKER_SCRIPT" ]] || {
  echo "ERROR: expected NuttX linker script not found: $LEGACY_LINKER_SCRIPT" >&2
  exit 1
}
git -C "$NUTTX_DIR" apply --check "$NUTTX_LINKER_PATCH"
git -C "$NUTTX_DIR" apply "$NUTTX_LINKER_PATCH"
grep -Fq '.world1.vectors : ALIGN(1024)' "$LEGACY_LINKER_SCRIPT" || {
  echo 'ERROR: World1 vector placement patch did not take effect' >&2
  exit 1
}
grep -Fq 'KEEP (*(.world1_vectors.text));' "$LEGACY_LINKER_SCRIPT" || {
  echo 'ERROR: World1 vector input section is not retained by linker script' >&2
  exit 1
}

printf '%s\n' '== Configure ESP32-S3-WROOM-1-N16R8 KERNEL =='
CONFIG_SRC="$ROOT_DIR/ci/config/esp32s3_n16r8_kernel/defconfig"
CONFIG_DEST="$NUTTX_DIR/boards/xtensa/esp32s3/esp32s3-devkit/configs/esp_os_n16r8/defconfig"
mkdir -p "$(dirname "$CONFIG_DEST")"
cp "$CONFIG_SRC" "$CONFIG_DEST"

cd "$NUTTX_DIR"
./tools/configure.sh -l esp32s3-devkit:esp_os_n16r8
make olddefconfig

check_config() {
  local expected="$1"
  grep -Fqx "$expected" .config || {
    echo "ERROR: effective NuttX configuration is missing: $expected" >&2
    exit 1
  }
}
check_config 'CONFIG_ARCH_CHIP_ESP32S3WROOM1N16R8=y'
check_config 'CONFIG_BUILD_KERNEL=y'
check_config 'CONFIG_ARCH_USE_MMU=y'
check_config 'CONFIG_ESP32S3_FLASH_MODE_QIO=y'
check_config 'CONFIG_ESP32S3_FLASH_16M=y'
check_config 'CONFIG_ESP32S3_PSRAM_8M=y'
check_config 'CONFIG_ESP32S3_FLASH_FREQ_40M=y'
check_config 'CONFIG_ESP32S3_SPIRAM_MODE_OCT=y'
check_config 'CONFIG_ESP32S3_APP_FORMAT_LEGACY=y'
check_config 'CONFIG_ESP32S3_UART0=y'
check_config 'CONFIG_ENABLE_ALL_SIGNALS=y'
check_config 'CONFIG_ESP32S3_USERFAULT_ABORT=y'
check_config 'CONFIG_XTENSA_TOOLCHAIN_ESP=y'
check_config 'CONFIG_MM_PGSIZE=65536'
check_config 'CONFIG_ARCH_PGPOOL_PBASE=0x10000'
check_config 'CONFIG_ARCH_PGPOOL_SIZE=8323072'
check_config 'CONFIG_INIT_FILEPATH="/system/bin/init"'
if grep -Fqx 'CONFIG_ESPRESSIF_SIMPLE_BOOT=y' .config; then
  echo 'ERROR: Simple Boot/RAM-only image selected; ESP-OS needs the standard ESP image header' >&2
  exit 1
fi

cp .config "$OUT_DIR/nuttx.config"
printf '%s\n' '== Build NuttX kernel (pre-application link) =='
if ! make -j"$JOBS"; then
  report_kernel_link_failure
  exit 1
fi
[[ -s nuttx ]] || { echo 'ERROR: NuttX ELF was not produced' >&2; exit 1; }

printf '%s\n' '== Export kernel headers/libraries for KERNEL-mode applications =='
make export
shopt -s nullglob
EXPORTS=("$NUTTX_DIR"/nuttx-export-*.tar.gz)
[[ ${#EXPORTS[@]} -eq 1 ]] || {
  echo "ERROR: expected one nuttx-export archive, found ${#EXPORTS[@]}" >&2
  exit 1
}

printf '%s\n' '== Import and build NuttX applications =='
cd "$APPS_DIR"
./tools/mkimport.sh -z -x "${EXPORTS[0]}"
make import -j"$JOBS"
[[ -d "$APPS_DIR/bin" ]] || { echo 'ERROR: apps/bin was not created' >&2; exit 1; }
find "$APPS_DIR/bin" -maxdepth 1 -type f -print -quit | grep -q . || {
  echo 'ERROR: apps/bin contains no built programs' >&2
  exit 1
}

printf '%s\n' '== Generate the kernel bootstrap ROMFS source =='
BOOT_ROMFS_C="$NUTTX_DIR/arch/xtensa/src/board/board/romfs_boot.c"
./tools/mkromfsimg.sh "$BOOT_ROMFS_C"
[[ -s "$BOOT_ROMFS_C" ]] || { echo 'ERROR: embedded boot ROMFS source missing' >&2; exit 1; }

printf '%s\n' '== Relink kernel with the bootstrap ROMFS =='
cd "$NUTTX_DIR"
make -j"$JOBS"
[[ -s nuttx ]] || { echo 'ERROR: final NuttX ELF missing' >&2; exit 1; }
[[ -s nuttx.bin ]] || { echo 'ERROR: ESP32-S3 nuttx.bin missing' >&2; exit 1; }

ROMFS_SYMBOL="$(xtensa-esp32s3-elf-nm -S nuttx | awk '$NF == "romfs_img" { print; exit }')"
[[ -n "$ROMFS_SYMBOL" ]] || {
  echo 'ERROR: romfs_img symbol missing from linked kernel' >&2
  exit 1
}
ROMFS_SYMBOL_SIZE_HEX="$(awk '{print $2}' <<<"$ROMFS_SYMBOL")"
ROMFS_SYMBOL_SIZE=$((16#$ROMFS_SYMBOL_SIZE_HEX))
(( ROMFS_SYMBOL_SIZE > 1 )) || {
  echo "ERROR: kernel linked the one-byte ROMFS stub instead of boot ROMFS" >&2
  exit 1
}

KERNEL_SIZE="$(stat -c '%s' nuttx.bin)"
(( KERNEL_SIZE <= KERNEL_PARTITION_MAX )) || {
  echo "ERROR: kernel image ${KERNEL_SIZE} bytes exceeds 6 MiB kernel partition" >&2
  exit 1
}

printf '%s\n' '== Build external read-only /system ROMFS rootfs image =='
mkdir -p "$ROOTFS_STAGE"
cp -a "$ROOT_DIR/ci/rootfs/system/." "$ROOTFS_STAGE/"
cat > "$ROOTFS_STAGE/share/esp-os/build-info.txt" <<INFO
ESP-OS external /system ROMFS scaffold
NuttX commit: $NUTTX_COMMIT
NuttX apps commit: $NUTTX_APPS_COMMIT
Toolchain: $TOOLCHAIN_VERSION
Kernel backend: Apache NuttX KERNEL
Target: ESP32-S3-WROOM-1-N16R8
Mount intent: /system (runtime mount integration not yet implemented)
INFO

genromfs -f "$OUT_DIR/rootfs.img" -d "$ROOTFS_STAGE" -V 'ESP-OS-System'
ROOTFS_SIZE="$(stat -c '%s' "$OUT_DIR/rootfs.img")"
(( ROOTFS_SIZE <= ROOTFS_PARTITION_MAX )) || {
  echo "ERROR: rootfs image ${ROOTFS_SIZE} bytes exceeds 6 MiB rootfs partition" >&2
  exit 1
}

printf '%s\n' '== Collect build outputs =='
cp nuttx "$OUT_DIR/nuttx.elf"
cp nuttx.bin "$OUT_DIR/nuttx.bin"
cp "$BOOT_ROMFS_C" "$OUT_DIR/romfs_boot.c"
cp "${EXPORTS[0]}" "$OUT_DIR/"

cat > "$OUT_DIR/build-manifest.txt" <<INFO
ESP-OS N16R8 kernel/rootfs CI build
NuttX repository: https://github.com/apache/nuttx
NuttX commit: $NUTTX_COMMIT
NuttX apps repository: https://github.com/apache/nuttx-apps
NuttX apps commit: $NUTTX_APPS_COMMIT
Xtensa toolchain: $TOOLCHAIN_VERSION
Target: ESP32-S3-WROOM-1-N16R8
Flash: 16 MiB Quad-SPI
PSRAM: 8 MiB Octal-SPI
Kernel MTD: offset 0x130000, maximum 0x600000
RootFS MTD: offset 0x730000, maximum 0x600000
RootFS filesystem: ROMFS (read-only)
RootFS mount intent: /system
ROMF: NOT BUILT (explicitly deferred)
DTB: built separately; not modified by this workflow
Flash programming: NOT PERFORMED
Bootloader compatibility: NOT YET VERIFIED
NBIF early-consume/reserve: NOT YET IMPLEMENTED/VERIFIED BY THIS CI BUILD
INFO

cat > "$OUT_DIR/build-manifest.json" <<INFO
{
  "project": "ESP-OS",
  "target": "ESP32-S3-WROOM-1-N16R8",
  "kernel_backend": "Apache NuttX KERNEL",
  "nuttx_commit": "$NUTTX_COMMIT",
  "nuttx_apps_commit": "$NUTTX_APPS_COMMIT",
  "xtensa_toolchain": "$TOOLCHAIN_VERSION",
  "flash_mode": "QIO",
  "flash_size_bytes": 16777216,
  "psram_mode": "OCTAL",
  "psram_size_bytes": 8388608,
  "kernel_offset": "0x130000",
  "kernel_partition_size": "0x600000",
  "kernel_image_size": $KERNEL_SIZE,
  "rootfs_offset": "0x730000",
  "rootfs_partition_size": "0x600000",
  "rootfs_image_size": $ROOTFS_SIZE,
  "rootfs_format": "ROMFS",
  "rootfs_mount_intent": "/system",
  "embedded_boot_romfs_symbol_size": $ROMFS_SYMBOL_SIZE,
  "romfile_built": false,
  "flash_programmed": false,
  "bootloader_compatibility_verified": false,
  "nbif_handoff_verified": false
}
INFO

python3 "$ROOT_DIR/ci/scripts/validate_build.py" \
  --kernel "$OUT_DIR/nuttx.bin" \
  --rootfs "$OUT_DIR/rootfs.img" \
  --config "$OUT_DIR/nuttx.config" \
  --manifest "$OUT_DIR/build-manifest.json" \
  --romfs-symbol-size "$ROMFS_SYMBOL_SIZE"

(
  cd "$OUT_DIR"
  sha256sum nuttx.bin nuttx.elf rootfs.img romfs_boot.c nuttx.config build-manifest.json > SHA256SUMS
)

printf '\nBUILD OUTPUTS:\n'
ls -lh "$OUT_DIR"
printf '\nBUILD COMPLETE (no flash writes, no ROMF generation).\n'
