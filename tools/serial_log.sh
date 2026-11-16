#!/bin/zsh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Mykle Hansen
# Specified by Mykle Hansen, implemented by Claude (Anthropic).
#
# serial_log.sh [port] - saves the board's USB serial output to
# logs/serial-<date>-<time>.log, each line stamped with the time of day,
# and shows it as it goes. For long unattended runs like SDBurnIn's. When
# the board resets, its port disappears; this waits for it to come back
# and carries on in the same file. Ctrl-C to stop.
#
# Stop it before unplugging the board: the Arduino core's serial port can
# hang the board if the cable comes out while a computer has the port open.

port=${1:-$(ls /dev/cu.usbmodem* 2>/dev/null | head -1)}
if [[ -z $port ]]; then
  echo "No board found - pass its port, e.g. tools/serial_log.sh /dev/cu.usbmodem12201"
  exit 1
fi

mkdir -p logs
log=logs/serial-$(date +%Y%m%d-%H%M%S).log
echo "Logging $port to $log"

while true; do
  if [[ -e $port ]] && stty -f $port 115200 2>/dev/null; then
    while IFS= read -r line; do
      print -r -- "$(date +%H:%M:%S) ${line%$'\r'}"
    done < $port | tee -a $log
    print -r -- "$(date +%H:%M:%S) [port closed - waiting for the board]" | tee -a $log
  fi
  sleep 1
done
