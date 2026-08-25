@echo off
setlocal

set "DRIVER_DIR=%~dp0"
if "%DRIVER_DIR:~-1%"=="\" set "DRIVER_DIR=%DRIVER_DIR:~0,-1%"

if defined VRPATHREG if exist "%VRPATHREG%" goto :found

set "VRPATHREG=%ProgramFiles(x86)%\Steam\steamapps\common\SteamVR\bin\win64\vrpathreg.exe"
if exist "%VRPATHREG%" goto :found

set "VRPATHREG=%ProgramFiles%\Steam\steamapps\common\SteamVR\bin\win64\vrpathreg.exe"
if exist "%VRPATHREG%" goto :found

echo Could not find vrpathreg.exe.
echo Set the VRPATHREG environment variable to your SteamVR bin\win64\vrpathreg.exe and run this again.
exit /b 1

:found
echo Removing driver:
echo   %DRIVER_DIR%
"%VRPATHREG%" removedriver "%DRIVER_DIR%"
if errorlevel 1 exit /b %errorlevel%

echo Driver removed. Restart SteamVR if it is already running.
endlocal
