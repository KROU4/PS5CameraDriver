# PS5 HD Camera driver installer. Runs elevated (through Install.cmd); talks Russian on a Russian
# Windows and English otherwise.
#   .\install.ps1 [-Bokeh ask|on|off|keep] [-Tray] [-Original FILE] [-VirtualCamera [-KeepRawCamera]] [-Yes] [-NoPause]
#   -Bokeh          on: background always blurred; off: plain camera; ask (default): ask;
#                   keep: as set before (on for a first installation)
#   -Yes            do not ask to confirm the system changes (unattended install)
#   -Tray           tray icon for development (switches modes on the fly); none without it
#   -Original       Sony's original firmware (else sony-firmware.bin next to this script, or a download)
#   -VirtualCamera  the previous way: a separate virtual camera with the camera itself hidden (shown
#                   with -KeepRawCamera). By default the effect runs inside the camera (Device MFT).
#   -FromMsi        run by the MSI package (as SYSTEM, no console): its files are already in place,
#                   it owns the "Installed apps" entry; implies -Yes -NoPause, output goes to -Log.
#                   Its TRAY and SONYFIRMWARE properties arrive as -MsiTray (1 on, 0 off, empty: as
#                   before) and -MsiOriginal PATH.
# The depth camera "PS5 Camera Depth" is switched from the console only: ps5cam-ctl set depthcamera 1.
param(
    [ValidateSet('ask', 'on', 'off', 'keep')][string]$Bokeh = 'ask',
    [switch]$Tray,
    [string]$Original,
    [switch]$VirtualCamera,
    [switch]$KeepRawCamera,
    [switch]$Yes,
    [switch]$NoPause,
    [switch]$FromMsi,
    [string]$MsiTray,
    [string]$MsiOriginal,
    [string]$Log
)
$ErrorActionPreference = 'Stop'
$runKey = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Run'
if ($FromMsi) {
    $Yes = $true
    $NoPause = $true
    # Not given (an update, a repair): as the installation before.
    $Tray = [switch]$(if ($MsiTray) { $MsiTray -eq '1' } else { [bool](Get-ItemProperty $runKey -Name PS5CameraTray -ErrorAction SilentlyContinue) })
    if ($MsiOriginal) { $Original = $MsiOriginal }
}
# The transcript starts in this account's own temp folder and goes to -Log (in %ProgramData%\PS5Camera)
# only after "ps5cam-ctl setup" has secured that folder: a user could have prepared a link there.
$transcript = $null
$logFolderReady = $false
if ($Log) {
    $transcript = Join-Path $env:TEMP "ps5cam-install-$([guid]::NewGuid().ToString('N')).log"
    Start-Transcript -Path $transcript -Force | Out-Null
}
$src = $PSScriptRoot
# The MSI may install elsewhere (INSTALLFOLDER); its files are where this script is.
$target = if ($FromMsi) { $PSScriptRoot.TrimEnd('\') } else { Join-Path $env:ProgramFiles 'PS5Camera' }
$version = if (Test-Path (Join-Path $src 'version.txt')) { (Get-Content (Join-Path $src 'version.txt') -TotalCount 1).Trim() } else { '1.0.0' }
$files = 'ps5cam-vcam.dll', 'ps5cam-dmft.dll', 'ps5cam-svc.exe', 'ps5cam-ctl.exe', 'ps5cam-tray.exe', 'ps5cam-firmware.json',
    'firmware.ps1', 'uninstall.ps1'
# Subject of the per-computer certificate that signs the boot driver's catalog (uninstall.ps1 too).
$signerSubject = 'CN=PS5 Camera driver signer (this computer only)'
$russian = (Get-UICulture).TwoLetterISOLanguageName -eq 'ru'

function T([string]$ru, [string]$en) { if ($script:russian) { $ru } else { $en } }
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
    if ($transcript) {
        Stop-Transcript | Out-Null
        if ($script:logFolderReady -and (Copy-Item $transcript $Log -Force -PassThru -ErrorAction SilentlyContinue)) {
            Remove-Item $transcript -Force -ErrorAction SilentlyContinue
        } else { Write-Host "    log: $transcript" }
    }
    if (-not $NoPause) { Write-Host ''; Read-Host (T 'Нажмите Enter, чтобы закрыть окно' 'Press Enter to close this window') | Out-Null }
    exit $code
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
    $inf = Join-Path $dir 'ps5cam-boot.inf'
    $cat = Join-Path $dir 'ps5cam-boot.cat'
    if ($FromMsi) {
        Remove-Item $cat -Force -ErrorAction SilentlyContinue  # the MSI put the .inf there itself
    } else {
        Remove-Item $dir -Recurse -Force -ErrorAction SilentlyContinue
        New-Item -ItemType Directory -Force $dir | Out-Null
        Copy-Item (Join-Path $src 'driver\ps5cam-boot.inf') $inf
    }
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
    if ($rc -notin 0, 259, 3010) { throw ((T 'драйвер загрузчика не установлен' 'the boot loader driver was not installed') + " (pnputil $rc)") }
}

$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    # Re-launch elevated. Arguments go as an array, so any characters in the folder path are safe.
    $argList = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSCommandPath`"", '-Bokeh', $Bokeh)
    if ($Tray) { $argList += '-Tray' }
    if ($Original) { $argList += @('-Original', "`"$((Resolve-Path -LiteralPath $Original).ProviderPath)`"") }
    if ($VirtualCamera) { $argList += '-VirtualCamera' }
    if ($KeepRawCamera) { $argList += '-KeepRawCamera' }
    if ($Yes) { $argList += '-Yes' }
    if ($NoPause) { $argList += '-NoPause' }
    if ($Log) { $argList += @('-Log', "`"$Log`"") }
    if ($transcript) { Stop-Transcript | Out-Null }
    Start-Process powershell.exe -Verb RunAs -ArgumentList $argList
    exit
}

try {
    # Windows 10 is not refused (nothing this installation does needs Windows 11), only warned: the
    # driver has only been tested on Windows 11, and Windows 10 has neither the background effects
    # settings nor virtual cameras.
    if ([Environment]::OSVersion.Version.Build -lt 22000) {
        if ($VirtualCamera) {
            throw (T 'Виртуальной камере нужна Windows 11 (сборка 22000 и новее).' `
                'The virtual camera needs Windows 11 (build 22000 or later).')
        }
        Write-Host (T '    Внимание: драйвер проверялся только на Windows 11. На Windows 10 нет параметров эффектов фона: боке включается командой ps5cam-ctl set mode 0.' `
            '    Warning: the driver has only been tested on Windows 11. Windows 10 has no background effects settings: bokeh is turned on with ps5cam-ctl set mode 0.') -ForegroundColor Yellow
    }
    foreach ($f in $files + 'driver\ps5cam-boot.inf') {
        if (-not (Test-Path (Join-Path $src $f))) { throw ((T 'В папке установщика нет файла ' 'The installer folder lacks the file ') + $f) }
    }
    if ($Original -and -not (Test-Path -LiteralPath $Original)) { throw ((T 'нет файла ' 'no such file: ') + $Original) }
    if ($Original) { $Original = (Resolve-Path -LiteralPath $Original).ProviderPath }  # .NET reads relative to another folder
    # The service runs programs and firmware.ps1 from here as SYSTEM: Program Files only (the MSI's
    # launch condition compares the text; this resolves "..").
    $full = [IO.Path]::GetFullPath($target) + '\'
    $inside = @($env:ProgramW6432, $env:ProgramFiles) | Where-Object { $_ } |
        Where-Object { $full.StartsWith($_.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase) }
    if (-not $inside) { throw ((T 'Папка установки должна быть внутри ' 'The installation folder must be inside ') + $env:ProgramW6432) }

    if ($Bokeh -eq 'ask') {
        Write-Host (T 'PS5 HD Camera: как должна работать камера?' 'PS5 HD Camera: how should the camera work?') -ForegroundColor Cyan
        Write-Host (T '  1  С боке: фон всегда размыт по глубине (родные 1080p60)' '  1  With bokeh: the background is always blurred by depth (native 1080p60)')
        Write-Host (T '  2  Без боке: обычная камера (родные 1080p60)' '  2  Without bokeh: a plain camera (native 1080p60)')
        for ($tries = 0; $Bokeh -eq 'ask'; $tries++) {
            if ($tries -ge 5) { throw (T 'режим не выбран: запустите установщик с -Bokeh on или -Bokeh off' 'no mode chosen: run the installer with -Bokeh on or -Bokeh off') }
            $answer = ([string](Read-Host (T 'Введите 1 или 2' 'Enter 1 or 2'))).Trim()
            if ($answer -eq '1') { $Bokeh = 'on' } elseif ($answer -eq '2') { $Bokeh = 'off' }
        }
    }

    if (-not $Yes) {
        Write-Host ''
        Write-Host (T 'Установщик изменит систему:' 'The installer will change the system:') -ForegroundColor Cyan
        Write-Host (T "  - скопирует программы в $target и установит службу PS5CameraService (от имени системы)," `
            "  - copy the programs to $target and install the PS5CameraService service (running as SYSTEM),")
        Write-Host (T '    которая при каждом подключении загружает прошивку в камеру;' '    which uploads the firmware to the camera on every plug-in;')
        Write-Host (T '  - скачает оригинальную прошивку Sony (если её нет рядом) и соберёт из неё прошивку драйвера;' `
            "  - download Sony's original firmware (unless it is next to the installer) and build the driver's firmware from it;")
        Write-Host (T '  - установит драйвер WinUSB для камеры в режиме загрузчика: для его подписи создаст сертификат' `
            '  - install a WinUSB driver for the camera in boot mode: to sign it, create a certificate for this')
        Write-Host (T '    только этого компьютера (закрытый ключ сразу удаляется) и добавит его в доверенные;' `
            '    computer only (its private key is deleted at once) and add it to the trusted stores;')
        if ($VirtualCamera) {
            Write-Host (T '  - зарегистрирует виртуальную камеру «PS5 Camera» и скроет исходную USB-камеру;' `
                '  - register the "PS5 Camera" virtual camera and hide the USB camera itself;')
        } else {
            Write-Host (T '  - подключит видеоэффект к самой камере в службе Windows Camera Frame Server и назовёт её' `
                '  - attach the video effect to the camera itself in the Windows Camera Frame Server service and name it')
            Write-Host (T '    «PS5 Camera» (службу Frame Server перезапустит: открытые камеры на миг отключатся);' `
                '    "PS5 Camera" (Frame Server is restarted: open cameras drop out for a moment);')
        }
        Write-Host (T '  - сохранит настройки в HKLM\SOFTWARE\PS5Camera, журналы — в %ProgramData%\PS5Camera.' `
            '  - keep settings in HKLM\SOFTWARE\PS5Camera and logs in %ProgramData%\PS5Camera.')
        Write-Host (T 'Всё это отменяет Uninstall.cmd.' 'Uninstall.cmd undoes all of this.')
        $go = $null
        for ($tries = 0; $null -eq $go; $tries++) {
            if ($tries -ge 5) { throw (T 'установка не подтверждена' 'the installation was not confirmed') }
            $answer = ([string](Read-Host (T 'Продолжить? (Y — да, N — нет)' 'Continue? (Y — yes, N — no)'))).Trim().ToUpperInvariant()
            if ($answer -in 'Y', 'Д') { $go = $true } elseif ($answer -in 'N', 'Н') { $go = $false }
        }
        if (-not $go) {
            Write-Host (T 'Установка отменена, система не изменена.' 'Installation cancelled; the system was not changed.')
            Finish 0
        }
    }

    New-Item -ItemType Directory -Force $target | Out-Null

    # The camera firmware the service uploads (firmware.ps1): Sony's original + the driver's changes,
    # built before anything else changes. Without the original (no internet here, e.g.) the
    # installation goes on: the service builds the firmware itself once the camera is plugged in and
    # the computer is online.
    Step (T 'Прошивка камеры: оригинал Sony + изменения драйвера' "Camera firmware: Sony's original + the driver's changes")
    $fwArgs = @{ Patch = (Join-Path $src 'ps5cam-firmware.json'); Out = (Join-Path $target 'firmware.bin') }
    $localOriginal = if ($Original) { $Original } elseif (Test-Path -LiteralPath (Join-Path $src 'sony-firmware.bin')) { Join-Path $src 'sony-firmware.bin' }
    if ($localOriginal) { $fwArgs.Original = $localOriginal }
    & (Join-Path $src 'firmware.ps1') @fwArgs
    $firmwareCode = $LASTEXITCODE
    if ($firmwareCode -eq 2) {
        Write-Host (T '    Оригинальную прошивку Sony скачать не удалось (нет интернета?). Служба скачает её сама, когда камера будет подключена к компьютеру с интернетом.' `
            "    Could not download Sony's original firmware (no internet?). The service fetches it itself once the camera is plugged into a computer that is online.") -ForegroundColor Yellow
    } elseif ($firmwareCode -ne 0) {
        throw ((T 'не удалось собрать прошивку камеры' 'could not build the camera firmware') + " ($firmwareCode)")
    }

    Step (T 'Драйвер загрузчика камеры (WinUSB для 05A9:0580)' 'Camera boot loader driver (WinUSB for 05A9:0580)')
    Install-BootDriver

    Step (T 'Остановка работающих компонентов' 'Stopping running components')
    Get-Process ps5cam-tray -ErrorAction SilentlyContinue | Stop-Process -Force
    if (Get-Service PS5CameraService -ErrorAction SilentlyContinue) {
        Stop-Service PS5CameraService -Force -ErrorAction SilentlyContinue
    }
    # Frame Server keeps the video DLLs loaded; stop it to replace them.
    Stop-Service FrameServerMonitor, FrameServer -Force -ErrorAction SilentlyContinue

    if (-not $FromMsi) { Step ((T 'Копирование файлов в ' 'Copying files to ') + $target) }
    New-Item -ItemType Directory -Force $target | Out-Null
    Get-ChildItem $target -Filter '*.old-*' -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue
    foreach ($f in $(if ($FromMsi) { @() } else { $files })) {  # the MSI has put them there
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
    $ctl = Join-Path $target 'ps5cam-ctl.exe'

    Step (T 'Регистрация компонентов видео' 'Registering the video components')
    # Both, so that ps5cam-ctl can switch between the two ways later.
    foreach ($dll in 'ps5cam-dmft.dll', 'ps5cam-vcam.dll') {
        $p = Start-Process regsvr32.exe -ArgumentList '/s', "`"$target\$dll`"" -Wait -PassThru
        if ($p.ExitCode -ne 0) { throw ("regsvr32 $dll " + (T 'завершился с кодом ' 'exited with code ') + $p.ExitCode) }
    }

    Step (T 'Настройки и журналы' 'Settings and logs')
    $rc = Run $ctl @('setup')
    if ($rc -ne 0) { throw ((T 'не удалось настроить права на настройки и журналы' 'could not set up the settings and log permissions') + " ($rc)") }
    $logFolderReady = $true  # secured by setup: the log may go there now
    $hadSettings = [bool](Get-ItemProperty 'HKLM:\SOFTWARE\PS5Camera' -Name Mode -ErrorAction SilentlyContinue)
    if (-not $hadSettings) { Run $ctl @('defaults') | Out-Null }
    # Settings of 1.1.0 and earlier, where the installer wrote every default: the noise reduction
    # then defaulted to 70 (its weak level to 40). A value still at one of those moves once to the
    # new level (90, 50); SettingsVersion 2 marks the settings as migrated.
    $saved = Get-ItemProperty 'HKLM:\SOFTWARE\PS5Camera' -ErrorAction SilentlyContinue
    if ($hadSettings -and -not ($saved.PSObject.Properties['SettingsVersion'] -and $saved.SettingsVersion -ge 2)) {
        if ($saved.Denoise -eq 70) { Run $ctl @('set', 'denoise', '90') | Out-Null }
        elseif ($saved.Denoise -eq 40) { Run $ctl @('set', 'denoise', '50') | Out-Null }
    }
    & reg.exe add 'HKLM\SOFTWARE\PS5Camera' /v SettingsVersion /t REG_DWORD /d 2 /f 2>&1 | Out-Null
    if ($Bokeh -eq 'keep') {
        # An update keeps the user's choice; a first installation starts with bokeh.
        $Bokeh = if ($hadSettings -and (Get-ItemProperty 'HKLM:\SOFTWARE\PS5Camera').Mode -ne 0) { 'off' } else { 'on' }
    } else {
        # Mode 0 = bokeh, 1 = plain camera (src/core/pipeline.h ViewMode).
        $rc = Run $ctl @('set', 'mode', $(if ($Bokeh -eq 'on') { '0' } else { '1' }))
        if ($rc -ne 0) { throw ((T 'не удалось записать режим камеры' 'could not save the camera mode') + " ($rc)") }
    }
    # Admin-only subkey: the SYSTEM service restarts the camera according to these values.
    $rc = Run 'reg.exe' @('add', 'HKLM\SOFTWARE\PS5Camera\Service', '/v', 'HideRawCamera', '/t', 'REG_DWORD', '/d', [string][int](-not $KeepRawCamera), '/f')
    if ($rc -ne 0) { throw ((T 'не удалось записать настройку' 'could not save the setting') + " HideRawCamera ($rc)") }
    $rc = Run 'reg.exe' @('add', 'HKLM\SOFTWARE\PS5Camera\Service', '/v', 'UseDeviceMft', '/t', 'REG_DWORD', '/d', [string][int](-not $VirtualCamera), '/f')
    if ($rc -ne 0) { throw ((T 'не удалось записать настройку' 'could not save the setting') + " UseDeviceMft ($rc)") }

    # Before the service starts, so the two do not switch the camera at the same time. The service
    # repeats it whenever the camera appears (also on another USB port).
    if ($VirtualCamera) {
        Run $ctl @('dmft', 'off') | Out-Null  # a previous installation may have used the device MFT
    } else {
        Step (T 'Эффект внутри самой камеры (Device MFT), имя "PS5 Camera"' 'The effect inside the camera itself (Device MFT), named "PS5 Camera"')
        $plugged = [bool](Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -match 'VID_05A9&PID_058C&MI_00' })
        $rc = Run $ctl @('dmft', 'on')
        # Without a camera ever plugged in there is nothing to set yet.
        if ($rc -ne 0 -and $plugged) { throw ((T 'не удалось подключить эффект к камере' 'could not attach the effect to the camera') + " ($rc)") }
    }

    Step (T 'Служба камеры (автозагрузка прошивки при подключении)' 'Camera service (uploads the firmware on plug-in)')
    $rc = Run (Join-Path $target 'ps5cam-svc.exe') @('install')
    if ($rc -ne 0) { throw ((T 'не удалось установить службу' 'could not install the service') + " ($rc)") }

    Step (T 'Ожидание камеры' 'Waiting for the camera')
    $found = $false
    for ($i = 0; $i -lt 30; $i++) {
        if (Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -match 'VID_05A9&PID_058C&MI_00' }) { $found = $true; break }
        Start-Sleep -Milliseconds 500
    }
    Write-Host $(if ($found) { T '    камера подключена' '    the camera is connected' } else {
            T '    камера пока не подключена: всё включится само, когда вы её подключите' '    the camera is not connected yet: everything turns on by itself when you plug it in' })

    if ($VirtualCamera) {
        Step (T 'Виртуальная камера "PS5 Camera"' 'The "PS5 Camera" virtual camera')
        # The service does this itself when it starts (and hides the raw camera); repeated here to show
        # errors at once. While the service recreates the camera, registration may be refused for a
        # moment, hence the retries.
        Start-Sleep -Seconds 3
        $rc = 1
        for ($i = 0; $i -lt 6 -and $rc -ne 0; $i++) {
            if ($i -gt 0) { Start-Sleep -Seconds 2 }
            $rc = Run $ctl @('register')
        }
        if ($rc -ne 0) { throw ((T 'не удалось зарегистрировать виртуальную камеру' 'could not register the virtual camera') + " ($rc)") }
    }

    $runKey = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Run'
    if ($Tray) {
        Step (T 'Значок в трее (для разработки)' 'Tray icon (for development)')
        Set-ItemProperty $runKey -Name PS5CameraTray -Value "`"$target\ps5cam-tray.exe`""
        # Not elevated; from the MSI (SYSTEM, no desktop) it starts at the next sign-in instead.
        if (-not $FromMsi) { Start-Process explorer.exe -ArgumentList "`"$target\ps5cam-tray.exe`"" }
    } else {
        # A previous installation may have put the icon into autostart.
        Remove-ItemProperty $runKey -Name PS5CameraTray -ErrorAction SilentlyContinue
    }

    $un = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\PS5Camera'
    if ($FromMsi) {
        # The MSI has its own entry; one left by an installation from the ZIP package goes.
        Run 'reg.exe' @('delete', 'HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\PS5Camera', '/f') | Out-Null
    } else {
        Step (T 'Запись в "Установленные приложения"' 'Entry in "Installed apps"')
        New-Item $un -Force | Out-Null
        Set-ItemProperty $un -Name DisplayName -Value 'PS5 HD Camera'
        Set-ItemProperty $un -Name Publisher -Value 'PS5CameraDriver'
        Set-ItemProperty $un -Name DisplayVersion -Value $version
        Set-ItemProperty $un -Name InstallLocation -Value $target
        Set-ItemProperty $un -Name DisplayIcon -Value "$target\ps5cam-tray.exe"
        Set-ItemProperty $un -Name UninstallString -Value "powershell.exe -NoProfile -ExecutionPolicy Bypass -File `"$target\uninstall.ps1`""
        New-ItemProperty $un -Name NoModify -Value 1 -PropertyType DWord -Force | Out-Null
        New-ItemProperty $un -Name NoRepair -Value 1 -PropertyType DWord -Force | Out-Null
    }

    Write-Host ''
    if ($Bokeh -eq 'on') {
        Write-Host (T 'Готово: камера с боке. В любом приложении выберите камеру "PS5 Camera".' `
            'Done: the camera with bokeh. Choose the "PS5 Camera" camera in any program.') -ForegroundColor Green
    } else {
        Write-Host (T 'Готово: камера без боке. В любом приложении выберите камеру "PS5 Camera".' `
            'Done: the camera without bokeh. Choose the "PS5 Camera" camera in any program.') -ForegroundColor Green
    }
    if (-not $VirtualCamera) {
        Write-Host (T 'Приложения, открытые во время установки, увидят камеру после перезапуска.' `
            'Programs that were open during the installation see the camera after a restart.')
    }
    if ($firmwareCode -eq 2) {
        $retry = if ($FromMsi) {
            T 'или установите пакет снова с SONYFIRMWARE=путь к оригиналу' 'or install the package again with SONYFIRMWARE=path of the original'
        } else {
            T 'или положите оригинал рядом с установщиком как sony-firmware.bin и запустите его снова' `
                'or put the original next to the installer as sony-firmware.bin and run it again'
        }
        $kept = Get-Item -LiteralPath (Join-Path $target 'firmware.bin') -ErrorAction SilentlyContinue
        # (the sizes the service accepts, LoadFirmwareFile in src\common\firmware.cpp)
        $offline = if ($kept -and $kept.Length -ge 4096 -and $kept.Length -le 0x40000) {
            T "Камера пока работает с прошивкой прежней установки: для новой запустите установку снова, когда появится интернет ($retry)." `
                "The camera keeps the previous installation's firmware for now: for the new one run the installation again once the computer is online ($retry)."
        } else {
            T "Прошивки камеры пока нет: служба скачает её сама, когда появится интернет ($retry)." `
                "The camera has no firmware yet: the service downloads it once the computer is online ($retry)."
        }
        Write-Host $offline -ForegroundColor Yellow
    }
    if ($Tray) {
        Write-Host (T 'Значок в трее: клик включает и выключает боке, правый клик открывает настройки.' `
            'Tray icon: a click turns bokeh on and off, a right click opens the settings.')
    } elseif (-not $VirtualCamera -and [Environment]::OSVersion.Version.Build -ge 22000) {  # Windows 10: no such page
        Write-Host (T 'Боке включается и выключается в Параметры → Bluetooth и устройства → Камеры → PS5 Camera →' `
            'Bokeh is switched on and off in Settings → Bluetooth & devices → Cameras → PS5 Camera →')
        Write-Host (T '  Эффекты камеры (размытие фона: портретное или стандартное), а также в приложениях с эффектами камеры.' `
            '  Camera effects (background blur: portrait or standard), and in apps that offer camera effects.')
    } else {
        Write-Host (T 'Сменить режим: запустите установщик ещё раз или выполните в PowerShell' `
            'To change the mode, run the installer again or run in PowerShell')
        Write-Host ("  & `"$ctl`" set mode 0    " + (T '(с боке)' '(with bokeh)'))
        Write-Host ("  & `"$ctl`" set mode 1    " + (T '(без боке)' '(without bokeh)'))
    }
    Finish 0
} catch {
    Write-Host ''
    Write-Host ((T 'Ошибка: ' 'Error: ') + $_.Exception.Message) -ForegroundColor Red
    Finish 1
}
