# Changelog

本项目采用 [Semantic Versioning](https://semver.org/lang/zh-CN/)。
格式参考 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)。

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

### 变更

- `PowerGuard.bat` 不再默认监控 `Starfield.exe`：双击启动后提示输入游戏进程名
  （回车则只记录功率、不做控制），仍支持传入游戏名、拖拽 exe 和
  `--diagnose` 等选项透传。文件保持纯 ASCII + CRLF，避免代码页吞掉换行。
- 增加 `LICENSE`（MIT）、`CONTRIBUTING.md`、发布打包说明。

## [0.3.0] 及更早

早期版本完成了核心闭环：电池缺口能量积分、冷却/迟滞、GPU 功耗上限与锁频、
CPU 最大处理器状态、`profiles.ini` 分游戏参数、状态文件与登录恢复任务、
本地 Web 控制台、CSV 记录。仓库没有逐版本留存记录，这里不再拆分。
