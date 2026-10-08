#!/usr/bin/env bash
# Removes the PS5 HD Camera firmware daemon (sudo bash uninstall.sh). Firmware already in a plugged
# camera stays until it is unplugged.
set -euo pipefail
if [[ $EUID -ne 0 ]]; then
    echo "Запустите с правами администратора: sudo bash uninstall.sh" >&2
    exit 1
fi
plist=/Library/LaunchDaemons/com.ps5camera.fwload.plist
launchctl bootout system "$plist" 2>/dev/null || true
rm -f "$plist"
rm -rf "/Library/Application Support/PS5Camera"
echo "PS5 HD Camera удалена (журнал /Library/Logs/PS5Camera.log оставлен)."
