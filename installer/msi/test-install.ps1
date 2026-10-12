# End-to-end MSI test for a disposable, elevated Windows machine (GitHub Actions or Windows
# Sandbox). Installs the English package, changes a user setting, upgrades with the Russian package
# of the same version, and removes it again. Logs survive the removal under -Out.
param(
    [Parameter(Mandatory = $true)][string]$Dist,
    [Parameter(Mandatory = $true)][string]$Out
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'
$serviceName = 'PS5CameraService'
$displayName = 'PS5 HD Camera'
$signerSubject = 'CN=PS5 Camera driver signer (this computer only)'
$settingsKey = 'HKLM:\SOFTWARE\PS5Camera'
$installFolder = Join-Path $env:ProgramFiles 'PS5Camera'
$programDataFolder = Join-Path $env:ProgramData 'PS5Camera'
$msiTimedOut = $false

function Assert-True([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}

function Get-MsiProperty([string]$Path, [string]$Name) {
    $installer = New-Object -ComObject WindowsInstaller.Installer
    $database = $null
    $view = $null
    $record = $null
    try {
        $database = $installer.GetType().InvokeMember('OpenDatabase', 'InvokeMethod', $null, $installer, @($Path, 0))
        $query = "SELECT ``Value`` FROM ``Property`` WHERE ``Property``='$Name'"
        $view = $database.GetType().InvokeMember('OpenView', 'InvokeMethod', $null, $database, @($query))
        $view.GetType().InvokeMember('Execute', 'InvokeMethod', $null, $view, $null) | Out-Null
        $record = $view.GetType().InvokeMember('Fetch', 'InvokeMethod', $null, $view, $null)
        if (-not $record) { throw "MSI property $Name is absent in $Path" }
        return [string]$record.GetType().InvokeMember('StringData', 'GetProperty', $null, $record, 1)
    } finally {
        foreach ($object in $record, $view, $database, $installer) {
            if ($null -ne $object -and [Runtime.InteropServices.Marshal]::IsComObject($object)) {
                [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($object)
            }
        }
    }
}

function Get-ProductRegistration([string]$ProductCode) {
    $path = "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\$ProductCode"
    if (Test-Path -LiteralPath $path) { return Get-ItemProperty -LiteralPath $path }
    return $null
}

function Get-PS5CameraProducts {
    $roots = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall',
        'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall'
    foreach ($root in $roots) {
        Get-ChildItem -LiteralPath $root -ErrorAction SilentlyContinue | ForEach-Object {
            $item = Get-ItemProperty -LiteralPath $_.PSPath -ErrorAction SilentlyContinue
            if ($item) {
                $nameProperty = $item.PSObject.Properties['DisplayName']
                $publisherProperty = $item.PSObject.Properties['Publisher']
                if (($nameProperty -and $nameProperty.Value -eq $displayName) -or
                    ($publisherProperty -and $publisherProperty.Value -eq 'PS5CameraDriver')) { $item }
            }
        }
    }
}

function Get-BootDrivers {
    @(Get-WindowsDriver -Online | Where-Object { $_.OriginalFileName -like '*\ps5cam-boot.inf' })
}

function Get-SignerCertificates([string]$Store) {
    @(Get-ChildItem "Cert:\LocalMachine\$Store" | Where-Object { $_.Subject -eq $signerSubject })
}

function Invoke-Msi([ValidateSet('install', 'uninstall')][string]$Action, [string]$Target, [string]$Log) {
    $switch = if ($Action -eq 'install') { '/i' } else { '/x' }
    $arguments = @($switch, "`"$Target`"", '/qn', '/norestart', '/L*vx', "`"$Log`"")
    if ($Action -eq 'install') { $arguments += 'BOKEH=keep' }
    Write-Host "msiexec $switch $Target"
    $process = Start-Process msiexec.exe -ArgumentList $arguments -PassThru
    try {
        if (-not $process.WaitForExit(300000)) {
            $script:msiTimedOut = $true
            throw "msiexec $Action did not finish within 300 seconds; see $Log"
        }
        $process.WaitForExit()
        Assert-True ($process.ExitCode -in 0, 3010) "msiexec $Action failed with exit code $($process.ExitCode); see $Log"
        return $process.ExitCode
    } finally {
        $process.Dispose()
    }
}

function Copy-InstallLog([string]$Phase) {
    $source = Join-Path $programDataFolder 'install.log'
    Assert-True (Test-Path -LiteralPath $source) "install.ps1 did not leave $source after $Phase"
    Copy-Item -LiteralPath $source -Destination (Join-Path $Out "install-$Phase.log") -Force
}

function Copy-EarlyInstallLogs {
    $index = 0
    foreach ($folder in @((Join-Path $env:windir 'Temp'), $env:TEMP) | Select-Object -Unique) {
        foreach ($log in Get-ChildItem -LiteralPath $folder -Filter 'ps5cam-install-*.log' -File -ErrorAction SilentlyContinue) {
            $index++
            Copy-Item -LiteralPath $log.FullName -Destination (Join-Path $Out "install-early-$index.log") -Force -ErrorAction SilentlyContinue
        }
    }
}

function Assert-Installed([hashtable]$Msi, [int]$Blur) {
    $registration = Get-ProductRegistration $Msi.ProductCode
    Assert-True ($null -ne $registration) "MSI product $($Msi.ProductCode) is not registered"
    Assert-True ([string]$registration.DisplayName -eq $displayName) "unexpected ARP name: $($registration.DisplayName)"
    Assert-True ([string]$registration.DisplayVersion -eq $Msi.ProductVersion) "ARP version is $($registration.DisplayVersion), expected $($Msi.ProductVersion)"
    Assert-True ([int]$registration.Language -eq [int]$Msi.ProductLanguage) "ARP language is $($registration.Language), expected $($Msi.ProductLanguage)"

    $versionFile = Join-Path $installFolder 'version.txt'
    Assert-True (Test-Path -LiteralPath $versionFile) "$versionFile is absent"
    Assert-True ((Get-Content -LiteralPath $versionFile -TotalCount 1).Trim() -eq $Msi.ProductVersion) 'installed version.txt does not match the MSI'

    $service = Get-Service -Name $serviceName -ErrorAction SilentlyContinue
    Assert-True ($null -ne $service) "$serviceName is not installed"
    try {
        $service.Refresh()
        Assert-True ($service.Status -eq [ServiceProcess.ServiceControllerStatus]::Running) "$serviceName is $($service.Status), expected Running"
        Assert-True ($service.StartType -eq [ServiceProcess.ServiceStartMode]::Automatic) "$serviceName start type is $($service.StartType), expected Automatic"
    } finally {
        $service.Dispose()
    }

    $settings = Get-ItemProperty -LiteralPath $settingsKey
    $serviceSettings = Get-ItemProperty -LiteralPath (Join-Path $settingsKey 'Service')
    Assert-True ([int]$settings.Mode -eq 0) "Mode is $($settings.Mode), expected fresh/default bokeh mode 0"
    Assert-True ([int]$settings.Blur -eq $Blur) "Blur is $($settings.Blur), expected $Blur"
    Assert-True ([int]$settings.SettingsVersion -eq 3) "SettingsVersion is $($settings.SettingsVersion), expected 3"
    Assert-True ([int]$serviceSettings.UseDeviceMft -eq 1) 'UseDeviceMft is not 1'
    Assert-True ([int]$serviceSettings.HideRawCamera -eq 1) 'HideRawCamera is not 1'

    Assert-True (@(Get-BootDrivers).Count -gt 0) 'the ps5cam-boot.inf WinUSB package is absent from the driver store'
    $root = @(Get-SignerCertificates 'Root')
    $publisher = @(Get-SignerCertificates 'TrustedPublisher')
    $personal = @(Get-SignerCertificates 'My')
    Assert-True ($root.Count -eq 1) "expected one signer certificate in LocalMachine Root, found $($root.Count)"
    Assert-True ($publisher.Count -eq 1) "expected one signer certificate in LocalMachine TrustedPublisher, found $($publisher.Count)"
    Assert-True ($root[0].Thumbprint -eq $publisher[0].Thumbprint) 'Root and TrustedPublisher contain different signer certificates'
    Assert-True (-not $root[0].HasPrivateKey -and -not $publisher[0].HasPrivateKey) 'a trusted signer certificate still exposes its private key'
    Assert-True ($personal.Count -eq 0) 'the signer certificate/private key remains in LocalMachine My'
}

function Assert-Removed([string[]]$ProductCodes) {
    for ($i = 0; $i -lt 20 -and (Get-Service -Name $serviceName -ErrorAction SilentlyContinue); $i++) {
        Start-Sleep -Milliseconds 250
    }
    Assert-True (-not (Get-Service -Name $serviceName -ErrorAction SilentlyContinue)) "$serviceName remains after uninstall"
    foreach ($code in $ProductCodes | Select-Object -Unique) {
        Assert-True ($null -eq (Get-ProductRegistration $code)) "MSI product $code remains registered after uninstall"
    }
    Assert-True (@(Get-PS5CameraProducts).Count -eq 0) 'a PS5 Camera ARP entry remains after uninstall'
    Assert-True (-not (Test-Path -LiteralPath $settingsKey)) 'HKLM\SOFTWARE\PS5Camera remains after uninstall'
    Assert-True (-not (Test-Path -LiteralPath $installFolder)) "$installFolder remains after uninstall"
    Assert-True (@(Get-BootDrivers).Count -eq 0) 'the ps5cam-boot.inf package remains in the driver store after uninstall'
    foreach ($store in 'Root', 'TrustedPublisher', 'My') {
        Assert-True (@(Get-SignerCertificates $store).Count -eq 0) "the signer certificate remains in LocalMachine $store"
    }
}

if ([Environment]::OSVersion.Platform -ne [PlatformID]::Win32NT) { throw 'this test needs Windows' }
$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'run this test from an elevated PowerShell session on a disposable Windows machine'
}

$Dist = (Resolve-Path -LiteralPath $Dist).ProviderPath
if (-not [IO.Path]::IsPathRooted($Out)) { $Out = Join-Path (Get-Location).ProviderPath $Out }
New-Item -ItemType Directory -Path $Out -Force | Out-Null
$Out = (Resolve-Path -LiteralPath $Out).ProviderPath
$englishPath = (Resolve-Path -LiteralPath (Join-Path $Dist 'PS5CameraDriver.msi')).ProviderPath
$russianPath = (Resolve-Path -LiteralPath (Join-Path $Dist 'PS5CameraDriver-ru.msi')).ProviderPath
$english = @{
    Path = $englishPath
    ProductCode = Get-MsiProperty $englishPath 'ProductCode'
    ProductVersion = Get-MsiProperty $englishPath 'ProductVersion'
    ProductLanguage = Get-MsiProperty $englishPath 'ProductLanguage'
    UpgradeCode = Get-MsiProperty $englishPath 'UpgradeCode'
}
$russian = @{
    Path = $russianPath
    ProductCode = Get-MsiProperty $russianPath 'ProductCode'
    ProductVersion = Get-MsiProperty $russianPath 'ProductVersion'
    ProductLanguage = Get-MsiProperty $russianPath 'ProductLanguage'
    UpgradeCode = Get-MsiProperty $russianPath 'UpgradeCode'
}
Assert-True ($english.ProductVersion -eq $russian.ProductVersion) 'the English and Russian MSIs have different versions'
Assert-True ($english.UpgradeCode -eq $russian.UpgradeCode) 'the English and Russian MSIs have different UpgradeCodes'
Assert-True ([int]$english.ProductLanguage -eq 1033) "English MSI language is $($english.ProductLanguage), expected 1033"
Assert-True ([int]$russian.ProductLanguage -eq 1049) "Russian MSI language is $($russian.ProductLanguage), expected 1049"

# This test removes everything it installs. Refuse to run where that could destroy a real or partly
# installed copy, including its settings, driver package, or per-machine trust certificate.
$existingService = Get-Service -Name $serviceName -ErrorAction SilentlyContinue
$existingProducts = @(Get-PS5CameraProducts)
$existingFootprint = (Test-Path -LiteralPath $settingsKey) -or (Test-Path -LiteralPath $installFolder) -or
    @(Get-BootDrivers).Count -gt 0 -or @(Get-SignerCertificates 'Root').Count -gt 0 -or
    @(Get-SignerCertificates 'TrustedPublisher').Count -gt 0 -or @(Get-SignerCertificates 'My').Count -gt 0
if ($existingService -or $existingProducts.Count -gt 0 -or $existingFootprint) {
    throw 'refusing to run: this machine already has a PS5 Camera service, product, settings, files, driver, or certificate; use a fresh disposable Windows runner/Sandbox'
}

$failure = $null
$cleanupFailure = $null
$installed = $false
try {
    Invoke-Msi install $english.Path (Join-Path $Out 'msi-install-en.log') | Out-Null
    $installed = $true
    Copy-InstallLog 'en'
    Assert-Installed $english 25

    $ctl = Join-Path $installFolder 'ps5cam-ctl.exe'
    & $ctl set blur 75
    Assert-True ($LASTEXITCODE -eq 0) "ps5cam-ctl set blur 75 failed with exit code $LASTEXITCODE"
    Assert-True ([int](Get-ItemProperty -LiteralPath $settingsKey).Blur -eq 75) 'ps5cam-ctl did not save blur 75'

    Invoke-Msi install $russian.Path (Join-Path $Out 'msi-upgrade-ru.log') | Out-Null
    Copy-InstallLog 'ru-upgrade'
    Assert-Installed $russian 75
    if ($english.ProductCode -ne $russian.ProductCode) {
        Assert-True ($null -eq (Get-ProductRegistration $english.ProductCode)) 'the English ProductCode remains after the Russian major upgrade'
    }

    Invoke-Msi uninstall $russian.ProductCode (Join-Path $Out 'msi-uninstall.log') | Out-Null
    $installed = $false
    Assert-Removed @($english.ProductCode, $russian.ProductCode)
} catch {
    $failure = $_
    Copy-EarlyInstallLogs
} finally {
    # Keep the custom-action transcript before uninstall.ps1 removes ProgramData. This also captures
    # the failed phase when an assertion or the Russian upgrade stops the main flow.
    $liveLog = Join-Path $programDataFolder 'install.log'
    if (Test-Path -LiteralPath $liveLog) {
        Copy-Item -LiteralPath $liveLog -Destination (Join-Path $Out 'install-before-cleanup.log') -Force -ErrorAction SilentlyContinue
    }
    if (-not $msiTimedOut -and ($installed -or @(Get-PS5CameraProducts).Count -gt 0)) {
        try {
            $registered = @($russian.ProductCode, $english.ProductCode) | Where-Object { Get-ProductRegistration $_ } | Select-Object -First 1
            if ($registered) {
                Invoke-Msi uninstall $registered (Join-Path $Out 'msi-cleanup.log') | Out-Null
                Assert-Removed @($english.ProductCode, $russian.ProductCode)
            }
        } catch {
            $cleanupFailure = $_
        }
    }
}

if ($failure -and $cleanupFailure) { throw "$($failure.Exception.Message); cleanup also failed: $($cleanupFailure.Exception.Message)" }
if ($failure) { throw $failure }
if ($cleanupFailure) { throw $cleanupFailure }
Write-Host "MSI integration test passed for version $($english.ProductVersion); logs: $Out"
