# build-kernel.ps1
# Build the KMDF kernel driver

param(
    [string]$Configuration = "Release",
    [string]$Platform = "x64",
    [string]$WindowsSdkDirOverride = ""
)

Write-Host "=== Building PersonalSafer Kernel Driver ===" -ForegroundColor Cyan

. (Join-Path $PSScriptRoot 'build-tools.ps1')

$resolvedWdkPath = Resolve-WindowsSdkRoot -Override $WindowsSdkDirOverride
$env:WindowsSdkDir = if ($resolvedWdkPath.EndsWith('\')) { $resolvedWdkPath } else { "$resolvedWdkPath\" }

Write-Host "WDK Path: $env:WindowsSdkDir" -ForegroundColor Green

$msbuildPath = Resolve-MSBuildPath

Write-Host "MSBuild: $msbuildPath" -ForegroundColor Green

$projectPath = Join-Path $PSScriptRoot "..\src\kernel\PersonalSafer.vcxproj"
$outputPath = Join-Path $PSScriptRoot "..\build\kernel\$Platform\$Configuration"
$outputPathWithSlash = if ($outputPath.EndsWith('\')) { $outputPath } else { "$outputPath\" }
$driverKitTasks18 = Get-ChildItem -Path (Join-Path $env:WindowsSdkDir 'build\*\bin\Microsoft.DriverKit.Build.Tasks.18.0.dll') -File -ErrorAction SilentlyContinue |
    Select-Object -First 1
$vsVersion = if ($driverKitTasks18) { "18.0" } else { "17.0" }

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
