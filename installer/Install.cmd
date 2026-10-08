@echo off
rem Installs the PS5 HD Camera driver package (the script asks for administrator rights itself).
rem Asks "with bokeh / without bokeh"; options: -Bokeh on^|off  -Tray (tray icon for development)
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1" %*
