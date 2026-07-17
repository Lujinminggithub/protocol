param(
    [string]$InstallDir = "",
    [string]$OutDir = ""
)

$ErrorActionPreference = "Stop"

function Test-IsAdmin {
    $currentIdentity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($currentIdentity)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Find-InstallDir {
    if (-not [string]::IsNullOrWhiteSpace($InstallDir) -and (Test-Path $InstallDir)) {
        return (Resolve-Path $InstallDir).Path
    }

    $candidates = @(
        "$env:ProgramFiles\PersonalSafer",
        "${env:ProgramFiles(x86)}\PersonalSafer",
        "$env:LOCALAPPDATA\Programs\PersonalSafer"
    )

    foreach ($candidate in $candidates) {
        if (Test-Path $candidate) {
            return (Resolve-Path $candidate).Path
        }
    }

    $uninstallRoots = @(
        'HKLM:\Software\Microsoft\Windows\CurrentVersion\Uninstall\*',
        'HKLM:\Software\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\*',
        'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\*'
    )
    foreach ($root in $uninstallRoots) {
        try {
            $item = Get-ItemProperty $root -ErrorAction SilentlyContinue |
                Where-Object { $_.DisplayName -eq 'PersonalSafer' -and $_.InstallLocation } |
                Select-Object -First 1
            if ($item -and (Test-Path $item.InstallLocation)) {
                return (Resolve-Path $item.InstallLocation).Path
            }
        } catch {}
    }

    throw "Cannot locate installed PersonalSafer directory. Please pass -InstallDir."
}

function Copy-IfExists {
    param(
        [string]$Path,
        [string]$Destination
    )

    if (Test-Path $Path) {
        Copy-Item -Recurse -Force $Path $Destination
    }
}

if (-not (Test-IsAdmin)) {
    throw "This script must be run from an elevated PowerShell window."
}

$resolvedInstallDir = Find-InstallDir
if ([string]::IsNullOrWhiteSpace($OutDir)) {
    $timestamp = Get-Date -Format "yyyyMMdd-HHmmss"
    $OutDir = Join-Path $env:TEMP "PersonalSafer-installed-evidence-$timestamp"
}

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

$exePath = Join-Path $resolvedInstallDir 'PersonalSafer.exe'
$resourceDriver = Join-Path $resolvedInstallDir 'resources\driver'
$resourceNative = Join-Path $resolvedInstallDir 'resources\native'
$installLogDir = Join-Path $resolvedInstallDir 'log'
$userDataDirs = @(
    "$env:APPDATA\PersonalSafer",
    "$env:APPDATA\Electron"
)

Set-Content -Path (Join-Path $OutDir 'install-dir.txt') -Value $resolvedInstallDir -Encoding utf8

if (Test-Path $exePath) {
    Get-Item $exePath | Select-Object FullName,Length,LastWriteTime,VersionInfo |
        Format-List | Out-File (Join-Path $OutDir 'exe-info.txt') -Encoding utf8
}

Copy-IfExists $resourceDriver (Join-Path $OutDir 'driver')
Copy-IfExists $resourceNative (Join-Path $OutDir 'native')
Copy-IfExists $installLogDir (Join-Path $OutDir 'install-log')

foreach ($dir in $userDataDirs) {
    if (Test-Path $dir) {
        $name = Split-Path $dir -Leaf
        Copy-IfExists (Join-Path $dir 'log') (Join-Path $OutDir "$name-log")
        Copy-IfExists (Join-Path $dir 'mitm') (Join-Path $OutDir "$name-mitm")
    }
}

cmd /c fltmc filters > (Join-Path $OutDir 'fltmc-filters.txt')
cmd /c fltmc instances > (Join-Path $OutDir 'fltmc-instances.txt')
sc.exe query PersonalSafer > (Join-Path $OutDir 'sc-query.txt')
sc.exe qc PersonalSafer > (Join-Path $OutDir 'sc-qc.txt')
driverquery /v /fo list > (Join-Path $OutDir 'driverquery.txt')
netstat -ano > (Join-Path $OutDir 'netstat-ano.txt')
Get-NetTCPConnection | Sort-Object LocalPort | Format-Table -AutoSize |
    Out-File (Join-Path $OutDir 'Get-NetTCPConnection.txt') -Encoding utf8

Get-ChildItem -Recurse $resolvedInstallDir |
    Select-Object FullName,Length,LastWriteTime |
    Format-Table -AutoSize |
    Out-File (Join-Path $OutDir 'install-tree.txt') -Encoding utf8

Write-Host "[InstalledEvidence] done: $OutDir"
