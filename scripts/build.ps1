param([switch]$Clean)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
$firmwarePath = Join-Path $projectRoot 'firmware'
$image = 'espressif/idf:v5.5.3'
docker version --format '{{.Server.Version}}'
if ($LASTEXITCODE -ne 0) { throw 'Docker Desktop must be running.' }
$actions = @('build')
if ($Clean) { $actions = @('fullclean', 'build') }
docker run --rm --mount "type=bind,source=$firmwarePath,target=/project" --mount 'type=volume,source=waveshare-idf-cache,target=/root/.cache' -w /project $image idf.py @actions
if ($LASTEXITCODE -ne 0) { throw 'Firmware build failed.' }
$distPath = Join-Path $projectRoot 'dist'
New-Item -ItemType Directory -Path $distPath -Force | Out-Null
docker run --rm --mount "type=bind,source=$firmwarePath,target=/project" -w /project $image idf.py merge-bin -o waveshare-launcher-usb.bin
if ($LASTEXITCODE -ne 0) { throw 'Firmware merge failed.' }
Copy-Item -LiteralPath (Join-Path $firmwarePath 'build/waveshare-launcher-usb.bin') -Destination $distPath
Copy-Item -LiteralPath (Join-Path $firmwarePath 'build/bootloader/bootloader.bin') -Destination $distPath
Copy-Item -LiteralPath (Join-Path $firmwarePath 'build/partition_table/partition-table.bin') -Destination $distPath
Copy-Item -LiteralPath (Join-Path $firmwarePath 'build/waveshare_launcher.bin') -Destination $distPath
Get-ChildItem -LiteralPath $distPath -Filter '*.bin' | Get-FileHash -Algorithm SHA256 | Select-Object Hash,Path | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $distPath 'sha256.json') -Encoding utf8
Write-Host "Firmware: $distPath\waveshare-launcher-usb.bin"
