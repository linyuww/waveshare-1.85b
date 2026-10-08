$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\_host-env.ps1"
$root = Split-Path $PSScriptRoot -Parent
$build = Join-Path $root '.cache\render-tests-windows'
New-Item -ItemType Directory -Path $build -Force | Out-Null
Push-Location (Join-Path $root 'firmware\components\codex_micro\assets')
try {
    foreach ($theme in @('day','night')) {
        ld -r -b binary "bg_$theme.bin" -o (Join-Path $build "bg_$theme.o")
        if ($LASTEXITCODE -ne 0) { throw 'Background asset conversion failed.' }
    }
} finally { Pop-Location }
$test = Join-Path $build 'render_test.exe'
g++ -std=c++17 -g -I (Join-Path $root 'tests\stubs') -I (Join-Path $root 'firmware\components\codex_micro') (Join-Path $root 'tests\render_codex.cpp') (Join-Path $root 'firmware\components\codex_micro\gfx.cpp') (Join-Path $build 'bg_day.o') (Join-Path $build 'bg_night.o') -o $test
if ($LASTEXITCODE -ne 0) { throw 'Render test compilation failed.' }
Push-Location $root
try {
    & $test
    if ($LASTEXITCODE -ne 0) { throw 'Render tests failed.' }
} finally { Pop-Location }
