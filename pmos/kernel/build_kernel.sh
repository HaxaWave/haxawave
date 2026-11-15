#!/bin/bash
# SPDX-FileCopyrightText: 2026 Piotr Krysik
# SPDX-License-Identifier: GPL-3.0-only
# build_kernel.sh - build the Nura (postmarketOS) kernel (msm8916-mainline v6.12.1-msm8916) with the SDR patches:
#   0001  DTS l8150: modem (mpss) region enlarged to 88 MiB (0x8e800000, size 0x5800000)
#   0002  arm64: /dev/mem read()/write() on any physical range            (skip with PERMISSIVE_RW=0)
#   0003  net/wwan: expose the modem DIAG channel as /dev/wwan0qcdm0 (needed for FTM); builds the patched module
#   config: CONFIG_STRICT_DEVMEM=n  (mmap of /dev/mem on any physical address, incl. modem RAM)
# The base configuration is the one embedded (IKCONFIG) in the official pmOS kernel; only STRICT_DEVMEM is changed.
# The kernel release stays 6.12.1-msm8916, so the modules shipped in the official rootfs keep loading.
#
# Usage: build_kernel.sh OFFICIAL_VMLINUZ [WORKDIR]
#   OFFICIAL_VMLINUZ  /vmlinuz from the boot partition of the official pmOS image (see extract_official.sh)
#   WORKDIR           work directory (default ${TMPDIR:-/tmp}/pmos-kbuild)
#   output:           WORKDIR/out/{vmlinuz,msm8916-longcheer-l8150.dtb,rpmsg_wwan_ctrl.ko,config.final,config.diff}
# Requires: docker (your user in the docker group; otherwise "sudo docker" is used and may ask for a password), curl, python3.
set -euo pipefail
if docker info >/dev/null 2>&1; then DOCKER=(docker); else DOCKER=(sudo docker); fi
HERE=$(cd "$(dirname "$0")" && pwd)
OFFICIAL=$(readlink -f "${1:?usage: build_kernel.sh OFFICIAL_VMLINUZ [WORKDIR]}")
W=${2:-${TMPDIR:-/tmp}/pmos-kbuild}
W=$(mkdir -p "$W" && cd "$W" && pwd)
TAG=v6.12.1-msm8916
# the kernel of the official image must be the release these patches are made for (its modules have to keep loading)
OFFICIAL_RELEASE=$(gzip -dc "$OFFICIAL" | grep -a -o -m1 'Linux version [^ ]*' | head -1 | cut -d' ' -f3 || true)
if [ "$OFFICIAL_RELEASE" != "${TAG#v}" ]; then
  echo "the official image has kernel ${OFFICIAL_RELEASE:-?}, these patches are for ${TAG#v}: use an image with ${TAG#v}" >&2
  exit 1
fi
# src/ and out/ are created as root inside the container, so they are also removed from inside a container
if [ "${CLEAN:-1}" = 1 ] && { [ -e "$W/src" ] || [ -e "$W/out" ]; }; then
  "${DOCKER[@]}" run --rm --network host -v "$W":/work debian:trixie rm -rf /work/src /work/out
fi
mkdir -p "$W/out"
SRC_URL=https://github.com/msm8916-mainline/linux/archive/$TAG.tar.gz
TARBALL=$W/linux-$TAG.tar.gz
PERMISSIVE_RW=${PERMISSIVE_RW:-1}

if [ ! -s "$TARBALL" ]; then echo "[1/4] downloading kernel sources $TAG"; curl -fL -o "$TARBALL" "$SRC_URL"; fi
echo "tarball sha256: $(sha256sum "$TARBALL" | cut -d' ' -f1)" | tee "$W/out/tarball.sha256"

if [ ! -d "$W/src" ]; then
  echo "[2/4] unpacking"; mkdir -p "$W/src"; tar -xzf "$TARBALL" -C "$W/src" --strip-components=1
fi
cp "$OFFICIAL" "$W/official_vmlinuz"
cp "$HERE/kbuild_inner.sh" "$W/kbuild_inner.sh"
mkdir -p "$W/patches"; cp "$HERE"/patches/*.patch "$W/patches/"
[ "$PERMISSIVE_RW" = 1 ] || rm -f "$W"/patches/0002-*.patch

echo "[3/4] building in a debian:trixie container (aarch64 cross toolchain)"
"${DOCKER[@]}" run --rm --network host -e HOST_UID="$(id -u)" -e HOST_GID="$(id -g)" \
  -v "$W":/work -w /work debian:trixie bash /work/kbuild_inner.sh > "$W/build.log" 2>&1 || { tail -30 "$W/build.log"; exit 1; }
tail -5 "$W/build.log"
echo "[4/4] output:"; ls -la "$W/out"
