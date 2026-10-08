#Requires -Version 7.4
[CmdletBinding()]
param(
    [ValidateSet('Start','Stop','Status','Install')][string]$Action = 'Start',
    [string]$Python = 'D:\Espressif\python_env\idf5.5_py3.12_env\Scripts\python.exe',
    [switch]$Foreground
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$server = Join-Path $PSScriptRoot 'quota_server.py'
$entry = $PSCommandPath
$taskName = 'CodexMicroQuotaHttp'

if ($Action -eq 'Install') {
    $old = Get-ScheduledTask -TaskName 'CodexMicroAllowanceCompanion' -ErrorAction SilentlyContinue
    if ($old) {
        if ($old.Actions.Arguments -notlike "*$root*") { throw 'Old quota task belongs to another checkout.' }
        Stop-ScheduledTask -TaskName $old.TaskName
        Unregister-ScheduledTask -TaskName $old.TaskName -Confirm:$false
    }
    $identity = [System.Security.Principal.WindowsIdentity]::GetCurrent().Name
    $pwsh = (Get-Process -Id $PID).Path
    if (!(Test-Path -LiteralPath $Python -PathType Leaf)) { throw 'Python executable not found.' }
    $Python = (Resolve-Path -LiteralPath $Python).Path
    $taskAction = New-ScheduledTaskAction -Execute $pwsh -Argument "-NoProfile -NonInteractive -File `"$entry`" -Action Start -Foreground -Python `"$Python`"" -WorkingDirectory $root
    $trigger = New-ScheduledTaskTrigger -AtLogOn -User $identity
    $trigger.Delay = 'PT20S'
    $principal = New-ScheduledTaskPrincipal -UserId $identity -LogonType Interactive -RunLevel Limited
    $settings = New-ScheduledTaskSettingsSet -Hidden -StartWhenAvailable -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -ExecutionTimeLimit ([TimeSpan]::Zero) -MultipleInstances IgnoreNew -RestartCount 3 -RestartInterval (New-TimeSpan -Minutes 1)
    Register-ScheduledTask -TaskName $taskName -Action $taskAction -Trigger $trigger -Principal $principal -Settings $settings -Description 'Read-only hotspot HTTP quota service.' -Force | Out-Null
    if (!(Get-NetFirewallRule -Name $taskName -ErrorAction SilentlyContinue)) {
        New-NetFirewallRule -Name $taskName -DisplayName 'Codex Micro quota HTTP' -Direction Inbound -Action Allow -Protocol TCP -LocalPort 8787 -RemoteAddress LocalSubnet -Program $Python -Profile Any | Out-Null
    } else {
        Set-NetFirewallRule -Name $taskName -Program $Python | Out-Null
    }
    Write-Host 'HTTP quota autostart installed.'
} elseif ($Action -eq 'Start') {
    $bridgeRoot = Join-Path $PSScriptRoot 'bridge'
    $bridge = Join-Path $bridgeRoot 'bin/codex-ornament-bridge.exe'
    $logs = Join-Path $root 'logs/quota-http'
    New-Item -ItemType Directory -Path $logs -Force | Out-Null
    $manifest = Get-Content -LiteralPath (Join-Path $bridgeRoot 'runtime.json') -Raw | ConvertFrom-Json
    if ((Get-FileHash -LiteralPath $bridge -Algorithm SHA256).Hash -ne $manifest.executable.sha256) { throw 'Bridge checksum mismatch.' }
    if (!(Test-Path -LiteralPath $Python)) { throw 'Python executable not found.' }

    # The full bridge stays on loopback; the hotspot endpoint serves only quota.
    $listeners = @(Get-NetTCPConnection -LocalPort 8786 -State Listen -ErrorAction SilentlyContinue)
    $owned = @(Get-Process -Name 'codex-ornament-bridge' -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $bridge -and $_.Id -in $listeners.OwningProcess })
    if ($listeners.Count -and !$owned.Count) { throw 'Port 8786 is occupied by another program.' }
    if (!$owned.Count) {
        Start-Process -FilePath $bridge -WorkingDirectory $bridgeRoot -WindowStyle Hidden -PassThru `
            -RedirectStandardOutput (Join-Path $logs 'bridge-stdout.log') `
            -RedirectStandardError (Join-Path $logs 'bridge-stderr.log') `
            -Environment @{ CODEX_ORNAMENT_BIND = '127.0.0.1:8786'; CODEX_QUOTA_STATE = (Join-Path $logs 'quota-state.json') } | Out-Null
    }
    $ready = $false
    for ($attempt = 0; $attempt -lt 30; $attempt++) {
        try { $ready = (Invoke-RestMethod 'http://127.0.0.1:8786/ready' -TimeoutSec 2 -NoProxy).ok } catch { $ready = $false }
        if ($ready) { break }
        Start-Sleep -Milliseconds 500
    }
    if (!$ready) { throw 'Quota bridge did not become ready; see logs/quota-http.' }
    $servers = @(Get-CimInstance Win32_Process -Filter "Name = 'python.exe'" | Where-Object { $_.CommandLine -like "*`"$server`"*" })
    if (!$servers.Count) {
        if (Get-NetTCPConnection -LocalPort 8787 -State Listen -ErrorAction SilentlyContinue) { throw 'Port 8787 is occupied. Stop the previous quota service before starting.' }
        if ($Foreground) { & $Python -u $server; exit $LASTEXITCODE }
        Start-Process -FilePath $Python -ArgumentList @('-u', "`"$server`"") -WorkingDirectory $root -WindowStyle Hidden `
            -RedirectStandardOutput (Join-Path $logs 'server-stdout.log') `
            -RedirectStandardError (Join-Path $logs 'server-stderr.log') | Out-Null
    }
    Write-Host 'Quota HTTP server started on port 8787. Connect the board to this computer hotspot.'
} elseif ($Action -eq 'Stop') {
    $task = Get-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue
    if ($task) { Stop-ScheduledTask -TaskName $taskName }
    $processes = Get-CimInstance Win32_Process -Filter "Name = 'python.exe'" | Where-Object { $_.CommandLine -like "*`"$server`"*" }
    foreach ($process in $processes) { Stop-Process -Id $process.ProcessId -ErrorAction SilentlyContinue }
    $bridge = Join-Path $PSScriptRoot 'bridge/bin/codex-ornament-bridge.exe'
    Get-Process -Name codex-ornament-bridge -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $bridge } | Stop-Process
} else {
    Invoke-RestMethod 'http://127.0.0.1:8787/quota' -NoProxy -TimeoutSec 22
}
