#!/bin/bash
# SPDX-FileCopyrightText: 2026 Piotr Krysik
# SPDX-License-Identifier: GPL-3.0-only
# start.sh - wait until the phone's sample service answers and check that the SoapySDR driver sees it.
#
# Usage: start.sh [--switch] [HOST]        HOST default 172.16.42.1 (QCOM_PHONE_HOST also works)
#
# Without options nothing is changed on the phone and no SSH is used: the script polls the control port (TCP 5701)
# and runs SoapySDRUtil. Use it after boot when the custom firmware is loaded automatically
# (sudo systemctl enable qcom-sdr-modem-fw.service on the phone).
#
# --switch  also loads the custom modem firmware (only if the phone runs the stock one) and starts sdr-sdrd.
#           It uses SSH (user@HOST, password typed by you), and the modem is restarted once.
set -u
SWITCH=0
[ "${1:-}" = "--switch" ] && { SWITCH=1; shift; }
HOST=${1:-${QCOM_PHONE_HOST:-172.16.42.1}}
LIMIT=${QCOM_START_TIMEOUT:-240}

port_open() { timeout 2 bash -c "exec 3<>/dev/tcp/$HOST/$1" 2>/dev/null; }
wait_for() {      # wait_for DESCRIPTION COMMAND... ; polls every 2 s up to $LIMIT seconds
  local what=$1; shift; local t=0
  until "$@"; do
    t=$((t + 2)); [ "$t" -gt "$LIMIT" ] && { echo "timeout: $what"; return 1; }
    sleep 2
  done
}

echo "waiting for the phone at $HOST"
wait_for "no answer from $HOST (USB network down? phone still booting?)" ping -c1 -W1 "$HOST" >/dev/null 2>&1 || exit 1

if [ "$SWITCH" = 1 ]; then
  echo "loading the custom modem firmware and starting sdr-sdrd (SSH and sudo passwords)"
  ssh -t -o StrictHostKeyChecking=accept-new "user@$HOST" '
    set -e
    sudo systemctl mask --runtime --now ModemManager          # before the custom firmware starts: it does not tolerate ModemManager
    if sudo qcom-sdr-switch-fw status | grep -q "firmware files: stock"; then
      sudo qcom-sdr-switch-fw custom
      sudo systemctl restart sdr-sdrd
    else
      sudo systemctl start sdr-sdrd
    fi
    sudo qcom-sdr-switch-fw status; systemctl is-active sdr-sdrd' || exit 1
fi

wait_for "control port $HOST:5701 does not answer (is sdr-sdrd running? is the custom firmware loaded?)" port_open 5701 || exit 1
echo "service is up; asking the SoapySDR driver"
if command -v SoapySDRUtil >/dev/null; then
  SoapySDRUtil --find="driver=qcom,host=$HOST" 2>&1 | grep -E "Found device|driver|label|host" || {
    echo "the driver does not list the device: is libqcomSupport.so in the SoapySDR modules directory?"; exit 1; }
else
  echo "SoapySDRUtil not found (install the SoapySDR utilities)"; exit 1
fi
echo "ready: select the device driver=qcom in your application"
