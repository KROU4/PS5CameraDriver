#!/usr/bin/env bash
# PS5 HD Camera for Linux: builds the camera firmware (Sony's original, downloaded or given with
# --original, plus the driver's changes) and installs the loader. On every plug-in udev starts a
# systemd service that uploads it; the camera then works as a normal UVC webcam (uvcvideo).
# With bokeh it also installs ps5cam-bokehd: a service that takes both sensors' picture, computes
# depth on the GPU (Vulkan) and writes the picture with the background blurred to the v4l2loopback
# device "PS5 Camera". Talks Russian in a Russian locale, English otherwise.
#   sudo bash install.sh [--bokeh on|off] [--original SONY_FIRMWARE.bin]
# echo "$(t ...)" is no useless echo: t prints without the newline that echo adds.
# shellcheck disable=SC2005
set -euo pipefail
# Files in /etc are root's and readable by all, whatever the calling shell's umask.
umask 022

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
target=/opt/ps5camera
rules=/etc/udev/rules.d/70-ps5camera.rules
unit=/etc/systemd/system/ps5camera-fwload.service
bokeh_unit=/etc/systemd/system/ps5camera-bokeh.service
bokeh_conf=/etc/ps5cam/bokeh.conf
loopback_rules=/etc/udev/rules.d/70-ps5camera-loopback.rules
modprobe_conf=/etc/modprobe.d/ps5camera-v4l2loopback.conf
modules_conf=/etc/modules-load.d/ps5camera.conf
# The service's calibration and shader cache (DynamicUser keeps them under private/).
bokeh_state=(/var/lib/ps5cam /var/lib/private/ps5cam /var/cache/ps5cam /var/cache/private/ps5cam)

case "${LC_ALL:-${LC_MESSAGES:-${LANG:-}}}" in ru*) ru=1 ;; *) ru="" ;; esac
t() { if [[ -n "$ru" ]]; then printf '%s' "$1"; else printf '%s' "$2"; fi; }

# PCI vendor ids of the graphics cards (0x1002 AMD, 0x8086 Intel, 0x10de NVIDIA).
has_gpu() { grep -qsix "$1" /sys/class/drm/card*/device/vendor; }

# The device node of the "PS5 Camera" loopback device, if it exists.
loopback_node() {
    local n
    for n in /sys/class/video4linux/video*; do
        if [[ -r "$n/name" && "$(cat "$n/name")" == "PS5 Camera" ]]; then
            echo "/dev/${n##*/}"
            return 0
        fi
    done
    return 1
}

secure_boot_on() {
    local f
    for f in /sys/firmware/efi/efivars/SecureBoot-*; do
        # 4 bytes of attributes, then the value.
        if [[ -r "$f" && "$(od -An -t u1 -j 4 -N 1 "$f" | tr -d ' ')" == 1 ]]; then return 0; fi
    done
    return 1
}

# The Vulkan loader with Mesa's drivers; NVIDIA's proprietary driver brings its own Vulkan driver.
# Installed before ps5cam-bokehd is tried, which needs libvulkan.so.1 to start at all.
install_vulkan_packages() {
    local pkgs
    if command -v apt-get >/dev/null; then
        apt-get update || true
        apt-get install -y libvulkan1 mesa-vulkan-drivers || return 1
    elif command -v dnf >/dev/null; then
        dnf install -y vulkan-loader mesa-vulkan-drivers || return 1
    elif command -v pacman >/dev/null; then
        pkgs=(vulkan-icd-loader)
        if has_gpu 0x1002; then pkgs+=(vulkan-radeon); fi
        if has_gpu 0x8086; then pkgs+=(vulkan-intel); fi
        pacman -S --needed --noconfirm "${pkgs[@]}" || return 1
    elif command -v zypper >/dev/null; then
        pkgs=(libvulkan1)
        if has_gpu 0x1002; then pkgs+=(libvulkan_radeon); fi
        if has_gpu 0x8086; then pkgs+=(libvulkan_intel); fi
        zypper --non-interactive install "${pkgs[@]}" || return 1
    else
        echo "$(t 'Неизвестный менеджер пакетов: установите сами Vulkan (загрузчик libvulkan.so.1 и драйвер видеокарты).' 'Unknown package manager: install Vulkan (the libvulkan.so.1 loader and your graphics driver) yourself.')" >&2
        return 1
    fi
}

# v4l2loopback: a kernel module that DKMS or akmods builds for the running kernel.
install_loopback_packages() {
    local kernel owner headers pkgs
    kernel="$(uname -r)"
    if command -v apt-get >/dev/null; then
        pkgs=(v4l2loopback-dkms)
        if apt-cache show "linux-headers-$kernel" >/dev/null 2>&1; then pkgs+=("linux-headers-$kernel"); fi
        apt-get install -y "${pkgs[@]}" || return 1
    elif command -v dnf >/dev/null; then
        if ! dnf install -y akmod-v4l2loopback v4l2loopback; then
            echo "$(t 'Для Fedora модуль v4l2loopback есть в репозитории RPM Fusion (free). Подключите его:' 'On Fedora the v4l2loopback module comes from the RPM Fusion (free) repository. Enable it:')" >&2
            echo "  sudo dnf install https://mirrors.rpmfusion.org/free/fedora/rpmfusion-free-release-\$(rpm -E %fedora).noarch.rpm" >&2
            echo "$(t 'и запустите установщик снова.' 'and run the installer again.')" >&2
            return 1
        fi
        dnf install -y "kernel-devel-$kernel" || true
        # akmods builds the module in the background at boot; build it for this kernel right now.
        if command -v akmods >/dev/null; then akmods --force --kernels "$kernel" || true; fi
    elif command -v pacman >/dev/null; then
        # The headers package goes with the kernel package: linux, linux-lts, linux-zen ...
        owner="$(pacman -Qqo "/usr/lib/modules/$kernel" 2>/dev/null | head -n 1 || true)"
        headers="${owner:-linux}-headers"
        pacman -S --needed --noconfirm v4l2loopback-dkms "$headers" || return 1
    elif command -v zypper >/dev/null; then
        zypper --non-interactive install v4l2loopback-kmp-default || return 1
    else
        echo "$(t 'Неизвестный менеджер пакетов: установите сами модуль ядра v4l2loopback.' 'Unknown package manager: install the v4l2loopback kernel module yourself.')" >&2
        return 1
    fi
}

install_bokeh() {
    local f node version unit_source
    echo "==> $(t 'Боке: v4l2loopback' 'Bokeh: v4l2loopback')"
    if secure_boot_on; then
        echo "    $(t 'Включена безопасная загрузка (Secure Boot): модуль v4l2loopback собирается на этом компьютере' 'Secure Boot is on: the v4l2loopback module is built on this computer (DKMS) and loads only')"
        echo "    $(t '(DKMS) и загрузится, только если подписан ключом, которому доверяет прошивка. Ubuntu и Debian' 'when signed with a key the firmware trusts. Ubuntu and Debian may ask for a password for the')"
        echo "    $(t 'могут попросить пароль для ключа подписи (MOK): тогда при следующей перезагрузке выберите' 'signing key (MOK): then at the next reboot choose "Enroll MOK" on the blue screen and enter')"
        echo "    $(t '«Enroll MOK» на синем экране и введите его. Fedora (akmods): после установки выполните' 'it. Fedora (akmods): after the installation run')"
        echo "      sudo mokutil --import /etc/pki/akmods/certs/public_key.der"
    fi
    install_loopback_packages || true
    if ! modinfo v4l2loopback >/dev/null 2>&1; then
        echo "$(t 'Модуль v4l2loopback не установился (подробности выше), боке не установлено.' 'The v4l2loopback module did not install (details above); bokeh is not installed.')" >&2
        echo "$(t 'Если ядро обновлялось после загрузки, перезагрузите компьютер: модуль собирается для нового ядра.' 'If the kernel was updated since the last boot, reboot: the module is built for the new kernel.')" >&2
        echo "$(t 'Камера без боке установлена и работает. Исправьте ошибку и запустите установщик снова.' 'The camera without bokeh is installed and works. Fix the error and run the installer again.')" >&2
        exit 1
    fi

    echo "==> $(t 'Устройство «PS5 Camera» (v4l2loopback)' 'The "PS5 Camera" device (v4l2loopback)')"
    install -m 0644 /dev/stdin "$modprobe_conf" <<'EOF'
# PS5 HD Camera bokeh: the "PS5 Camera" device that ps5cam-bokehd writes to (install.sh).
options v4l2loopback devices=1 video_nr=42 card_label="PS5 Camera" exclusive_caps=1 max_buffers=4
EOF
    install -m 0644 /dev/stdin "$modules_conf" <<'EOF'
# PS5 HD Camera bokeh (install.sh)
v4l2loopback
EOF
    # The same access as the camera itself: the video group, and the user at the screen.
    install -m 0644 /dev/stdin "$loopback_rules" <<'EOF'
# PS5 HD Camera bokeh: "PS5 Camera" gets the raw camera's permissions (install.sh).
SUBSYSTEM=="video4linux", ATTR{name}=="PS5 Camera", GROUP="video", MODE="0660", TAG+="uaccess"
EOF
    udevadm control --reload-rules
    udevadm trigger --action=change --subsystem-match=video4linux --attr-match="name=PS5 Camera" || true
    for f in /etc/modprobe.d/*.conf; do
        if [[ "$f" != "$modprobe_conf" ]] && grep -qsE '^[[:space:]]*options[[:space:]]+v4l2loopback' "$f"; then
            echo "    $(t 'в' 'note:') $f $(t 'тоже есть параметры v4l2loopback: modprobe объединит их с нашими' 'also has v4l2loopback options: modprobe combines them with ours')"
        fi
    done
    if [[ -d /sys/module/v4l2loopback ]]; then
        if node="$(loopback_node)"; then
            echo "    $node"
        else
            echo "    $(t 'v4l2loopback уже загружен с другими параметрами (виртуальная камера другой программы?).' "v4l2loopback is already loaded with other options (another program's virtual camera?).")"
            echo "    $(t 'Перезагрузите компьютер или закройте программы, которые им пользуются, и выполните:' 'Reboot, or close the programs that use it and run:')"
            echo "      sudo modprobe -r v4l2loopback && sudo modprobe v4l2loopback"
        fi
    elif modprobe v4l2loopback; then
        if node="$(loopback_node)"; then
            echo "    $node"
        else
            echo "    $(t 'модуль загружен, но устройства «PS5 Camera» нет: проверьте другие параметры v4l2loopback (выше)' 'the module loaded but there is no "PS5 Camera" device: check the other v4l2loopback options (above)')" >&2
        fi
    else
        echo "    $(t 'модуль v4l2loopback не загрузился.' 'the v4l2loopback module did not load.')" >&2
        if secure_boot_on; then
            echo "    $(t 'При Secure Boot его ключ должен быть зарегистрирован (см. выше): перезагрузитесь и зарегистрируйте ключ.' 'With Secure Boot its key must be enrolled (see above): reboot and enroll the key.')" >&2
        else
            echo "    $(t 'Если ядро недавно обновлялось, перезагрузитесь: модуль собран для нового ядра.' 'If the kernel was updated recently, reboot: the module was built for the new kernel.')" >&2
        fi
        echo "    $(t 'Служба боке подождёт устройство «PS5 Camera».' 'The bokeh service waits for the "PS5 Camera" device.')" >&2
    fi
    version="$(modinfo -F version v4l2loopback 2>/dev/null || true)"
    if [[ -n "$version" && "$(printf '%s\n' 0.13.0 "$version" | sort -V | head -n 1)" != 0.13.0 ]]; then
        echo "    $(t "v4l2loopback $version старше 0.13 и не сообщает, смотрит ли программа «PS5 Camera»: служба" "v4l2loopback $version is older than 0.13 and does not tell when a program watches \"PS5 Camera\": the")"
        echo "    $(t 'включает камеру, пока устройство открыто какой-нибудь программой (даже только ради списка камер).' 'service runs the camera while any program has the device open (even just to list cameras).')"
    fi

    echo "==> $(t 'Служба боке' 'Bokeh service')"
    systemctl stop ps5camera-bokeh.service 2>/dev/null || true
    # Checked to run on this system before anything was installed (see below).
    mv -f "$target/ps5cam-bokehd.new" "$target/ps5cam-bokehd"
    install -d -m 0755 "$(dirname "$bokeh_conf")"
    if [[ -f "$bokeh_conf" ]]; then
        echo "    $(t 'настройки остаются прежними:' 'keeping the settings in') $bokeh_conf"
    else
        install -m 0644 "$here/bokeh.conf" "$bokeh_conf"
    fi
    # Distributions without the render group give the GPU's render nodes to video.
    unit_source="$here/ps5camera-bokeh.service"
    if getent group render >/dev/null; then
        install -m 0644 "$unit_source" "$bokeh_unit"
    else
        sed 's/^SupplementaryGroups=video render$/SupplementaryGroups=video/' "$unit_source" |
            install -m 0644 /dev/stdin "$bokeh_unit"
    fi
    systemctl daemon-reload
    if ! systemctl enable --now ps5camera-bokeh.service; then
        echo "    $(t 'служба не запустилась:' 'the service did not start:') journalctl -u ps5camera-bokeh" >&2
    fi
}

# --bokeh off on a machine that had bokeh: the service, its program, calibration and cache, the
# module settings and the udev rule go, as with uninstall.sh; the v4l2loopback package and the
# settings in /etc/ps5cam stay.
remove_bokeh() {
    if [[ ! -e "$bokeh_unit" && ! -e "$target/ps5cam-bokehd" && ! -e "$modprobe_conf" && ! -e "$modules_conf" &&
          ! -e "$loopback_rules" ]]; then
        return 0
    fi
    echo "==> $(t 'Удаление боке (пакет v4l2loopback остаётся)' 'Removing bokeh (the v4l2loopback package stays)')"
    systemctl disable --now ps5camera-bokeh.service 2>/dev/null || true
    rm -f "$bokeh_unit" "$target/ps5cam-bokehd" "$target/ps5cam-bokehd.new" "$modprobe_conf" "$modules_conf" \
        "$loopback_rules"
    rm -rf "${bokeh_state[@]}"
    systemctl daemon-reload
    udevadm control --reload-rules
    if loopback_node >/dev/null && ! modprobe -r v4l2loopback 2>/dev/null; then
        echo "    $(t 'устройство «PS5 Camera» исчезнет после перезагрузки' 'the "PS5 Camera" device disappears after a reboot')"
    fi
}

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
    echo "  1  $(t 'С боке (фон размыт по глубине; нужна видеокарта с Vulkan)' 'With bokeh (background blurred by depth; needs a GPU with Vulkan)')"
    echo "  2  $(t 'Без боке: обычная камера (родные 1080p, 30 и 60 к/с)' 'Without bokeh: a plain camera (native 1080p, 30 and 60 fps)')"
    while [[ "$bokeh" != on && "$bokeh" != off ]]; do
        read -r -p "$(t 'Введите 1 или 2: ' 'Enter 1 or 2: ')" answer || { echo; echo "$(t 'Нет ввода: запустите с --bokeh off' 'No input: run with --bokeh off')" >&2; exit 1; }
        case "$answer" in 1) bokeh=on ;; 2) bokeh=off ;; esac
    done
fi
if [[ "$bokeh" == on && "$(uname -m)" != x86_64 ]]; then
    echo
    echo "$(t "Боке нужен процессор x86_64 (64-битный Intel или AMD), а здесь $(uname -m)." "Bokeh needs an x86_64 (64-bit Intel or AMD) processor; this one is $(uname -m).")"
    read -r -p "$(t 'Установить камеру без боке? [Y/n] ' 'Install the camera without bokeh? [Y/n] ')" answer || answer=""
    [[ "$answer" =~ ^[Nn] ]] && exit 1
    bokeh=off
fi
if [[ "$bokeh" == on ]]; then
    for f in ps5cam-bokehd bokeh.conf ps5camera-bokeh.service; do
        [[ -f "$here/$f" ]] || { echo "$(t 'В папке установщика нет файла' 'The installer folder lacks the file') $f $(t '(он нужен для боке)' '(bokeh needs it)')" >&2; exit 1; }
    done
    echo "==> $(t 'Боке: Vulkan (загрузчик и драйверы Mesa)' 'Bokeh: Vulkan (the loader and Mesa drivers)')"
    install_vulkan_packages || echo "    $(t 'Vulkan не установился (подробности выше)' 'Vulkan did not install (details above)')" >&2
    if has_gpu 0x10de; then
        echo "    $(t 'NVIDIA: фирменный драйвер NVIDIA ставит свой драйвер Vulkan' "NVIDIA: NVIDIA's proprietary driver installs its own Vulkan driver")"
    fi
    # Tried here, before the rest is installed: the program needs glibc 2.34 or newer and the Vulkan
    # loader. It goes in place under its own name later (a rename, so a running old copy is no
    # obstacle); whatever stops the installer before that removes it.
    install -d "$target"
    trap 'rm -f "$target/ps5cam-bokehd.new"' EXIT
    install -m 0755 "$here/ps5cam-bokehd" "$target/ps5cam-bokehd.new"
    if bokehd_says="$("$target/ps5cam-bokehd.new" --version 2>&1)"; then
        echo "    $bokehd_says"
    else
        rm -f "$target/ps5cam-bokehd.new"
        echo
        if [[ "$bokehd_says" == *GLIBC_* ]]; then
            echo "$(t 'Служба боке (ps5cam-bokehd) не запускается в этой системе: ей нужна glibc 2.34 или новее' 'The bokeh service (ps5cam-bokehd) does not run on this system: it needs glibc 2.34 or newer')"
            echo "$(t '(Ubuntu 22.04, Debian 12, Fedora 35 и новее).' '(Ubuntu 22.04, Debian 12, Fedora 35 or newer).')"
        else
            echo "$(t 'Служба боке (ps5cam-bokehd) не запускается в этой системе.' 'The bokeh service (ps5cam-bokehd) does not run on this system.')"
        fi
        echo "$(t 'Ошибка' 'The error'): $bokehd_says"
        read -r -p "$(t 'Установить камеру без боке? [Y/n] ' 'Install the camera without bokeh? [Y/n] ')" answer || answer=""
        [[ "$answer" =~ ^[Nn] ]] && exit 1
        bokeh=off
    fi
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
install -m 0644 /dev/stdin "$unit" <<EOF
[Unit]
Description=PS5 HD Camera firmware upload

[Service]
Type=oneshot
ExecStart=/usr/bin/env python3 $target/ps5cam_fwload.py load $target/firmware.bin --wait 5
EOF
install -m 0644 /dev/stdin "$rules" <<EOF
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

if [[ "$bokeh" == on ]]; then
    install_bokeh
    echo
    echo "$(t 'Готово: PS5 HD Camera с боке. В программах выберите камеру «PS5 Camera» (исходная' 'Done: PS5 HD Camera with bokeh. In your programs choose the camera "PS5 Camera" (the raw')"
    echo "$(t '«USB Camera-OV580» тоже видна: это камера без обработки). Настройки: /etc/ps5cam/bokeh.conf' '"USB Camera-OV580" stays visible too: that is the camera without processing). Settings: /etc/ps5cam/bokeh.conf')"
    echo "$(t '(применяются через пару секунд после сохранения). Журнал: journalctl -u ps5camera-bokeh' '(applied a couple of seconds after you save the file). Log: journalctl -u ps5camera-bokeh')"
else
    remove_bokeh
    echo
    echo "$(t 'Готово: PS5 HD Camera без боке. Подключите камеру и выберите её в приложении (устройство' 'Done: PS5 HD Camera without bokeh. Plug the camera in and choose it in your program (device')"
    echo "\"USB Camera-OV580\"; $(t 'формат 1920x1080, 30 или 60 к/с). Журнал' 'format 1920x1080, 30 or 60 fps). Log'): journalctl -u ps5camera-fwload"
fi
echo "$(t 'Удаление' 'Uninstall'): sudo bash $target/uninstall.sh"
