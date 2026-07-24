# Reinstall native and Electron dependencies and rebuild the native addon.

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$nativeDir = Join-Path $repoRoot 'src\native'
$electronDir = Join-Path $repoRoot 'src\electron'
$nativeOutput = Join-Path $repoRoot 'build\native'

Write-Host '=== PersonalSafer build repair ===' -ForegroundColor Cyan

Write-Host "`n[1/4] Cleaning native module..." -ForegroundColor Yellow
Remove-Item -LiteralPath (Join-Path $nativeDir 'node_modules') -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath (Join-Path $nativeDir 'build') -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath $nativeOutput -Recurse -Force -ErrorAction SilentlyContinue

Write-Host '[2/4] Installing native dependencies...' -ForegroundColor Yellow
Push-Location $nativeDir
try {
    & npm.cmd install --no-audit
    if ($LASTEXITCODE -ne 0) { throw "npm install failed: $LASTEXITCODE" }

    Write-Host '[3/4] Building N-API module...' -ForegroundColor Yellow
    & npx.cmd node-gyp rebuild --verbose
    if ($LASTEXITCODE -ne 0) { throw "node-gyp rebuild failed: $LASTEXITCODE" }
} finally {
    Pop-Location
}

Write-Host '[4/4] Reinstalling Electron dependencies...' -ForegroundColor Yellow
Remove-Item -LiteralPath (Join-Path $electronDir 'node_modules') -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath (Join-Path $electronDir 'package-lock.json') -Force -ErrorAction SilentlyContinue
Push-Location $electronDir
try {
    & npm.cmd install --no-audit
    if ($LASTEXITCODE -ne 0) { throw "Electron npm install failed: $LASTEXITCODE" }
} finally {
    Pop-Location
}

Write-Host "`n=== Build repair completed ===" -ForegroundColor Green
