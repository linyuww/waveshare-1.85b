#Requires -Version 7.4
[CmdletBinding()]
param([ValidateSet('Start','Stop','Status','Install')][string]$Action = 'Start')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$server = Join-Path $PSScriptRoot 'quota_server.py'
$entry = Join-Path $PSScriptRoot 'start-quota-server.ps1'
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
    $taskAction = New-ScheduledTaskAction -Execute $pwsh -Argument "-NoProfile -NonInteractive -File `"$entry`" -Foreground" -WorkingDirectory $root
    $trigger = New-ScheduledTaskTrigger -AtLogOn -User $identity
    $trigger.Delay = 'PT20S'
    $principal = New-ScheduledTaskPrincipal -UserId $identity -LogonType Interactive -RunLevel Limited
    $settings = New-ScheduledTaskSettingsSet -Hidden -StartWhenAvailable -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -ExecutionTimeLimit ([TimeSpan]::Zero) -MultipleInstances IgnoreNew -RestartCount 3 -RestartInterval (New-TimeSpan -Minutes 1)
    Register-ScheduledTask -TaskName $taskName -Action $taskAction -Trigger $trigger -Principal $principal -Settings $settings -Description 'Read-only hotspot HTTP quota service.' -Force | Out-Null
    $python = 'D:\Espressif\python_env\idf5.5_py3.12_env\Scripts\python.exe'
    if (!(Get-NetFirewallRule -Name $taskName -ErrorAction SilentlyContinue)) {
        New-NetFirewallRule -Name $taskName -DisplayName 'Codex Micro quota HTTP' -Direction Inbound -Action Allow -Protocol TCP -LocalPort 8787 -RemoteAddress LocalSubnet -Program $python -Profile Any | Out-Null
    }
    Write-Host 'HTTP quota autostart installed.'
} elseif ($Action -eq 'Start') {
    & $entry
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
