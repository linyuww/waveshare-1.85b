$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
. "$PSScriptRoot\_env.ps1"
. "$PSScriptRoot\_host-env.ps1"
$hostBuild = Join-Path $projectRoot '.cache\ui-preview-windows'
$previewOutput = Join-Path $projectRoot '.cache\ui-preview'
New-Item -ItemType Directory -Path $previewOutput -Force | Out-Null
New-Item -ItemType Directory -Path (Join-Path $projectRoot 'logs') -Force | Out-Null
python (Join-Path $PSScriptRoot 'generate_icons.py')
if ($LASTEXITCODE -ne 0) { throw 'Asset generation failed.' }
# Preview compilation only: firmware is built after visual inspection.
cmake -S (Join-Path $projectRoot 'tests\ui_preview') -B $hostBuild -G Ninja -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ -DCMAKE_BUILD_TYPE=Release *> (Join-Path $projectRoot 'logs\ui-preview-build.log')
if ($LASTEXITCODE -ne 0) { throw 'Native preview configuration failed.' }
cmake --build $hostBuild -j 8 *>> (Join-Path $projectRoot 'logs\ui-preview-build.log')
if ($LASTEXITCODE -ne 0) { throw 'Native preview compilation failed.' }
& (Join-Path $hostBuild 'settings_preview.exe') $previewOutput
if ($LASTEXITCODE -ne 0) { throw 'Settings interaction/lifecycle checks failed.' }
python (Join-Path $PSScriptRoot 'preview_settings_images.py')
if ($LASTEXITCODE -ne 0) { throw 'Settings image conversion failed.' }
& (Join-Path $hostBuild 'preview.exe') $previewOutput
if ($LASTEXITCODE -ne 0) { throw 'LVGL preview failed; see logs/ui-preview-build.log.' }
$previousTimezone = $env:TZ
try {
    $env:TZ = 'CST-8'
    & (Join-Path $hostBuild 'fitness_preview.exe') $previewOutput
    $previewExitCode = $LASTEXITCODE
} finally {
    $env:TZ = $previousTimezone
}
if ($previewExitCode -ne 0) { throw 'Fitness preview validation failed.' }
if ($LASTEXITCODE -ne 0) { throw 'Fitness preview validation failed.' }
python (Join-Path $PSScriptRoot 'preview_fitness_images.py')
if ($LASTEXITCODE -ne 0) { throw 'Fitness preview conversion failed.' }
python (Join-Path $PSScriptRoot 'preview_images.py')
if ($LASTEXITCODE -ne 0) { throw 'Preview image conversion failed.' }
