# PowerGuard

[![build](https://github.com/soulCatsZ/PowerGuard/actions/workflows/build.yml/badge.svg)](https://github.com/soulCatsZ/PowerGuard/actions/workflows/build.yml)
[![license: MIT](https://img.shields.io/badge/license-MIT-green.svg)](LICENSE)
![platform](https://img.shields.io/badge/platform-Windows%2010%20%7C%2011-blue)

> 用电池的"缺口"当传感器，让只吃 100W USB-C 的笔记本在游戏里**既不断电、也不白瞎性能**。

<details>
<summary><b>English overview</b></summary>

PowerGuard is a Windows tool (C++17, single executable) that keeps a gaming
laptop alive on a 100W USB-C PD charger. It treats "AC connected but the battery
is discharging" as a real-time power-deficit sensor and continuously adjusts the
GPU power limit / locked clocks / CPU maximum processor state, instead of
applying one static cap. It also supports per-game profiles, percentage-based
clock targets, capability probing (`--diagnose`), a no-battery budget mode
(`--budget`) and a local web console.

GPU control currently supports **NVIDIA only** (via NVML) and requires
administrator rights. CPU control works without elevation.

**Disclaimer:** PowerGuard writes GPU power limits, GPU clocks and CPU power
settings. Use at your own risk; no warranty. See [LICENSE](LICENSE).

</details>

> **安全提示**：本工具会修改 GPU 功耗上限/核心频率和 CPU 最大处理器状态。
> 请只在自己的设备上使用，并先用 `--diagnose` / `--dry-run` 确认行为；
> 作者不对硬件损坏、数据丢失、保修失效或断电事故负责。详见 [LICENSE](LICENSE)。

---

## 为什么需要它

目标场景：i7-14650HX + RTX 5060 Laptop，只靠一个 **100W USB-C PD 适配器**供电。

100W 足够跑系统和办公，但大型游戏短时可把整机拉到 130W 以上，超出部分由电池补。
电池补得上时一切正常、只是电量在掉；补不上的那一瞬间电压跌落，**整机瞬断黑屏**。

厂商性能模式、NVIDIA 控制面板、ThrottleStop 都只能给**静态档位**——要么一直卡，
要么关键时刻照样断。而游戏负载每秒都在变，静态档位注定错配。

PowerGuard 的判据只有一句话：

```
插着电  &&  电池在放电  &&  放电功率 ≥ 阈值   →  输入功率不足  →  降档
```

电池放电是全机唯一不需要建模、不需要猜、永远真实的"超了"信号。

---

## 核心机制

1. **缺口能量积分**：每秒把放电功率积分成 Wh，缺口消失后按时间常数指数衰减。
   0.5 秒的尖峰累积不到多少能量；持续缺电才会越过阈值（默认 `0.10 Wh`，
   约等于 10W 缺口持续 36 秒）。
2. **降档冷却**：两次降档至少间隔 15 秒，降一档先观察电池反应，避免一口气砸到底。
3. **双杠杆**：GPU 优先（默认）/ CPU 优先（`prefer = cpu`）/
   自动（`prefer = auto`，按近 10 秒 CPU/GPU 功率占比选顺序）。
4. **自动回升**：缺口消失持续 `hold-up`（默认 300 秒）就回升一档，过场/菜单里性能还回来。
5. **每 10 秒回写**：厂商控制中心会偷偷改功耗墙，程序定期校验并写回。

杠杆本身：`GPU 功耗上限 → GPU 锁频 → CPU 最大处理器状态`。
笔记本 TGP 常被 EC 托管、`nvmlDeviceSetPowerManagementLimit` 返回 `NOT_SUPPORTED`，
程序自动退化为锁频（`SetGpuLockedClocks`）；锁频只设**上限**、下限取 P0 最低频率，
所以低负载时驱动仍可降频省电。

**预算模式**：没有电池或用不了电池信号时，`--budget 80` 改看 CPU+GPU 合计功率
与预算的差值，迟滞/冷却/升降档逻辑完全复用。

---

## 快速开始

前置：Windows 10/11 x64 + MinGW-w64 g++（MSYS2 的 `mingw-w64-x86_64-gcc` 即可）。
不想装编译器就直接下 [Releases](https://github.com/soulCatsZ/PowerGuard/releases)
里的 `PowerGuard-<版本>-win64.zip`，解压即用。

> **首次运行被 SmartScreen 拦下是正常的**：exe 没有代码签名，而它又要申请管理员权限
> 去改显卡频率。选"更多信息 → 仍要运行"即可。源码和 CI 构建流程都在仓库里，
> 不放心可以自己 `./build.sh` 编译。

```bash
./build.sh                                  # 自动收集 src/**/*.cpp，产物是单文件 exe
./bin/PowerGuard.exe --diagnose             # 换机器先探测（只读，不需要管理员）
PowerGuard.bat                              # 双击后直接输入游戏进程名
PowerGuard.bat Cyberpunk2077.exe            # 也可以从命令行/拖拽传入
```

不带参数启动时会提示输入进程名（`.exe` 可省略，进程还没启动也没关系，它跑起来才
开始控制；直接回车则只记录功率、不做控制）。随后自动提权并以
`--profile usbc --web --process <游戏> --only-running` 运行，
启动后浏览器打开 <http://127.0.0.1:8080>。

### 命令行

```bash
./bin/PowerGuard.exe                                        # 只记录功率
./bin/PowerGuard.exe --process "bg3.exe,bg3_dx11.exe" --only-running
./bin/PowerGuard.exe --profile usbc --dry-run --apply-always # 只看策略，不改系统
./bin/PowerGuard.exe --profile usbc --budget 80 --process bg3.exe  # 无电池预算模式
./bin/PowerGuard.exe --restore                               # 恢复原始设置
./bin/PowerGuard.exe --restore-if-stale                      # 只清理残留（登录任务用）
./bin/PowerGuard.exe --diagnose-validate                     # 探测 + 写权限实测（管理员）
./bin/PowerGuard.exe --version
```

输入 `quit` 回车退出；Ctrl+C / 关窗口同样会恢复原始值。

---

## 主要参数

| 参数 | 默认 | 说明 |
| --- | --- | --- |
| `--profile usbc` | 关 | 启用限功耗闭环 |
| `--gpu-limit W` / `--min-gpu W` / `--max-gpu W` | 30 / 15 / =gpu-limit | GPU 上限 / 降档下限 / 回升上限 |
| `--gpu-clock MHZ` / `--gpu-clock-pct PCT` | 0 | 显式锁频，或按最大核心频率的百分比 |
| `--min-gpu-clock` / `--clock-step` | 800 / 100 | 锁频下限 / 每次升降幅度 |
| `--cpu-max PCT` / `--cpu-step PCT` | 99 / 5 | 最大处理器状态（99 关睿频）/ 每档步进 |
| `--prefer-cpu` / `--prefer-auto` | 关 | 降档顺序：CPU 优先 / 按功率占比自动 |
| `--no-cpu-step` / `--no-gpu-clock` | 关 | 不把 CPU 当杠杆 / 不自动锁频 |
| `--budget W` | 0（关） | 预算模式：CPU+GPU 合计超过该值就降档 |
| `--battery-threshold W` | 2.0 | 放电（或超预算）超过该值视为缺口 |
| `--deficit-energy Wh` / `--deficit-window S` | 0.10 / 120 | 缺口能量阈值 / 衰减时间常数 |
| `--step-cooldown S` / `--step W` | 15 / 5 | 降档冷却 / GPU 瓦数步进 |
| `--hold-down S` / `--hold-up S` | 5 / 300 | 缺口持续多久降档 / 稳定多久回升 |
| `--dry-run` / `--apply-always` | 关 | 只记录不改系统 / 不等进程启动即应用 |
| `--web` / `--web-port N` | 关 / 8080 | 本地 Web 控制台（仅监听 `127.0.0.1`） |

完整列表见 `--help`。

---

## 分游戏参数（profiles.ini）

不同游戏吃电的部位不同：博德之门3 的缺口主要靠 CPU 降档才压得住，星空则 GPU
长期稳定、几乎不需要降档。`profiles.ini` 按进程名给每个游戏一份**起点**：

```ini
[bg3_dx11]          # 匹配 bg3_dx11.exe，大小写不敏感，.exe 可省略
gpu_clock = 900
cpu_max = 89
prefer = cpu        # cpu / gpu / auto

[starfield]
gpu_clock_pct = 55  # 跨机器推荐用百分比，优先于绝对 MHz
prefer = gpu
```

- 段名 = 进程名；`[default]` 先应用，游戏段再覆盖。多个候选进程名时用**实际命中**的那个找段。
- 这些值只是起点，运行时的闭环仍会继续升降档；网页上改的参数同步写回基底。
- 百分比键 `gpu_clock_pct` / `min_gpu_clock_pct` / `auto_clock_pct` 按 NVML 报告的
  最大核心频率换算（10MHz 对齐），换显卡不用重调数值。
- 无电池时用 `budget = 80` 取代电池缺口信号。

---

## 无人值守的安全网

- **状态文件**：应用前把原始值写到 `%LOCALAPPDATA%\PowerGuard\control_state.txt` 并记录 PID。
- **正常/异常退出恢复**：`quit`、Ctrl+C、关窗口、注销、关机事件都会先恢复再退出。
- **`--restore` / `PowerGuard-restore.bat`**：一键恢复。
- **`--restore-if-stale` + 登录计划任务**（`PowerGuard-recovery-task.bat`）：
  蓝屏/断电后下次登录自动清掉残留限制。
- **单实例互斥**；状态文件被占用时**暂停控制而非覆盖**，保证唯一原始值副本不丢。

---

## 数据与日志

- 每秒写 CSV：CPU/GPU/电池功率、电量、AC 状态、当前生效上限、锁频值、动作、
  命中的游戏名。表头变化时旧文件自动改名 `.legacy`。
- 分级日志 `logs/powerguard_YYYY-MM-DD.log`。
- **隐私**：只写本机 `logs/`，Web 控制台只绑定回环地址，无遥测、不上传。

数据来源：CPU 用 PDH RAPL（`\Energy Meter(RAPL_Package0_PKG)\Power`）；
GPU 用 `nvmlDeviceGetPowerUsage`；电池方向优先用 `IOCTL_BATTERY_QUERY_STATUS`
（`SYSTEM_BATTERY_STATE` 在本机会同时报 `Charging=1` 和 `Discharging=1`，不能单独依赖）。

---

## 权限与限制

- **GPU 功耗上限 / 锁频**需要管理员；非管理员启动会明确提示并退化为只做 CPU 控制。
- **CPU 上限**改当前电源计划的"最大处理器状态"（AC），普通用户即可，限制的是频率上限。
- 只支持 **NVIDIA GPU**（NVML）。AMD/Intel 会在 `--diagnose` 中明确显示"GPU 不可用"，
  此时仍可用 CPU 杠杆或预算模式。
- 不同机器 GPU 能力差异大：先 `--diagnose` / `--diagnose-validate` 确认哪个杠杆可写。
- 限功耗不改硬件、不是降压超频，重启后设置自然消失。
- 场景变化是**被动响应**，不做帧级预测；`profiles.ini` 只存起点，不写回学习结果。

---

## 目录结构

```text
PowerGuard/
├── build.sh / scripts/package.sh   # 编译 / 发布打包
├── .github/workflows/build.yml     # CI 构建；打 v* 标签时自动发布
├── PowerGuard.bat                  # 快捷启动（双击输入游戏名，自动提权）
├── PowerGuard-restore.bat          # 快捷恢复
├── PowerGuard-recovery-task.*      # 登录时清理残留限制的计划任务
├── profiles.ini                    # 分游戏参数
├── bin/                            # 编译产物
├── logs/                           # 日志与 CSV（程序自动创建）
└── src/
    ├── app/        主流程：事件循环 / CSV / 控制闭环
    ├── collectors/ GPU(NVML) / CPU(PDH RAPL) / 电池
    ├── control/    CPU 上限、状态文件、profiles.ini、参数
    ├── diag/       --diagnose 能力探测
    ├── monitor/    目标进程监控
    ├── web/        本地 Web 控制台
    └── common/     日志 / 去抖 / 版本号
```

---

## 验证建议

1. `--diagnose` 确认杠杆与电池信号，需要写权限时用管理员跑 `--diagnose-validate`。
2. `--dry-run --apply-always` 确认参数解析与原始值读取正常。
3. 普通权限 `--apply-always`：CPU 设 99%，退出后恢复 100%。
4. 管理员跑同一条：用 `nvidia-smi -q -d POWER` 对照 GPU 走的是上限还是锁频。
5. 游戏内看 CSV：`battery_w` 应逐步降到阈值附近，`action` 出现 `step_down` / `step_up`。
6. 任务管理器强杀后重启：确认残留限制被识别并恢复。

---

## 参与贡献与许可

- 构建、测试和提交约定见 [CONTRIBUTING.md](CONTRIBUTING.md)，版本变更见 [CHANGELOG.md](CHANGELOG.md)。
- 使用 [MIT 许可证](LICENSE)。
- 反馈问题时附上 `--diagnose` 输出（可去掉个人路径）、复现步骤和期望行为；
  涉及硬件写入的 issue 请说明是否以管理员运行、是否有电池信号。
