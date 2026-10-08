PS5 HD Camera for Linux
=======================
(Русская версия: README.ru.txt)

Without firmware the PlayStation 5 camera (CFI-ZEY1) does not work as a webcam: the firmware has to
be uploaded on every plug-in. This package installs a loader that does that by itself and, if you
choose it, the bokeh service: the background blurred by depth, as in the Windows version.

Install
-------
    sudo bash install.sh                  (asks for the mode)
    sudo bash install.sh --bokeh on       (with bokeh)
    sudo bash install.sh --bokeh off      (a plain camera; removes bokeh if it was installed)

Requires systemd, udev and python3 with pyusb (package python3-usb / python3-pyusb); the installer
installs pyusb itself through apt, dnf, pacman or zypper.

After installing, plug the camera into a USB 3 port (on USB 2.0 it only delivers 640x400): within a
couple of seconds it appears as /dev/video* named "USB Camera-OV580". Choose the 1920x1080 format
(30 or 60 fps) in your program. Wide formats such as 3840x1080 and 2448x1088 are both sensors side by
side; ordinary calls do not need them.

Bokeh
-----
The service ps5cam-bokehd takes the picture of both sensors, computes depth on the graphics card and
blurs the background like a lens with a shallow depth of field. Programs get the result as a
separate camera, "PS5 Camera" (a v4l2loopback device): choose it instead of "USB Camera-OV580",
which stays in the list as the camera without processing.

Requirements:
  - an x86_64 (64-bit Intel or AMD) computer with glibc 2.34 or newer: Ubuntu 22.04, Debian 12,
    Fedora 35 or newer (the installer checks and otherwise offers the camera without bokeh);
  - a graphics card with Vulkan 1.1: any recent AMD, Intel or NVIDIA (Mesa's drivers, or NVIDIA's
    proprietary driver, which brings its own Vulkan driver). Vulkan's CPU driver (lavapipe) is far
    too slow for a camera and is not used: without a GPU "PS5 Camera" stays black;
  - the v4l2loopback kernel module: the installer installs it (apt, pacman, zypper; on Fedora from
    RPM Fusion) together with the Vulkan loader and Mesa's drivers;
  - with Secure Boot on, v4l2loopback, which is built on your computer (DKMS), loads only once its
    signing key is enrolled. Ubuntu and Debian ask for a password during the installation; at the
    next reboot choose "Enroll MOK" on the blue screen and enter it.

The camera and the GPU work only while a program shows "PS5 Camera" and stop 3 s after it closes.
v4l2loopback older than 0.13 (Ubuntu 24.04, Debian 12) does not tell when a program watches; with it
the service runs the camera while any program has "PS5 Camera" open, even just to list the cameras.
With always_on = 1 (below) the camera and the GPU run whenever the camera is connected, and other
programs cannot open "USB Camera-OV580" then.

Settings are in /etc/ps5cam/bokeh.conf and apply a couple of seconds after the file is saved
(defaults in brackets):
  mode            0 bokeh, 1 main sensor, 2 second sensor, 3 depth, 4 both sensors side by side (0)
  blur            background blur 0..100 (60)
  autofocus       1 focus follows the subject, 0 focus stays at "focus" (1)
  focus           manual focus 0 far .. 100 near (50)
  highlights      emphasis of bright spots in the blur, percent 0..400 (150)
  temporal        how quickly depth follows motion 5..100 (40)
  autobrightness  digital brightening of dim rooms 0 or 1 (1)
  maxgain         the most it may amplify, in tenths 10..160 (60, that is 6x)
  denoise         noise reduction 0..100 (90)
  sharpen         edge sharpening that leaves the noise alone 0..100 (50)
  antiflicker     lamp flicker: 0 auto, 1 50 Hz, 2 60 Hz, 3 off (0)
  mains           mains frequency 50 or 60 (50)
  fps             frame rate 30 or 60 (60)
  always_on       1 run whenever the camera is connected, 0 only while a program watches (0)
  camera, output  device nodes; auto finds them (auto)

The two sensors calibrate themselves the first time the camera streams (aim it at a lit room with
some things in it); the result is kept in /var/lib/ps5cam/calibration. To calibrate again, delete
that file and run: sudo systemctl restart ps5camera-bokeh

"--bokeh off" and uninstall.sh both remove the service with its calibration and shader cache, the
"PS5 Camera" device settings and its udev rule; the v4l2loopback package stays installed, and
"--bokeh off" also keeps the settings in /etc/ps5cam.

Firmware loader log: journalctl -u ps5camera-fwload
Bokeh service log:   journalctl -u ps5camera-bokeh
Devices it uses:     /opt/ps5camera/ps5cam-bokehd --list
Uninstall:           sudo bash /opt/ps5camera/uninstall.sh
