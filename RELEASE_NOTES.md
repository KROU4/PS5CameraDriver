# Version 1.2.9 — 14 October 2026

Adds native macOS installer checks on Apple Silicon and Intel, and observable Windows video
timing through `ps5cam-ctl latency`. Picture settings and calibration are preserved on upgrade.
This release does not add a 1080p30 mode or change the video-quality algorithms.

The macOS installer uses binary-only dependencies with exact versions and SHA-256 hashes,
including the complete Python 3.9 dependency graph. Both native CI jobs install the assembled
ZIP, check the libusb architecture and launchd service, and uninstall it. Firmware fixtures
are synthetic; Sony's firmware is not included in the release.

Windows Device MFT keeps the newest undelivered frame instead of a three-frame FIFO. Replaced
frames are counted and marked discontinuous. Device timestamps and timing metadata follow the
actual image returned by the one-frame GPU readback pipeline, including after flush/recovery.

Close other camera applications and measure 360 frames after 30 warm-up frames:

```powershell
& "$env:ProgramFiles\PS5Camera\ps5cam-ctl.exe" latency 360 0 latency.csv
```

Use `360 40 slow-consumer.csv` to simulate a slow application. Reports contain mean, p50, p95,
maximum and coverage for available metrics; the CSV contains timings and counters, without images.
Unavailable stages remain explicitly unavailable. The measurement ends at the local Media
Foundation reader; preview/display, encoding and network latency require separate measurements.

In short 1080p60 bokeh tests on an RTX 3060 Ti / Ryzen 5 5600, the local 1.2.9 candidate delivered
59.83 fps with a mean capture-source-to-reader time of 34.70 ms (1.2.8: 34.12 ms). A 40 ms pause
between reads produced about 109 ms in both versions. The new telemetry located 75.55 ms of
that delay **after the driver had delivered the sample**, while the driver's ready queue averaged
0.029 ms and GPU compute 6.54 ms. The newest-frame policy is covered by a production Device MFT
regression; this reader test did not demonstrate lower total latency. The candidate test also ran
alongside Sandbox, so the small difference between the short series is not an isolated benchmark.

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
