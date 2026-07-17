# 一键修复脚本
# 在 E:\project\safer 目录下以管理员身份运行 PowerShell

Write-Host "=== PersonalSafer 构建修复 ===" -ForegroundColor Cyan

# 1. 清理 native 模块
Write-Host "`n[1/4] 清理原生模块..." -ForegroundColor Yellow
Set-Location "src\native"
if (Test-Path node_modules) { Remove-Item -Recurse -Force node_modules }
if (Test-Path build) { Remove-Item -Recurse -Force build }
if (Test-Path ..\..\build\native) { Remove-Item -Recurse -Force ..\..\build\native }

# 2. 安装依赖（不运行 audit fix！）
Write-Host "[2/4] 安装依赖..." -ForegroundColor Yellow
npm install --no-audit

# 3. 编译原生模块
Write-Host "[3/4] 编译 N-API 模块..." -ForegroundColor Yellow
npx node-gyp rebuild --verbose

if ($LASTEXITCODE -ne 0) {
    Write-Host "`n[FAIL] N-API 模块编译失败！" -ForegroundColor Red
    Write-Host "请将 error.txt 内容贴给开发者。" -ForegroundColor Yellow
    exit 1
}

Write-Host "[OK] N-API 模块编译成功！" -ForegroundColor Green

# 4. 清理 electron 模块
Write-Host "`n[4/4] 清理 Electron 模块..." -ForegroundColor Yellow
Set-Location "..\electron"
if (Test-Path node_modules) { Remove-Item -Recurse -Force node_modules }
if (Test-Path package-lock.json) { Remove-Item package-lock.json }

npm install --no-audit

Write-Host "`n=== 修复完成！运行 'npm run dev' 启动应用 ===" -ForegroundColor Green
