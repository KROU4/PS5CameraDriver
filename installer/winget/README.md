# winget

[Русский](README.ru.md)

The package is meant for the [Windows Package Manager](https://learn.microsoft.com/windows/package-manager/)
as `KROU4.PS5CameraDriver`:

```powershell
winget install KROU4.PS5CameraDriver
```

The manifests live in [microsoft/winget-pkgs](https://github.com/microsoft/winget-pkgs), not here.
For every release the build (`.github/workflows/build.yml`, on a `v*` tag) leaves them as the
`winget-manifests` artifact, made by [make-manifests.ps1](make-manifests.ps1) from the MSI packages
it uploaded to the release. To submit a version:

1. Download the artifact and check it: `winget validate --manifest manifests\k\KROU4\PS5CameraDriver\<version>`.
2. Optionally try it: `winget settings --enable LocalManifestFiles`, then
   `winget install --manifest manifests\k\KROU4\PS5CameraDriver\<version>`.
3. Open a pull request to microsoft/winget-pkgs with the folder `manifests\k\KROU4\PS5CameraDriver\<version>`
   (or use [wingetcreate](https://github.com/microsoft/winget-create):
   `wingetcreate submit manifests\k\KROU4\PS5CameraDriver\<version>`).

The MSI accepts `BOKEH=on|off`, `TRAY=1` and `SONYFIRMWARE=<path>`; with winget:
`winget install KROU4.PS5CameraDriver --override "/qn BOKEH=off"`.

The Windows build also tests a fresh English MSI installation, an upgrade to the Russian MSI
with the user's blur setting preserved, and removal. To run the same check in an elevated
PowerShell on a disposable Windows machine (for example Windows Sandbox):

```powershell
./installer/msi/test-install.ps1 -Dist dist -Out out/msi-test
```

The test refuses an existing installation and leaves MSI and custom-action logs in `-Out`.
It verifies the running service, boot driver, certificate stores and removal of the signing
private key. This catches failures that manifest validation alone cannot detect, including
certificate import from the MSI's SYSTEM custom action. The installer uses `certutil` for the
machine certificate stores and checks each import; the PowerShell `Import-Certificate` cmdlet
can fail for `TrustedPublisher` under SYSTEM on a fresh Windows Sandbox.
