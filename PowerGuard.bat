@echo off
setlocal EnableExtensions
cd /d "%~dp0"
rem
rem  PowerGuard one-click launcher
rem
rem    Double-click                   start with the saved target
rem    Drag a game .exe onto it       save that target, then start
rem    PowerGuard.bat Game.exe        same as dragging
rem    PowerGuard.bat --clear         forget the saved target
rem    PowerGuard.bat --diagnose ...  forward options to PowerGuard.exe
rem
rem  The target is remembered in %LOCALAPPDATA%\PowerGuard\launcher_target.txt
rem  so later double-clicks do not ask again. On the first run without a saved
rem  target the script asks for the process name once. GPU control needs
rem  administrator; the script re-launches itself elevated only when a target
rem  is set.
rem
rem  Keep this file ASCII-only and CRLF: cmd parses batch files in the OEM
rem  code page, and non-ASCII bytes can swallow line endings.
rem

set "EXE=bin\PowerGuard.exe"
set "DATA=%LOCALAPPDATA%\PowerGuard"
set "TARGET_FILE=%DATA%\launcher_target.txt"

if not exist "%EXE%" (
    echo [ERROR] %EXE% not found. Run build.sh first.
    pause
    exit /b 1
)

if /i "%~1"=="--clear" goto clear_target
if /i "%~1"=="--monitor-only" goto monitor_removed
if /i "%~1"=="--elevated" goto elevated

set "ARG1=%~1"
if "%ARG1:~0,2%"=="--" goto passthrough

rem A dropped file or a command-line name becomes the remembered target.
set "GAME="
if not "%ARG1%"=="" set "GAME=%~nx1"
if not "%GAME%"=="" goto save_target

rem No argument: reuse the target saved by the last drag/command line.
if exist "%TARGET_FILE%" set /p GAME=<"%TARGET_FILE%"
if not "%GAME%"=="" goto have_target

rem First run and no saved target: ask once, then remember the answer.
echo   No target saved yet.
echo   Drag the game .exe onto PowerGuard.bat, or type its process name.
echo   Press Enter with no input to cancel.
echo.
set /p GAME=  Game:
if not defined GAME goto no_target
set "GAME=%GAME:"=%"
if "%GAME%"=="" goto no_target
for %%F in ("%GAME%") do set "GAME=%%~nxF"
if "%GAME%"=="" goto no_target
goto save_target

:save_target
if /i not "%GAME:~-4%"==".exe" set "GAME=%GAME%.exe"
if not exist "%DATA%" mkdir "%DATA%" >nul 2>&1
> "%TARGET_FILE%" echo %GAME%
goto have_target

:have_target
if "%GAME%"=="" goto no_target
fltmc.exe >nul 2>&1
if not errorlevel 1 goto run_control

echo   Requesting administrator privileges...
powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -ArgumentList '--elevated' -Verb RunAs"
if errorlevel 1 (
    echo [ERROR] Elevation cancelled; no limits will be applied.
    pause
)
exit /b 0

:elevated
rem Re-entered through UAC: the target comes from the saved file.
set "GAME="
if exist "%TARGET_FILE%" set /p GAME=<"%TARGET_FILE%"
if "%GAME%"=="" goto no_target
goto run_control

:no_target
echo [ERROR] No target saved or given. Drag the game .exe onto PowerGuard.bat.
pause
exit /b 1

:monitor_removed
echo [ERROR] Monitor-only mode has been removed. Drag a game .exe onto PowerGuard.bat.
pause
exit /b 1

:run_control
title PowerGuard - %GAME%
echo   Target : %GAME%
echo   Web    : http://127.0.0.1:8080
echo   Type quit + Enter (or close this window) to restore settings.
echo   Saved target: %TARGET_FILE%
echo.
"%EXE%" --profile usbc --web --process "%GAME%" --only-running
if errorlevel 1 goto app_failed
goto done

:app_failed
echo.
echo   PowerGuard did not start or exited with an error.
echo   If another instance is already running, open http://127.0.0.1:8080.
pause
exit /b 1

:done
echo.
echo   PowerGuard exited. Settings have been restored.
pause
exit /b 0

:clear_target
if exist "%TARGET_FILE%" del /q "%TARGET_FILE%"
echo   Saved target cleared. The next start will ask for a game process.
pause
exit /b 0

:passthrough
"%EXE%" %*
echo.
pause
exit /b 0
