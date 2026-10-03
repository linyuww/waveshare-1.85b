param()
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$source = Join-Path $root 'firmware'
$project = Join-Path $root '.cache/assistant-build/firmware'
$dist = Join-Path $root 'dist/assistant'
$image = 'espressif/idf:v5.5.3'
docker version --format '{{.Server.Version}}'
if ($LASTEXITCODE -ne 0) { throw 'Docker Desktop must be running.' }
New-Item -ItemType Directory -Path $project -Force | Out-Null
foreach ($directory in @('main', 'components')) {
    $destination = Join-Path $project $directory
    New-Item -ItemType Directory -Path $destination -Force | Out-Null
    Copy-Item -Path (Join-Path $source "$directory/*") -Destination $destination -Recurse -Force
}
foreach ($name in @('CMakeLists.txt', 'sdkconfig.defaults', 'dependencies.lock', 'partitions.csv')) {
    Copy-Item -LiteralPath (Join-Path $source $name) -Destination (Join-Path $project $name) -Force
}
$mounts = @('--rm', '--mount', "type=bind,source=$project,target=/project",
    '--mount', 'type=volume,source=waveshare-assistant-components,target=/project/managed_components',
    '--mount', 'type=volume,source=waveshare-idf-cache,target=/root/.cache', '-w', '/project')
docker run @mounts $image idf.py -D SDKCONFIG=/project/sdkconfig.assistant build
if ($LASTEXITCODE -ne 0) { throw 'Assistant firmware build failed.' }
docker run @mounts $image idf.py -D SDKCONFIG=/project/sdkconfig.assistant merge-bin -o waveshare-launcher-usb.bin
if ($LASTEXITCODE -ne 0) { throw 'Assistant firmware merge failed.' }
New-Item -ItemType Directory -Path $dist -Force | Out-Null
foreach ($relative in @('waveshare-launcher-usb.bin', 'bootloader/bootloader.bin',
    'partition_table/partition-table.bin', 'waveshare_launcher.bin')) {
    Copy-Item -LiteralPath (Join-Path $project "build/$relative") -Destination $dist -Force
}
Get-ChildItem -LiteralPath $dist -Filter '*.bin' | Get-FileHash -Algorithm SHA256 |
    Select-Object Hash,Path | ConvertTo-Json |
    Set-Content -LiteralPath (Join-Path $dist 'sha256.json') -Encoding utf8
Write-Host "Assistant firmware: $dist\waveshare-launcher-usb.bin (not flashed)"
