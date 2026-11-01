@echo off
setlocal EnableExtensions
cd /d "%~dp0"
rem
rem  PowerGuard quick start
rem    PowerGuard.bat              ask for the process name, then run
rem    PowerGuard.bat Game.exe     monitor that process
rem    PowerGuard.bat --diagnose   pass options straight to the exe
rem
rem  Keep this file ASCII-only and CRLF: cmd parses batch files in the
rem  OEM code page, and non-ASCII bytes can swallow line endings.
rem

set "EXE=bin\PowerGuard.exe"
if not exist "%EXE%" (
    echo [ERROR] %EXE% not found. Run build.sh first.
    pause
    exit /b 1
)

rem Re-entered after UAC; the target arrives through a temp file.
if /i "%~1"=="--elevated" goto from_elevated

set "ARG1=%~1"
if "%ARG1:~0,2%"=="--" (
    "%EXE%" %*
    echo.
    pause
    exit /b
)

rem An argument (typed or a dropped exe) sets the target; a full path is
rem fine, PowerGuard keeps only the file name.
set "GAME=%~nx1"
if not "%GAME%"=="" goto normalize

echo   Enter the game process name, for example Cyberpunk2077.exe
echo   (.exe optional; it does not need to be running yet)
echo   Press Enter to only record power, without controlling anything.
echo.
set /p "GAME=Process name: "
echo.
goto normalize

:from_elevated
set "CAME_ELEVATED=1"
set "GAME="
if exist "%TEMP%\PowerGuard_target.txt" set /p GAME=<"%TEMP%\PowerGuard_target.txt"
del "%TEMP%\PowerGuard_target.txt" >nul 2>&1
if not "%GAME%"=="" goto normalize

:normalize
if "%GAME%"=="" goto monitor_only
if /i not "%GAME:~-4%"==".exe" set "GAME=%GAME%.exe"

fltmc >nul 2>&1
if not errorlevel 1 goto run
if defined CAME_ELEVATED goto run

rem GPU control needs administrator rights. The name goes through a temp
rem file because Start-Process -Verb RunAs cannot pass a quoted argument
rem to a .bat (the launch silently does nothing) and an unquoted one
rem breaks on spaces.
> "%TEMP%\PowerGuard_target.txt" echo %GAME%
echo Requesting administrator privileges...
powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -ArgumentList '--elevated' -Verb RunAs"
if errorlevel 1 (
    del "%TEMP%\PowerGuard_target.txt" >nul 2>&1
    echo [ERROR] Elevation cancelled; no limits will be applied.
    pause
)
exit /b 0

:monitor_only
title PowerGuard - monitor only
echo   No target process: recording power, no limits applied.
echo   Web console: http://127.0.0.1:8080
echo.
"%EXE%" --web
echo.
pause
exit /b

:run
title PowerGuard - %GAME%
echo   Target  : %GAME%
echo   Web     : http://127.0.0.1:8080
echo   Type quit + Enter (or close this window) to restore settings.
echo.
"%EXE%" --profile usbc --web --process "%GAME%" --only-running
echo.
echo PowerGuard exited. Settings have been restored.
pause
