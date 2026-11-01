@echo off
setlocal EnableExtensions
cd /d "%~dp0"

rem ============================================================
rem  PowerGuard quick start
rem    PowerGuard.bat Game.exe     -> monitor that process (asks for UAC)
rem    PowerGuard.bat --diagnose   -> pass options through as-is
rem    Elevation (UAC) is only requested for the game-launch path.
rem    GPU power-limit / clock-lock need administrator rights.
rem ============================================================

set "EXE=bin\PowerGuard.exe"
if not exist "%EXE%" (
    echo [ERROR] %EXE% not found. Build it first with build.sh
    pause
    exit /b 1
)

if "%~1"=="" (
    echo Usage: PowerGuard.bat Game.exe
    echo        PowerGuard.bat --diagnose
    echo.
    echo Examples:
    echo   PowerGuard.bat Cyberpunk2077.exe
    echo   PowerGuard.bat --diagnose
    pause
    exit /b 1
)

rem Options are passed straight to PowerGuard.exe (no UAC needed for --diagnose).
set "ARG1=%~1"
if "%ARG1:~0,2%"=="--" (
    "%EXE%" %*
    echo.
    pause
    exit /b
)

set "GAME=%~1"
if /i not "%GAME:~-4%"==".exe" set "GAME=%GAME%.exe"

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
