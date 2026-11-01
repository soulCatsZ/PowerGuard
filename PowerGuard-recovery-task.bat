@echo off
setlocal EnableExtensions
cd /d "%~dp0"
rem
rem  Install or remove the logon task that runs --restore-if-stale, so a
rem  leftover limit from a crash or power-off is cleaned up at next logon.
rem    PowerGuard-recovery-task.bat         install
rem    PowerGuard-recovery-task.bat remove  delete
rem

set "PS1=%~dp0PowerGuard-recovery-task.ps1"
if not exist "%PS1%" (
    echo [ERROR] %PS1% not found
    pause
    exit /b 1
)

set "REMOVE="
if /i "%~1"=="remove" set "REMOVE=1"

fltmc >nul 2>&1
if not errorlevel 1 goto run

rem ArgumentList must be empty or one quote-free token: a quoted argument
rem is silently dropped when elevating a .bat.
echo Requesting administrator privileges...
if defined REMOVE (
    powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -ArgumentList 'remove' -Verb RunAs"
) else (
    powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
)
if errorlevel 1 (
    echo [ERROR] Elevation cancelled.
    pause
)
exit /b 0

:run
if defined REMOVE (
    powershell -NoProfile -ExecutionPolicy Bypass -File "%PS1%" -Remove
) else (
    powershell -NoProfile -ExecutionPolicy Bypass -File "%PS1%"
)
echo.
pause
