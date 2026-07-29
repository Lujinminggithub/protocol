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
$portText = ($Listen -split ':')[-1]
$listenPort = 0
if (-not [int]::TryParse($portText, [ref]$listenPort) -or $listenPort -lt 1 -or $listenPort -gt 65535) {
    throw "Listen must end with a valid TCP port"
}
$listeners = @(Get-NetTCPConnection -State Listen -LocalPort $listenPort -ErrorAction SilentlyContinue)
if ($listeners.Count -gt 0) {
    $owners = ($listeners | Select-Object -ExpandProperty OwningProcess -Unique) -join ', '
    throw "TCP port $listenPort is already in use by PID $owners. Stop the old nb-web process before restarting."
}
New-Item -ItemType Directory -Path "build" -Force | Out-Null
go build -trimpath -o "build/nb-web.exe" ./cmd/nb-web
$env:NB_WEB_LISTEN = $Listen
$env:NB_WEB_STATE_DIR = $StateDirectory
& "$root/build/nb-web.exe"
