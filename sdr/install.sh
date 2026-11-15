#!/bin/bash
# SPDX-FileCopyrightText: 2026 Piotr Krysik
# SPDX-License-Identifier: GPL-3.0-only
# install.sh - installs the sample stream service on the phone (Nura) and the SoapySDR driver on this host.
# Run it after firmware/install_firmware.sh has put the custom firmware on the phone.
#
# Usage: install.sh [WORKDIR] [user@HOST]     (WORKDIR as given to build.sh; HOST default user@172.16.42.1)
# Login is by password, typed by you: SSH asks once (shared connection), sudo asks on the phone and on this host.
# SSH is used only for this installation; streaming and control later use plain TCP to the phone (ports 5701 and 5700).
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
W=$(readlink -f "${1:-${TMPDIR:-/tmp}/modem-fw-build}")
HOST=${2:-user@172.16.42.1}
OUT=$W/sdr
for f in dsp_blobs.bin wtr_luts.bin ccs_tune.bin sdrd sample_server; do [ -f "$OUT/$f" ] || { echo "$OUT/$f not found: run build.sh first"; exit 1; }; done

CP=$(mktemp -u /tmp/sdr-ssh-XXXXXX)
SSHO=(-o StrictHostKeyChecking=accept-new -o ControlMaster=auto -o ControlPath="$CP" -o ControlPersist=120)
trap 'ssh "${SSHO[@]}" -O exit "$HOST" 2>/dev/null || true' EXIT

echo "copying to $HOST (SSH password)"
tar -C "$OUT" -cf - dsp_blobs.bin wtr_luts.bin ccs_tune.bin sdrd sample_server -C "$HERE" sdr-sdrd.service | ssh "${SSHO[@]}" "$HOST" \
  'rm -rf /tmp/sdr_install && mkdir /tmp/sdr_install && tar -C /tmp/sdr_install -xf -'
echo "installing and enabling the service (sudo password on the phone)"
ssh -t "${SSHO[@]}" "$HOST" "set -e
sudo sh -c 'mkdir -p /usr/lib/sdr && cp /tmp/sdr_install/dsp_blobs.bin /tmp/sdr_install/wtr_luts.bin /tmp/sdr_install/ccs_tune.bin /usr/lib/sdr/ && install -m 755 /tmp/sdr_install/sdrd /tmp/sdr_install/sample_server /usr/lib/sdr/ && cp /tmp/sdr_install/sdr-sdrd.service /etc/systemd/system/ && systemctl daemon-reload && systemctl enable sdr-sdrd && systemctl restart sdr-sdrd'
rm -rf /tmp/sdr_install
systemctl is-active sdr-sdrd | sed 's/^/service: /'"

if [ -f "$OUT/libqcomSupport.so" ]; then
  DIR=$(SoapySDRUtil --info 2>/dev/null | sed -n 's/^Search path: *\([^ ]*\).*/\1/p' | head -1)
  if [ -n "$DIR" ]; then
    echo "installing the SoapySDR driver into $DIR (sudo password on this host)"
    sudo install -D -m 755 "$OUT/libqcomSupport.so" "$DIR/libqcomSupport.so"
    SoapySDRUtil --find="driver=qcom" 2>&1 | grep -E "Found device|driver|label" || echo "driver installed; the phone service is not reachable yet"
  else
    echo "SoapySDRUtil not found: copy $OUT/libqcomSupport.so into the SoapySDR modules directory by hand"
  fi
fi
