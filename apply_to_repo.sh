#!/usr/bin/env bash
set -Eeuo pipefail
SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DST="${1:-.}"
mkdir -p "$DST"
cp -a "$SRC/.github" "$SRC/ci" "$SRC/README.md" "$SRC/BUILD_KIT_MANIFEST.json" "$DST/"
echo "ESP-OS build kit overlay copied to: $DST"
echo "Review README.md, then run: bash ci/scripts/build_kernel_rootfs.sh"
