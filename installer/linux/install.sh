#!/usr/bin/env bash
# PS5 HD Camera for Linux: builds the camera firmware (Sony's original, downloaded or given with
# --original, plus the driver's changes) and installs the loader. On every plug-in udev starts a
# systemd service that uploads it; the camera then works as a normal UVC webcam (uvcvideo).
# Talks Russian in a Russian locale, English otherwise.
#   sudo bash install.sh [--bokeh on|off] [--original SONY_FIRMWARE.bin]
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
target=/opt/ps5camera
rules=/etc/udev/rules.d/70-ps5camera.rules
unit=/etc/systemd/system/ps5camera-fwload.service

case "${LC_ALL:-${LC_MESSAGES:-${LANG:-}}}" in ru*) ru=1 ;; *) ru="" ;; esac
t() { if [[ -n "$ru" ]]; then printf '%s' "$1"; else printf '%s' "$2"; fi; }

usage() { echo "$(t 'Использование' 'Usage'): sudo bash install.sh [--bokeh on|off] [--original SONY_FIRMWARE.bin]" >&2; exit 2; }
bokeh=ask
original=""
answer=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --bokeh)
            bokeh="${2:-}"
            [[ "$bokeh" == on || "$bokeh" == off ]] || { echo "--bokeh $(t 'принимает on или off' 'takes on or off')" >&2; exit 2; }
            shift 2 ;;
        --original)
            original="${2:-}"
            [[ -f "$original" ]] || { echo "--original: $(t 'нет файла' 'no such file') ${original:-(-)}" >&2; exit 2; }
            original="$(cd "$(dirname "$original")" && pwd)/$(basename "$original")"
            shift 2 ;;
        *) usage ;;
    esac
done
# A copy of Sony's original placed next to the installer is used instead of downloading it.
if [[ -z "$original" && -f "$here/sony-firmware.bin" ]]; then original="$here/sony-firmware.bin"; fi

if [[ $EUID -ne 0 ]]; then
    echo "$(t 'Запустите с правами root' 'Run as root'): sudo bash install.sh" >&2
    exit 1
fi
for f in ps5cam_fwload.py ps5cam-firmware.json; do
    [[ -f "$here/$f" ]] || { echo "$(t 'В папке установщика нет файла' 'The installer folder lacks the file') $f" >&2; exit 1; }
done
if ! command -v systemctl >/dev/null || ! command -v udevadm >/dev/null; then
    echo "$(t 'Нужны systemd и udev. Без них загружайте прошивку вручную:' 'systemd and udev are required. Without them upload the firmware by hand:')" >&2
    echo "  python3 ps5cam_fwload.py make ps5cam-firmware.json firmware.bin" >&2
    echo "  sudo python3 ps5cam_fwload.py load firmware.bin --wait 30   ($(t 'и переподключите камеру' 'and replug the camera'))" >&2
    exit 1
fi

if [[ "$bokeh" == ask ]]; then
    echo "$(t 'PS5 HD Camera: как должна работать камера?' 'PS5 HD Camera: how should the camera work?')"
    echo "  1  $(t 'С боке (фон размыт по глубине)' 'With bokeh (background blurred by depth)')"
    echo "  2  $(t 'Без боке: обычная камера (родные 1080p, 30 и 60 к/с)' 'Without bokeh: a plain camera (native 1080p, 30 and 60 fps)')"
    while [[ "$bokeh" != on && "$bokeh" != off ]]; do
        read -r -p "$(t 'Введите 1 или 2: ' 'Enter 1 or 2: ')" answer || { echo; echo "$(t 'Нет ввода: запустите с --bokeh off' 'No input: run with --bokeh off')" >&2; exit 1; }
        case "$answer" in 1) bokeh=on ;; 2) bokeh=off ;; esac
    done
fi
if [[ "$bokeh" == on ]]; then
    echo
    echo "$(t 'Боке пока есть только в версии для Windows: там глубину по двум сенсорам считает видеокарта' 'Bokeh is Windows-only for now: there the graphics card computes depth from the two sensors')"
    echo "$(t 'внутри самой камеры. Для Linux такой модуль ещё не написан.' 'inside the camera itself. No such module exists for Linux yet.')"
    read -r -p "$(t 'Установить камеру без боке? [Y/n] ' 'Install the camera without bokeh? [Y/n] ')" answer || answer=""
    [[ "$answer" =~ ^[Nn] ]] && exit 1
fi

echo "==> Python $(t 'и' 'and') pyusb"
if ! python3 -c 'import usb.core' 2>/dev/null; then
    if command -v apt-get >/dev/null; then
        apt-get install -y python3-usb || { apt-get update && apt-get install -y python3-usb; }
    elif command -v dnf >/dev/null; then dnf install -y python3-pyusb
    elif command -v pacman >/dev/null; then pacman -S --needed --noconfirm python-pyusb
    elif command -v zypper >/dev/null; then zypper --non-interactive install python3-pyusb
    else
        echo "$(t 'Установите python3 и pyusb (пакет python3-usb / python3-pyusb) и запустите снова.' 'Install python3 and pyusb (package python3-usb / python3-pyusb) and run again.')" >&2
        exit 1
    fi
fi
python3 -c 'import usb.core' || { echo "$(t 'pyusb не установился' 'pyusb did not install')" >&2; exit 1; }

echo "==> $(t 'Файлы в' 'Files in') $target"
install -d "$target"
install -m 0755 "$here/ps5cam_fwload.py" "$target/ps5cam_fwload.py"
install -m 0644 "$here/ps5cam-firmware.json" "$target/ps5cam-firmware.json"
install -m 0755 "$here/uninstall.sh" "$target/uninstall.sh"

echo "==> $(t 'Прошивка камеры: оригинал Sony + изменения драйвера' "Camera firmware: Sony's original + the driver's changes")"
make_args=(make "$target/ps5cam-firmware.json" "$target/firmware.bin")
if [[ -n "$original" ]]; then make_args+=(--original "$original"); fi
if ! python3 "$target/ps5cam_fwload.py" "${make_args[@]}"; then
    echo "$(t 'Не удалось получить оригинальную прошивку Sony. Нужен интернет, либо положите её рядом' "Could not get Sony's original firmware. An internet connection is needed, or put it next to")" >&2
    echo "$(t 'с установщиком как sony-firmware.bin (или укажите --original ФАЙЛ).' 'the installer as sony-firmware.bin (or pass --original FILE).')" >&2
    exit 1
fi

echo "==> $(t 'Служба загрузки прошивки и правило udev' 'Firmware upload service and udev rule')"
cat > "$unit" <<EOF
[Unit]
Description=PS5 HD Camera firmware upload

[Service]
Type=oneshot
ExecStart=/usr/bin/env python3 $target/ps5cam_fwload.py load $target/firmware.bin --wait 5
EOF
cat > "$rules" <<EOF
# PS5 HD Camera in boot mode (05a9:0580): upload the firmware, then it comes back as a UVC camera.
ACTION=="add", SUBSYSTEM=="usb", ENV{DEVTYPE}=="usb_device", ATTR{idVendor}=="05a9", ATTR{idProduct}=="0580", TAG+="systemd", ENV{SYSTEMD_WANTS}+="ps5camera-fwload.service"
EOF
systemctl daemon-reload
udevadm control --reload-rules

echo "==> $(t 'Камера' 'Camera')"
if python3 "$target/ps5cam_fwload.py" load "$target/firmware.bin"; then
    echo "    $(t 'если камера подключена, через пару секунд она появится как /dev/video*' 'if the camera is plugged in, it appears as /dev/video* within a couple of seconds')"
else
    echo "    $(t 'прошивку загрузить не удалось (подробности выше): переподключите камеру' 'the firmware could not be uploaded (details above): replug the camera')" >&2
fi

echo
echo "$(t 'Готово: PS5 HD Camera без боке. Подключите камеру и выберите её в приложении (устройство' 'Done: PS5 HD Camera without bokeh. Plug the camera in and choose it in your program (device')"
echo "\"USB Camera-OV580\"; $(t 'формат 1920x1080, 30 или 60 к/с). Журнал' 'format 1920x1080, 30 or 60 fps). Log'): journalctl -u ps5camera-fwload"
echo "$(t 'Удаление' 'Uninstall'): sudo bash $target/uninstall.sh"
