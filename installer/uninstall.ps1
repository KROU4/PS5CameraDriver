# PS5 HD Camera driver uninstaller. Elevates itself; talks Russian on a Russian Windows, else English.
#   -FromMsi  run by the MSI package while it removes itself: it deletes its own files and its
#             "Installed apps" entry, this script only what the installation made besides them
#   -KeepInstallLog  leaves %ProgramData%\PS5Camera\install.log (the MSI undoing a failed installation)
param([switch]$NoPause, [switch]$FromMsi, [switch]$KeepInstallLog)
$ErrorActionPreference = 'Continue'
if ($FromMsi) { $NoPause = $true }
$russian = (Get-UICulture).TwoLetterISOLanguageName -eq 'ru'
function T([string]$ru, [string]$en) { if ($script:russian) { $ru } else { $en } }
$target = if ($FromMsi) { $PSScriptRoot.TrimEnd('\') } else { Join-Path $env:ProgramFiles 'PS5Camera' }
$ctl = Join-Path $target 'ps5cam-ctl.exe'

$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    $argList = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSCommandPath`"")
    if ($NoPause) { $argList += '-NoPause' }
    if ($FromMsi) { $argList += '-FromMsi' }
    if ($KeepInstallLog) { $argList += '-KeepInstallLog' }
    Start-Process powershell.exe -Verb RunAs -ArgumentList $argList
    exit
}

Get-Process ps5cam-tray -ErrorAction SilentlyContinue | Stop-Process -Force
if (Test-Path (Join-Path $target 'ps5cam-svc.exe')) { & (Join-Path $target 'ps5cam-svc.exe') uninstall 2>&1 | Out-Host }
if (Test-Path $ctl) {
    # Takes the device MFT off and gives back the camera's own name, then clears the hiding flags,
    # on every camera instance, also those not plugged in right now.
    & $ctl dmft off 2>&1 | Out-Host
    & $ctl unhide 2>&1 | Out-Host
    & $ctl remove 2>&1 | Out-Host
}
Stop-Service FrameServerMonitor, FrameServer -Force -ErrorAction SilentlyContinue
foreach ($dll in 'ps5cam-vcam.dll', 'ps5cam-dmft.dll') {
    if (Test-Path (Join-Path $target $dll)) { Start-Process regsvr32.exe -ArgumentList '/s', '/u', "`"$target\$dll`"" -Wait }
}
Remove-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Run' -Name PS5CameraTray -ErrorAction SilentlyContinue
# The boot-mode WinUSB package and the certificate that install.ps1 made to sign it.
Get-WindowsDriver -Online -ErrorAction SilentlyContinue | Where-Object { $_.OriginalFileName -like '*\ps5cam-boot.inf' } |
    ForEach-Object { & pnputil.exe /delete-driver $_.Driver /uninstall /force 2>&1 | Out-Null }
foreach ($store in 'Root', 'TrustedPublisher') {
    Get-ChildItem "Cert:\LocalMachine\$store" | Where-Object { $_.Subject -eq 'CN=PS5 Camera driver signer (this computer only)' } |
        Remove-Item -ErrorAction SilentlyContinue
}
# reg.exe rather than the PowerShell registry provider, which is refused access to HKLM\SOFTWARE here.
if (-not $FromMsi) { & reg.exe delete 'HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\PS5Camera' /f 2>&1 | Out-Null }
& reg.exe delete 'HKLM\SOFTWARE\PS5Camera' /f 2>&1 | Out-Null
Start-Sleep -Milliseconds 500
if ($FromMsi) {
    # What the installation made next to the MSI's files: the firmware, the signed boot driver
    # package, DLLs moved aside while in use.
    Remove-Item (Join-Path $target 'firmware.bin'), (Join-Path $target 'driver\ps5cam-boot.cat') -Force -ErrorAction SilentlyContinue
    Get-ChildItem $target -Filter '*.old-*' -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue
} else {
    Remove-Item $target -Recurse -Force -ErrorAction SilentlyContinue
}
$data = Join-Path $env:ProgramData 'PS5Camera'
if ($KeepInstallLog) {
    Get-ChildItem $data -Force -ErrorAction SilentlyContinue | Where-Object { $_.Name -ne 'install.log' } |
        Remove-Item -Recurse -Force -ErrorAction SilentlyContinue
} else {
    Remove-Item $data -Recurse -Force -ErrorAction SilentlyContinue
}
Write-Host (T 'PS5 Camera удалена вместе с драйвером загрузчика и его сертификатом; камера снова видна приложениям как обычная USB-камера.' `
    'PS5 Camera was removed together with the boot loader driver and its certificate; programs see the camera as a plain USB camera again.')
if (-not $NoPause) { Read-Host (T 'Нажмите Enter, чтобы закрыть окно' 'Press Enter to close this window') | Out-Null }
