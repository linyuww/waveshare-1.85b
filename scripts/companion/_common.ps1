#Requires -Version 7.0
<#
    companion/_common.ps1

    Shared helpers for the companion launchers. Dot-source it, never run it:

        . (Join-Path $PSScriptRoot '_common.ps1')

    It resolves what the launchers all need:
      * the repository root (this folder is <repo>/scripts/companion),
      * the sibling script folders (../windows, ../../logs/companion),
      * a usable Python interpreter,
      * the board's BLE address (from config, or auto-detected by name).
#>

$script:CompanionDir = $PSScriptRoot                        # <repo>/scripts/companion
$script:ScriptsDir = Split-Path -Parent $PSScriptRoot       # <repo>/scripts
$script:RepoRoot = Split-Path -Parent $script:ScriptsDir    # <repo>
$script:CompanionScript = Join-Path $script:ScriptsDir 'windows\windows_companion.py'
$script:ToolsDir = Join-Path $script:ScriptsDir 'windows'
$script:LogsDir = Join-Path $script:RepoRoot 'logs\companion'

function Get-CompanionConfig {
    <#
        Return config.psd1 merged over the defaults.

        The merge matters because the callers run under StrictMode: reading a
        key that a user deleted from config.psd1 would otherwise throw instead
        of falling back.
    #>
    $defaults = @{
        DeviceAddress   = ''
        IntervalSeconds = 60
        WriteAttempts   = 4
        WriteTimeoutMs  = 12000
        PythonPath      = ''
        CodexPath       = ''
    }

    $path = Join-Path $script:CompanionDir 'config.psd1'
    if (-not (Test-Path -LiteralPath $path)) { return $defaults }

    $loaded = Import-PowerShellDataFile -LiteralPath $path
    foreach ($key in @($loaded.Keys)) {
        $defaults[$key] = $loaded[$key]
    }
    return $defaults
}

function Resolve-Python {
    <#
        Return the full path of a Python interpreter, or $null.

        Order: explicit config -> repo virtualenv -> PATH -> the Windows "py"
        launcher -> the managed interpreter this workspace ships. No path is
        hard-coded as the only option; the first one that exists wins.
    #>
    param([string] $Configured)

    if ($Configured -and (Test-Path -LiteralPath $Configured)) {
        return (Resolve-Path -LiteralPath $Configured).Path
    }

    foreach ($relative in @('.venv\Scripts\python.exe', 'venv\Scripts\python.exe')) {
        $candidate = Join-Path $script:RepoRoot $relative
        if (Test-Path -LiteralPath $candidate) { return $candidate }
    }

    $onPath = Get-Command python.exe -ErrorAction SilentlyContinue
    if ($onPath) { return $onPath.Source }

    $launcher = Get-Command py.exe -ErrorAction SilentlyContinue
    if ($launcher) {
        $resolved = & $launcher.Source -3 -c 'import sys; print(sys.executable)' 2>$null
        if ($LASTEXITCODE -eq 0 -and $resolved) { return $resolved.Trim() }
    }

    $managed = Join-Path $env:USERPROFILE '.workbuddy-ai\binaries\python'
    $managedEnv = Join-Path $managed 'envs\default\Scripts\python.exe'
    if (Test-Path -LiteralPath $managedEnv) { return $managedEnv }

    $versioned = Get-ChildItem -Path (Join-Path $managed 'versions') -Filter 'python.exe' `
        -Recurse -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($versioned) { return $versioned.FullName }

    return $null
}

function Get-BoardAddress {
    <#
        Resolve the board's BLE address.

        A configured address always wins. Otherwise the address is read back
        from scripts/windows/ble_devices.py, because the firmware derives the advertised
        address from a bond generation byte: it changes whenever the board is
        re-flashed with a new generation, and a stale literal in a script is the
        fastest way to a confusing failure.
    #>
    param(
        [string] $Configured,
        [Parameter(Mandatory)] [string] $Python,
        [string] $Name = 'Codex Micro'
    )

    if ($Configured) { return $Configured.Trim().ToUpper() }

    $lister = Join-Path $script:ToolsDir 'ble_devices.py'
    if (-not (Test-Path -LiteralPath $lister)) { return $null }

    $lines = & $Python $lister 2>$null
    $inBleSection = $false
    foreach ($line in $lines) {
        if ($line -match '^===\s*ble\s*\(') { $inBleSection = $true; continue }
        if ($inBleSection -and $line -match '^===') { break }
        if ($inBleSection -and $line -match '^\s*([0-9A-Fa-f]{2}(?::[0-9A-Fa-f]{2}){5})\s+(.+?)\s*$') {
            if ($Matches[2] -like "*$Name*") { return $Matches[1].ToUpper() }
        }
    }
    return $null
}

function Write-Section {
    param([Parameter(Mandatory)] [string] $Title)
    Write-Host ''
    Write-Host ("--- $Title " + ('-' * [Math]::Max(0, 60 - $Title.Length))) -ForegroundColor DarkCyan
}

function Write-Ok {
    param([Parameter(Mandatory)] [string] $Message)
    Write-Host "  [ok]   $Message" -ForegroundColor Green
}

function Write-Warn2 {
    param([Parameter(Mandatory)] [string] $Message)
    Write-Host "  [warn] $Message" -ForegroundColor Yellow
}

function Write-Bad {
    param([Parameter(Mandatory)] [string] $Message)
    Write-Host "  [fail] $Message" -ForegroundColor Red
}

function Test-Elevated {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = [Security.Principal.WindowsPrincipal]::new($identity)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}
