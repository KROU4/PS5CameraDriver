#!/usr/bin/env bash
# PS5 HD Camera for macOS: builds the camera firmware (Sony's original, downloaded or given with
# --original, plus the driver's changes) and installs a launchd daemon that uploads it whenever
# the camera is plugged in; the camera then works as a normal UVC webcam.
#   sudo bash install.sh [--bokeh on|off] [--original SONY_FIRMWARE.bin]
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
target="/Library/Application Support/PS5Camera"
plist=/Library/LaunchDaemons/com.ps5camera.fwload.plist
label=com.ps5camera.fwload

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
    echo "Запустите с правами администратора: sudo bash install.sh" >&2
    exit 1
fi
for f in ps5cam_fwload.py ps5cam-firmware.json; do
    [[ -f "$here/$f" ]] || { echo "В папке установщика нет файла $f" >&2; exit 1; }
done

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
    echo "Боке пока есть только в версии для Windows. На macOS для него нужна системная камера-расширение"
    echo "(Camera Extension), а её macOS запускает только с подписью разработчика Apple."
    read -r -p "Установить камеру без боке? [Y/n] " answer || answer=""
    [[ "$answer" =~ ^[Nn] ]] && exit 1
fi

if ! /usr/bin/python3 -c 'import sys; assert sys.version_info >= (3, 8)' 2>/dev/null; then
    echo "Нужен python3: установите Command Line Tools (xcode-select --install) и запустите снова." >&2
    exit 1
fi

echo "==> Файлы и Python-окружение в $target"
install -d "$target"
install -m 0755 "$here/ps5cam_fwload.py" "$target/ps5cam_fwload.py"
install -m 0644 "$here/ps5cam-firmware.json" "$target/ps5cam-firmware.json"
install -m 0755 "$here/uninstall.sh" "$target/uninstall.sh"
/usr/bin/python3 -m venv "$target/venv"
# libusb-package carries its own libusb, so Homebrew is not needed.
"$target/venv/bin/pip" install --quiet --no-cache-dir --disable-pip-version-check pyusb libusb-package

echo "==> Прошивка камеры: оригинал Sony + изменения драйвера"
make_args=(make "$target/ps5cam-firmware.json" "$target/firmware.bin")
if [[ -n "$original" ]]; then make_args+=(--original "$original"); fi
if ! "$target/venv/bin/python" "$target/ps5cam_fwload.py" "${make_args[@]}"; then
    echo "Не удалось получить оригинальную прошивку Sony. Нужен интернет, либо положите её рядом" >&2
    echo "с установщиком как sony-firmware.bin (или укажите --original ФАЙЛ)." >&2
    exit 1
fi

echo "==> Служба загрузки прошивки (launchd)"
launchctl bootout system "$plist" 2>/dev/null || true
cat > "$plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key><string>$label</string>
    <key>ProgramArguments</key>
    <array>
        <string>$target/venv/bin/python</string>
        <string>$target/ps5cam_fwload.py</string>
        <string>load</string>
        <string>$target/firmware.bin</string>
        <string>--watch</string>
    </array>
    <key>RunAtLoad</key><true/>
    <key>KeepAlive</key><true/>
    <key>StandardErrorPath</key><string>/Library/Logs/PS5Camera.log</string>
</dict>
</plist>
EOF
chmod 0644 "$plist"
launchctl bootstrap system "$plist"

echo
echo "Готово: PS5 HD Camera без боке. Подключите камеру и выберите \"USB Camera-OV580\" в приложении."
echo "Журнал: /Library/Logs/PS5Camera.log. Удаление: sudo bash \"$target/uninstall.sh\""
