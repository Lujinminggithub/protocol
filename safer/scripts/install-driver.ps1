# install-driver.ps1
# 安装 PersonalSafer 内核驱动

param(
    [string]$SysPath,
    [string]$InfPath
)

Write-Host "=== Installing PersonalSafer Driver ===" -ForegroundColor Cyan

# 需要管理员权限
$isAdmin = ([Security.Principal.WindowsPrincipal] [Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    Write-Host "ERROR: This script must be run as Administrator." -ForegroundColor Red
    exit 1
}

# 默认路径
if (-not $SysPath) {
    $SysPath = Join-Path $PSScriptRoot "..\build\kernel\x64\Release\PersonalSafer.sys"
}
if (-not $InfPath) {
    $InfPath = Join-Path $PSScriptRoot "..\src\kernel\inf\PersonalSafer.inf"
}

# 复制驱动文件到系统目录
$driverDest = "$env:SystemRoot\System32\Drivers\PersonalSafer.sys"
Write-Host "Copying driver to $driverDest ..." -ForegroundColor Yellow
Copy-Item $SysPath $driverDest -Force

# 安装驱动 INF
if ($InfPath -and (Test-Path $InfPath)) {
    Write-Host "Installing driver INF..." -ForegroundColor Yellow
    $result = pnputil /add-driver $InfPath /install

    if ($result -match "Successfully") {
        Write-Host "Driver INF installed successfully." -ForegroundColor Green
    } else {
        Write-Host "INF install output: $result" -ForegroundColor Yellow
    }
}

# 启动驱动服务
Write-Host "Starting driver service..." -ForegroundColor Yellow
sc.exe start PersonalSafer

if ($?) {
    Write-Host "=== Driver installed and started successfully ===" -ForegroundColor Green
} else {
    Write-Host "WARNING: Driver service may not have started. Check with 'sc.exe query PersonalSafer'" -ForegroundColor Yellow
}
