#!/usr/bin/env bash
# Removes the PS5 HD Camera firmware loader and the bokeh service (sudo bash uninstall.sh). Firmware
# already in a plugged camera stays until it is unplugged; the v4l2loopback package stays installed.
# echo "$(t ...)" is no useless echo: t prints without the newline that echo adds.
# shellcheck disable=SC2005
set -euo pipefail
case "${LC_ALL:-${LC_MESSAGES:-${LANG:-}}}" in ru*) ru=1 ;; *) ru="" ;; esac
t() { if [[ -n "$ru" ]]; then printf '%s' "$1"; else printf '%s' "$2"; fi; }
if [[ $EUID -ne 0 ]]; then
    echo "$(t 'Запустите с правами root' 'Run as root'): sudo bash uninstall.sh" >&2
    exit 1
fi
systemctl disable --now ps5camera-bokeh.service 2>/dev/null || true
rm -f /etc/udev/rules.d/70-ps5camera.rules /etc/systemd/system/ps5camera-fwload.service \
    /etc/systemd/system/ps5camera-bokeh.service /etc/modprobe.d/ps5camera-v4l2loopback.conf \
    /etc/modules-load.d/ps5camera.conf /etc/udev/rules.d/70-ps5camera-loopback.rules
# Settings, and the calibration and shader cache of the service's dynamic user.
rm -rf /etc/ps5cam /var/lib/ps5cam /var/lib/private/ps5cam /var/cache/ps5cam /var/cache/private/ps5cam
systemctl daemon-reload || true
udevadm control --reload-rules || true
# The "PS5 Camera" device goes away with the module, unless a program still has it open.
for n in /sys/class/video4linux/video*; do
    if [[ -r "$n/name" && "$(cat "$n/name")" == "PS5 Camera" ]]; then
        modprobe -r v4l2loopback 2>/dev/null ||
            echo "$(t 'Устройство «PS5 Camera» исчезнет после перезагрузки.' 'The "PS5 Camera" device disappears after a reboot.')"
        break
    fi
done
rm -rf /opt/ps5camera
echo "$(t 'PS5 HD Camera удалена. Пакет v4l2loopback оставлен: он может быть нужен другим программам.' 'PS5 HD Camera was removed. The v4l2loopback package stays installed: other programs may need it.')"
