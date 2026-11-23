@echo off
setlocal
cd /d "%~dp0"
rem Run from "x64 Native Tools Command Prompt for VS 2022" (free Build Tools is enough).
where cl >nul 2>nul
if errorlevel 1 (
  echo [ERROR] Cannot find MSVC cl.exe.
  echo Please open "x64 Native Tools Command Prompt for VS 2022" and run this file again.
  echo Download build tools: https://visualstudio.microsoft.com/visual-cpp-build-tools/
  pause
  exit /b 1
)
if not exist dist mkdir dist
cl /nologo /std:c++14 /O2 /EHsc /W4 /DUNICODE /D_UNICODE /utf-8 ^
  /Fe:"dist\RazerBatteryLite.exe" "src\RazerBatteryLite.cpp" ^
  /link /SUBSYSTEM:WINDOWS user32.lib gdi32.lib shell32.lib setupapi.lib hid.lib advapi32.lib
if errorlevel 1 (
  echo Build failed.
  pause
  exit /b 1
)
echo.
echo Built: dist\RazerBatteryLite.exe
pause
