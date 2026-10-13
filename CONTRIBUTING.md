# Contributing

Thanks for your interest! Bug reports, ideas and pull requests are welcome. Issues and discussions
may be written in English or Russian.

## Bugs and ideas

Open an [issue](https://github.com/KROU4/PS5CameraDriver/issues/new/choose) using a template. For a
bug, please include:
- your Windows version (Win+R → `winver`), graphics card and the USB port the camera is plugged into;
- the output of `"C:\Program Files\PS5Camera\ps5cam-ctl.exe" status`;
- the logs from `%ProgramData%\PS5Camera` (`dmft.log`, `vcam.log`, `service\service.log`).

Questions about setup and usage go to [Discussions](https://github.com/KROU4/PS5CameraDriver/discussions).

## Pull requests

- One topic per pull request; describe what changes and how you tested it.
- Build with `.\build.ps1`, package with `.\package.ps1` (requires VS 2022 Build Tools and Windows SDK
  10.0.26100). The GitHub Actions check runs the same steps.
- After building, run `ctest --test-dir build/Release --output-on-failure`. These tests cover service
  failures, camera modes, CPU logic and GPU denoise quality without opening a camera. GPU tests
  use WARP on Windows and lavapipe in Linux CI, so they need no physical GPU.
  CI also tests MSI installation, upgrade and removal on its disposable Windows runner.
- Follow the style of the surrounding code: C++20, comments explain *why* rather than *what*.
- Do not add Sony's firmware or any other third-party binaries to the repository.

## License of contributions

The GPU regression explicitly enables the experimental `EffectSettings::fineDetail` path.
It remains off by default and has no application setting: recorded camera clips still show a
noise regression in one scene. Passing synthetic tests alone does not qualify it for release.

The project is licensed under the [GNU GPL 3.0](LICENSE). By submitting a pull request you confirm
that the contribution is your own work (or that you have the right to submit it) and that you
license it under the same GPL-3.0.

## Release signing

Release files are signed through SignPath.io (see the
[Code signing policy](README.md#code-signing-policy)). Every pull request is reviewed by a team
member with write access, with particular attention to the build scripts (`build.ps1`,
`package.ps1`, `.github/workflows`): a signed file certifies that it was built from this
repository's code.
