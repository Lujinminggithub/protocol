# Create or reuse a test code-signing certificate and sign the kernel driver.

$ErrorActionPreference = 'Stop'

$subject = 'CN=PersonalSafer Test'
$sysPath = Join-Path $PSScriptRoot 'build\kernel\x64\Release\PersonalSafer.sys'
$cerOut = Join-Path $PSScriptRoot 'PersonalSafer-Test.cer'

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$isAdmin = ([Security.Principal.WindowsPrincipal]$identity).IsInRole(
  [Security.Principal.WindowsBuiltInRole]::Administrator)
$storeScope = if ($isAdmin) { 'LocalMachine' } else { 'CurrentUser' }
$storePath = "Cert:\$storeScope\My"

$signtool = Get-ChildItem @(
  'D:\Windows Kits\10\bin\*\x64\signtool.exe',
  'C:\Program Files (x86)\Windows Kits\10\bin\*\x64\signtool.exe'
) -ErrorAction SilentlyContinue |
  Sort-Object FullName -Descending |
  Select-Object -First 1 -ExpandProperty FullName

if (-not $signtool) { throw 'signtool.exe not found. Install the Windows SDK/WDK.' }
if (-not (Test-Path $sysPath)) { throw "Driver not found: $sysPath" }

Write-Host "[1/4] Certificate store: $storeScope\My"
$cert = Get-ChildItem $storePath |
  Where-Object { $_.Subject -eq $subject -and $_.HasPrivateKey } |
  Select-Object -First 1

if (-not $cert) {
  Write-Host '[2/4] Creating self-signed test certificate...'
  $cert = New-SelfSignedCertificate `
    -Subject $subject `
    -Type CodeSigningCert `
    -KeyUsage DigitalSignature `
    -KeyAlgorithm RSA `
    -KeyLength 2048 `
    -HashAlgorithm SHA256 `
    -CertStoreLocation $storePath `
    -NotAfter (Get-Date).AddYears(5)
} else {
  Write-Host "[2/4] Reusing certificate: $($cert.Thumbprint)"
}

Write-Host '[3/4] Exporting and trusting public certificate...'
Export-Certificate -Cert $cert -FilePath $cerOut -Force | Out-Null
foreach ($store in @('Root', 'TrustedPublisher')) {
  Import-Certificate `
    -FilePath $cerOut `
    -CertStoreLocation "Cert:\$storeScope\$store" | Out-Null
}

Write-Host '[4/4] Signing and verifying driver...'
$signArgs = @('sign', '/v')
if ($isAdmin) { $signArgs += '/sm' }
$signArgs += @('/s', 'My', '/sha1', $cert.Thumbprint, '/fd', 'sha256', '/ph', $sysPath)
& $signtool @signArgs
if ($LASTEXITCODE -ne 0) { throw "signtool sign failed: $LASTEXITCODE" }

& $signtool verify /v /pa $sysPath
if ($LASTEXITCODE -ne 0) { throw "signtool verify failed: $LASTEXITCODE" }

Write-Host "Signed: $sysPath"
