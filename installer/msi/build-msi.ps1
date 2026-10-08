# Builds the MSI packages (English and Russian UI) from the Windows package that package.ps1 made:
#   dist\PS5CameraDriver.msi, dist\PS5CameraDriver-ru.msi
# Needs the WiX Toolset 5 .NET tool (dotnet tool install --global wix --version 5.0.2); the
# extensions are added here.
#   installer\msi\build-msi.ps1 [-Dist dist\PS5CameraDriver] [-Version 1.2.3]
param(
    [string]$Dist,
    [string]$Version
)
$ErrorActionPreference = 'Stop'
$root = Split-Path (Split-Path $PSScriptRoot)
if (-not $Dist) { $Dist = Join-Path $root 'dist\PS5CameraDriver' }
$Dist = (Resolve-Path $Dist).Path
if (-not $Version) { $Version = (Get-Content (Join-Path $root 'VERSION') -TotalCount 1).Trim() }
$wixVersion = '5.0.2'

$wix = Get-Command wix -ErrorAction SilentlyContinue
if (-not $wix) { $wix = Get-Command (Join-Path $env:USERPROFILE '.dotnet\tools\wix.exe') -ErrorAction SilentlyContinue }
if (-not $wix) { throw "wix not found: dotnet tool install --global wix --version $wixVersion" }
$wix = $wix.Source
foreach ($ext in 'WixToolset.Util.wixext', 'WixToolset.UI.wixext') {
    & $wix extension add -g "$ext/$wixVersion" | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "wix extension add $ext failed" }
}

# The license page shows GPL-3.0 from LICENSE, as RTF.
$obj = Join-Path $root 'build\msi'
New-Item -ItemType Directory -Force $obj | Out-Null
$license = [IO.File]::ReadAllText((Join-Path $root 'LICENSE'))
$escaped = $license.Replace('\', '\\').Replace('{', '\{').Replace('}', '\}') -replace "`r?`n", "\par`r`n"
$rtf = Join-Path $obj 'License.rtf'
[IO.File]::WriteAllText($rtf, "{\rtf1\ansi\deff0{\fonttbl{\f0 Consolas;}}\fs16 $escaped}", [Text.Encoding]::ASCII)

$out = Split-Path $Dist
foreach ($culture in 'en-US', 'ru-RU') {
    $msi = Join-Path $out $(if ($culture -eq 'en-US') { 'PS5CameraDriver.msi' } else { 'PS5CameraDriver-ru.msi' })
    & $wix build (Join-Path $PSScriptRoot 'PS5Camera.wxs') -arch x64 -culture $culture `
        -loc (Join-Path $PSScriptRoot "$culture.wxl") -ext WixToolset.Util.wixext -ext WixToolset.UI.wixext `
        -d "Version=$Version" -d "Dist=$Dist" -d "LicenseRtf=$rtf" -intermediatefolder (Join-Path $obj $culture) -o $msi
    if ($LASTEXITCODE -ne 0) { throw "wix build ($culture) failed" }
    Remove-Item ([IO.Path]::ChangeExtension($msi, '.wixpdb')) -ErrorAction SilentlyContinue
    "msi: $msi"
}
