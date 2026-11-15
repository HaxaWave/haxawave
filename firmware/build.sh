#!/bin/bash
# SPDX-FileCopyrightText: 2026 Piotr Krysik
# SPDX-License-Identifier: GPL-3.0-only
# build.sh - build the modem firmware image (PIL files modem.mdt + modem.bNN) for the MSM8916 modem.
#
# Steps:
#   1. download modem_proc.zip from GitLab (qcom-sources15) and check its sha256
#   2. unpack it and restore the executable bits that the zip does not carry
#   3. apply the patches in patches/ (in order) and empty the Q6ZIP lists
#   4. get the Hexagon tools 6.4.06.a (downloaded from GitHub and checked against its sha256, or taken from a directory you already have)
#   5. build the Docker image from docker/Dockerfile (once)
#   6. build the firmware inside the container
#   7. split the image into modem.mdt + modem.bNN
#
# Usage: build.sh [WORKDIR]            (default: ${TMPDIR:-/tmp}/modem-fw-build)
# Environment:
#   MODEM_PROC_ZIP   use this local copy of modem_proc.zip instead of downloading it
#   HEXAGON_TOOLS    directory that CONTAINS the 6.4.06.a directory (i.e. <HEXAGON_TOOLS>/6.4.06.a/gnu/bin/...);
#                    if unset, the tools are downloaded and unpacked into WORKDIR
# Requires: docker (your user in the docker group, otherwise "sudo docker" is used and may ask for a password),
#           curl, unzip, patch, python3.
# Output: WORKDIR/out/pil/modem.mdt, modem.b00 ... (and WORKDIR/out/qdsp6sw.mbn). The first build takes ~10 minutes.
set -euo pipefail

ZIP_URL=https://gitlab.com/qcom-sources15/msm8939_2014-12-08_amss_oem/-/raw/master/modem_proc.zip
ZIP_SHA256=df1fb91e8048821feb0c046ef7540b112a115237312b4cf33b9943cc55017099
# Hexagon tools 6.4.06.a: a third-party copy on GitHub, pinned to one commit and checked against its sha256
HEXAGON_COMMIT=672ab788648ffbc30d114efe88603b3adccec384
HEXAGON_URL=https://github.com/exl002/HEXAGON_Tools/archive/$HEXAGON_COMMIT.zip
HEXAGON_SHA256=89e8bc0e725f6d8226669ca0d17b99a0d9d4ebf41eb91f046818028495fa66b0
HEXAGON_VERSION=6.4.06.a
IMAGE=qcom-sdr-modem-build:ubuntu20.04

HERE=$(cd "$(dirname "$0")" && pwd)
W=${1:-${TMPDIR:-/tmp}/modem-fw-build}
mkdir -p "$W"; W=$(cd "$W" && pwd)
if docker info >/dev/null 2>&1; then DOCKER=(docker); else DOCKER=(sudo docker); fi

# ---- 1. modem_proc.zip -------------------------------------------------------------------------------------
ZIP=${MODEM_PROC_ZIP:-$W/modem_proc.zip}
if [ ! -s "$ZIP" ]; then
  echo "[1/7] downloading modem_proc.zip (745 MB)"
  curl -fL -C - -o "$ZIP" "$ZIP_URL"
fi
echo "[1/7] checking sha256 of $ZIP"
echo "$ZIP_SHA256  $ZIP" | sha256sum -c - >/dev/null || { echo "sha256 mismatch for $ZIP"; exit 1; }

# ---- 2. unpack ---------------------------------------------------------------------------------------------
TREE=$W/modem_proc
if [ ! -f "$TREE/.unpacked" ]; then
  echo "[2/7] unpacking"
  if [ -e "$TREE" ]; then "${DOCKER[@]}" run --rm --network host -v "$W":/w debian:trixie rm -rf /w/modem_proc; fi
  mkdir -p "$TREE"; unzip -q -o "$ZIP" -d "$TREE"
  # The zip carries no executable bits. Restore them for scripts and native tools.
  python3 - "$TREE" <<'PY'
import os, sys
root = sys.argv[1]; n = 0
for d, _, files in os.walk(root):
    for f in files:
        p = os.path.join(d, f)
        if os.path.islink(p): continue
        want = f.endswith(('.py', '.pl', '.sh'))
        if not want and not f.endswith(('.o', '.a', '.lib')):
            try:
                with open(p, 'rb') as fh: h = fh.read(4)
                want = h[:2] == b'#!' or h == b'\x7fELF'
            except OSError:
                continue
        if want:
            os.chmod(p, os.stat(p).st_mode | 0o111); n += 1
print('   executable bits restored on %d files' % n)
PY
  # ---- 3. patch ----
  echo "[3/7] applying the patches"
  for p in "$HERE"/patches/*.patch; do
    echo "  $(basename "$p")"
    (cd "$TREE" && patch -p1 --forward --quiet < "$p")
  done
  # Q6zip compression is switched off: a list with only a comment is an empty list. (Written here and not in the patch, so that the patch
  # does not have to contain the original lists.)
  for f in config/8916/ms/compress.lst config/8936.genns/ms/compress.lst; do
    printf '/* Q6ZIP compression disabled: the original list was removed. */\n' > "$TREE/$f"
  done
  touch "$TREE/.unpacked"
else
  echo "[2/7] [3/7] tree already unpacked and patched ($TREE); delete it to start over"
fi

# ---- 4. Hexagon tools --------------------------------------------------------------------------------------
if [ -n "${HEXAGON_TOOLS:-}" ]; then
  HEX=$(readlink -f "$HEXAGON_TOOLS")
else
  HEX=$W/hexagon/HEXAGON_Tools
  if [ ! -x "$HEX/$HEXAGON_VERSION/gnu/bin/hexagon-gcc" ]; then
    echo "[4/7] downloading Hexagon tools $HEXAGON_VERSION"
    mkdir -p "$W/hexagon"
    [ -s "$W/hexagon/hexagon_tools.zip" ] || curl -fL -o "$W/hexagon/hexagon_tools.zip" "$HEXAGON_URL"
    echo "$HEXAGON_SHA256  $W/hexagon/hexagon_tools.zip" | sha256sum -c - >/dev/null || { echo "sha256 mismatch for $W/hexagon/hexagon_tools.zip"; exit 1; }
    rm -rf "$W/hexagon/HEXAGON_Tools-$HEXAGON_COMMIT" "$HEX"
    unzip -q -o "$W/hexagon/hexagon_tools.zip" -d "$W/hexagon"
    mkdir -p "$HEX"; mv "$W/hexagon/HEXAGON_Tools-$HEXAGON_COMMIT" "$HEX/$HEXAGON_VERSION"
  fi
fi
[ -f "$HEX/$HEXAGON_VERSION/gnu/bin/hexagon-gcc" ] || { echo "Hexagon tools not found in $HEX/$HEXAGON_VERSION"; exit 1; }
# make sure the tool binaries are executable
find "$HEX/$HEXAGON_VERSION" -type f \( -path '*/bin/*' -o -path '*/libexec/*' \) ! -perm -u+x -exec chmod u+x {} + 2>/dev/null || true
echo "[4/7] Hexagon tools: $HEX/$HEXAGON_VERSION"

# ---- 5. Docker image ---------------------------------------------------------------------------------------
if ! "${DOCKER[@]}" image inspect "$IMAGE" >/dev/null 2>&1; then
  echo "[5/7] building the Docker image $IMAGE"
  "${DOCKER[@]}" build -q -t "$IMAGE" "$HERE/docker" >/dev/null
else
  echo "[5/7] Docker image $IMAGE present"
fi

# ---- 6. build ----------------------------------------------------------------------------------------------
# The tree is mounted as /modem_proc (the build records that path); --network host avoids a sysctl error that
# the default bridge network causes in some nested-container setups.
echo "[6/7] building the firmware (log: $W/build.log)"
"${DOCKER[@]}" rm -f modem-fw-build >/dev/null 2>&1 || true
set +e
# shellcheck disable=SC2016  # $rc and $HOST_UID/$HOST_GID are expanded by the shell inside the container
"${DOCKER[@]}" run --name modem-fw-build --rm --network host \
  -e HOST_UID="$(id -u)" -e HOST_GID="$(id -g)" \
  -v "$TREE":/modem_proc -v "$HEX":/hextools:ro -w /modem_proc/build/ms \
  -e HEXAGON_ROOT=/hextools -e HEXAGON_RTOS_RELEASE=$HEXAGON_VERSION -e HEXAGON_Q6VERSION=v5 \
  -e ARMTOOLS=ARMCT5 -e ARMHOME=/dev/null -e PYTHONDONTWRITEBYTECODE=1 \
  "$IMAGE" bash -c 'python build_variant.py 8916 BUILD_ID=EAAAANVZ -k; rc=$?; chown -R "$HOST_UID:$HOST_GID" /modem_proc; exit $rc' \
  > "$W/build.log" 2>&1
set -e
grep -n "Build .* returned" "$W/build.log" | tail -1 || true
MBN=$TREE/build/ms/bin/EAAAANVZ/qdsp6sw.mbn
if ! grep -q "Build 8916 returned code 0" "$W/build.log"; then echo "build failed, see $W/build.log"; tail -20 "$W/build.log"; exit 1; fi

# ---- 7. split ----------------------------------------------------------------------------------------------
echo "[7/7] splitting the image"
mkdir -p "$W/out"; cp "$MBN" "$W/out/qdsp6sw.mbn"
python3 "$HERE/pil_split.py" "$W/out/qdsp6sw.mbn" -o "$W/out/pil" | tail -3
(cd "$W/out/pil" && sha256sum modem.* > SHA256SUMS)
echo "DONE: $W/out/pil"
ls -la "$W/out/pil" | head -8
