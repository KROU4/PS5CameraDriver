#!/usr/bin/env bash
# Removes the PS5 HD Camera firmware daemon (sudo bash uninstall.sh). Firmware already in a plugged
# camera stays until it is unplugged.
set -euo pipefail
case "${LC_ALL:-${LC_MESSAGES:-${LANG:-}}}" in ru*) ru=1 ;; *) ru="" ;; esac
t() { if [[ -n "$ru" ]]; then printf '%s' "$1"; else printf '%s' "$2"; fi; }
if [[ $EUID -ne 0 ]]; then
    echo "$(t 'Запустите с правами администратора' 'Run as administrator'): sudo bash uninstall.sh" >&2
    exit 1
fi
plist=/Library/LaunchDaemons/com.ps5camera.fwload.plist
launchctl bootout system "$plist" 2>/dev/null || true
rm -f "$plist"
rm -rf "/Library/Application Support/PS5Camera"
echo "$(t 'PS5 HD Camera удалена (журнал /Library/Logs/PS5Camera.log оставлен).' 'PS5 HD Camera was removed (the log /Library/Logs/PS5Camera.log is kept).')"
