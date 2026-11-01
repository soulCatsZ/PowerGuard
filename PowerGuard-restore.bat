@echo off
setlocal EnableExtensions
cd /d "%~dp0"

rem ============================================================
rem  Emergency restore: put GPU limit / GPU clocks / CPU max
rem  state back to the values saved before PowerGuard applied
rem  them. Use this after a crash or a hard power-off.
rem ============================================================

set "EXE=bin\PowerGuard.exe"

if not exist "%EXE%" (
    echo [ERROR] %EXE% not found. Build it first with build.sh
    pause
    exit /b 1
)

fltmc >nul 2>&1
if not errorlevel 1 goto run

echo Requesting administrator privileges...
powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
exit /b 0

:run
title PowerGuard - restore
"%EXE%" --restore
echo.
pause
