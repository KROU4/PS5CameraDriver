#!/usr/bin/env bash
# Removes the PS5 HD Camera firmware loader (sudo bash uninstall.sh). Firmware already in a plugged
# camera stays until it is unplugged.
set -euo pipefail
case "${LC_ALL:-${LC_MESSAGES:-${LANG:-}}}" in ru*) ru=1 ;; *) ru="" ;; esac
t() { if [[ -n "$ru" ]]; then printf '%s' "$1"; else printf '%s' "$2"; fi; }
if [[ $EUID -ne 0 ]]; then
    echo "$(t 'Запустите с правами root' 'Run as root'): sudo bash uninstall.sh" >&2
    exit 1
fi
rm -f /etc/udev/rules.d/70-ps5camera.rules /etc/systemd/system/ps5camera-fwload.service
systemctl daemon-reload || true
udevadm control --reload-rules || true
rm -rf /opt/ps5camera
echo "$(t 'PS5 HD Camera удалена.' 'PS5 HD Camera was removed.')"
