# Установщик драйвера PS5 HD Camera. Запускается с правами администратора (через Install.cmd).
#   .\install.ps1 [-Bokeh ask|on|off] [-Tray] [-Original FILE] [-VirtualCamera [-KeepRawCamera]] [-NoPause]
#   -Bokeh          on: фон всегда размыт; off: обычная камера; ask (по умолчанию): спросить
#   -Tray           значок в трее для разработки (переключение режимов на лету); без него значка нет
#   -Original       оригинальная прошивка Sony (иначе sony-firmware.bin рядом или загрузка из интернета)
#   -VirtualCamera  прежний способ: отдельная виртуальная камера, а сама камера скрыта (с -KeepRawCamera
#                   видна). По умолчанию эффект работает внутри самой камеры (Device MFT).
param(
    [ValidateSet('ask', 'on', 'off')][string]$Bokeh = 'ask',
    [switch]$Tray,
    [string]$Original,
    [switch]$VirtualCamera,
    [switch]$KeepRawCamera,
    [switch]$NoPause
)
$ErrorActionPreference = 'Stop'
$src = $PSScriptRoot
$target = Join-Path $env:ProgramFiles 'PS5Camera'
$files = 'ps5cam-vcam.dll', 'ps5cam-dmft.dll', 'ps5cam-svc.exe', 'ps5cam-ctl.exe', 'ps5cam-tray.exe', 'ps5cam-firmware.json',
    'uninstall.ps1'
# Subject of the per-computer certificate that signs the boot driver's catalog (uninstall.ps1 too).
$signerSubject = 'CN=PS5 Camera driver signer (this computer only)'

function Step($text) { Write-Host "==> $text" -ForegroundColor Cyan }
# Runs a console tool; its stderr is diagnostics, not a script error. Returns the exit code.
function Run([string]$exe, [string[]]$arguments) {
    $old = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    & $exe @arguments 2>&1 | ForEach-Object { Write-Host "    $_" }
    $code = $LASTEXITCODE
    $ErrorActionPreference = $old
    return $code
}
function Finish($code) {
    if (-not $NoPause) { Write-Host ''; Read-Host 'Нажмите Enter, чтобы закрыть окно' | Out-Null }
    exit $code
}
function Sha256([byte[]]$bytes) {
    $h = [Security.Cryptography.SHA256]::Create()
    try { return -join ($h.ComputeHash($bytes) | ForEach-Object { $_.ToString('x2') }) } finally { $h.Dispose() }
}

# The camera firmware: Sony's original image (never shipped with the driver) plus our byte changes
# from ps5cam-firmware.json; both the original and the result are checked by hash.
function Build-Firmware([string]$patchPath) {
    $patch = Get-Content $patchPath -Raw | ConvertFrom-Json
    $local = if ($Original) { $Original } elseif (Test-Path (Join-Path $src 'sony-firmware.bin')) { Join-Path $src 'sony-firmware.bin' }
    $candidates = if ($local) { @($local) } else { @($patch.sources) }
    [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
    foreach ($where in $candidates) {
        try {
            $bytes = if ($local) { [IO.File]::ReadAllBytes($where) } else { (New-Object Net.WebClient).DownloadData($where) }
        } catch {
            Write-Host "    не удалось получить ${where}: $($_.Exception.Message)"
            continue
        }
        if ($bytes.Length -ne $patch.original.size -or (Sha256 $bytes) -ne $patch.original.sha256) {
            Write-Host "    ${where}: это не оригинальная прошивка Sony 21.01-03.20.00.04"
            continue
        }
        Write-Host "    оригинал: $where"
        foreach ($run in $patch.runs) {
            $offset = [int]$run[0]
            $hex = [string]$run[1]
            for ($i = 0; $i -lt $hex.Length / 2; $i++) { $bytes[$offset + $i] = [Convert]::ToByte($hex.Substring(2 * $i, 2), 16) }
        }
        if ((Sha256 $bytes) -ne $patch.result.sha256) { throw 'собранная прошивка не совпала с ожидаемой' }
        return , $bytes
    }
    throw 'не удалось получить оригинальную прошивку Sony: нужен интернет, либо положите её рядом с установщиком как sony-firmware.bin (или укажите -Original ФАЙЛ)'
}

function Remove-BootDriver {
    Get-WindowsDriver -Online -ErrorAction SilentlyContinue | Where-Object { $_.OriginalFileName -like '*\ps5cam-boot.inf' } |
        ForEach-Object { Run 'pnputil.exe' @('/delete-driver', $_.Driver, '/uninstall', '/force') | Out-Null }
    foreach ($store in 'Root', 'TrustedPublisher') {
        Get-ChildItem "Cert:\LocalMachine\$store" | Where-Object { $_.Subject -eq $signerSubject } | Remove-Item -ErrorAction SilentlyContinue
    }
}

# WinUSB for the camera in boot mode. The package has no binary of its own (winusb.sys comes with
# Windows); its catalog is signed by a certificate made here, trusted on this computer only, whose
# private key is deleted at once, so it can never sign anything else. That is enough for Windows to
# accept the package without test mode.
function Install-BootDriver {
    Remove-BootDriver
    $dir = Join-Path $target 'driver'
    Remove-Item $dir -Recurse -Force -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force $dir | Out-Null
    $inf = Join-Path $dir 'ps5cam-boot.inf'
    Copy-Item (Join-Path $src 'driver\ps5cam-boot.inf') $inf
    $cat = Join-Path $dir 'ps5cam-boot.cat'
    New-FileCatalog -Path $dir -CatalogFilePath $cat -CatalogVersion 2.0 | Out-Null
    $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject $signerSubject -CertStoreLocation Cert:\LocalMachine\My -NotAfter (Get-Date).AddYears(50)
    try {
        Set-AuthenticodeSignature -FilePath $cat -Certificate $cert -HashAlgorithm SHA256 | Out-Null
        $cer = Join-Path $env:TEMP "ps5cam-signer-$($cert.Thumbprint).cer"
        Export-Certificate -Cert $cert -FilePath $cer | Out-Null
        Import-Certificate -FilePath $cer -CertStoreLocation Cert:\LocalMachine\Root | Out-Null
        Import-Certificate -FilePath $cer -CertStoreLocation Cert:\LocalMachine\TrustedPublisher | Out-Null
        Remove-Item $cer -Force
    } finally {
        Remove-Item "Cert:\LocalMachine\My\$($cert.Thumbprint)" -DeleteKey -ErrorAction SilentlyContinue
    }
    $rc = Run 'pnputil.exe' @('/add-driver', $inf, '/install')
    if ($rc -notin 0, 259, 3010) { throw "драйвер загрузчика не установлен (pnputil $rc)" }
}

$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    # Re-launch elevated. Arguments go as an array, so any characters in the folder path are safe.
    $argList = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSCommandPath`"", '-Bokeh', $Bokeh)
    if ($Tray) { $argList += '-Tray' }
    if ($Original) { $argList += @('-Original', "`"$((Resolve-Path $Original).Path)`"") }
    if ($VirtualCamera) { $argList += '-VirtualCamera' }
    if ($KeepRawCamera) { $argList += '-KeepRawCamera' }
    if ($NoPause) { $argList += '-NoPause' }
    Start-Process powershell.exe -Verb RunAs -ArgumentList $argList
    exit
}

try {
    if ([Environment]::OSVersion.Version.Build -lt 22000) {
        throw 'Нужна Windows 11 (сборка 22000 и новее): драйвер проверялся только на ней.'
    }
    foreach ($f in $files + 'driver\ps5cam-boot.inf') {
        if (-not (Test-Path (Join-Path $src $f))) { throw "В папке установщика нет файла $f" }
    }
    if ($Original -and -not (Test-Path $Original)) { throw "нет файла $Original" }

    if ($Bokeh -eq 'ask') {
        Write-Host 'PS5 HD Camera: как должна работать камера?' -ForegroundColor Cyan
        Write-Host '  1  С боке: фон всегда размыт по глубине (родные 1080p, 30 и 60 к/с)'
        Write-Host '  2  Без боке: обычная камера (родные 1080p, 30 и 60 к/с)'
        for ($tries = 0; $Bokeh -eq 'ask'; $tries++) {
            if ($tries -ge 5) { throw 'режим не выбран: запустите установщик с -Bokeh on или -Bokeh off' }
            $answer = ([string](Read-Host 'Введите 1 или 2')).Trim()
            if ($answer -eq '1') { $Bokeh = 'on' } elseif ($answer -eq '2') { $Bokeh = 'off' }
        }
    }

    Step 'Прошивка камеры: оригинал Sony + изменения драйвера'
    $firmware = Build-Firmware (Join-Path $src 'ps5cam-firmware.json')

    Step 'Драйвер загрузчика камеры (WinUSB для 05A9:0580)'
    Install-BootDriver

    Step 'Остановка работающих компонентов'
    Get-Process ps5cam-tray -ErrorAction SilentlyContinue | Stop-Process -Force
    if (Get-Service PS5CameraService -ErrorAction SilentlyContinue) {
        Stop-Service PS5CameraService -Force -ErrorAction SilentlyContinue
    }
    # Frame Server держит DLL источника загруженной; останавливаем, чтобы заменить файл.
    Stop-Service FrameServerMonitor, FrameServer -Force -ErrorAction SilentlyContinue

    Step "Копирование файлов в $target"
    New-Item -ItemType Directory -Force $target | Out-Null
    Get-ChildItem $target -Filter '*.old-*' -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue
    foreach ($f in $files) {
        $dst = Join-Path $target $f
        try {
            Copy-Item (Join-Path $src $f) $dst -Force -ErrorAction Stop
        } catch {
            # Frame Server (or an app enumerating cameras) can reload the old DLL at any moment. A loaded
            # image can still be renamed: move it aside and put the new file in its place.
            $aside = "$dst.old-$([DateTime]::Now.Ticks)"
            Move-Item $dst $aside -Force
            Copy-Item (Join-Path $src $f) $dst -Force
        }
    }
    [IO.File]::WriteAllBytes((Join-Path $target 'firmware.bin'), $firmware)  # the service uploads this file
    $ctl = Join-Path $target 'ps5cam-ctl.exe'

    Step 'Регистрация компонентов видео'
    # Both, so that ps5cam-ctl can switch between the two ways later.
    foreach ($dll in 'ps5cam-dmft.dll', 'ps5cam-vcam.dll') {
        $p = Start-Process regsvr32.exe -ArgumentList '/s', "`"$target\$dll`"" -Wait -PassThru
        if ($p.ExitCode -ne 0) { throw "regsvr32 $dll завершился с кодом $($p.ExitCode)" }
    }

    Step 'Настройки и журналы'
    $rc = Run $ctl @('setup')
    if ($rc -ne 0) { throw "не удалось настроить права на настройки и журналы ($rc)" }
    if (-not (Get-ItemProperty 'HKLM:\SOFTWARE\PS5Camera' -Name Mode -ErrorAction SilentlyContinue)) { Run $ctl @('defaults') | Out-Null }
    # Mode 0 = bokeh, 1 = plain camera (src/core/pipeline.h ViewMode).
    $rc = Run $ctl @('set', 'mode', $(if ($Bokeh -eq 'on') { '0' } else { '1' }))
    if ($rc -ne 0) { throw "не удалось записать режим камеры ($rc)" }
    # Admin-only subkey: the SYSTEM service restarts the camera according to these values.
    $rc = Run 'reg.exe' @('add', 'HKLM\SOFTWARE\PS5Camera\Service', '/v', 'HideRawCamera', '/t', 'REG_DWORD', '/d', [string][int](-not $KeepRawCamera), '/f')
    if ($rc -ne 0) { throw "не удалось записать настройку HideRawCamera ($rc)" }
    $rc = Run 'reg.exe' @('add', 'HKLM\SOFTWARE\PS5Camera\Service', '/v', 'UseDeviceMft', '/t', 'REG_DWORD', '/d', [string][int](-not $VirtualCamera), '/f')
    if ($rc -ne 0) { throw "не удалось записать настройку UseDeviceMft ($rc)" }

    # Before the service starts, so the two do not switch the camera at the same time. The service
    # repeats it whenever the camera appears (also on another USB port).
    if ($VirtualCamera) {
        Run $ctl @('dmft', 'off') | Out-Null  # a previous installation may have used the device MFT
    } else {
        Step 'Эффект внутри самой камеры (Device MFT), имя "PS5 Camera"'
        $plugged = [bool](Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -match 'VID_05A9&PID_058C&MI_00' })
        $rc = Run $ctl @('dmft', 'on')
        # Without a camera ever plugged in there is nothing to set yet.
        if ($rc -ne 0 -and $plugged) { throw "не удалось подключить эффект к камере ($rc)" }
    }

    Step 'Служба камеры (автозагрузка прошивки при подключении)'
    $rc = Run (Join-Path $target 'ps5cam-svc.exe') @('install')
    if ($rc -ne 0) { throw "не удалось установить службу ($rc)" }

    Step 'Ожидание камеры'
    $found = $false
    for ($i = 0; $i -lt 30; $i++) {
        if (Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -match 'VID_05A9&PID_058C&MI_00' }) { $found = $true; break }
        Start-Sleep -Milliseconds 500
    }
    Write-Host ($(if ($found) { '    камера подключена' } else { '    камера пока не подключена: всё включится само, когда вы её подключите' }))

    if ($VirtualCamera) {
        Step 'Виртуальная камера "PS5 Camera"'
        # Служба делает это сама при старте (и скрывает сырую камеру); здесь повторяем, чтобы сразу показать
        # ошибки. Пока служба пересоздаёт камеру, регистрация может быть временно отклонена, поэтому повторяем.
        Start-Sleep -Seconds 3
        $rc = 1
        for ($i = 0; $i -lt 6 -and $rc -ne 0; $i++) {
            if ($i -gt 0) { Start-Sleep -Seconds 2 }
            $rc = Run $ctl @('register')
        }
        if ($rc -ne 0) { throw "не удалось зарегистрировать виртуальную камеру ($rc)" }
    }

    $runKey = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Run'
    if ($Tray) {
        Step 'Значок в трее (для разработки)'
        Set-ItemProperty $runKey -Name PS5CameraTray -Value "`"$target\ps5cam-tray.exe`""
        Start-Process explorer.exe -ArgumentList "`"$target\ps5cam-tray.exe`""  # без прав администратора
    } else {
        # A previous installation may have put the icon into autostart.
        Remove-ItemProperty $runKey -Name PS5CameraTray -ErrorAction SilentlyContinue
    }

    Step 'Запись в "Установленные приложения"'
    $un = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\PS5Camera'
    New-Item $un -Force | Out-Null
    Set-ItemProperty $un -Name DisplayName -Value 'PS5 HD Camera'
    Set-ItemProperty $un -Name Publisher -Value 'PS5CameraDriver'
    Set-ItemProperty $un -Name DisplayVersion -Value '1.0.0'
    Set-ItemProperty $un -Name InstallLocation -Value $target
    Set-ItemProperty $un -Name DisplayIcon -Value "$target\ps5cam-tray.exe"
    Set-ItemProperty $un -Name UninstallString -Value "powershell.exe -NoProfile -ExecutionPolicy Bypass -File `"$target\uninstall.ps1`""
    New-ItemProperty $un -Name NoModify -Value 1 -PropertyType DWord -Force | Out-Null
    New-ItemProperty $un -Name NoRepair -Value 1 -PropertyType DWord -Force | Out-Null

    Write-Host ''
    Write-Host ("Готово: камера {0}. В любом приложении выберите камеру `"PS5 Camera`"." -f $(if ($Bokeh -eq 'on') { 'с боке' } else { 'без боке' })) -ForegroundColor Green
    if (-not $VirtualCamera) { Write-Host 'Приложения, открытые во время установки, увидят камеру после перезапуска.' }
    if ($Tray) {
        Write-Host 'Значок в трее: клик включает и выключает боке, правый клик открывает настройки.'
    } else {
        Write-Host 'Сменить режим: запустите установщик ещё раз или выполните в PowerShell'
        Write-Host "  & `"$ctl`" set mode 0    (с боке)"
        Write-Host "  & `"$ctl`" set mode 1    (без боке)"
    }
    Finish 0
} catch {
    Write-Host ''
    Write-Host "Ошибка: $($_.Exception.Message)" -ForegroundColor Red
    Finish 1
}
