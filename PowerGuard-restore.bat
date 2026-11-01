@echo off
setlocal EnableExtensions
cd /d "%~dp0"
rem
rem  Put GPU limit / GPU clocks / CPU max state back to the values saved
rem  before PowerGuard applied them. Run this after a crash or a hard
rem  power-off.
rem

set "EXE=bin\PowerGuard.exe"
if not exist "%EXE%" (
    echo [ERROR] %EXE% not found. Run build.sh first.
    pause
    exit /b 1
)

fltmc >nul 2>&1
if not errorlevel 1 goto run

echo Requesting administrator privileges...
powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
if errorlevel 1 (
    echo [ERROR] Elevation cancelled.
    pause
)
exit /b 0

:run
title PowerGuard - restore
"%EXE%" --restore
echo.
pause
