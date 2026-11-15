#!/bin/bash
# SPDX-FileCopyrightText: 2026 Piotr Krysik
# SPDX-License-Identifier: GPL-3.0-only
# build.sh - builds the sample stream components for the firmware image produced by ../firmware/build.sh
#
# Usage: build.sh [WORKDIR]    (the WORKDIR given to firmware/build.sh; default: ${TMPDIR:-/tmp}/modem-fw-build)
#
# Needs: Docker, python3, and for the host driver cmake, a C++ compiler and the SoapySDR development files
# (Debian/Ubuntu: libsoapysdr-dev). The phone programs are cross-compiled inside a Docker container.
# Optional: DOCKER_ARGS (extra docker options);  HEXAGON_TOOLS = directory that CONTAINS 6.4.06.a (default: the one firmware/build.sh unpacked in WORKDIR).
#
# Output in WORKDIR/sdr:
#   dsp_blobs.bin    DSP programs linked for exactly this firmware build
#   wtr_luts.bin     receiver gain / filter tables read from the firmware image
#   ccs_tune.bin     receiver tuning scripts (copied from data/)
#   sdrd             phone service (static aarch64)
#   sample_server    phone sample sender (static aarch64)
#   libqcomSupport.so  SoapySDR driver for this host
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
W=$(readlink -f "${1:-${TMPDIR:-/tmp}/modem-fw-build}")
ELF=$W/modem_proc/build/ms/M8916EAAAANVZQ0000.elf
HEX=${HEXAGON_TOOLS:-$W/hexagon/HEXAGON_Tools}
[ -f "$ELF" ] || { echo "firmware image $ELF not found: run firmware/build.sh with the same WORKDIR first"; exit 1; }
[ -x "$HEX/6.4.06.a/gnu/bin/hexagon-nm" ] || { echo "Hexagon tools not found in $HEX/6.4.06.a"; exit 1; }
OUT=$W/sdr; mkdir -p "$OUT"
if docker info >/dev/null 2>&1; then DOCKER=(docker); else DOCKER=(sudo docker); fi

echo "[1/3] DSP programs"
"$HEX/6.4.06.a/gnu/bin/hexagon-nm" -S "$ELF" > "$OUT/image.nm"
python3 "$HERE/dsp/build_blobs.py" --tools "$HEX/6.4.06.a" --nm "$OUT/image.nm" --out "$OUT/dsp_blobs.bin"
cp "$HERE/data/ccs_tune.bin" "$OUT/ccs_tune.bin"
python3 "$HERE/dsp/build_luts.py" --nm "$OUT/image.nm" --elf "$ELF" --out "$OUT/wtr_luts.bin"

echo "[2/3] phone programs (Docker, cross-compiled for aarch64, static)"
# shellcheck disable=SC2086,SC2016  # DOCKER_ARGS: several options, split on purpose; $p and $(...) expand in the container
"${DOCKER[@]}" run --rm --network host ${DOCKER_ARGS:-} -v "$HERE":/src:ro -v "$OUT":/out debian:trixie sh -c '
  set -e; apt-get update -qq >/dev/null; apt-get install -y -qq --no-install-recommends g++-aarch64-linux-gnu >/dev/null
  for p in sdrd sample_server; do
    aarch64-linux-gnu-g++ -std=c++11 -O2 -Wall -fno-exceptions -fno-rtti -pthread -static /src/$p.cpp -o /out/$p
  done
  chown "$(stat -c %u:%g /out)" /out/sdrd /out/sample_server'

echo "[3/3] host driver"
if pkg-config --exists SoapySDR 2>/dev/null && command -v cmake >/dev/null; then
  mkdir -p "$OUT/host-build" && ( cd "$OUT/host-build" && cmake "$HERE/soapy" >/dev/null && make 2>&1 | tail -1 ) && cp "$OUT/host-build/libqcomSupport.so" "$OUT/"
else
  echo "SoapySDR development files or cmake not found: install them (libsoapysdr-dev, cmake) and run this script again"
fi
ls -l "$OUT" | sed 's/^/  /'
