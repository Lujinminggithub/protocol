# sign-driver.ps1
# 对内核驱动进行代码签名

param(
    [string]$CertificatePath,
    [string]$CertificatePassword,
    [string]$TimestampUrl = "http://timestamp.digicert.com"
)

Write-Host "=== Signing PersonalSafer Kernel Driver ===" -ForegroundColor Cyan

. (Join-Path $PSScriptRoot 'build-tools.ps1')
$signtool = Resolve-WindowsSdkTool -Name 'signtool.exe'

$sysPath = Join-Path $PSScriptRoot "..\build\kernel\x64\Release\PersonalSafer.sys"
if (-not (Test-Path $sysPath)) {
    Write-Host "ERROR: Driver not found at $sysPath" -ForegroundColor Red
    exit 1
}

$signArgs = @(
    "sign",
    "/fd", "sha256",
    "/td", "sha256",
    "/tr", $TimestampUrl
)

if ($CertificatePath) {
    $signArgs += "/f"
    $signArgs += $CertificatePath
    if ($CertificatePassword) {
        $signArgs += "/p"
        $signArgs += $CertificatePassword
    }
} else {
    Write-Host "WARNING: No certificate specified. Skipping signing." -ForegroundColor Yellow
    exit 0
}

& $signtool $signArgs $sysPath

if ($LASTEXITCODE -ne 0) {
    Write-Host "ERROR: Driver signing failed!" -ForegroundColor Red
    exit 1
}

Write-Host "=== Driver signed successfully ===" -ForegroundColor Green
