#!/bin/sh
# SPDX-FileCopyrightText: 2026 Piotr Krysik
# SPDX-License-Identifier: GPL-3.0-only
# Replace the modem firmware files exposed by msm-firmware-loader (a tmpfs directory of symlinks
# into the read-only modem partition) with the image from FW_DIR.
# Nothing on flash is modified; a reboot restores the stock firmware.
# Runs before the modem remoteproc starts (see qcom-sdr-modem-fw.service).
set -eu
. /etc/qcom-sdr/modem-fw.conf
DST=/run/msm-firmware-loader/target
[ -f "$FW_DIR/modem.mdt" ] || exit 0
[ -d "$DST" ] || exit 0
rm -f "$DST"/modem.b* "$DST"/modem.mdt "$DST"/modem.mbn
cp "$FW_DIR"/modem.* "$DST"/
ln -sf "$DST/modem.mdt" "$DST/modem.mbn"
