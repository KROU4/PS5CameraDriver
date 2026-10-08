PS5 HD Camera for Linux
=======================
(Русская версия: README.ru.txt)

Without firmware the PlayStation 5 camera (CFI-ZEY1) does not work as a webcam: the firmware has to
be uploaded on every plug-in. This package installs a loader that does that by itself.

Install
-------
    sudo bash install.sh

The script asks for the mode. Bokeh (depth-based background blur) is Windows-only for now, so
Linux gets a plain camera: native 1920x1080 at 30 and 60 fps.

Requires systemd, udev and python3 with pyusb (package python3-usb / python3-pyusb); the installer
installs pyusb itself through apt, dnf, pacman or zypper.

After installing, plug the camera into a USB 3 port (on USB 2.0 it only delivers 640x400): within a
couple of seconds it appears as /dev/video* named "USB Camera-OV580". Choose the 1920x1080 format
(30 or 60 fps) in your program. Wide formats such as 3840x1080 and 2448x1088 are both sensors side by
side; ordinary calls do not need them.

Firmware loader log: journalctl -u ps5camera-fwload
Uninstall:           sudo bash /opt/ps5camera/uninstall.sh
