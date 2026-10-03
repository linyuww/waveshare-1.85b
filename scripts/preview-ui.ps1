$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
python (Join-Path $PSScriptRoot 'generate_icons.py')
if ($LASTEXITCODE -ne 0) { throw 'Asset generation failed.' }
# Preview compilation only: firmware is built after visual inspection.
docker run --rm --mount "type=bind,source=$projectRoot,target=/work" --mount 'type=volume,source=waveshare-ui-preview,target=/preview-build' espressif/idf:v5.5.3 bash -c 'cmake -S /work/tests/ui_preview -B /preview-build -DCMAKE_BUILD_TYPE=Release > /work/logs/ui-preview-build.log 2>&1 && cmake --build /preview-build -j 8 >> /work/logs/ui-preview-build.log 2>&1 && /preview-build/preview /work/.cache/ui-preview'
if ($LASTEXITCODE -ne 0) { throw 'LVGL preview failed; see logs/ui-preview-build.log.' }
python (Join-Path $PSScriptRoot 'preview_images.py')
if ($LASTEXITCODE -ne 0) { throw 'Preview image conversion failed.' }

