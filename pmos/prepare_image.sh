#!/bin/bash
# SPDX-FileCopyrightText: 2026 Piotr Krysik
# SPDX-License-Identifier: GPL-3.0-only
# prepare_image.sh - put the patched kernel and DTB into the official pmOS rootfs image (offline, the phone is not touched).
# The result is flashed with `fastboot flash userdata`.
#
# lk2nd boots the system from /extlinux/extlinux.conf on the boot partition INSIDE this image
# (kernel /vmlinuz, fdt /msm8916-longcheer-l8150.dtb, initrd /initramfs), so replacing those two files is enough.
# The modules in the rootfs match because the patched kernel has the same release and configuration.
#
# Usage: prepare_image.sh ROOTFS.img.xz KERNEL_OUT_DIR OUT_DIR
#   ROOTFS.img.xz    official image (...-nura-v26.06-phosh-wileyfox-crackling.img.xz)
#   KERNEL_OUT_DIR   output of kernel/build_kernel.sh (vmlinuz, msm8916-longcheer-l8150.dtb)
#   OUT_DIR          result: pmos-rootfs-modified.img (Android sparse), rootfs.raw, SHA256SUMS
# Requires: xz, simg2img, img2simg, debugfs and e2fsck (e2fsprogs), python3, dd.
set -euo pipefail
XZ=$(readlink -f "${1:?ROOTFS.img.xz}"); KO=$(readlink -f "${2:?KERNEL_OUT_DIR}"); OUT=${3:?OUT_DIR}
mkdir -p "$OUT"; OUT=$(readlink -f "$OUT")
for f in "$KO/vmlinuz" "$KO/msm8916-longcheer-l8150.dtb"; do [ -s "$f" ] || { echo "missing $f"; exit 1; }; done

echo "[1/6] unpack and convert sparse -> raw"
xz -dc "$XZ" > "$OUT/rootfs.sparse"
simg2img "$OUT/rootfs.sparse" "$OUT/rootfs.raw"; rm -f "$OUT/rootfs.sparse"

echo "[2/6] partition table (MBR)"
read -r BOOT_START BOOT_SECT < <(python3 - "$OUT/rootfs.raw" <<'PY'
import struct, sys
b = open(sys.argv[1], 'rb').read(512)
assert b[510:512] == b'\x55\xaa', 'no MBR found'
print(*struct.unpack_from('<II', b, 446 + 8))
PY
)
echo "    boot partition: start LBA $BOOT_START, $BOOT_SECT sectors"

echo "[3/6] extract the boot partition (ext2)"
dd if="$OUT/rootfs.raw" of="$OUT/bootpart.img" bs=512 skip="$BOOT_START" count="$BOOT_SECT" status=none
debugfs -R "cat /extlinux/extlinux.conf" "$OUT/bootpart.img" 2>/dev/null | tee "$OUT/extlinux.conf.orig"
debugfs -R "stat /vmlinuz" "$OUT/bootpart.img" 2>/dev/null | grep -q "regular" || { echo "no /vmlinuz in the boot partition"; exit 1; }

echo "[4/6] replace /vmlinuz and /msm8916-longcheer-l8150.dtb"
debugfs -w -f - "$OUT/bootpart.img" >/dev/null 2>"$OUT/debugfs.err" <<DBG
rm /vmlinuz
write $KO/vmlinuz /vmlinuz
set_inode_field /vmlinuz mode 0100644
rm /msm8916-longcheer-l8150.dtb
write $KO/msm8916-longcheer-l8150.dtb /msm8916-longcheer-l8150.dtb
set_inode_field /msm8916-longcheer-l8150.dtb mode 0100644
DBG
cmp <(debugfs -R "cat /vmlinuz" "$OUT/bootpart.img" 2>/dev/null) "$KO/vmlinuz" && echo "    vmlinuz: content matches"
cmp <(debugfs -R "cat /msm8916-longcheer-l8150.dtb" "$OUT/bootpart.img" 2>/dev/null) "$KO/msm8916-longcheer-l8150.dtb" && echo "    dtb: content matches"
e2fsck -fn "$OUT/bootpart.img" 2>&1 | tail -2

echo "[5/6] write the partition back"
dd if="$OUT/bootpart.img" of="$OUT/rootfs.raw" bs=512 seek="$BOOT_START" conv=notrunc status=none
rm -f "$OUT/bootpart.img"

echo "[6/6] sparse image and checksums"
img2simg "$OUT/rootfs.raw" "$OUT/pmos-rootfs-modified.img"
( cd "$OUT"; sha256sum rootfs.raw pmos-rootfs-modified.img > SHA256SUMS )
ls -la "$OUT"
echo "DONE. Flash (ERASES userdata):  fastboot flash userdata $OUT/pmos-rootfs-modified.img"
