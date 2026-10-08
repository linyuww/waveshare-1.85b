$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\_env.ps1"
. "$PSScriptRoot\_host-env.ps1"
$root = Split-Path $PSScriptRoot -Parent
$build = Join-Path $root '.cache\audio-tests-windows'
cmake -S (Join-Path $root 'tests\audio') -B $build -G Ninja -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ -DCMAKE_BUILD_TYPE=Debug
if ($LASTEXITCODE -ne 0) { throw 'Audio test configuration failed.' }
cmake --build $build -j 4
if ($LASTEXITCODE -ne 0) { throw 'Audio test compilation failed.' }
ctest --test-dir $build --output-on-failure
if ($LASTEXITCODE -ne 0) { throw 'Audio tests failed.' }
