# build-electron.ps1
# 构建 Electron 应用

param(
    [switch]$Publish
)

Write-Host "=== Building PersonalSafer Electron App ===" -ForegroundColor Cyan

$electronDir = Join-Path $PSScriptRoot "..\src\electron"

# 检查 Node.js
$nodePath = & where.exe node
if (-not $nodePath) {
    Write-Host "ERROR: Node.js not found." -ForegroundColor Red
    exit 1
}

# 安装依赖
Write-Host "Installing Electron dependencies..." -ForegroundColor Yellow
Set-Location $electronDir
npm install

# 构建前端
Write-Host "Building frontend (Vue 3 + Vite)..." -ForegroundColor Yellow
npx vite build

# 构建原生模块
Write-Host "Building N-API native module..." -ForegroundColor Yellow
& "$PSScriptRoot\build-native.ps1"

# 打包 Electron
Write-Host "Packaging Electron app..." -ForegroundColor Yellow
npx electron-builder --win

if ($LASTEXITCODE -ne 0) {
    Write-Host "ERROR: Electron build failed!" -ForegroundColor Red
    exit 1
}

Write-Host "=== Electron app built successfully ===" -ForegroundColor Green
