param(
    [Parameter(Mandatory = $true)][string]$Port,
    [int]$Baud = 460800,
    [string]$Python = 'python'
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
$binary = Join-Path $projectRoot 'dist/waveshare_launcher.bin'
if (!(Test-Path -LiteralPath $binary)) { throw 'Build the firmware first with scripts/build.ps1.' }
# Host esptool accesses Windows COM ports; no USB forwarding to Docker is required.
$bootloader = Join-Path $projectRoot 'dist/bootloader.bin'
$partitions = Join-Path $projectRoot 'dist/partition-table.bin'
# Write individual partitions so subsequent updates preserve the launcher's NVS settings.
& $Python -m esptool --chip esp32s3 --port $Port --baud $Baud write_flash --flash_mode dio --flash_size 16MB --flash_freq 80m 0x0 $bootloader 0x8000 $partitions 0x10000 $binary
if ($LASTEXITCODE -ne 0) { throw 'Flashing failed. Check esptool and the selected COM port.' }
