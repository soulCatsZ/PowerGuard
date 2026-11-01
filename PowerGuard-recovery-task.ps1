# ============================================================
# 注册/删除登录恢复任务
#   登录时以最高权限运行:
#     PowerGuard.exe --restore-if-stale
#   只有状态文件属于已经退出的进程时才恢复，不会打扰
#   正在运行的 PowerGuard 实例。
# 用法:
#   powershell -ExecutionPolicy Bypass -File PowerGuard-recovery-task.ps1
#   powershell -ExecutionPolicy Bypass -File PowerGuard-recovery-task.ps1 -Remove
# 一般直接双击 PowerGuard-recovery-task.bat 即可（会自动提权）。
# ============================================================
param([switch]$Remove)

$ErrorActionPreference = 'Stop'
$taskName = 'PowerGuard-Restore'

if ($Remove) {
    Unregister-ScheduledTask -TaskName $taskName -Confirm:$false -ErrorAction SilentlyContinue
    Write-Host "已删除计划任务: $taskName"
    exit 0
}

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$exe  = Join-Path $root 'bin\PowerGuard.exe'
if (-not (Test-Path $exe)) {
    Write-Host "[ERROR] 找不到 $exe，请先运行 build.sh" -ForegroundColor Red
    exit 1
}

$user = "$env:USERDOMAIN\$env:USERNAME"
$action = New-ScheduledTaskAction -Execute $exe -Argument '--restore-if-stale' -WorkingDirectory $root
$trigger = New-ScheduledTaskTrigger -AtLogOn -User $user
$principal = New-ScheduledTaskPrincipal -UserId $user -LogonType Interactive -RunLevel Highest

Register-ScheduledTask -TaskName $taskName -Action $action -Trigger $trigger `
    -Principal $principal -Force | Out-Null

Write-Host "已注册登录任务: $taskName"
Write-Host "  触发: 用户登录 ($user)"
Write-Host "  执行: $exe --restore-if-stale"
Write-Host "  登录时可能闪一下控制台窗口，属正常现象。"
Write-Host "  删除: 运行 PowerGuard-recovery-task.bat remove"
