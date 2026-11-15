#!/bin/bash
# SPDX-FileCopyrightText: 2026 Piotr Krysik
# SPDX-License-Identifier: GPL-3.0-only
# kbuild_inner.sh - runs INSIDE the build container (debian:trixie); /work holds src/, patches/ and official_vmlinuz.
set -euo pipefail
# hand the files back to the host user on exit (the container runs as root)
trap '[ -n "${HOST_UID:-}" ] && chown -R "$HOST_UID:${HOST_GID:-$HOST_UID}" /work 2>/dev/null || true' EXIT
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq patch gcc-aarch64-linux-gnu make bc flex bison libssl-dev libelf-dev python3 \
    xz-utils zstd perl cpio kmod dwarves device-tree-compiler llvm >/dev/null
cd /work/src

echo "== patches"
for p in /work/patches/*.patch; do echo "patch $(basename "$p")"; patch -p1 --forward < "$p"; done

echo "== configuration from the official kernel (IKCONFIG)"
python3 - <<'PY'
import zlib
d = open('/work/official_vmlinuz', 'rb').read()
open('/work/Image.official', 'wb').write(zlib.decompressobj(31).decompress(d))
PY
bash scripts/extract-ikconfig /work/Image.official > /work/config.stock
cp /work/config.stock .config
# the only configuration change: STRICT_DEVMEM off (DEVMEM stays on)
scripts/config --file .config -d STRICT_DEVMEM -d IO_STRICT_DEVMEM -e DEVMEM
# GNU binutils < 2.43 (e.g. Ubuntu 24.04) ignores -z pack-relative-relocs on aarch64, which silently turns RELR off and
# changes the configuration; Debian trixie ships binutils 2.44. llvm-nm/llvm-objcopy understand RELR sections.
export ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- NM=llvm-nm OBJCOPY=llvm-objcopy
make olddefconfig >/dev/null
diff <(grep -v '^#\|^$' /work/config.stock | sort) <(grep -v '^#\|^$' .config | sort) > /work/out/config.diff || true
cp .config /work/out/config.final
echo "== configuration differences (stock -> final): $(grep -c '^[<>]' /work/out/config.diff) lines"; head -40 /work/out/config.diff

echo "== kernelrelease: $(make -s kernelrelease)"
make -j"$(nproc)" KBUILD_BUILD_VERSION=6 KBUILD_BUILD_USER=release KBUILD_BUILD_HOST=build \
    Image.gz qcom/msm8916-longcheer-l8150.dtb 2>&1 | tail -15
cp arch/arm64/boot/Image.gz /work/out/vmlinuz
# the rpmsg_wwan_ctrl module is part of the rootfs (not of the kernel image): build the patched one, to be installed
# into /lib/modules/<release>/updates/ on the phone (see install_config.sh)
make -j"$(nproc)" KBUILD_MODPOST_WARN=1 drivers/net/wwan/rpmsg_wwan_ctrl.ko 2>&1 | tail -3
aarch64-linux-gnu-strip --strip-debug -o /work/out/rpmsg_wwan_ctrl.ko drivers/net/wwan/rpmsg_wwan_ctrl.ko
echo "module vermagic: $(strings /work/out/rpmsg_wwan_ctrl.ko | grep '^vermagic=')"
cp arch/arm64/boot/dts/qcom/msm8916-longcheer-l8150.dtb /work/out/
ls -la /work/out
