# Configures and builds the project with the VS 2022 Build Tools toolchain (x64, Ninja).
# Usage: .\build.ps1 [-Config Release|Debug] [-Clean]
param(
    [ValidateSet('Release', 'Debug', 'RelWithDebInfo')][string]$Config = 'Release',
    [switch]$Clean
)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$build = Join-Path $root "build\$Config"

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsPath = if (Test-Path $vswhere) { & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>$null }
if (-not $vsPath) { $vsPath = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\BuildTools" }
$devCmd = Join-Path $vsPath 'Common7\Tools\VsDevCmd.bat'
if (-not (Test-Path $devCmd)) { throw "VsDevCmd.bat not found under $vsPath" }

if ($Clean -and (Test-Path $build)) { Remove-Item -Recurse -Force $build }

$cmake = Join-Path $vsPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$ninja = Join-Path $vsPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe'
$installerDir = Split-Path $vswhere
$cmd = "set `"PATH=$installerDir;%PATH%`" && `"$devCmd`" -arch=x64 -host_arch=x64 -no_logo && " +
       "`"$cmake`" -S `"$root`" -B `"$build`" -G Ninja -DCMAKE_BUILD_TYPE=$Config -DCMAKE_MAKE_PROGRAM=`"$ninja`" && " +
       "`"$cmake`" --build `"$build`""
$ErrorActionPreference = 'Continue'
cmd.exe /d /c "$cmd 2>&1"
if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" }
Write-Output "OK: $build"
