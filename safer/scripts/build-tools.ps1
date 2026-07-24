# Shared discovery helpers for build tools installed outside the repository.

function Resolve-MSBuildPath {
    $command = Get-Command msbuild.exe -ErrorAction SilentlyContinue
    if ($command) {
        return $command.Source
    }

    $vswhereCandidates = @()
    if (${env:ProgramFiles(x86)}) {
        $vswhereCandidates += Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    }
    if ($env:ProgramFiles) {
        $vswhereCandidates += Join-Path $env:ProgramFiles 'Microsoft Visual Studio\Installer\vswhere.exe'
    }

    $vswhereCommand = Get-Command vswhere.exe -ErrorAction SilentlyContinue
    if ($vswhereCommand) {
        $vswhereCandidates = @($vswhereCommand.Source) + $vswhereCandidates
    }

    $vswhere = $vswhereCandidates |
        Where-Object { $_ -and (Test-Path -LiteralPath $_) } |
        Select-Object -First 1
    if ($vswhere) {
        $resolved = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' |
            Select-Object -First 1
        if ($resolved -and (Test-Path -LiteralPath $resolved)) {
            return $resolved
        }
    }

    throw 'MSBuild.exe was not found in PATH or through Visual Studio Installer.'
}

function Resolve-WindowsSdkRoot {
    param([string]$Override = '')

    $candidates = @()
    if ($Override) { $candidates += $Override }
    if ($env:WindowsSdkDir) { $candidates += $env:WindowsSdkDir }

    foreach ($registryPath in @(
        'HKLM:\SOFTWARE\Microsoft\Windows Kits\Installed Roots',
        'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows Kits\Installed Roots'
    )) {
        try {
            $kitsRoot = Get-ItemPropertyValue -Path $registryPath -Name KitsRoot10 -ErrorAction Stop
            if ($kitsRoot) { $candidates += $kitsRoot }
        } catch {
            # Continue with the remaining discovery sources.
        }
    }

    $root = $candidates |
        Where-Object { $_ -and (Test-Path -LiteralPath $_) } |
        ForEach-Object { (Resolve-Path -LiteralPath $_).Path } |
        Select-Object -Unique |
        Select-Object -First 1
    if (-not $root) {
        throw 'Windows SDK/WDK was not found through WindowsSdkDir or Windows Kits registration.'
    }

    return $root
}

function Resolve-WindowsSdkTool {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [string]$Architecture = 'x64',
        [string]$SdkRoot = ''
    )

    $command = Get-Command $Name -ErrorAction SilentlyContinue
    if ($command) {
        return $command.Source
    }

    if (-not $SdkRoot) {
        $SdkRoot = Resolve-WindowsSdkRoot
    }
    $tool = Get-ChildItem -Path (Join-Path $SdkRoot "bin\*\$Architecture\$Name") -File -ErrorAction SilentlyContinue |
        Sort-Object FullName -Descending |
        Select-Object -First 1 -ExpandProperty FullName
    if (-not $tool) {
        throw "$Name was not found in PATH or the Windows SDK: $SdkRoot"
    }

    return $tool
}
