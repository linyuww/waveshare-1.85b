$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\_host-env.ps1"
$root = Split-Path $PSScriptRoot -Parent
$build = Join-Path $root '.cache\fitness-tests-windows'
New-Item -ItemType Directory -Path $build -Force | Out-Null
$test = Join-Path $build 'fitness_test.exe'
g++ -std=c++17 -Wall -Wextra -Werror -g -I (Join-Path $root 'firmware\main') (Join-Path $root 'tests\fitness_model_test.cpp') (Join-Path $root 'firmware\main\fitness_model.cpp') -o $test
if ($LASTEXITCODE -ne 0) { throw 'Fitness test compilation failed.' }
& $test
if ($LASTEXITCODE -ne 0) { throw 'Fitness tests failed.' }
