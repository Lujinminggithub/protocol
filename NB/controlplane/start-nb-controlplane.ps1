param(
    [string]$Listen = "127.0.0.1:9091",
    [string]$StateDirectory = "E:\NBData\nb-web",
    [string]$Registry = "../tools/private/nb-web-worker.json",
    [string]$Version = "1.0.1.5"
)

$ErrorActionPreference = "Stop"
if ([string]::IsNullOrWhiteSpace($env:NB_WEB_ADMIN_TOKEN) -or
    [string]::IsNullOrWhiteSpace($env:NB_WEB_AGENT_TOKEN)) {
    throw "NB_WEB_ADMIN_TOKEN and NB_WEB_AGENT_TOKEN must be set"
}

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$registryPath = [System.IO.Path]::GetFullPath((Join-Path $root $Registry))
$portText = ($Listen -split ':')[-1]
$listenPort = 0
if (-not [int]::TryParse($portText, [ref]$listenPort) -or $listenPort -lt 1 -or $listenPort -gt 65535) {
    throw "Listen must end with a valid TCP port"
}
if (Get-NetTCPConnection -State Listen -LocalPort $listenPort -ErrorAction SilentlyContinue) {
    throw "TCP port $listenPort is already in use"
}
if (-not (Test-Path -LiteralPath $registryPath -PathType Leaf)) {
    throw "worker registry does not exist: $registryPath"
}

Push-Location $root
try {
    New-Item -ItemType Directory -Path "build" -Force | Out-Null
    go build -trimpath -o "build/nb-web.exe" ./cmd/nb-web
    go build -trimpath -ldflags "-X main.version=$Version" -o "build/nb-web-worker.exe" ./cmd/nb-web-worker

    $webEnvironment = @{
        NB_WEB_LISTEN = $Listen
        NB_WEB_STATE_DIR = $StateDirectory
    }
    $workerEnvironment = @{
        NB_WEB_BASE_URL = "http://127.0.0.1:$listenPort"
        NB_WEB_WORKER_REGISTRY = $registryPath
    }
    foreach ($entry in $webEnvironment.GetEnumerator()) {
        [Environment]::SetEnvironmentVariable($entry.Key, $entry.Value, "Process")
    }
    $web = Start-Process -FilePath "$root/build/nb-web.exe" -NoNewWindow -PassThru
    try {
        $ready = $false
        for ($attempt = 0; $attempt -lt 50; $attempt++) {
            if ($web.HasExited) {
                throw "nb-web exited before becoming ready (exit=$($web.ExitCode))"
            }
            if (Get-NetTCPConnection -State Listen -LocalPort $listenPort -ErrorAction SilentlyContinue) {
                $ready = $true
                break
            }
            Start-Sleep -Milliseconds 200
        }
        if (-not $ready) {
            throw "nb-web did not listen on port $listenPort within 10 seconds"
        }
        foreach ($entry in $workerEnvironment.GetEnumerator()) {
            [Environment]::SetEnvironmentVariable($entry.Key, $entry.Value, "Process")
        }
        $worker = Start-Process -FilePath "$root/build/nb-web-worker.exe" -NoNewWindow -PassThru
        try {
            Write-Host "NB control plane running: http://127.0.0.1:$listenPort (web PID=$($web.Id), worker PID=$($worker.Id))"
            while (-not $web.HasExited -and -not $worker.HasExited) {
                Start-Sleep -Seconds 1
            }
            if ($web.HasExited) {
                throw "nb-web exited (exit=$($web.ExitCode))"
            }
            throw "nb-web-worker exited (exit=$($worker.ExitCode))"
        }
        finally {
            if ($worker -and -not $worker.HasExited) {
                Stop-Process -Id $worker.Id -ErrorAction SilentlyContinue
            }
        }
    }
    finally {
        if ($web -and -not $web.HasExited) {
            Stop-Process -Id $web.Id -ErrorAction SilentlyContinue
        }
    }
}
finally {
    Pop-Location
}
