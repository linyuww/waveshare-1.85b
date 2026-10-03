#Requires -Version 7.0
<#
.SYNOPSIS
    Keep the Codex Micro board's allowance in sync, continuously.

.DESCRIPTION
    Runs windows_companion.py --watch in the foreground and restarts it if it
    ever exits, so the board never sits idle long enough for Windows to power
    the Bluetooth radio down.

    Why "continuously" matters: with no GATT client attached, Windows is allowed
    to drop the radio's power, the board sees no traffic for its 32 s
    supervision timeout and tears the link down, and the cycle repeats. A
    companion that holds the link is the no-admin fix (debug/porting-log.md 6.14).

    The board address is re-discovered on every attempt rather than resolved
    once. Two reasons: at logon the Bluetooth stack may still be coming up, and
    the firmware derives its advertised address from a bond generation byte, so
    re-flashing the board changes the address. Neither case should require
    editing a script or a scheduled task.

.PARAMETER Once
    Read and write the allowance exactly once, then exit. Useful as a test.

.PARAMETER Probe
    Discover the quota service and hold the link; never writes. Read-only.

.PARAMETER Interval
    Override the refresh interval from config.psd1.

.PARAMETER NoRestart
    Exit when the companion exits instead of restarting it.

.EXAMPLE
    .\start-companion.ps1
    .\start-companion.ps1 -Once
    .\start-companion.ps1 -Probe
#>
[CmdletBinding()]
param(
    [switch] $Once,
    [switch] $Probe,
    [int] $Interval = 0,
    [switch] $NoRestart
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '_common.ps1')

if (-not (Test-Path -LiteralPath $script:CompanionScript)) {
    Write-Bad "windows_companion.py not found at $script:CompanionScript"
    Write-Host '  Run this script from inside the repository checkout.'
    exit 2
}

$config = Get-CompanionConfig
$python = Resolve-Python -Configured $config.PythonPath
if (-not $python) {
    Write-Bad 'No Python interpreter found.'
    Write-Host '  Install Python 3.10+ or set PythonPath in scripts\companion\config.psd1.'
    exit 2
}

if ($Interval -le 0) { $Interval = [int] $config.IntervalSeconds }
if ($Interval -lt 10) {
    Write-Warn2 "interval $Interval is below the 10 s floor; using 10."
    $Interval = 10
}

# Everything except the board address, which is appended per attempt.
$baseArguments = @('-v')
if ($Probe) {
    $baseArguments += @('--probe-only', '--hold-seconds', '120')
}
elseif ($Once) {
    $baseArguments += '--once'
}
else {
    $baseArguments += @('--watch', '--interval', "$Interval")
}
if ($config.WriteAttempts) { $baseArguments += @('--write-attempts', "$($config.WriteAttempts)") }
if ($config.WriteTimeoutMs) { $baseArguments += @('--write-timeout-ms', "$($config.WriteTimeoutMs)") }
if ($config.CodexPath) { $baseArguments += @('--codex-path', $config.CodexPath) }

$logDirectory = $script:LogsDir
New-Item -ItemType Directory -Force -Path $logDirectory | Out-Null
$logFile = Join-Path $logDirectory ('companion-{0:yyyy-MM-dd}.log' -f (Get-Date))

function Write-Log {
    param([Parameter(Mandatory)] [string] $Line)
    "$((Get-Date).ToString('yyyy-MM-dd HH:mm:ss'))  $Line" |
        Out-File -LiteralPath $logFile -Append -Encoding utf8
}

Write-Host ''
Write-Host '  Codex Micro allowance companion' -ForegroundColor Cyan
Write-Host "    python  : $python"
Write-Host "    board   : $(if ($config.DeviceAddress) { $config.DeviceAddress } else { 'auto-detect by name, every attempt' })"
Write-Host "    mode    : $(if ($Probe) { 'probe only' } elseif ($Once) { 'single write' } else { "watch, every ${Interval}s" })"
Write-Host "    log     : $logFile"
Write-Host ''
Write-Host '  Press Ctrl+C to stop.' -ForegroundColor DarkGray
Write-Host ''

$attempt = 0
while ($true) {
    $attempt++

    # Re-resolved every attempt on purpose -- see the header comment.
    $address = Get-BoardAddress -Configured $config.DeviceAddress -Python $python
    if (-not $address) {
        $wait = [Math]::Min(30 * $attempt, 120)
        Write-Log "run #$attempt  board not visible over BLE; retrying in ${wait}s"
        Write-Warn2 "board not visible over BLE; retrying in ${wait}s"
        if ($Once -or $Probe) {
            Write-Bad 'Nothing to talk to. Run .\diagnose.ps1 to see which layer is stuck.'
            exit 3
        }
        Start-Sleep -Seconds $wait
        continue
    }

    $arguments = @($script:CompanionScript, '--device-address', $address) + $baseArguments
    $started = Get-Date
    Write-Log "run #$attempt  board=$address  $python $($arguments -join ' ')"

    & $python @arguments 2>&1 | Tee-Object -FilePath $logFile -Append
    $code = $LASTEXITCODE
    $ran = ((Get-Date) - $started).TotalSeconds
    Write-Log "run #$attempt exited code=$code after $([int] $ran)s"

    if ($Once -or $Probe -or $NoRestart) { exit $code }

    # A healthy --watch run is long; a short one means the link or the Codex CLI
    # is unhappy, so back off instead of hammering it.
    $backoff = if ($ran -lt 30) { [Math]::Min(15 * $attempt, 60) } else { 5 }
    Write-Warn2 "companion exited (code $code) after $([int] $ran)s; restarting in ${backoff}s"
    Start-Sleep -Seconds $backoff
}
