param(
    [Parameter(Mandatory = $true)]
    [string]$PdbPath,
    [int]$MaxProjectFrameBytes = 1024,
    [string]$LlvmPdbUtilPath = ""
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path -LiteralPath $PdbPath)) {
    throw "Kernel PDB not found: $PdbPath"
}

if (-not $LlvmPdbUtilPath) {
    $command = Get-Command llvm-pdbutil.exe -ErrorAction SilentlyContinue
    if (-not $command) {
        throw "llvm-pdbutil.exe is required for kernel stack-frame verification."
    }
    $LlvmPdbUtilPath = $command.Source
}
if (-not (Test-Path -LiteralPath $LlvmPdbUtilPath)) {
    throw "llvm-pdbutil.exe not found: $LlvmPdbUtilPath"
}

$runtimeAllowances = @{
    "_woutput_l" = 1280
}

$lines = & $LlvmPdbUtilPath dump -symbols $PdbPath
if ($LASTEXITCODE -ne 0) {
    throw "llvm-pdbutil failed with exit code $LASTEXITCODE"
}

$functionName = ""
$frames = @()
for ($i = 0; $i -lt $lines.Count; $i++) {
    if ($lines[$i] -match 'S_[LG]PROC32.*`([^`]+)`') {
        $functionName = $Matches[1]
        continue
    }
    if ($lines[$i] -match 'S_FRAMEPROC' -and
        $i + 1 -lt $lines.Count -and
        $lines[$i + 1] -match 'size = ([0-9]+)') {
        $frames += [pscustomobject]@{
            Function = $functionName
            FrameBytes = [int]$Matches[1]
        }
    }
}

if ($frames.Count -eq 0) {
    throw "No S_FRAMEPROC records were found in $PdbPath"
}

$violations = @()
foreach ($frame in $frames) {
    if ($frame.FrameBytes -le $MaxProjectFrameBytes) {
        continue
    }
    if ($runtimeAllowances.ContainsKey($frame.Function) -and
        $frame.FrameBytes -le $runtimeAllowances[$frame.Function]) {
        continue
    }
    $violations += $frame
}

$largest = $frames |
    Sort-Object FrameBytes -Descending |
    Select-Object -First 15
$largest | Format-Table -AutoSize

if ($violations.Count -gt 0) {
    $details = ($violations |
        Sort-Object FrameBytes -Descending |
        ForEach-Object { "$($_.Function)=$($_.FrameBytes)" }) -join ", "
    throw "Kernel stack-frame limit exceeded ($MaxProjectFrameBytes bytes): $details"
}

Write-Host "Kernel stack-frame verification passed: project limit=$MaxProjectFrameBytes bytes, frames=$($frames.Count)"
