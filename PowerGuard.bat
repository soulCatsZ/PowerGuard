@echo off
setlocal EnableExtensions
cd /d "%~dp0"

rem ============================================================
rem  PowerGuard quick start
rem    PowerGuard.bat              -> ask for the process name, then run
rem    PowerGuard.bat Game.exe     -> monitor that process (asks for UAC)
rem    PowerGuard.bat --diagnose   -> pass options through as-is
rem    Elevation (UAC) is only requested for the game-launch path.
rem    GPU power-limit / clock-lock need administrator rights.
rem
rem  Keep this file ASCII-only: cmd parses batch files in the OEM
rem  code page, and non-ASCII bytes can swallow line endings.
rem ============================================================

set "EXE=bin\PowerGuard.exe"
if not exist "%EXE%" (
    echo [ERROR] %EXE% not found. Build it first with build.sh
    pause
    exit /b 1
)

rem --- Stage 2: re-entered elevated. -------------------------------------
rem The target name arrives through a temp file, not as an argument:
rem Start-Process -Verb RunAs cannot pass a quoted argument to a .bat
rem (the launch silently does nothing), and an unquoted one is split at
rem spaces, so "Some Game.exe" would arrive as just "Some".
rem The read happens at the :from_elevation label below, not inside an
rem if(...) block: %GAME% in a block is expanded at parse time, before
rem set /p has read anything.
if /i "%~1"=="--elevated" goto from_elevation

rem Options are passed straight to PowerGuard.exe (no UAC needed for --diagnose).
set "ARG1=%~1"
if "%ARG1:~0,2%"=="--" (
    "%EXE%" %*
    echo.
    pause
    exit /b
)

rem Drag-and-drop hands us a full path; keep the file name only.
set "GAME=%~nx1"
if not "%GAME%"=="" goto normalize
goto prompt

:from_elevation
set "CAME_ELEVATED=1"
set "GAME="
if exist "%TEMP%\PowerGuard_target.txt" set /p GAME=<"%TEMP%\PowerGuard_target.txt"
del "%TEMP%\PowerGuard_target.txt" >nul 2>&1
if not "%GAME%"=="" goto normalize

:prompt
echo ============================================================
echo  PowerGuard  USB-C 100W profile
echo ============================================================
echo.
echo   Enter the game process to watch, e.g. Cyberpunk2077.exe
echo   Case-insensitive, .exe optional; you can also drop an exe
echo   on this file. Not running yet is fine - control starts on
echo   its own when the game launches.
echo.
set /p "GAME=Process name [Enter = record only, no control]: "
echo.

:normalize
rem A full path typed here is fine: PowerGuard keeps only the file name.
if "%GAME%"=="" goto monitor_only
if /i not "%GAME:~-4%"==".exe" set "GAME=%GAME%.exe"

fltmc >nul 2>&1
if not errorlevel 1 goto run
if defined CAME_ELEVATED goto run

rem Hand the target over through a temp file, then elevate without arguments.
> "%TEMP%\PowerGuard_target.txt" echo %GAME%
echo Requesting administrator privileges...
powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -ArgumentList '--elevated' -Verb RunAs"
if errorlevel 1 (
    del "%TEMP%\PowerGuard_target.txt" >nul 2>&1
    echo [ERROR] Elevation was cancelled or denied.
    echo         Right-click PowerGuard.bat and pick "Run as administrator".
    pause
)
exit /b 0

:monitor_only
title PowerGuard - monitor only
echo No target process: recording power and serving the web console,
echo without applying any limits.
echo.
"%EXE%" --web
echo.
pause
exit /b

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
