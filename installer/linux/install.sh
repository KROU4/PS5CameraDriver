#!/usr/bin/env bash
# PS5 HD Camera for Linux: builds the camera firmware (Sony's original, downloaded or given with
# --original, plus the driver's changes) and installs the loader. On every plug-in udev starts a
# systemd service that uploads it; the camera then works as a normal UVC webcam (uvcvideo).
#   sudo bash install.sh [--bokeh on|off] [--original SONY_FIRMWARE.bin]
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
target=/opt/ps5camera
rules=/etc/udev/rules.d/70-ps5camera.rules
unit=/etc/systemd/system/ps5camera-fwload.service

usage() { echo "Использование: sudo bash install.sh [--bokeh on|off] [--original SONY_FIRMWARE.bin]" >&2; exit 2; }
bokeh=ask
original=""
answer=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --bokeh)
            bokeh="${2:-}"
            [[ "$bokeh" == on || "$bokeh" == off ]] || { echo "--bokeh принимает on или off" >&2; exit 2; }
            shift 2 ;;
        --original)
            original="${2:-}"
            [[ -f "$original" ]] || { echo "--original: нет файла ${original:-(пусто)}" >&2; exit 2; }
            original="$(cd "$(dirname "$original")" && pwd)/$(basename "$original")"
            shift 2 ;;
        *) usage ;;
    esac
done
# A copy of Sony's original placed next to the installer is used instead of downloading it.
if [[ -z "$original" && -f "$here/sony-firmware.bin" ]]; then original="$here/sony-firmware.bin"; fi

if [[ $EUID -ne 0 ]]; then
    echo "Запустите с правами root: sudo bash install.sh" >&2
    exit 1
fi
for f in ps5cam_fwload.py ps5cam-firmware.json; do
    [[ -f "$here/$f" ]] || { echo "В папке установщика нет файла $f" >&2; exit 1; }
done
if ! command -v systemctl >/dev/null || ! command -v udevadm >/dev/null; then
    echo "Нужны systemd и udev. Без них загружайте прошивку вручную:" >&2
    echo "  python3 ps5cam_fwload.py make ps5cam-firmware.json firmware.bin" >&2
    echo "  sudo python3 ps5cam_fwload.py load firmware.bin --wait 30   (и переподключите камеру)" >&2
    exit 1
fi

if [[ "$bokeh" == ask ]]; then
    echo "PS5 HD Camera: как должна работать камера?"
    echo "  1  С боке (фон размыт по глубине)"
    echo "  2  Без боке: обычная камера (родные 1080p, 30 и 60 к/с)"
    while [[ "$bokeh" != on && "$bokeh" != off ]]; do
        read -r -p "Введите 1 или 2: " answer || { echo; echo "Нет ввода: запустите с --bokeh off" >&2; exit 1; }
        case "$answer" in 1) bokeh=on ;; 2) bokeh=off ;; esac
    done
fi
if [[ "$bokeh" == on ]]; then
    echo
    echo "Боке пока есть только в версии для Windows: там глубину по двум сенсорам считает видеокарта"
    echo "внутри системной виртуальной камеры. Для Linux такой модуль ещё не написан."
    read -r -p "Установить камеру без боке? [Y/n] " answer || answer=""
    [[ "$answer" =~ ^[Nn] ]] && exit 1
fi

echo "==> Python и pyusb"
if ! python3 -c 'import usb.core' 2>/dev/null; then
    if command -v apt-get >/dev/null; then
        apt-get install -y python3-usb || { apt-get update && apt-get install -y python3-usb; }
    elif command -v dnf >/dev/null; then dnf install -y python3-pyusb
    elif command -v pacman >/dev/null; then pacman -S --needed --noconfirm python-pyusb
    elif command -v zypper >/dev/null; then zypper --non-interactive install python3-pyusb
    else
        echo "Установите python3 и pyusb (пакет python3-usb / python3-pyusb) и запустите снова." >&2
        exit 1
    fi
fi
python3 -c 'import usb.core' || { echo "pyusb не установился" >&2; exit 1; }

echo "==> Файлы в $target"
install -d "$target"
install -m 0755 "$here/ps5cam_fwload.py" "$target/ps5cam_fwload.py"
install -m 0644 "$here/ps5cam-firmware.json" "$target/ps5cam-firmware.json"
install -m 0755 "$here/uninstall.sh" "$target/uninstall.sh"

echo "==> Прошивка камеры: оригинал Sony + изменения драйвера"
make_args=(make "$target/ps5cam-firmware.json" "$target/firmware.bin")
if [[ -n "$original" ]]; then make_args+=(--original "$original"); fi
if ! python3 "$target/ps5cam_fwload.py" "${make_args[@]}"; then
    echo "Не удалось получить оригинальную прошивку Sony. Нужен интернет, либо положите её рядом" >&2
    echo "с установщиком как sony-firmware.bin (или укажите --original ФАЙЛ)." >&2
    exit 1
fi

echo "==> Служба загрузки прошивки и правило udev"
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

echo "==> Камера"
if python3 "$target/ps5cam_fwload.py" load "$target/firmware.bin"; then
    echo "    если камера подключена, через пару секунд она появится как /dev/video*"
else
    echo "    прошивку загрузить не удалось (подробности выше): переподключите камеру" >&2
fi

echo
echo "Готово: PS5 HD Camera без боке. Подключите камеру и выберите её в приложении (устройство"
echo "\"USB Camera-OV580\"; формат 1920x1080, 30 или 60 к/с). Журнал: journalctl -u ps5camera-fwload"
echo "Удаление: sudo bash $target/uninstall.sh"
