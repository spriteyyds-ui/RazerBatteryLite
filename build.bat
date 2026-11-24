@echo off
setlocal
cd /d "%~dp0"
rem Windows SDK RC.EXE and MSVC CL.EXE are required.
rem Run inside "x64 Native Tools Command Prompt for VS 2022".
where cl >nul 2>nul
if errorlevel 1 (
  echo [ERROR] Missing cl.exe. Use x64 Native Tools Command Prompt for VS 2022.
  pause
  exit /b 1
)
where rc >nul 2>nul
if errorlevel 1 (
  echo [ERROR] Missing rc.exe. Install the Windows SDK with Visual Studio Build Tools.
  pause
  exit /b 1
)
if not exist dist mkdir dist
rc /nologo /I res /fo "dist\RazerBatteryLite.res" "res\icons.rc"
if errorlevel 1 goto fail
cl /nologo /std:c++14 /O2 /EHsc /W4 /DUNICODE /D_UNICODE /utf-8 /Ires ^
  /Fe:"dist\RazerBatteryLite.exe" "src\RazerBatteryLite.cpp" "dist\RazerBatteryLite.res" ^
  /link /SUBSYSTEM:WINDOWS user32.lib gdi32.lib shell32.lib setupapi.lib hid.lib advapi32.lib dwmapi.lib
if errorlevel 1 goto fail
echo Built: dist\RazerBatteryLite.exe
pause
exit /b 0
:fail
echo [ERROR] Build failed.
pause
exit /b 1
