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

- GPU 锁频改为“上限”语义：下限取硬件 P0 最低频率，低负载时仍可降频省电；
  驱动拒绝带下限的写法时自动退回 `min=max`。
- 电池充放电方向改用电池类驱动 `IOCTL_BATTERY_QUERY_STATUS`，
  避免 `SYSTEM_BATTERY_STATE` 同时报告充电/放电导致的“幽灵缺口”误降档。
- `--diagnose` 在非管理员下不再把 GPU 杠杆误判为“不可写/不可用”，
  而是明确显示“需要管理员/未验证”。

### 变更

- `PowerGuard.bat` 不再默认监控 `Starfield.exe`；无参数时打印用法，
  传入 `--diagnose` 等选项时直接透传。
- 增加 `LICENSE`（MIT）、`CONTRIBUTING.md`、发布打包说明。

## [0.3.0] 及更早

早期版本完成了核心闭环：电池缺口能量积分、冷却/迟滞、GPU 功耗上限与锁频、
CPU 最大处理器状态、`profiles.ini` 分游戏参数、状态文件与登录恢复任务、
本地 Web 控制台、CSV 记录。仓库没有逐版本留存记录，这里不再拆分。
