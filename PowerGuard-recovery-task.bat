@echo off
setlocal EnableExtensions
cd /d "%~dp0"

rem 注册/删除“登录时恢复残留限制”的计划任务。
rem   PowerGuard-recovery-task.bat         安装
rem   PowerGuard-recovery-task.bat remove  删除

set "PS1=%~dp0PowerGuard-recovery-task.ps1"
if not exist "%PS1%" (
    echo [ERROR] %PS1% not found
    pause
    exit /b 1
)

fltmc >nul 2>&1
if not errorlevel 1 goto run

echo Requesting administrator privileges...
powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -ArgumentList '%*' -Verb RunAs"
exit /b 0

:run
set "ARG="
if /i "%~1"=="remove" set "ARG=-Remove"
powershell -NoProfile -ExecutionPolicy Bypass -File "%PS1%" %ARG%
echo.
pause
