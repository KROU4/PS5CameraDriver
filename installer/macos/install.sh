#!/usr/bin/env bash
# PS5 HD Camera for macOS: builds the camera firmware (Sony's original, downloaded or given with
# --original, plus the driver's changes) and installs a launchd daemon that uploads it whenever
# the camera is plugged in; the camera then works as a normal UVC webcam.
# Talks Russian in a Russian locale, English otherwise.
#   sudo bash install.sh [--bokeh on|off] [--original SONY_FIRMWARE.bin]
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
target="/Library/Application Support/PS5Camera"
plist=/Library/LaunchDaemons/com.ps5camera.fwload.plist
label=com.ps5camera.fwload

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
    echo "$(t 'Запустите с правами администратора' 'Run as administrator'): sudo bash install.sh" >&2
    exit 1
fi
for f in ps5cam_fwload.py ps5cam-firmware.json; do
    [[ -f "$here/$f" ]] || { echo "$(t 'В папке установщика нет файла' 'The installer folder lacks the file') $f" >&2; exit 1; }
done

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
    echo "$(t 'Боке пока есть только в версии для Windows. На macOS для него нужна системная камера-расширение' 'Bokeh is Windows-only for now. On macOS it would need a camera system extension')"
    echo "$(t '(Camera Extension), а её macOS запускает только с подписью разработчика Apple.' '(Camera Extension), which macOS only runs with an Apple developer signature.')"
    read -r -p "$(t 'Установить камеру без боке? [Y/n] ' 'Install the camera without bokeh? [Y/n] ')" answer || answer=""
    [[ "$answer" =~ ^[Nn] ]] && exit 1
fi

if ! /usr/bin/python3 -c 'import sys; assert sys.version_info >= (3, 8)' 2>/dev/null; then
    echo "$(t 'Нужен python3: установите Command Line Tools (xcode-select --install) и запустите снова.' 'python3 is required: install the Command Line Tools (xcode-select --install) and run again.')" >&2
    exit 1
fi

echo "==> $(t 'Файлы и Python-окружение в' 'Files and Python environment in') $target"
install -d "$target"
install -m 0755 "$here/ps5cam_fwload.py" "$target/ps5cam_fwload.py"
install -m 0644 "$here/ps5cam-firmware.json" "$target/ps5cam-firmware.json"
install -m 0755 "$here/uninstall.sh" "$target/uninstall.sh"
/usr/bin/python3 -m venv "$target/venv"
# libusb-package carries its own libusb, so Homebrew is not needed.
"$target/venv/bin/pip" install --quiet --no-cache-dir --disable-pip-version-check pyusb libusb-package

echo "==> $(t 'Прошивка камеры: оригинал Sony + изменения драйвера' "Camera firmware: Sony's original + the driver's changes")"
make_args=(make "$target/ps5cam-firmware.json" "$target/firmware.bin")
if [[ -n "$original" ]]; then make_args+=(--original "$original"); fi
if ! "$target/venv/bin/python" "$target/ps5cam_fwload.py" "${make_args[@]}"; then
    echo "$(t 'Не удалось получить оригинальную прошивку Sony. Нужен интернет, либо положите её рядом' "Could not get Sony's original firmware. An internet connection is needed, or put it next to")" >&2
    echo "$(t 'с установщиком как sony-firmware.bin (или укажите --original ФАЙЛ).' 'the installer as sony-firmware.bin (or pass --original FILE).')" >&2
    exit 1
fi

echo "==> $(t 'Служба загрузки прошивки (launchd)' 'Firmware upload service (launchd)')"
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
echo "$(t 'Готово: PS5 HD Camera без боке. Подключите камеру и выберите' 'Done: PS5 HD Camera without bokeh. Plug the camera in and choose') \"USB Camera-OV580\" $(t 'в приложении.' 'in your program.')"
echo "$(t 'Журнал' 'Log'): /Library/Logs/PS5Camera.log. $(t 'Удаление' 'Uninstall'): sudo bash \"$target/uninstall.sh\""
