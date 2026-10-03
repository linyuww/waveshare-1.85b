#Requires -Version 7.4
[CmdletBinding()]
param([string] $QuotaUrl = 'http://127.0.0.1:8787/quota')

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bridgeRoot = Join-Path $repoRoot 'scripts\bridge'
$executable = Join-Path $bridgeRoot 'bin\codex-ornament-bridge.exe'
$manifest = Get-Content -LiteralPath (Join-Path $bridgeRoot 'runtime.json') -Raw | ConvertFrom-Json
if ((Get-FileHash -LiteralPath $executable -Algorithm SHA256).Hash -ne $manifest.executable.sha256) {
    throw 'The bundled bridge checksum does not match runtime.json.'
}
$quotaUri = [uri] $QuotaUrl
if (-not $quotaUri.IsAbsoluteUri -or -not $quotaUri.IsLoopback -or $quotaUri.Scheme -ne 'http' -or $quotaUri.AbsolutePath -ne '/quota') {
    throw 'The bundled bridge requires a loopback HTTP /quota URL.'
}
$readyUri = [uri]::new($quotaUri, '/ready')
$logDirectory = Join-Path $repoRoot 'logs\companion'
New-Item -ItemType Directory -Path $logDirectory -Force | Out-Null

function Test-BridgeReady {
    try {
        $response = Invoke-RestMethod -Uri $readyUri -TimeoutSec 2 -NoProxy -ErrorAction Stop
        return $response.ok -eq $true
    }
    catch {
        return $false
    }
}

$listeners = @(Get-NetTCPConnection -State Listen -LocalPort $quotaUri.Port -ErrorAction SilentlyContinue)
$listenerIds = @($listeners | Select-Object -ExpandProperty OwningProcess -Unique)
$owned = @(Get-Process -Name 'codex-ornament-bridge' -ErrorAction SilentlyContinue |
    Where-Object { $_.Path -eq $executable -and $_.Id -in $listenerIds })
$ownedIds = @($owned | Select-Object -ExpandProperty Id)
if (@($listenerIds | Where-Object { $_ -notin $ownedIds }).Count -gt 0) {
    throw "Port $($quotaUri.Port) is occupied by another program. Stop the old bridge before starting this copy."
}
if ($owned.Count -gt 0) {
    if (-not (Test-BridgeReady)) { throw 'The project bridge is running but not ready; restart it before syncing.' }
    Write-Host "  [bridge] Already running from this project (PID $($ownedIds -join ', '))."
    return
}

$process = Start-Process -FilePath $executable -WorkingDirectory $bridgeRoot -WindowStyle Hidden -PassThru `
    -RedirectStandardOutput (Join-Path $logDirectory 'bridge-stdout.log') `
    -RedirectStandardError (Join-Path $logDirectory 'bridge-stderr.log') `
    -Environment @{
        CODEX_ORNAMENT_BIND = "127.0.0.1:$($quotaUri.Port)"
        CODEX_QUOTA_STATE = (Join-Path $logDirectory 'quota-state.json')
        CODEX_ORNAMENT_EVENT_LOG = (Join-Path $logDirectory 'bridge-events.jsonl')
    }
for ($attempt = 0; $attempt -lt 30; $attempt++) {
    $process.Refresh()
    if ($process.HasExited) { throw "The bridge exited with code $($process.ExitCode); see logs/companion/bridge-stderr.log." }
    if (Test-BridgeReady) {
        Write-Host "  [bridge] Ready at $QuotaUrl (PID $($process.Id))."
        return
    }
    Start-Sleep -Milliseconds 500
}
throw 'The bridge did not become ready; see logs/companion/bridge-stderr.log.'
