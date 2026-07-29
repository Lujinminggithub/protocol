param(
    [string]$Listen = "127.0.0.1:9091",
    [string]$StateDirectory = "./build/nb-web",
    [string]$Registry = "../tools/private/nb-web-worker.json",
    [string]$Version = "1.0.1.5",
    [string]$BaseURL = ""
)

$ErrorActionPreference = "Stop"
if ([string]::IsNullOrWhiteSpace($env:NB_WEB_ADMIN_TOKEN) -or
    [string]::IsNullOrWhiteSpace($env:NB_WEB_AGENT_TOKEN)) {
    throw "NB_WEB_ADMIN_TOKEN and NB_WEB_AGENT_TOKEN must be set"
}
if ($Listen -notmatch '^(?<host>.+):(?<port>[0-9]+)$') {
    throw "Listen must use host:port"
}
$listenHost = $Matches.host.Trim('[', ']')
$listenPort = [int]$Matches.port
if ($listenPort -lt 1 -or $listenPort -gt 65535) {
    throw "Listen must end with a valid TCP port"
}
$listeners = @(Get-NetTCPConnection -State Listen -LocalPort $listenPort -ErrorAction SilentlyContinue)
if ($listeners.Count -gt 0) {
    $owners = ($listeners | Select-Object -ExpandProperty OwningProcess -Unique) -join ', '
    throw "TCP port $listenPort is already in use by PID $owners. Stop the old control plane before restarting."
}

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $root
New-Item -ItemType Directory -Path "build" -Force | Out-Null
go build -trimpath -o "build/nb-web.exe" ./cmd/nb-web
go build -trimpath -ldflags "-X main.version=$Version" -o "build/nb-web-worker.exe" ./cmd/nb-web-worker

if ([string]::IsNullOrWhiteSpace($BaseURL)) {
    $workerHost = $listenHost
    if ($workerHost -eq "0.0.0.0" -or $workerHost -eq "::") {
        $workerHost = "127.0.0.1"
    }
    $BaseURL = "http://${workerHost}:$listenPort"
}
$stdoutLog = Join-Path $root "build/nb-web-combined.stdout.log"
$stderrLog = Join-Path $root "build/nb-web-combined.stderr.log"
$env:NB_WEB_LISTEN = $Listen
$env:NB_WEB_STATE_DIR = $StateDirectory
$env:NB_WEB_BASE_URL = $BaseURL
$env:NB_WEB_WORKER_REGISTRY = $Registry

$web = Start-Process -FilePath "$root/build/nb-web.exe" -WindowStyle Hidden -PassThru `
    -RedirectStandardOutput $stdoutLog -RedirectStandardError $stderrLog
try {
    $ready = $false
    for ($attempt = 0; $attempt -lt 40; $attempt++) {
        if ($web.HasExited) {
            $tail = Get-Content $stderrLog -Tail 10 -ErrorAction SilentlyContinue
            throw "nb-web exited during startup: $tail"
        }
        try {
            $response = Invoke-WebRequest -UseBasicParsing -Uri "$BaseURL/healthz" -TimeoutSec 1
            if ($response.StatusCode -eq 200) {
                $ready = $true
                break
            }
        } catch {
            Start-Sleep -Milliseconds 250
        }
    }
    if (-not $ready) {
        throw "nb-web did not become healthy at $BaseURL"
    }
    Write-Host "NB control plane ready at $BaseURL (web PID $($web.Id)); worker is running in this window"
    & "$root/build/nb-web-worker.exe"
    if ($LASTEXITCODE -ne 0) {
        throw "nb-web-worker exited with code $LASTEXITCODE"
    }
} finally {
    if ($null -ne $web -and -not $web.HasExited) {
        Stop-Process -Id $web.Id -ErrorAction SilentlyContinue
        $web.WaitForExit()
    }
}
