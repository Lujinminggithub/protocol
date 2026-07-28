param(
    [string]$Registry = "../tools/private/nb-web-worker.json",
    [string]$Version = "1.0.1.5"
)

$ErrorActionPreference = "Stop"
if ([string]::IsNullOrWhiteSpace($env:NB_WEB_BASE_URL) -or
    [string]::IsNullOrWhiteSpace($env:NB_WEB_AGENT_TOKEN)) {
    throw "NB_WEB_BASE_URL and NB_WEB_AGENT_TOKEN must be set"
}

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $root
New-Item -ItemType Directory -Path "build" -Force | Out-Null
go build -trimpath -ldflags "-X main.version=$Version" -o "build/nb-web-worker.exe" ./cmd/nb-web-worker
$env:NB_WEB_WORKER_REGISTRY = $Registry
& "$root/build/nb-web-worker.exe"
