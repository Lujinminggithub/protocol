# build-native.ps1
# 编译 N-API 原生模块

param(
    [string]$Target = "electron"
)

Write-Host "=== Building PersonalSafer N-API Module ===" -ForegroundColor Cyan

. (Join-Path $PSScriptRoot 'build-tools.ps1')

$nativeDir = Join-Path $PSScriptRoot "..\src\native"

# 检查 Node.js
$nodePath = & where.exe node
if (-not $nodePath) {
    Write-Host "ERROR: Node.js not found." -ForegroundColor Red
    exit 1
}

# 检查 Python (node-gyp 需要)
$pythonPath = & where.exe python
if (-not $pythonPath) {
    Write-Host "WARNING: Python not found. Install Python 3.x for node-gyp." -ForegroundColor Yellow
}

# 安装依赖
Write-Host "Installing native dependencies..." -ForegroundColor Yellow
Push-Location $nativeDir
npm.cmd install

# 构建
Write-Host "Building native addon..." -ForegroundColor Yellow
npx.cmd node-gyp rebuild --directory="$nativeDir"

if ($LASTEXITCODE -ne 0) {
    Write-Host "Incremental native build failed; retrying without file tracking..." -ForegroundColor Yellow
    $msbuildPath = Resolve-MSBuildPath
    $solutionPath = Join-Path $nativeDir "build\binding.sln"

    if ($msbuildPath -and (Test-Path $solutionPath)) {
        & $msbuildPath $solutionPath `
            /t:Rebuild `
            /p:Configuration=Release `
            /p:Platform=x64 `
            /p:TrackFileAccess=false `
            /v:minimal
    }

    if (-not $msbuildPath -or $LASTEXITCODE -ne 0) {
        Pop-Location
        Write-Host "ERROR: Native addon build failed!" -ForegroundColor Red
        exit 1
    }
}
Pop-Location

# 复制到 Electron 目录
$buildOutput = Join-Path $PSScriptRoot "..\build\native"
if (Test-Path $buildOutput) {
    Remove-Item $buildOutput -Recurse -Force
}
New-Item -ItemType Directory -Path $buildOutput -Force | Out-Null

Copy-Item (Join-Path $nativeDir "build\Release\personal_safer.node") $buildOutput -Force

Write-Host "=== N-API module built successfully ===" -ForegroundColor Green
Write-Host "Output: $buildOutput" -ForegroundColor Yellow
