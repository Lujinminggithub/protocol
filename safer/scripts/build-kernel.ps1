# build-kernel.ps1
# Build the KMDF kernel driver

param(
    [string]$Configuration = "Release",
    [string]$Platform = "x64",
    [string]$WindowsSdkDirOverride = ""
)

Write-Host "=== Building PersonalSafer Kernel Driver ===" -ForegroundColor Cyan

$candidateSdkDirs = @()
if ($WindowsSdkDirOverride) { $candidateSdkDirs += $WindowsSdkDirOverride }
if ($env:WindowsSdkDir) { $candidateSdkDirs += $env:WindowsSdkDir }
$candidateSdkDirs += "D:\Windows Kits\10"
$candidateSdkDirs += "C:\Program Files (x86)\Windows Kits\10"

$wdkPath = $candidateSdkDirs |
    Where-Object { $_ -and (Test-Path $_) } |
    Select-Object -First 1

if (-not $wdkPath) {
    Write-Host "ERROR: WDK not found. Please install Windows Driver Kit." -ForegroundColor Red
    exit 1
}

$resolvedWdkPath = (Resolve-Path $wdkPath).Path
$env:WindowsSdkDir = if ($resolvedWdkPath.EndsWith('\')) { $resolvedWdkPath } else { "$resolvedWdkPath\" }

Write-Host "WDK Path: $env:WindowsSdkDir" -ForegroundColor Green

$msbuildCandidates = @()
$msbuildFromWhere = (& where.exe msbuild 2>$null | Select-Object -First 1)
if ($msbuildFromWhere) { $msbuildCandidates += $msbuildFromWhere }
$msbuildCandidates += @(
    "D:\Program Files\Microsoft Visual Studio\2022\Professional\MSBuild\Current\Bin\MSBuild.exe",
    "D:\Program Files\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe",
    "C:\Program Files\Microsoft Visual Studio\2022\Professional\MSBuild\Current\Bin\MSBuild.exe",
    "C:\Program Files\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe"
)

$msbuildPath = $msbuildCandidates |
    Where-Object { $_ -and (Test-Path $_) } |
    Select-Object -First 1

if (-not $msbuildPath) {
    Write-Host "ERROR: MSBuild not found. Please install Visual Studio Build Tools." -ForegroundColor Red
    exit 1
}

Write-Host "MSBuild: $msbuildPath" -ForegroundColor Green

$projectPath = Join-Path $PSScriptRoot "..\src\kernel\PersonalSafer.vcxproj"
$outputPath = Join-Path $PSScriptRoot "..\build\kernel\$Platform\$Configuration"
$outputPathWithSlash = if ($outputPath.EndsWith('\')) { $outputPath } else { "$outputPath\" }
$driverKitTasks18 = Join-Path $env:WindowsSdkDir "build\10.0.28000.0\bin\Microsoft.DriverKit.Build.Tasks.18.0.dll"
$vsVersion = if (Test-Path $driverKitTasks18) { "18.0" } else { "17.0" }

& $msbuildPath $projectPath `
    /p:Configuration=$Configuration `
    /p:Platform=$Platform `
    /p:OutDir="$outputPathWithSlash" `
    /p:VisualStudioVersion=$vsVersion `
    /v:minimal

if ($LASTEXITCODE -ne 0) {
    Write-Host "Incremental build failed; retrying a full build without file tracking..." -ForegroundColor Yellow
    & $msbuildPath $projectPath `
        /t:Rebuild `
        /p:Configuration=$Configuration `
        /p:Platform=$Platform `
        /p:OutDir="$outputPathWithSlash" `
        /p:VisualStudioVersion=$vsVersion `
        /p:TrackFileAccess=false `
        /v:minimal

    if ($LASTEXITCODE -ne 0) {
        Write-Host "ERROR: Kernel driver build failed!" -ForegroundColor Red
        exit 1
    }
}

$pdbPath = Join-Path $outputPath "PersonalSafer.pdb"
$stackVerifier = Join-Path $PSScriptRoot "verify-kernel-stack-frames.ps1"
& $stackVerifier -PdbPath $pdbPath -MaxProjectFrameBytes 1024
if ($LASTEXITCODE -ne 0) {
    Write-Host "ERROR: Kernel stack-frame verification failed!" -ForegroundColor Red
    exit 1
}

Write-Host "=== Kernel driver built successfully ===" -ForegroundColor Green
Write-Host "Output: $outputPathWithSlash" -ForegroundColor Yellow
