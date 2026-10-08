# Builds Release and assembles the installable packages: dist\PS5CameraDriver (Windows) and
# dist\PS5CameraDriver-linux / -macos (firmware loader; Linux also the bokeh daemon's installer),
# each with a .zip, and the Windows MSI packages (dist\PS5CameraDriver.msi, -ru.msi) when the WiX
# tool is there (or -Msi demands them). No package carries Sony's firmware: the installers build it
# from the original image and firmware\ps5cam-firmware.json. The version comes from VERSION.
#   -LinuxDaemon PATH  the ps5cam-bokehd binary built on Linux, put into the Linux package (CI adds
#                      it to the release package itself)
param([switch]$NoBuild, [switch]$Msi, [switch]$NoMsi, [string]$LinuxDaemon)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$version = (Get-Content (Join-Path $root 'VERSION') -TotalCount 1).Trim()
if (-not $NoBuild) { & (Join-Path $root 'build.ps1') -Config Release }
$b = Join-Path $root 'build\Release\src'
$patch = Join-Path $root 'firmware\ps5cam-firmware.json'
$dist = Join-Path $root 'dist\PS5CameraDriver'
if (Test-Path $dist) { Remove-Item -Recurse -Force $dist }
New-Item -ItemType Directory -Force $dist | Out-Null

Copy-Item "$b\vcam\ps5cam-vcam.dll", "$b\dmft\ps5cam-dmft.dll", "$b\service\ps5cam-svc.exe", "$b\ctl\ps5cam-ctl.exe",
    "$b\tray\ps5cam-tray.exe" $dist
Copy-Item $patch $dist
Copy-Item (Join-Path $root 'installer\Install.cmd'), (Join-Path $root 'installer\Uninstall.cmd') $dist
New-Item -ItemType Directory -Force (Join-Path $dist 'driver') | Out-Null
Copy-Item (Join-Path $root 'installer\driver\ps5cam-boot.inf') (Join-Path $dist 'driver')
# Windows PowerShell 5.1 needs a BOM to read UTF-8 scripts with Cyrillic text.
$utf8Bom = New-Object System.Text.UTF8Encoding($true)
foreach ($s in 'install.ps1', 'uninstall.ps1', 'firmware.ps1') {
    $text = [IO.File]::ReadAllText((Join-Path $root "installer\$s"))
    [IO.File]::WriteAllText((Join-Path $dist $s), $text, $utf8Bom)
}
foreach ($r in 'README.txt', 'README.ru.txt') {
    $readme = [IO.File]::ReadAllText((Join-Path $root "installer\$r"))
    [IO.File]::WriteAllText((Join-Path $dist $r), $readme, $utf8Bom)
}
Set-Content (Join-Path $dist 'version.txt') $version -Encoding ascii

$zip = Join-Path $root 'dist\PS5CameraDriver.zip'
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path "$dist\*" -DestinationPath $zip
Get-ChildItem $dist -Recurse | Select-Object @{n = 'File'; e = { $_.FullName.Substring($dist.Length + 1) } }, Length
"package: $zip (version $version)"

$haveWix = (Get-Command wix -ErrorAction SilentlyContinue) -or (Test-Path (Join-Path $env:USERPROFILE '.dotnet\tools\wix.exe'))
if ($Msi -or ($haveWix -and -not $NoMsi)) { & (Join-Path $root 'installer\msi\build-msi.ps1') -Dist $dist -Version $version }

# Linux and macOS: the firmware tool, its installer and the same firmware patch.
foreach ($os in 'linux', 'macos') {
    $d = Join-Path $root "dist\PS5CameraDriver-$os"
    if (Test-Path $d) { Remove-Item -Recurse -Force $d }
    New-Item -ItemType Directory -Force $d | Out-Null
    Copy-Item (Join-Path $root "installer\$os\*") $d
    Copy-Item (Join-Path $root 'installer\unix\ps5cam_fwload.py'), $patch $d
    if ($os -eq 'linux' -and $LinuxDaemon) { Copy-Item $LinuxDaemon (Join-Path $d 'ps5cam-bokehd') }
    $z = "$d.zip"
    if (Test-Path $z) { Remove-Item $z -Force }
    Compress-Archive -Path "$d\*" -DestinationPath $z
    "package: $z"
}
