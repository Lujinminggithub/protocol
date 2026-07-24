# Install the PersonalSafer minifilter driver. Run from elevated PowerShell.

param(
    [string]$SysPath = '',
    [string]$InfPath = ''
)

$ErrorActionPreference = 'Stop'
$serviceName = 'PersonalSafer'

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$isAdmin = ([Security.Principal.WindowsPrincipal]$identity).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    throw 'This script must be run as Administrator.'
}

if (-not $SysPath) {
    $SysPath = Join-Path $PSScriptRoot '..\build\kernel\x64\Release\PersonalSafer.sys'
}
if (-not $InfPath) {
    $InfPath = Join-Path $PSScriptRoot '..\src\kernel\inf\PersonalSafer.inf'
}
if (-not (Test-Path -LiteralPath $SysPath)) {
    throw "Driver not found: $SysPath"
}

$resolvedSysPath = (Resolve-Path -LiteralPath $SysPath).Path
$driverDest = Join-Path $env:SystemRoot 'System32\drivers\PersonalSafer.sys'
Write-Host "Copying driver to $driverDest ..." -ForegroundColor Yellow
Copy-Item -LiteralPath $resolvedSysPath -Destination $driverDest -Force

if ($InfPath -and (Test-Path -LiteralPath $InfPath)) {
    $resolvedInfPath = (Resolve-Path -LiteralPath $InfPath).Path
    Write-Host "Installing driver INF: $resolvedInfPath" -ForegroundColor Yellow
    & pnputil.exe /add-driver $resolvedInfPath /install
    if ($LASTEXITCODE -ne 0) { throw "pnputil failed: $LASTEXITCODE" }
}

Write-Host "Starting driver service: $serviceName" -ForegroundColor Yellow
& sc.exe start $serviceName
if ($LASTEXITCODE -ne 0) { throw "sc.exe start failed: $LASTEXITCODE" }

Write-Host '=== Driver installed and started successfully ===' -ForegroundColor Green
