param(
    [string]$ServiceName = "PersonalSafer",
    [string]$DriverPath = "$env:SystemRoot\System32\drivers\PersonalSafer.sys",
    [switch]$IncludeMemoryDump
)

$ErrorActionPreference = "Continue"
$timestamp = Get-Date -Format "yyyyMMdd-HHmmss"
$desktop = [Environment]::GetFolderPath("Desktop")
$workDir = Join-Path $env:TEMP "PersonalSafer-BSOD-$timestamp"
$zipPath = Join-Path $desktop "PersonalSafer-BSOD-$timestamp.zip"

New-Item -ItemType Directory -Path $workDir -Force | Out-Null

& sc.exe config $ServiceName start= disabled 2>&1 |
    Out-File (Join-Path $workDir "service-disable.txt") -Encoding utf8
& sc.exe queryex $ServiceName 2>&1 |
    Out-File (Join-Path $workDir "service-query.txt") -Encoding utf8
& fltmc.exe filters 2>&1 |
    Out-File (Join-Path $workDir "fltmc-filters.txt") -Encoding utf8

Get-CimInstance Win32_OperatingSystem |
    Select-Object Caption, Version, BuildNumber, OSArchitecture, LastBootUpTime |
    Format-List |
    Out-File (Join-Path $workDir "os.txt") -Encoding utf8

if (Test-Path -LiteralPath $DriverPath) {
    Get-Item -LiteralPath $DriverPath |
        Format-List FullName, Length, CreationTimeUtc, LastWriteTimeUtc |
        Out-File (Join-Path $workDir "driver-file.txt") -Encoding utf8
    Get-FileHash -LiteralPath $DriverPath -Algorithm SHA256 |
        Format-List * |
        Out-File (Join-Path $workDir "driver-sha256.txt") -Encoding utf8
    Get-AuthenticodeSignature -LiteralPath $DriverPath |
        Format-List Status, StatusMessage, Path, SignerCertificate, TimeStamperCertificate |
        Out-File (Join-Path $workDir "driver-signature.txt") -Encoding utf8
}

reg.exe query "HKLM\SYSTEM\CurrentControlSet\Services\$ServiceName" /s 2>&1 |
    Out-File (Join-Path $workDir "service-registry.txt") -Encoding utf8

Get-WinEvent -FilterHashtable @{ LogName = "System"; Id = 41, 1001, 6008, 7000, 7001 } -MaxEvents 30 |
    Select-Object TimeCreated, Id, ProviderName, LevelDisplayName, Message |
    Format-List |
    Out-File (Join-Path $workDir "system-events.txt") -Encoding utf8

wevtutil.exe qe "Microsoft-Windows-CodeIntegrity/Operational" /c:30 /rd:true /f:text 2>&1 |
    Out-File (Join-Path $workDir "code-integrity-events.txt") -Encoding utf8

$miniDump = Get-ChildItem "$env:SystemRoot\Minidump\*.dmp" -ErrorAction SilentlyContinue |
    Where-Object { $_.Length -gt 0 } |
    Sort-Object LastWriteTimeUtc -Descending |
    Select-Object -First 1
if ($miniDump) {
    try {
        Copy-Item -LiteralPath $miniDump.FullName -Destination $workDir -Force -ErrorAction Stop
    } catch {
        "Minidump copy failed: $($_.Exception.Message)" |
            Out-File (Join-Path $workDir "dump-status.txt") -Encoding utf8
    }
} else {
    "No non-empty minidump was found." |
        Out-File (Join-Path $workDir "dump-status.txt") -Encoding utf8
}

$memoryDump = Get-Item "$env:SystemRoot\MEMORY.DMP" -ErrorAction SilentlyContinue
if ($memoryDump) {
    $memoryDump |
        Format-List FullName, Length, CreationTimeUtc, LastWriteTimeUtc |
        Out-File (Join-Path $workDir "memory-dump-location.txt") -Encoding utf8
    if ($IncludeMemoryDump) {
        try {
            Copy-Item -LiteralPath $memoryDump.FullName -Destination $workDir -Force -ErrorAction Stop
        } catch {
            "MEMORY.DMP copy failed: $($_.Exception.Message)" |
                Add-Content (Join-Path $workDir "dump-status.txt") -Encoding utf8
        }
    }
}

Compress-Archive -Path (Join-Path $workDir "*") -DestinationPath $zipPath -Force
Remove-Item -LiteralPath $workDir -Recurse -Force
Write-Host "Evidence package: $zipPath"
