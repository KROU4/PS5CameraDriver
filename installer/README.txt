PS5 HD Camera for Windows 11
============================
(Русская версия: README.ru.txt)

The PlayStation 5 camera (CFI-ZEY1) works as one regular webcam, "PS5 Camera".
It works the way the PS5 does: the main sensor gives the picture, the second sensor
measures the depth of the scene together with the first, and the graphics card blurs
the background by that depth (bokeh).

Install
-------
1. Plug the camera into a USB 3 port.
2. Run PS5CameraDriver.msi (PS5CameraDriver-ru.msi in Russian) from the release page and
   confirm the administrator rights prompt. The installer downloads Sony's original firmware
   and builds the driver's firmware from it; without internet access the installation still
   completes, and the service fetches the firmware itself when the camera is plugged into a
   computer that is online (it tries every 10 minutes). Bokeh is on after the first
   installation; an update keeps your choice.
   Unattended: msiexec /i PS5CameraDriver.msi /qn [BOKEH=on|off] [TRAY=1]
   [SONYFIRMWARE=C:\path\to\sony-firmware.bin]; the log is %ProgramData%\PS5Camera\install.log.
   Or, from this ZIP package: run Install.cmd. It asks for the mode (1 — with bokeh, 2 —
   without; or Install.cmd -Bokeh on / -Bokeh off), lists what it will change in the system
   and asks to confirm (to skip that: Install.cmd -Bokeh on -Yes).
3. In Zoom, Discord, Teams, Telegram, OBS, a browser or the Camera app choose
   "PS5 Camera". Programs that were open during the installation see it after a restart.

Nothing else needs to be started: on every plug-in the service uploads the firmware
to the camera and the camera appears in the system.

The effect runs inside the camera itself (the Windows "Device MFT" component, with no
kernel driver and no test mode): it is the same USB camera, just named "PS5 Camera" and
with bokeh. The previous way — a separate virtual camera "PS5 Camera (Windows Virtual
Camera)" with the USB camera itself hidden: Install.cmd -VirtualCamera.

Bokeh on and off: Settings → Bluetooth & devices → Cameras → PS5 Camera → Camera effects
→ Background effects ("Portrait blur" is the bokeh as set, "Standard blur" the strongest),
or the camera effects of programs that offer them. Windows remembers the choice there and
applies it whenever a program opens the camera. From a command prompt (cmd):
  "C:\Program Files\PS5Camera\ps5cam-ctl.exe" set mode 0   (with bokeh)
  "C:\Program Files\PS5Camera\ps5cam-ctl.exe" set mode 1   (without bokeh)
(in PowerShell put & before the command); the service then updates Windows' remembered
choice too, while an administrator is signed in.

Picture
-------
- Noise reduction (on by default, medium): in a dim room the sensor noise is averaged over
  several frames; what moves is followed (motion compensation), and where that fails it is
  smoothed within the frame instead; colour noise is smoothed over a wide area.
  ps5cam-ctl set denoise 0..100 (default 90, 0 = off).
- Auto brightness: brightens a dim picture digitally. While the camera measures depth (bokeh on,
  or the depth camera) it meters the person (the head of what the bokeh keeps sharp): a face
  against a bright window is not left dark, and a face lit by the screen in a dark room is not
  blown out.
- Bokeh sharp zone: the whole head stays sharp — ears, hair and headphones, which depth shows
  joined to the face — while the background behind it is blurred. The zone is the same depth in
  centimetres from about 45 cm to 1.3 m from the camera.
- Sharpening (on by default): edges gain contrast, while detail as small as the noise is left
  alone, so in a dim room it all but stops. ps5cam-ctl set sharpen 0..100 (default 50, 0 = off).
- Anti-flicker (automatic by default): in a dim room the camera exposes up to the whole
  frame time instead of 10 ms steps, unless lamps flicker; when the picture shows the moving
  bands of flickering lamps it goes back to 50 Hz at once and stays there for half an hour.
  ps5cam-ctl set antiflicker 0 (auto), 1 (50 Hz), 2 (60 Hz), 3 (off). The mains frequency
  follows the country Windows is set to; a program that sets anti-flicker itself keeps its
  choice.

Tray icon (for development: Install.cmd -Tray)
---------
- A click on the icon turns bokeh on and off. Blue lenses on the icon mean bokeh is on,
  grey — off. A right click opens the menu.
- Mode: portrait (depth bokeh), plain camera, second sensor, depth map, both sensors
  side by side (in these views the second sensor comes at 960x540).
- Background blur: light, medium, strong, maximum.
- Focus: autofocus on the person or fixed (near, middle, far).
- Bokeh highlights, auto brightness for a dark room, noise reduction, sharpening, anti-flicker.
- A link to the camera's page in Windows Settings (background effects).
- "Full HD 60 fps only": programs see a single format, 1920x1080@60 (the default).
  Without it — 1080p and 720p at 30 and 60 fps, and "Prefer 60 fps" decides which
  mode programs see first.
Changes apply immediately; there is no need to reselect the camera in the program.

Modes
-----
- Plain camera: native 1920x1080 from one sensor, full field of view, at both 30 and
  60 fps. 60 fps comes from the driver's firmware (Sony limits 1080p to 30 fps).
- Portrait (bokeh), 30 and 60 fps: the picture is the same native 1920x1080 from the
  same sensor, full field of view, no scaling. The second sensor comes along downscaled
  to 960x540 (the camera's bridge chip does that in hardware) and is used only for
  depth: depth is computed at 640x360 anyway. No neural networks are used.
- Formats for programs: by default only 1920x1080 at 60 fps (NV12 and YUY2), so every
  program gets Full HD 60, even a browser that would take 1280x720@30 by itself.
  A program that needs less scales the picture down itself. If an old program insists
  on 1280x720 or 30 fps and does not start, bring back the full list (1920x1080 and
  1280x720, 30 and 60 fps):
    "C:\Program Files\PS5Camera\ps5cam-ctl.exe" set fullhdonly 0
  (back: set fullhdonly 1). The list changes when no program holds the camera.
When switching between the plain camera and the depth modes the camera reopens
itself, and the picture freezes for about half a second.
Why the second sensor is downscaled: this camera's USB 3.0 link carries at most
~393 MB/s, two full 1080p60 pictures need ~498 MB/s, and 1080p + 960x540 ~320 MB/s.
If the camera still runs an older firmware (until it is replugged after an update),
bokeh works the old way: at 30 fps from both full sensors, at 60 fps from a 1280x800 crop.
At 60 fps under lamps that do flicker at 100 Hz the exposure stays limited to 10 ms, so the
picture is darker there than at 30 fps; auto brightness and noise reduction make up for part
of it. If that is not enough, bring back the full list of formats (see above) and choose
30 fps in the program.

As with any webcam, one program uses the camera at a time. A second one gets a
"camera in use" error until the first one releases it.

Troubleshooting
---------------
- Logs: %ProgramData%\PS5Camera\dmft.log (video; vcam.log with -VirtualCamera) and
  %ProgramData%\PS5Camera\service\service.log (service).
- Status: "C:\Program Files\PS5Camera\ps5cam-ctl.exe" status
- If there is no picture after plugging the camera in, replug it.
- The camera measures depth from about half a metre. Anything closer turns into noise
  on the depth map, and bokeh blurs it unevenly. Sit further from the camera.
- With -VirtualCamera the raw stereo camera "USB Camera-OV580" is hidden. To show it:
  ps5cam-ctl unhide (as administrator).

Uninstall
---------
Settings → Apps → Installed apps → PS5 HD Camera (or Uninstall.cmd after an installation
from the ZIP package).
