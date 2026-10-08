param([switch]$Clean)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
$firmwarePath = Join-Path $projectRoot 'firmware'
. "$PSScriptRoot\_env.ps1"
$buildPath = Join-Path $firmwarePath 'build-windows'
$actions = @('build')
if ($Clean) { $actions = @('fullclean', 'build') }
idf.py -C $firmwarePath -B $buildPath @actions
if ($LASTEXITCODE -ne 0) { throw 'Firmware build failed.' }
$distPath = Join-Path $projectRoot 'dist'
New-Item -ItemType Directory -Path $distPath -Force | Out-Null
idf.py -C $firmwarePath -B $buildPath merge-bin -o waveshare-launcher-usb.bin
if ($LASTEXITCODE -ne 0) { throw 'Firmware merge failed.' }
Copy-Item -LiteralPath (Join-Path $buildPath 'waveshare-launcher-usb.bin') -Destination $distPath
Copy-Item -LiteralPath (Join-Path $buildPath 'bootloader/bootloader.bin') -Destination $distPath
Copy-Item -LiteralPath (Join-Path $buildPath 'partition_table/partition-table.bin') -Destination $distPath
Copy-Item -LiteralPath (Join-Path $buildPath 'waveshare_launcher.bin') -Destination $distPath
Get-ChildItem -LiteralPath $distPath -Filter '*.bin' | Get-FileHash -Algorithm SHA256 | Select-Object Hash,Path | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $distPath 'sha256.json') -Encoding utf8
Write-Host "Firmware: $distPath\waveshare-launcher-usb.bin"
