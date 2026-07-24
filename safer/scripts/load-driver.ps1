# Load the PersonalSafer minifilter for development and testing.
# Run from elevated PowerShell. Test signing must already be configured.

param([string]$SysPath = '')

$ErrorActionPreference = 'Stop'
$serviceName = 'PersonalSafer'
$altitude = '379950'
$instanceName = 'PersonalSafer Instance'

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$isAdmin = ([Security.Principal.WindowsPrincipal]$identity).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    throw 'This script must be run as Administrator.'
}

if (-not $SysPath) {
    $candidates = @(
        (Join-Path $PSScriptRoot '..\build\kernel\x64\Release\PersonalSafer.sys'),
        (Join-Path $PSScriptRoot '..\src\electron\driver\PersonalSafer.sys')
    )
    $SysPath = $candidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
}
if (-not $SysPath -or -not (Test-Path -LiteralPath $SysPath)) {
    throw 'PersonalSafer.sys was not found. Run build_kernel.cmd or pass -SysPath.'
}

$resolvedSysPath = (Resolve-Path -LiteralPath $SysPath).Path
$driverDest = Join-Path $env:SystemRoot 'System32\drivers\PersonalSafer.sys'
Write-Host "Driver source: $resolvedSysPath" -ForegroundColor Cyan

Write-Host 'Removing any previous test service...' -ForegroundColor Yellow
& fltmc.exe unload $serviceName 2>$null | Out-Null
& sc.exe stop $serviceName 2>$null | Out-Null
& sc.exe delete $serviceName 2>$null | Out-Null
Start-Sleep -Milliseconds 500

Copy-Item -LiteralPath $resolvedSysPath -Destination $driverDest -Force
& sc.exe create $serviceName type= filesys start= demand error= normal binPath= $driverDest DisplayName= 'PersonalSafer Security Driver' | Out-Null
if ($LASTEXITCODE -ne 0) { throw "sc.exe create failed: $LASTEXITCODE" }

$serviceKey = "HKLM:\SYSTEM\CurrentControlSet\Services\$serviceName"
$instancesKey = Join-Path $serviceKey 'Instances'
$instanceKey = Join-Path $instancesKey $instanceName
New-Item -Path $instanceKey -Force | Out-Null
New-ItemProperty -Path $instancesKey -Name DefaultInstance -Value $instanceName -PropertyType String -Force | Out-Null
New-ItemProperty -Path $instanceKey -Name Altitude -Value $altitude -PropertyType String -Force | Out-Null
New-ItemProperty -Path $instanceKey -Name Flags -Value 0 -PropertyType DWord -Force | Out-Null

Write-Host "Loading minifilter at altitude $altitude ..." -ForegroundColor Yellow
& fltmc.exe load $serviceName
if ($LASTEXITCODE -ne 0) { throw "fltmc load failed: $LASTEXITCODE" }

& fltmc.exe filters | Select-String -Pattern "$serviceName|Filter Name"
& sc.exe query $serviceName
Write-Host '=== Driver loaded successfully ===' -ForegroundColor Green
