@echo off
setlocal EnableExtensions
cd /d "%~dp0"

rem ============================================================
rem  PowerGuard quick start
rem    Double-click            -> monitors DEFAULT_GAME
rem    PowerGuard.bat Game.exe -> monitors that process
rem    Elevation (UAC) is requested automatically; GPU
rem    power-limit / clock-lock need administrator rights.
rem ============================================================

set "DEFAULT_GAME=Starfield.exe"

set "EXE=bin\PowerGuard.exe"
set "GAME=%~1"
if "%GAME%"=="" set "GAME=%DEFAULT_GAME%"
if /i not "%GAME:~-4%"==".exe" set "GAME=%GAME%.exe"

if not exist "%EXE%" (
    echo [ERROR] %EXE% not found. Build it first with build.sh
    pause
    exit /b 1
)

fltmc >nul 2>&1
if not errorlevel 1 goto run

echo Requesting administrator privileges...
powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -ArgumentList '%GAME%' -Verb RunAs"
exit /b 0

:run
title PowerGuard - %GAME%
echo ============================================================
echo  PowerGuard  USB-C 100W profile
echo  Target process : %GAME%
echo  Control starts only while the game is running.
echo  Web console    : http://127.0.0.1:8080
echo  Type quit + Enter (or close this window) to restore.
echo ============================================================
echo.
"%EXE%" --profile usbc --web --process "%GAME%" --only-running
echo.
echo PowerGuard exited. Settings have been restored.
pause
