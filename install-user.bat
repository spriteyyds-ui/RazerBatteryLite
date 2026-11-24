@echo off
setlocal
rem Optional per-user installation. Does not change Defender/SmartScreen policies.
set "SRC=%~dp0dist\RazerBatteryLite.exe"
if not exist "%SRC%" set "SRC=%~dp0RazerBatteryLite.exe"
if not exist "%SRC%" (
  echo [ERROR] EXE not found. Build first or place RazerBatteryLite.exe here.
  pause
  exit /b 1
)
set "TARGET=%LOCALAPPDATA%\Programs\RazerBatteryLite"
if not exist "%TARGET%" mkdir "%TARGET%"
if errorlevel 1 exit /b 1
copy /Y "%SRC%" "%TARGET%\RazerBatteryLite.exe" >nul
if errorlevel 1 (
  echo [ERROR] Cannot copy EXE. Exit the old tray program first.
  pause
  exit /b 1
)
echo Installed to: %TARGET%\RazerBatteryLite.exe
echo The program will start now. In the tray menu, enable automatic startup.
echo If Windows security blocks it, review the warning and follow README guidance.
start "" "%TARGET%\RazerBatteryLite.exe"
pause
