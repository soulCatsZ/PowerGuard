# Changelog

本项目采用 [Semantic Versioning](https://semver.org/lang/zh-CN/)。
格式参考 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)。

## [Unreleased]

### 修复

- 控制应用前先把原始值写入状态文件；写入失败则本次不应用，避免出现“已经改了
  限制、但恢复备份还没落盘”的窗口。状态文件替换改用 `MoveFileEx` 覆盖，
  减少先删后改名导致备份丢失的风险。
- GPU 功耗上限的原始值只采用 NVML 的“请求上限”；读不到时不再回退到含
  Dynamic Boost/EC 调整的 enforced 值，避免恢复时把用户设置改低。读不到原始值时
  放弃功耗上限杠杆，改为锁频/CPU 控制。
- `--diagnose-validate` 的写探测同样只在请求值可读时执行，避免探测本身改低上限；
  非管理员时的标题明确为“仅实测 CPU”。
- 暂停控制（网页点“恢复”）期间切换目标或游戏退出时，参数仍会更新为当前目标或
  `[default]`，再次点“立即应用”不会再套用上一个游戏的段；`--apply-always` 下
  游戏退出也会回到 `[default]`，而不是一直保留该游戏的上限。

### 新增

- `profiles.ini` 热重载：保存后 1~2 秒自动重新加载（带防抖，避开编辑器
  “先删后改名”的保存窗口）；游戏运行中会先恢复旧限制再套用新参数，
  不需要重启游戏或 PowerGuard。
- 分游戏参数校验：启动/重载时把未知键、非法值（带行号/段名）写入日志；
  新增 `--check-profiles` 只读校验命令，有问题时返回非 0。
- `profiles.ini` 支持按游戏关闭杠杆：`control_gpu` / `control_cpu`（以及反义写法
  `no_gpu` / `no_cpu`），与命令行 `--no-gpu` / `--no-cpu` 对齐；段内的值优先。
- Web 控制台新增“分游戏参数”面板：按当前监控目标读取、编辑、删除
  `profiles.ini` 中对应的段；保存前做与启动时相同的校验，错误带行号返回页面，
  失败不改文件；写入使用临时文件 + 原子替换，保存后由热重载自动套用。
- `tests/` 纯逻辑单元测试：`PowerChangeTracker`、`profiles.ini` 解析与校验、参数归一。
  `tests/run_tests.sh` 可重复运行，CI 已接入。
- `PowerGuard.bat` 一键启动：记住上次拖入/命令行传入的游戏进程，之后双击直接
  启动；没有目标时提示输入一次。新增 `--clear`，目标保存在
  `%LOCALAPPDATA%\PowerGuard\launcher_target.txt`。

### 变更

- 移除登录恢复任务辅助脚本 `PowerGuard-recovery-task.bat/.ps1`，打包脚本与文档
  同步去掉引用；`--restore` / `--restore-if-stale` 仍保留，可自行配置登录计划任务。
- Web 控制台界面刷新：进程列表改为可搜索、可用键盘操作的下拉选择器；增加
  入场/悬停/加载动效与骨架屏，并遵循 `prefers-reduced-motion`。

## [0.4.0] - 2026-11-01

### 新增

- `--budget <W>` 预算模式：不依赖电池信号，按 CPU+GPU 合计功率与预算做闭环，
  适用于台式机、电池不可用或不想依赖电池的机器。
- `--diagnose` / `--diagnose-validate`：能力探测（GPU 杠杆、CPU 写权限、
  电池信号、锁频范围），并给出可粘贴的 `profiles.ini` 起点。
- 百分比锁频参数 `gpu_clock_pct` / `min_gpu_clock_pct` / `auto_clock_pct`，
  按 GPU 最大核心频率换算，换机器不用改数值。
- `prefer = auto` / `--prefer-auto`：按近期 CPU/GPU 功率占比自动选择降档杠杆。
- `--version`。
- GitHub Actions 构建、标签发布与 `scripts/package.sh` 打包脚本。

### 修复

- `--process` 现在接受完整路径：把 `game.exe` 拖到 `PowerGuard.bat` 上时传入的是
  绝对路径，旧版会拿整条路径去匹配进程名，永远命中不了，控制因此完全不生效。
- 本地 Web 控制台增加两道校验：`Host` 必须是回环地址（防 DNS rebinding），
  POST 必须是 `application/json`（挡住跨站表单与 `no-cors fetch` 改参数）。
- GPU 锁频改为“上限”语义：下限取硬件 P0 最低频率，低负载时仍可降频省电；
  驱动拒绝带下限的写法时自动退回 `min=max`。
- 电池充放电方向改用电池类驱动 `IOCTL_BATTERY_QUERY_STATUS`，
  避免 `SYSTEM_BATTERY_STATE` 同时报告充电/放电导致的“幽灵缺口”误降档。
- `--diagnose` 在非管理员下不再把 GPU 杠杆误判为“不可写/不可用”，
  而是明确显示“需要管理员/未验证”。
- `PowerGuard.bat` 自动提权重启后没有窗口出现：`Start-Process -Verb RunAs`
  无法把带引号的参数传给 `.bat`（点了“是”也只是静默什么都不发生），
  不加引号又会在路径含空格时被截断。改为把目标进程名写进临时文件、
  只传一个无引号标记参数，提权后再读回。
- `PowerGuard-recovery-task.bat remove` 提权后没有任何效果（同一原因：
  `-ArgumentList '%*'` 带引号传给 `.bat` 会被丢弃），改为显式传单个 token。
- 三个 `.bat` 统一为纯 ASCII + CRLF 并精简注释；
  `PowerGuard-recovery-task.ps1` 改为 UTF-8 with BOM，避免 PowerShell 5.1
  按 ANSI/GBK 解读导致中文输出乱码。

### 变更

- `PowerGuard.bat` 不再默认监控 `Starfield.exe`：双击启动后提示输入游戏进程名
  （回车则只记录功率、不做控制），仍支持传入游戏名、拖拽 exe 和
  `--diagnose` 等选项透传。文件保持纯 ASCII + CRLF，避免代码页吞掉换行。
- 增加 `LICENSE`（MIT）、`CONTRIBUTING.md`、发布打包说明。

## [0.3.0] 及更早

早期版本完成了核心闭环：电池缺口能量积分、冷却/迟滞、GPU 功耗上限与锁频、
CPU 最大处理器状态、`profiles.ini` 分游戏参数、状态文件与登录恢复任务、
本地 Web 控制台、CSV 记录。仓库没有逐版本留存记录，这里不再拆分。
