#!/usr/bin/env bash
# Removes the PS5 HD Camera firmware loader (sudo bash uninstall.sh). Firmware already in a plugged
# camera stays until it is unplugged.
set -euo pipefail
if [[ $EUID -ne 0 ]]; then
    echo "Запустите с правами root: sudo bash uninstall.sh" >&2
    exit 1
fi
rm -f /etc/udev/rules.d/70-ps5camera.rules /etc/systemd/system/ps5camera-fwload.service
systemctl daemon-reload || true
udevadm control --reload-rules || true
rm -rf /opt/ps5camera
echo "PS5 HD Camera удалена."
