# Version 1.2.8 — 13 October 2026

Improves preservation of weak moving details during denoising, makes the subject contour in
depth-based background blur slightly more tolerant, and stabilizes the depth of a stationary
background. These three improvements are enabled by default on Windows and Linux.

The denoise, sharpening, exposure, calibration and camera-mode settings are preserved during
an upgrade. This release does not add a 1080p30 mode.

To return to the previous processing, run the following from an elevated Windows terminal:

```powershell
& "$env:ProgramFiles\PS5Camera\ps5cam-ctl.exe" set quality 0
```

Use `set quality 1` to enable the improvements again. On Linux, set `quality = 0` or
`quality = 1` in `/etc/ps5cam/bokeh.conf`; the service reloads it automatically.

The changes passed GPU tests for correlated noise, detail retention and large foreground motion,
and comparisons on five recorded camera scenes. In those recordings, mean background-depth
variation fell by 13.5% and blur inside the subject-edge evaluation band fell by 1.9%. These are
measurements of that corpus, not a guarantee for every camera, light or subject. Restoring detail
can slightly increase brightness noise; every measured scene remained within the 5% regression
limit.

Stereo-noise balancing, adaptive texture rejection and diagonal SGM remain disabled because
their recorded-scene results were insufficient for release.
