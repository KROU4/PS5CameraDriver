# Writes the winget manifests of a release (version, installer with both MSIs, en-US and ru-RU
# locales) for a submission to microsoft/winget-pkgs:
#   installer\winget\make-manifests.ps1 -Version 1.2.3 [-Dist dist] [-Out dist\winget]
# The installer URLs point at the GitHub release v<Version>; the hashes come from the MSIs in -Dist,
# which must be the files uploaded there. Check with: winget validate <Out>; test with
# winget install --manifest <Out> (needs "winget settings --enable LocalManifestFiles").
param(
    [Parameter(Mandatory = $true)][string]$Version,
    [string]$Dist,
    [string]$Out
)
$ErrorActionPreference = 'Stop'
$root = Split-Path (Split-Path $PSScriptRoot)
if (-not $Dist) { $Dist = Join-Path $root 'dist' }
if (-not $Out) { $Out = Join-Path $Dist 'winget' }
$id = 'KROU4.PS5CameraDriver'
$repo = 'https://github.com/KROU4/PS5CameraDriver'
$manifestVersion = '1.12.0'
$upgradeCode = '{51A018EE-D582-4CF1-BDBA-D278E34380B4}'  # installer\msi\PS5Camera.wxs

function Hash($file) { (Get-FileHash (Join-Path $Dist $file) -Algorithm SHA256).Hash }
$dir = Join-Path $Out "manifests\k\KROU4\PS5CameraDriver\$Version"
New-Item -ItemType Directory -Force $dir | Out-Null
$utf8 = New-Object System.Text.UTF8Encoding($false)
# Each file starts with the schema of its type, as winget validate and the winget-pkgs checks expect.
function Write-Manifest($name, $type, $text) {
    $header = "# yaml-language-server: `$schema=https://aka.ms/winget-manifest.$type.$manifestVersion.schema.json`n`n"
    [IO.File]::WriteAllText((Join-Path $dir $name), $header + $text.Replace("`r`n", "`n"), $utf8)
}

Write-Manifest "$id.yaml" 'version' @"
PackageIdentifier: $id
PackageVersion: $Version
DefaultLocale: en-US
ManifestType: version
ManifestVersion: $manifestVersion
"@

Write-Manifest "$id.installer.yaml" 'installer' @"
PackageIdentifier: $id
PackageVersion: $Version
MinimumOSVersion: 10.0.22000.0
InstallerType: msi
Scope: machine
InstallModes:
- interactive
- silent
- silentWithProgress
UpgradeBehavior: install
ElevationRequirement: elevatesSelf
AppsAndFeaturesEntries:
- UpgradeCode: '$upgradeCode'
Installers:
- Architecture: x64
  InstallerLocale: en-US
  InstallerUrl: $repo/releases/download/v$Version/PS5CameraDriver.msi
  InstallerSha256: $(Hash 'PS5CameraDriver.msi')
- Architecture: x64
  InstallerLocale: ru-RU
  InstallerUrl: $repo/releases/download/v$Version/PS5CameraDriver-ru.msi
  InstallerSha256: $(Hash 'PS5CameraDriver-ru.msi')
ManifestType: installer
ManifestVersion: $manifestVersion
"@

Write-Manifest "$id.locale.en-US.yaml" 'defaultLocale' @"
PackageIdentifier: $id
PackageVersion: $Version
PackageLocale: en-US
Publisher: KROU4
PublisherUrl: https://github.com/KROU4
PublisherSupportUrl: $repo/issues
PackageName: PS5 HD Camera
PackageUrl: $repo
License: GPL-3.0-only
LicenseUrl: $repo/blob/main/LICENSE
ShortDescription: Driver for the PlayStation 5 HD Camera on Windows 11, with depth-based background blur
Description: |-
  Makes the PlayStation 5 HD Camera (CFI-ZEY1) a regular webcam on Windows 11 at native 1920x1080 and
  60 fps. Its two sensors give depth, from which the graphics card blurs the background (bokeh); the
  effect is switched in Settings > Cameras like Windows' own camera effects. Needs a USB 3 port and an
  internet connection while installing: the installer downloads Sony's original camera firmware and
  applies the driver's changes to it.
Tags:
- bokeh
- camera
- driver
- playstation
- ps5
- webcam
ReleaseNotesUrl: $repo/releases/tag/v$Version
ManifestType: defaultLocale
ManifestVersion: $manifestVersion
"@

Write-Manifest "$id.locale.ru-RU.yaml" 'locale' @"
PackageIdentifier: $id
PackageVersion: $Version
PackageLocale: ru-RU
Publisher: KROU4
PackageName: PS5 HD Camera
License: GPL-3.0-only
ShortDescription: Драйвер камеры PlayStation 5 HD Camera для Windows 11 с размытием фона по глубине
Description: |-
  Делает PlayStation 5 HD Camera (CFI-ZEY1) обычной веб-камерой Windows 11 с родными 1920x1080 при
  60 к/с. Два сенсора камеры дают глубину, по которой видеокарта размывает фон (боке); эффект
  включается в Параметрах → Камеры, как собственные эффекты камеры Windows. Нужен порт USB 3 и
  интернет во время установки: установщик скачивает оригинальную прошивку Sony и вносит в неё
  изменения драйвера.
ManifestType: locale
ManifestVersion: $manifestVersion
"@
"winget manifests: $dir"
