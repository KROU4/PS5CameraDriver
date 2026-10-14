# Website maintenance

The English page is `docs/index.html`; the Russian page is `docs/ru/index.html`.
Both use `assets/site.css` and `assets/depth.js`. These are plain static files:
there is no package manager, bundler, external font dependency, or build step.
GitHub Pages publishes `/docs` on `main`.

## Design and content

The camera's two lenses anchor the first screen. Cool white (`#f3f6fa`), slate
(`#152b46`), cobalt (`#1759d1`), navy (`#122d4d`) and pale cyan (`#c5f1f5`)
distinguish the hardware introduction from the depth explanation. System fonts
keep English and Cyrillic consistent without downloading a font.

The interactive diagram has three views: stereo pair, estimated depth, and
bokeh. A distance slider illustrates inverse-depth disparity and background
separation while focus follows the subject. Its shapes, distances and blur
radius are illustrative; it is neither a captured camera image nor the actual
GPU pipeline. The background is fixed at 3 m. The real pipeline uses confidence
checks and filtering that the diagram explains in text but does not simulate.

With JavaScript disabled, all three diagrams and their explanations remain
visible, while interactive controls are disabled. Downloads, language links,
navigation and technical disclosure use normal HTML. Keyboard focus is visible;
the slider exposes a localized distance; reduced-motion preferences suppress
smooth scrolling.

## Technical source of truth

- `src/core/cameramodes.cpp`: packed 2448×1088 depth stream.
- `src/core/pipeline.h`: main 1920×1080 and half-size second 960×540 view.
- `src/core/pipeline.cpp`: approximately 640×360 matching, census, default
  four-path SGM, confidence/consistency checks, hole filling, temporal/guided
  filtering, subject analysis and compositing.
- `installer/README.txt`, `installer/linux/README.txt`,
  `installer/macos/README.txt`: installation and platform limitations.

Windows has Direct3D 11 stereo bokeh; Linux Vulkan bokeh is experimental;
macOS exposes the plain camera without bokeh. Firmware is obtained separately.
Keep limitations and unsigned-release information visible. Do not present the
pending WinGet submission as an available catalog package.

Performance measurements describe release 1.2.9 on Windows 11 with RTX 3060 Ti
and Ryzen 5 5600, live 1080p bokeh, 180 frames after 30 warm-up frames:
59.825 fps, 33.592 ms mean source timestamp to local Media Foundation reader,
6.431 ms mean GPU compute. Reader latency excludes display, application
rendering, encoding and network. Do not compare these numbers with a previous
benchmark without matching workload and configuration.

## Preview and publish

From the repository root: `python -m http.server 8765 --directory docs`.
Open `/` and `/ru/`. Check 320/390-pixel mobile widths, a desktop width,
all three diagram buttons, both slider endpoints, keyboard controls, FAQ,
technical disclosure, language switching, and relative asset/anchor links.
Run `node --check docs/assets/depth.js` and `git diff --check`.

Changes reach the public site after the repository's required PR check and
merge, followed by a successful GitHub Pages deployment. Verify both live
language URLs and the served CSS/JS; a merged PR alone is not deployment proof.
