# winget

[English](README.md)

Пакет рассчитан на [Windows Package Manager](https://learn.microsoft.com/windows/package-manager/)
под именем `KROU4.PS5CameraDriver`:

```powershell
winget install KROU4.PS5CameraDriver
```

Манифесты хранятся в [microsoft/winget-pkgs](https://github.com/microsoft/winget-pkgs), а не здесь.
Для каждого выпуска сборка (`.github/workflows/build.yml`, на тег `v*`) оставляет их артефактом
`winget-manifests`: их делает [make-manifests.ps1](make-manifests.ps1) из пакетов MSI, выложенных в
выпуск. Чтобы отправить версию:

1. Скачайте артефакт и проверьте: `winget validate --manifest manifests\k\KROU4\PS5CameraDriver\<версия>`.
2. По желанию попробуйте: `winget settings --enable LocalManifestFiles`, затем
   `winget install --manifest manifests\k\KROU4\PS5CameraDriver\<версия>`.
3. Откройте pull request в microsoft/winget-pkgs с папкой `manifests\k\KROU4\PS5CameraDriver\<версия>`
   (или через [wingetcreate](https://github.com/microsoft/winget-create):
   `wingetcreate submit manifests\k\KROU4\PS5CameraDriver\<версия>`).

MSI принимает `BOKEH=on|off`, `TRAY=1` и `SONYFIRMWARE=<путь>`; через winget:
`winget install KROU4.PS5CameraDriver --override "/qn BOKEH=off"`.
