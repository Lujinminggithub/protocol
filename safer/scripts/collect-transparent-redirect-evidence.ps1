param(
    [string]$OutDir = "",
    [switch]$Rebuild
)

$ErrorActionPreference = "Stop"

function Test-IsAdmin {
    $currentIdentity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($currentIdentity)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

if (-not (Test-IsAdmin)) {
    throw "This script must be run from an elevated PowerShell window."
}

$repoRoot = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($OutDir)) {
    $timestamp = Get-Date -Format "yyyyMMdd-HHmmss"
    $OutDir = Join-Path $repoRoot "artifacts\transparent-redirect-$timestamp"
}

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

$bundlePath = Join-Path $repoRoot "src\electron\main\modules\local_proxy.validate.cjs"
$electronDir = Join-Path $repoRoot "src\electron"
$nativeDir = Join-Path $repoRoot "src\native"
$electronCmd = Join-Path $electronDir "node_modules\.bin\electron.cmd"
$nodeCmd = "node"

Write-Host "[Evidence] output directory: $OutDir"

if ($Rebuild) {
    Write-Host "[Evidence] rebuilding kernel/native/electron"
    powershell -ExecutionPolicy Bypass -File (Join-Path $repoRoot "scripts\build-kernel.ps1") *>&1 |
        Tee-Object -FilePath (Join-Path $OutDir "build-kernel.log")
    Push-Location $nativeDir
    npm run build *>&1 | Tee-Object -FilePath (Join-Path $OutDir "build-native.log")
    Pop-Location
    Push-Location $electronDir
    npm exec -- esbuild .\main\modules\local_proxy.ts --bundle --platform=node --format=cjs --external:electron --outfile=.\main\modules\local_proxy.validate.cjs *>&1 |
        Tee-Object -FilePath (Join-Path $OutDir "bundle-local-proxy.log")
    npx vite build *>&1 | Tee-Object -FilePath (Join-Path $OutDir "build-electron.log")
    Pop-Location
} else {
    Write-Host "[Evidence] rebuild skipped; using deployed binaries"
}

Write-Host "[Evidence] collecting system state"
cmd /c fltmc filters > (Join-Path $OutDir "fltmc-filters.txt")
cmd /c fltmc instances > (Join-Path $OutDir "fltmc-instances.txt")
sc.exe query PersonalSafer > (Join-Path $OutDir "sc-query.txt")
sc.exe qc PersonalSafer > (Join-Path $OutDir "sc-qc.txt")
driverquery /v /fo list > (Join-Path $OutDir "driverquery.txt")
netstat -ano > (Join-Path $OutDir "netstat-ano.txt")
Get-NetTCPConnection | Sort-Object LocalPort | Format-Table -AutoSize |
    Out-File (Join-Path $OutDir "Get-NetTCPConnection.txt") -Encoding utf8

Write-Host "[Evidence] running transparent redirect validation"
Push-Location $electronDir
& $electronCmd ..\..\scripts\validate-transparent-proxy.js *>&1 |
    Tee-Object -FilePath (Join-Path $OutDir "validate-transparent-proxy.log")
& $electronCmd ..\..\scripts\validate-websocket-proxy.js *>&1 |
    Tee-Object -FilePath (Join-Path $OutDir "validate-websocket-proxy.log")
Pop-Location

Write-Host "[Evidence] collecting post-run state"
$driverStatusCmd = @"
const addon=require('./src/native/build/Release/personal_safer.node');
console.log('isLoaded=', addon.dlp.driver_loader.isLoaded());
console.log('connect=', addon.dlp.kernel_comm.connect());
try { console.log(JSON.stringify(addon.dlp.kernel_comm.getDriverStatus())); } catch (e) { console.log('statusErr', e.message); }
"@
Push-Location $repoRoot
& $nodeCmd -e $driverStatusCmd *>&1 | Tee-Object -FilePath (Join-Path $OutDir "driver-status-after.txt")
Pop-Location

Write-Host "[Evidence] done: $OutDir"
