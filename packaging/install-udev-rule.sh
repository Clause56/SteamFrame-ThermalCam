#!/usr/bin/env bash
# Installs the udev rule so thermal-viewer can open the camera without root.
#   sudo ./install-udev-rule.sh            (known cameras)
#   sudo ./install-udev-rule.sh 1234:abcd  (also add your camera's VID:PID from lsusb)
# On SteamOS /etc survives system updates. If sudo asks for a password you
# haven't set, run `passwd` once in Desktop Mode first.
set -euo pipefail
DIR="$(cd "$(dirname "$0")" && pwd)"
DEST=/etc/udev/rules.d/60-thermal-camera.rules
cp "$DIR/60-thermal-camera.rules" "$DEST"
for id in "$@"; do
  vid="${id%%:*}"; pid="${id##*:}"
  echo "SUBSYSTEM==\"usb\", ATTRS{idVendor}==\"$vid\", ATTRS{idProduct}==\"$pid\", MODE=\"0666\", TAG+=\"uaccess\"" >> "$DEST"
done
udevadm control --reload-rules
udevadm trigger --subsystem-match=usb
echo "installed $DEST; unplug and replug the camera"
