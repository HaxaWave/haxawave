#!/bin/bash
# SPDX-FileCopyrightText: 2026 Piotr Krysik
# SPDX-License-Identifier: GPL-3.0-only
# extract_official.sh - extract the kernel files from the boot partition of an official pmOS image.
# Usage: extract_official.sh ROOTFS.img.xz OUT_DIR
# The image is an Android sparse file holding an MBR disk (partition 1: ext2 /boot, partition 2: root).
# Output: vmlinuz (gzip Image), msm8916-longcheer-l8150.dtb, initramfs, extlinux.conf.
# vmlinuz is the source of the kernel configuration used by kernel/build_kernel.sh.
# Requires: xz, simg2img (android-sdk-libsparse-utils), debugfs (e2fsprogs), python3. Uses ~3 GB of temporary space.
set -euo pipefail
XZ=$(readlink -f "${1:?usage: extract_official.sh ROOTFS.img.xz OUT_DIR}"); OUT=${2:?OUT_DIR}
mkdir -p "$OUT"; OUT=$(readlink -f "$OUT")
T=$(mktemp -d "$OUT/.tmp.XXXX"); trap 'rm -rf "$T"' EXIT
xz -dc "$XZ" > "$T/s.img"; simg2img "$T/s.img" "$T/r.raw"; rm -f "$T/s.img"
read -r ST SZ < <(python3 - "$T/r.raw" <<'PY'
import struct, sys
b = open(sys.argv[1], 'rb').read(512)
assert b[510:512] == b'\x55\xaa', 'no MBR found'
print(*struct.unpack_from('<II', b, 446 + 8))
PY
)
dd if="$T/r.raw" of="$T/boot.img" bs=512 skip="$ST" count="$SZ" status=none
for f in vmlinuz msm8916-longcheer-l8150.dtb initramfs extlinux/extlinux.conf; do
  debugfs -R "dump /$f $OUT/$(basename "$f")" "$T/boot.img" 2>/dev/null
done
ls -la "$OUT"
