param(
    [string]$Listen = "127.0.0.1:9091",
    [string]$StateDirectory = "./build/nb-web"
)

$ErrorActionPreference = "Stop"
if ([string]::IsNullOrWhiteSpace($env:NB_WEB_ADMIN_TOKEN) -or
    [string]::IsNullOrWhiteSpace($env:NB_WEB_AGENT_TOKEN)) {
    throw "NB_WEB_ADMIN_TOKEN and NB_WEB_AGENT_TOKEN must be set"
}

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $root
New-Item -ItemType Directory -Path "build" -Force | Out-Null
go build -trimpath -o "build/nb-web.exe" ./cmd/nb-web
$env:NB_WEB_LISTEN = $Listen
$env:NB_WEB_STATE_DIR = $StateDirectory
& "$root/build/nb-web.exe"
