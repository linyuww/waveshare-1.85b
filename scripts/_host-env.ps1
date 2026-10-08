# Keep the host compiler's DLLs ahead of cross-toolchain DLLs on Windows.
$hostCompiler = Get-Command g++ -CommandType Application -ErrorAction Stop
$hostCompilerBin = Split-Path $hostCompiler.Source
$env:Path = "$hostCompilerBin;$env:Path"
