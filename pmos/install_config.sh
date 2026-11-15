#!/bin/bash
# SPDX-FileCopyrightText: 2026 Piotr Krysik
# SPDX-License-Identifier: GPL-3.0-only
# install_config.sh - apply the post-install configuration to a running pmOS phone over SSH. Idempotent.
#   - blacklist the unstable mainline audio modules (stable system, ~45 s boot)
#   - qcom-sdr-switch-fw: switch the modem between the stock firmware and a custom image in /var/lib/qcom-sdr/modem
#       (see ../firmware/install_firmware.sh); the phone stays on the stock firmware until you ask for the custom one
#   - qcom-sdr-modem-fw.service (installed, NOT enabled): use the custom image at every boot
#       (enable with: sudo systemctl enable qcom-sdr-modem-fw.service)
#   - sdr-tty-closing-wait.service: no 30 s UART drain wait when the console is closed (faster reboot)
# Login is by password, typed by you: SSH asks once (shared connection), sudo on the phone asks once.
# No SSH keys or passwordless sudo are required.
#   - optionally the patched rpmsg_wwan_ctrl module (exposes /dev/wwan0qcdm0, the modem DIAG channel needed for FTM):
#       installed into /lib/modules/6.12.1-msm8916/updates/, which depmod prefers over the module shipped in the rootfs
# Usage: install_config.sh [user@HOST] [rpmsg_wwan_ctrl.ko]      (default user@172.16.42.1; the module is built by
#        kernel/build_kernel.sh into WORKDIR/out/rpmsg_wwan_ctrl.ko)
set -euo pipefail
HOST=${1:-user@172.16.42.1}
MODULE=${2:-}
HERE=$(cd "$(dirname "$0")" && pwd)
KREL=6.12.1-msm8916
STAGE=$(mktemp -d /tmp/qcom-sdr-stage-XXXXXX)
cp -a "$HERE/rootfs/." "$STAGE/"
if [ -n "$MODULE" ]; then
  [ -f "$MODULE" ] || { echo "module not found: $MODULE"; exit 1; }
  mkdir -p "$STAGE/lib/modules/$KREL/updates"; cp "$MODULE" "$STAGE/lib/modules/$KREL/updates/rpmsg_wwan_ctrl.ko"
fi
CP=$(mktemp -u /tmp/qcom-sdr-ssh-XXXXXX)
SSHO=(-o StrictHostKeyChecking=accept-new -o ControlMaster=auto -o ControlPath="$CP" -o ControlPersist=120)
trap 'ssh "${SSHO[@]}" -O exit "$HOST" 2>/dev/null || true; rm -rf "$STAGE"' EXIT

echo "[1/2] copying files to the phone (SSH password)"
tar -C "$STAGE" -cf - . | ssh "${SSHO[@]}" "$HOST" \
  'rm -rf /tmp/sdr_rootfs && mkdir /tmp/sdr_rootfs && tar -C /tmp/sdr_rootfs -xf -'

echo "[2/2] installing (sudo password)"
ssh -t "${SSHO[@]}" "$HOST" 'set -e
sudo sh -c "
  chown -R root:root /tmp/sdr_rootfs
  cp -a /tmp/sdr_rootfs/etc/modprobe.d/. /etc/modprobe.d/
  mkdir -p /etc/qcom-sdr /var/lib/qcom-sdr/modem
  [ -f /etc/qcom-sdr/modem-fw.conf ] || cp /tmp/sdr_rootfs/etc/qcom-sdr/modem-fw.conf /etc/qcom-sdr/
  cp -a /tmp/sdr_rootfs/usr/local/sbin/. /usr/local/sbin/
  cp -a /tmp/sdr_rootfs/etc/systemd/system/. /etc/systemd/system/
  if [ -d /tmp/sdr_rootfs/lib/modules ]; then cp -a /tmp/sdr_rootfs/lib/modules/. /lib/modules/ && depmod -a '$KREL'; fi
  systemctl daemon-reload
  systemctl enable sdr-tty-closing-wait.service
  rm -rf /tmp/sdr_rootfs
"
echo "OK. Reboot the phone so the audio module blacklist takes effect."'
echo "IMPORTANT: change the default pmOS password when you are done:  ssh -t $HOST passwd"
