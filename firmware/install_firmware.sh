#!/bin/bash
# SPDX-FileCopyrightText: 2026 Piotr Krysik
# SPDX-License-Identifier: GPL-3.0-only
# install_firmware.sh - copy a firmware image built by build.sh to the phone (pmOS) so that the modem can be
# switched to it with qcom-sdr-switch-fw. Run pmos/install_config.sh first (it installs the switch script).
#
# Usage: install_firmware.sh PIL_DIR [user@HOST] [--switch]
#   PIL_DIR    directory with modem.mdt and modem.bNN (WORKDIR/out/pil from build.sh)
#   --switch   also start the custom firmware right away (restarts the modem)
# Login is by password, typed by you: SSH asks once (shared connection), sudo on the phone asks once.
set -euo pipefail
PIL=$(readlink -f "${1:?usage: install_firmware.sh PIL_DIR [user@HOST] [--switch]}")
HOST=${2:-user@172.16.42.1}
[ "$HOST" = "--switch" ] && { HOST=user@172.16.42.1; SWITCH=1; } || SWITCH=${3:+1}
[ -f "$PIL/modem.mdt" ] || { echo "$PIL/modem.mdt not found"; exit 1; }
( cd "$PIL" && sha256sum -c SHA256SUMS >/dev/null ) 2>/dev/null || echo "note: SHA256SUMS check skipped or failed"

CP=$(mktemp -u /tmp/qcom-sdr-ssh-XXXXXX)
SSHO=(-o StrictHostKeyChecking=accept-new -o ControlMaster=auto -o ControlPath="$CP" -o ControlPersist=120)
trap 'ssh "${SSHO[@]}" -O exit "$HOST" 2>/dev/null || true' EXIT

FILES=("$PIL"/modem.* "$PIL"/SHA256SUMS)
echo "copying ${#FILES[@]} files to $HOST (SSH password)"
(cd "$PIL" && tar -cf - modem.* SHA256SUMS) | ssh "${SSHO[@]}" "$HOST" \
  'rm -rf /tmp/sdr_fw && mkdir /tmp/sdr_fw && tar -C /tmp/sdr_fw -xf -'

# SHA256SUMS is installed next to modem.mdt: ../sdr/sdrd checks it against the dsp_blobs.bin it was given, so a
# dsp_blobs.bin built for different firmware is refused instead of silently calling the wrong address (see
# ../sdr/README.md, "Keeping dsp_blobs.bin matched to the firmware").
echo "installing into /var/lib/qcom-sdr/modem (sudo password)"
ssh -t "${SSHO[@]}" "$HOST" "set -e
sudo sh -c 'mkdir -p /var/lib/qcom-sdr/modem && rm -f /var/lib/qcom-sdr/modem/modem.* /var/lib/qcom-sdr/modem/SHA256SUMS && cp /tmp/sdr_fw/modem.* /tmp/sdr_fw/SHA256SUMS /var/lib/qcom-sdr/modem/ && sync'
rm -rf /tmp/sdr_fw
ls /var/lib/qcom-sdr/modem | wc -l | sed 's/^/files installed: /'
$( [ "${SWITCH:-}" = 1 ] && echo "sudo qcom-sdr-switch-fw custom force" || echo "echo 'Switch with: sudo qcom-sdr-switch-fw custom   (back: sudo qcom-sdr-switch-fw stock)'" )"
