# Builds the camera firmware: Sony's original image (never shipped with the driver) plus the
# driver's byte changes from ps5cam-firmware.json, the original and the result both checked by hash.
# The installer runs it, and so does the service (as SYSTEM) when firmware.bin is missing or broken
# because the installation had no internet access.
#   firmware.ps1 -Patch ps5cam-firmware.json -Out firmware.bin [-Original FILE]
# Exit code: 0 built (or already built), 2 the original could not be downloaded, 1 another failure
# (an -Original that is not Sony's original among them: the user asked for that file).
param(
    [Parameter(Mandatory = $true)][string]$Patch,
    [Parameter(Mandatory = $true)][string]$Out,
    [string]$Original
)
$ErrorActionPreference = 'Stop'
$russian = (Get-UICulture).TwoLetterISOLanguageName -eq 'ru'
function T([string]$ru, [string]$en) { if ($script:russian) { $ru } else { $en } }
function Sha256([byte[]]$bytes) {
    $h = [Security.Cryptography.SHA256]::Create()
    try { return -join ($h.ComputeHash($bytes) | ForEach-Object { $_.ToString('x2') }) } finally { $h.Dispose() }
}
# At most $limit bytes, 30 s without an answer is a failure (a source that silently drops packets
# must not use up the service's wait).
function Download([string]$url, [long]$limit) {
    $request = [Net.WebRequest]::Create($url)
    $request.Timeout = 30000
    $request.ReadWriteTimeout = 30000
    $response = $request.GetResponse()
    try {
        $stream = $response.GetResponseStream()
        $buffer = New-Object IO.MemoryStream
        $chunk = New-Object byte[] 65536
        while (($n = $stream.Read($chunk, 0, $chunk.Length)) -gt 0) {
            $buffer.Write($chunk, 0, $n)
            if ($buffer.Length -gt $limit) { throw (T 'файл больше ожидаемого' 'larger than expected') }
        }
        return , $buffer.ToArray()
    } finally { $response.Close() }
}

try {
    # .NET resolves relative paths against the process's folder, not PowerShell's.
    if ($Original -and (Test-Path -LiteralPath $Original)) { $Original = (Resolve-Path -LiteralPath $Original).ProviderPath }
    $Out = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Out)
    $p = Get-Content -LiteralPath $Patch -Raw | ConvertFrom-Json
    # An update with the same firmware changes needs no download: the built image is already there.
    if (-not $Original -and (Test-Path -LiteralPath $Out) -and
        (Sha256 ([IO.File]::ReadAllBytes($Out))) -eq $p.result.sha256) {
        Write-Host ("    " + (T 'прошивка уже собрана: ' 'firmware already built: ') + $Out)
        exit 0
    }
    $candidates = if ($Original) { @($Original) } else { @($p.sources) }
    [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
    foreach ($where in $candidates) {
        try {
            $bytes = if ($Original) { [IO.File]::ReadAllBytes($where) } else { Download $where ([long]$p.original.size) }
        } catch {
            Write-Host ("    " + (T "не удалось получить ${where}: " "could not get ${where}: ") + $_.Exception.Message)
            continue
        }
        if ($bytes.Length -ne $p.original.size -or (Sha256 $bytes) -ne $p.original.sha256) {
            Write-Host ("    ${where}: " + (T 'это не оригинальная прошивка Sony 21.01-03.20.00.04' 'this is not the original Sony firmware 21.01-03.20.00.04'))
            continue
        }
        Write-Host ("    " + (T 'оригинал: ' 'original: ') + $where)
        foreach ($run in $p.runs) {
            $offset = [int]$run[0]
            $hex = [string]$run[1]
            for ($i = 0; $i -lt $hex.Length / 2; $i++) { $bytes[$offset + $i] = [Convert]::ToByte($hex.Substring(2 * $i, 2), 16) }
        }
        if ((Sha256 $bytes) -ne $p.result.sha256) {
            Write-Host ("    " + (T 'собранная прошивка не совпала с ожидаемой' 'the built firmware does not match the expected one'))
            exit 1
        }
        # Whole or not at all: the service may read it at any moment.
        $tmp = "$Out.tmp"
        [IO.File]::WriteAllBytes($tmp, $bytes)
        Move-Item -LiteralPath $tmp -Destination $Out -Force
        exit 0
    }
    if ($Original) { exit 1 }
    Write-Host ("    " + (T 'не удалось скачать оригинальную прошивку Sony' "could not download Sony's original firmware"))
    exit 2
} catch {
    Write-Host ("    " + $_.Exception.Message)
    exit 1
}
