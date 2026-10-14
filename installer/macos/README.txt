PS5 HD Camera for macOS
=======================
(Русская версия: README.ru.txt)

Without firmware the PlayStation 5 camera (CFI-ZEY1) does not work as a webcam: the firmware has to
be uploaded on every plug-in. This package installs a background service that does that by itself.

Install
-------
    sudo bash install.sh

The script asks for the mode. Bokeh (depth-based background blur) is Windows-only for now: on macOS
it would need a camera system extension, which macOS only runs with an Apple developer signature.
So macOS gets a plain camera: native 1920x1080 at 30 and 60 fps.

Requires Python 3.9 or newer from the Command Line Tools (xcode-select --install). Homebrew is not
needed. The complete SHA-256-verified dependency set is pinned: PyUSB 1.3.1, libusb-package
1.0.30.0, importlib-resources 6.5.2 and, on Python 3.9, zipp 3.23.1.

After installing, plug the camera into a USB 3 port (on USB 2.0 it only delivers 640x400) and choose
"USB Camera-OV580" in your program.

Log:       /Library/Logs/PS5Camera.log
Uninstall: sudo bash "/Library/Application Support/PS5Camera/uninstall.sh"
