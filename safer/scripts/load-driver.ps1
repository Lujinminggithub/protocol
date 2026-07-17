# load-driver.ps1
# 加载 PersonalSafer MiniFilter 驱动（开发/测试用）
#
# 用法（必须管理员）:
#   powershell -ExecutionPolicy Bypass -File E:\project\safer\scripts\load-driver.ps1
#
# 前提:
#   1) 已禁用驱动强制签名 (bcdedit /set testsigning on + 重启, 或启动时选“禁用驱动程序强制签名”)
#   2) 已编译出 PersonalSafer.sys (build_kernel.cmd)
#
# 卸载:  fltmc unload PersonalSafer;  sc.exe delete PersonalSafer

param(
    [string]$SysPath
)

$ErrorActionPreference = "Stop"
$svc = "PersonalSafer"
$altitude = "379950"   # 必须与 driver.h 的 PS_ALTITUDE 一致
$instanceName = "PersonalSafer Instance"

# --- 管理员检查 ---
$isAdmin = ([Security.Principal.WindowsPrincipal] [Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    Write-Host "ERROR: 必须以管理员身份运行本脚本。" -ForegroundColor Red
    exit 1
}

# --- 定位 .sys ---
if (-not $SysPath) {
    $candidates = @(
        (Join-Path $PSScriptRoot "..\src\kernel\x64\Release\PersonalSafer.sys"),
        (Join-Path $PSScriptRoot "..\src\electron\driver\PersonalSafer.sys")
    )
    foreach ($c in $candidates) { if (Test-Path $c) { $SysPath = $c; break } }
}
if (-not $SysPath -or -not (Test-Path $SysPath)) {
    Write-Host "ERROR: 找不到 PersonalSafer.sys，请先运行 build_kernel.cmd，或用 -SysPath 指定。" -ForegroundColor Red
    exit 1
}
$SysPath = (Resolve-Path $SysPath).Path
Write-Host "驱动文件: $SysPath" -ForegroundColor Cyan

# --- 清理可能存在的旧实例 ---
Write-Host "清理旧的服务/加载(若有)..." -ForegroundColor Yellow
& fltmc.exe unload $svc 2>$null | Out-Null
& sc.exe stop $svc 2>$null | Out-Null
& sc.exe delete $svc 2>$null | Out-Null
Start-Sleep -Milliseconds 500

# --- 复制到 System32\drivers ---
$dest = "$env:SystemRoot\System32\drivers\PersonalSafer.sys"
Write-Host "复制到 $dest ..." -ForegroundColor Yellow
Copy-Item $SysPath $dest -Force

# --- 创建 MiniFilter 服务 (类型必须为 filesys) ---
Write-Host "创建服务 (type=filesys)..." -ForegroundColor Yellow
# 注意 sc.exe 语法: “key=” 与值之间要有空格
& sc.exe create $svc type= filesys start= demand error= normal binPath= $dest DisplayName= "PersonalSafer Security Driver" | Out-Null

# --- 写入 MiniFilter 必需的 Instances / Altitude 注册表 ---
Write-Host "配置 MiniFilter 实例 (Altitude=$altitude)..." -ForegroundColor Yellow
$svcKey  = "HKLM:\SYSTEM\CurrentControlSet\Services\$svc"
$instKey = "$svcKey\Instances"
New-Item -Path $instKey -Force | Out-Null
New-ItemProperty -Path $instKey -Name "DefaultInstance" -Value $instanceName -PropertyType String -Force | Out-Null
$oneInst = "$instKey\$instanceName"
New-Item -Path $oneInst -Force | Out-Null
New-ItemProperty -Path $oneInst -Name "Altitude" -Value $altitude -PropertyType String -Force | Out-Null
New-ItemProperty -Path $oneInst -Name "Flags"    -Value 0        -PropertyType DWord  -Force | Out-Null

# --- 加载 ---
Write-Host "加载 MiniFilter (fltmc load $svc)..." -ForegroundColor Yellow
$out = & fltmc.exe load $svc 2>&1
Write-Host $out

# --- 验证 ---
Start-Sleep -Milliseconds 500
Write-Host "`n当前已加载的过滤器:" -ForegroundColor Cyan
& fltmc.exe filters | Select-String -Pattern "PersonalSafer|Filter Name"

$state = (& sc.exe query $svc | Select-String "STATE")
Write-Host "服务状态: $state" -ForegroundColor Cyan

if ($out -match "success|成功" -or $state -match "RUNNING") {
    Write-Host "`n=== 驱动加载成功 ===" -ForegroundColor Green
} else {
    Write-Host "`n加载可能失败，检查上面的错误码。常见: 0x801f0011=实例配置问题, 未禁用签名会报 0x800705B4/0xC000037A" -ForegroundColor Yellow
}
