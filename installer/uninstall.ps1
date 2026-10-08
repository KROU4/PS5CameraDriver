# Удаление драйвера PS5 HD Camera. Повышает права сам.
param([switch]$NoPause)
$ErrorActionPreference = 'Continue'
$target = Join-Path $env:ProgramFiles 'PS5Camera'
$ctl = Join-Path $target 'ps5cam-ctl.exe'

$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    $argList = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSCommandPath`"")
    if ($NoPause) { $argList += '-NoPause' }
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
& reg.exe delete 'HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\PS5Camera' /f 2>&1 | Out-Null
& reg.exe delete 'HKLM\SOFTWARE\PS5Camera' /f 2>&1 | Out-Null
Start-Sleep -Milliseconds 500
Remove-Item $target -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item (Join-Path $env:ProgramData 'PS5Camera') -Recurse -Force -ErrorAction SilentlyContinue
Write-Host 'PS5 Camera удалена вместе с драйвером загрузчика и его сертификатом; камера снова видна приложениям как обычная USB-камера.'
if (-not $NoPause) { Read-Host 'Нажмите Enter, чтобы закрыть окно' | Out-Null }
