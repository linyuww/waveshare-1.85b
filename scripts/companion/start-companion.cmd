@echo off
rem Double-click entry point for the allowance companion.
rem ASCII only: cmd.exe reads this file in the OEM code page, so non-ASCII
rem text here would garble. All user-facing prose lives in the .ps1 files.

setlocal
set "SCRIPT=%~dp0start-companion.ps1"

set "PWSH=%ProgramFiles%\PowerShell\7\pwsh.exe"
if not exist "%PWSH%" set "PWSH="
if not defined PWSH for %%I in (pwsh.exe) do set "PWSH=%%~$PATH:I"

if not defined PWSH (
  echo.
  echo   PowerShell 7 is required but was not found.
  echo   Expected at: %ProgramFiles%\PowerShell\7\pwsh.exe
  echo.
  pause
  exit /b 1
)

"%PWSH%" -NoProfile -ExecutionPolicy Bypass -File "%SCRIPT%" %*
set "CODE=%ERRORLEVEL%"
if not "%CODE%"=="0" (
  echo.
  echo   Exited with code %CODE%.
  pause
)
endlocal
exit /b %CODE%
