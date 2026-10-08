@echo off
rem Removes the PS5 HD Camera driver package (the script asks for administrator rights itself).
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0uninstall.ps1"
