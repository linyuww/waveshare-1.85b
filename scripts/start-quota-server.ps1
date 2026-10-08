#Requires -Version 7.4
# Existing Windows tasks still use this path; all service logic lives in quota-server.ps1.
[CmdletBinding()]
param([string]$Python = 'D:\Espressif\python_env\idf5.5_py3.12_env\Scripts\python.exe', [switch]$Foreground)
& "$PSScriptRoot/quota-server.ps1" -Action Start -Python $Python -Foreground:$Foreground
