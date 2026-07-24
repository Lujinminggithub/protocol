# Install the PersonalSafer minifilter driver. Run from elevated PowerShell.

param(
    [string]$SysPath = '',
    [switch]$SkipStart
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
if (-not (Test-Path -LiteralPath $SysPath)) {
    throw "Driver not found: $SysPath"
}

$resolvedSysPath = (Resolve-Path -LiteralPath $SysPath).Path
$driverDest = Join-Path $env:SystemRoot 'System32\drivers\PersonalSafer.sys'
Write-Host "Copying driver to $driverDest ..." -ForegroundColor Yellow
Copy-Item -LiteralPath $resolvedSysPath -Destination $driverDest -Force

$serviceKey = "HKLM:\SYSTEM\CurrentControlSet\Services\$serviceName"
$parametersKey = Join-Path $serviceKey 'Parameters'
$parametersInstancesKey = Join-Path $parametersKey 'Instances'
$legacyInstancesKey = Join-Path $serviceKey 'Instances'
$instanceName = 'PersonalSafer Instance'
$altitude = '379950'

New-Item -Path $serviceKey -Force | Out-Null
New-ItemProperty -Path $serviceKey -Name DisplayName -Value 'PersonalSafer Security Minifilter' -PropertyType String -Force | Out-Null
New-ItemProperty -Path $serviceKey -Name Description -Value 'PersonalSafer file and network security minifilter' -PropertyType String -Force | Out-Null
New-ItemProperty -Path $serviceKey -Name Type -Value 2 -PropertyType DWord -Force | Out-Null
New-ItemProperty -Path $serviceKey -Name Start -Value 3 -PropertyType DWord -Force | Out-Null
New-ItemProperty -Path $serviceKey -Name ErrorControl -Value 1 -PropertyType DWord -Force | Out-Null
New-ItemProperty -Path $serviceKey -Name ImagePath -Value '\SystemRoot\System32\drivers\PersonalSafer.sys' -PropertyType ExpandString -Force | Out-Null
New-ItemProperty -Path $serviceKey -Name Group -Value 'FSFilter Activity Monitor' -PropertyType String -Force | Out-Null
New-ItemProperty -Path $serviceKey -Name DependOnService -Value @('FltMgr') -PropertyType MultiString -Force | Out-Null

New-Item -Path $parametersKey -Force | Out-Null
New-ItemProperty -Path $parametersKey -Name SupportedFeatures -Value 3 -PropertyType DWord -Force | Out-Null
foreach ($instancesKey in @($parametersInstancesKey, $legacyInstancesKey)) {
    $instanceKey = Join-Path $instancesKey $instanceName
    New-Item -Path $instanceKey -Force | Out-Null
    New-ItemProperty -Path $instancesKey -Name DefaultInstance -Value $instanceName -PropertyType String -Force | Out-Null
    New-ItemProperty -Path $instanceKey -Name Altitude -Value $altitude -PropertyType String -Force | Out-Null
    New-ItemProperty -Path $instanceKey -Name Flags -Value 0 -PropertyType DWord -Force | Out-Null
}

if (-not $SkipStart) {
    Write-Host "Starting driver service: $serviceName" -ForegroundColor Yellow
    & fltmc.exe load $serviceName
    if ($LASTEXITCODE -ne 0) { throw "fltmc load failed: $LASTEXITCODE" }
}

Write-Host '=== Driver files and service registry installed successfully ===' -ForegroundColor Green
