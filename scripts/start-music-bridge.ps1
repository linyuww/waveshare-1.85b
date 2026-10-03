[CmdletBinding()]
param([string] $ConfigPath = '')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if (-not $ConfigPath) { $ConfigPath = Join-Path $root '.cache/music-bridge/config.json' }
if (-not (Test-Path -LiteralPath $ConfigPath)) { return }
$ConfigPath = (Resolve-Path -LiteralPath $ConfigPath).Path
$config = Get-Content -LiteralPath $ConfigPath -Raw | ConvertFrom-Json
$health = 'http://{0}:{1}/{2}/music/health' -f $config.bind, $config.port, $config.token
try {
    $response = Invoke-RestMethod -Uri $health -TimeoutSec 2 -NoProxy
    if ($response.ok) { return }
} catch { }
if (Get-NetTCPConnection -LocalAddress $config.bind -LocalPort $config.port -State Listen -ErrorAction SilentlyContinue) {
    throw 'Music port is already occupied by another service.'
}
$python = (Get-Command python -ErrorAction Stop).Source
$bridge = Join-Path $PSScriptRoot 'music_bridge.py'
& $python $bridge --config $ConfigPath --check
if ($LASTEXITCODE -ne 0) { throw 'Invalid private music configuration.' }
$logs = Join-Path $root 'logs/music-bridge'
New-Item -ItemType Directory -Force -Path $logs | Out-Null
$process = Start-Process -FilePath $python -ArgumentList @('-u', "`"$bridge`"", '--config', "`"$ConfigPath`"") -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $logs 'stdout.log') -RedirectStandardError (Join-Path $logs 'service.log')
for ($attempt = 0; $attempt -lt 10; $attempt++) {
    Start-Sleep -Milliseconds 500
    try {
        $response = Invoke-RestMethod -Uri $health -TimeoutSec 2 -NoProxy
        if ($response.ok) {
            $process.Id | Set-Content -LiteralPath (Join-Path (Split-Path $ConfigPath -Parent) 'service.pid')
            Write-Host 'Music bridge started (private LAN, signed API, PCM streaming).'
            return
        }
    } catch { }
    if ($process.HasExited) { break }
}
throw 'Music bridge did not become healthy; inspect logs/music-bridge/service.log.'
