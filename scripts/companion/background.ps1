#Requires -Version 7.4
[CmdletBinding()]
param(
    [ValidateSet('Install', 'Start', 'Stop', 'Status', 'Disable')][string]$Action = 'Start',
    [switch]$MigrateLegacy
)

$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/_common.ps1"
$taskName = 'CodexMicroAllowanceCompanion'
$hostScript = Join-Path $PSScriptRoot 'background_companion.py'
$task = Get-ScheduledTask -TaskName $taskName -TaskPath '\' -ErrorAction SilentlyContinue
$foreignTask = $task -and $task.Actions.Arguments -notlike "*`"$hostScript`"*"
if ($foreignTask -and $Action -notin 'Start', 'Stop', 'Status' -and -not ($Action -eq 'Install' -and $MigrateLegacy)) {
    throw "Existing task $taskName belongs to another installation. Back it up and disable/remove it before installing."
}

if ($Action -in 'Install', 'Start') {
    $config = Get-CompanionConfig
    $python = Resolve-Python $config.PythonPath
    if (-not $python) { throw 'Python interpreter not found.' }
    $pythonw = Join-Path (Split-Path $python -Parent) 'pythonw.exe'
    if (-not (Test-Path -LiteralPath $pythonw)) { throw "pythonw.exe not found next to $python" }
    $pwsh = (Get-Process -Id $PID).Path
}

if ($Action -eq 'Install' -or ($Action -eq 'Start' -and -not $task)) {
    if ($foreignTask) {
        if ($task.Actions.Arguments -notlike '*codex-micro-1.85b\companion\start-companion.ps1*') { throw 'Unrecognized legacy task; migration refused.' }
        $backupDir = Join-Path $script:LogsDir 'startup-backup'
        New-Item -ItemType Directory -Path $backupDir -Force | Out-Null
        $backupPath = Join-Path $backupDir ('companion-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '.xml')
        Export-ScheduledTask -TaskName $taskName -TaskPath '\' | Set-Content -LiteralPath $backupPath -Encoding utf8
    }
    $identity = [System.Security.Principal.WindowsIdentity]::GetCurrent().Name
    $taskAction = New-ScheduledTaskAction -Execute $pythonw -Argument "`"$hostScript`" `"$pwsh`"" -WorkingDirectory $script:RepoRoot
    $trigger = New-ScheduledTaskTrigger -AtLogOn -User $identity
    $trigger.Delay = 'PT20S'
    $principal = New-ScheduledTaskPrincipal -UserId $identity -LogonType Interactive -RunLevel Limited
    $settings = New-ScheduledTaskSettingsSet -Hidden -StartWhenAvailable -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -ExecutionTimeLimit ([TimeSpan]::Zero) -MultipleInstances IgnoreNew -RestartCount 3 -RestartInterval (New-TimeSpan -Minutes 1)
    Register-ScheduledTask -TaskName $taskName -TaskPath '\' -Action $taskAction -Trigger $trigger -Principal $principal -Settings $settings -Description 'Local quota bridge and BLE sync; hidden, current-user logon.' -Force -ErrorAction Stop | Out-Null
    if ($MigrateLegacy) {
        $backupDir = Join-Path $script:LogsDir 'startup-backup'
        New-Item -ItemType Directory -Path $backupDir -Force | Out-Null
        $legacyBridge = Get-ScheduledTask -TaskName 'Codex Ornament Bridge' -TaskPath '\' -ErrorAction SilentlyContinue
        if ($legacyBridge -and $legacyBridge.Actions.Arguments -like '*codex-quota-widget\scripts\start-codex-ornament-bridge.ps1*') {
            $bridgeBackup = Join-Path $backupDir 'old-bridge.xml'
            if (-not (Test-Path -LiteralPath $bridgeBackup)) {
                Export-ScheduledTask -TaskName $legacyBridge.TaskName -TaskPath '\' | Set-Content -LiteralPath $bridgeBackup -Encoding utf8
            }
            Disable-ScheduledTask -TaskName $legacyBridge.TaskName -TaskPath '\' -ErrorAction Stop | Out-Null
        }
    }
    Write-Output 'Autostart installed for current-user logon.'
}

if ($Action -eq 'Start') {
    if ($foreignTask) {
        Start-Process -FilePath $pythonw -ArgumentList "`"$hostScript`" `"$pwsh`"" -WorkingDirectory $script:RepoRoot -WindowStyle Hidden | Out-Null
        Write-Warning 'Started silently for this session only. Legacy autostart is unchanged; run Install -MigrateLegacy as administrator before next logon.'
    } else {
        $registeredTask = Get-ScheduledTask -TaskName $taskName -TaskPath '\' -ErrorAction Stop
        if ($registeredTask.State -eq 'Disabled') {
            Enable-ScheduledTask -TaskName $taskName -TaskPath '\' -ErrorAction Stop | Out-Null
        }
        Start-ScheduledTask -TaskName $taskName -TaskPath '\' -ErrorAction Stop
    }
    Write-Output 'Background sync started. Logs: logs/companion/background.log'
} elseif ($Action -in 'Stop', 'Disable') {
    $hosts = Get-CimInstance Win32_Process -Filter "Name = 'pythonw.exe'" | Where-Object { $_.CommandLine -like "*`"$hostScript`"*" }
    foreach ($hostProcess in $hosts) {
        & "$env:SystemRoot/System32/taskkill.exe" /PID $hostProcess.ProcessId /T /F | Out-Null
        if ($LASTEXITCODE -ne 0) { throw 'Could not stop background process tree.' }
    }
    if ($task -and -not $foreignTask) { Stop-ScheduledTask -TaskName $taskName -TaskPath '\' -ErrorAction Stop }
    $bridgePath = Join-Path $script:RepoRoot 'scripts/bridge/bin/codex-ornament-bridge.exe'
    Get-CimInstance Win32_Process -Filter "Name = 'codex-ornament-bridge.exe'" | Where-Object { $_.ExecutablePath -eq $bridgePath } | ForEach-Object { Stop-Process -Id $_.ProcessId -ErrorAction SilentlyContinue }
    if ($Action -eq 'Disable' -and $task) { Disable-ScheduledTask -TaskName $taskName -TaskPath '\' -ErrorAction Stop | Out-Null }
    Write-Output "Background stopped ($Action)."
} elseif ($Action -eq 'Status') {
    if ($foreignTask) { Write-Warning 'The scheduled task still points to the legacy installation.' }
    Get-CimInstance Win32_Process -Filter "Name = 'pythonw.exe'" | Where-Object { $_.CommandLine -like "*`"$hostScript`"*" } | Select-Object ProcessId,ExecutablePath
    Get-ScheduledTask -TaskName $taskName -TaskPath '\' | Select-Object TaskName,State
    Get-ScheduledTaskInfo -TaskName $taskName -TaskPath '\' | Select-Object LastRunTime,LastTaskResult,NextRunTime
}
