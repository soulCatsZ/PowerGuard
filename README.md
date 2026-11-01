# PowerGuard

> 用电池的"缺口"当传感器，让只吃 100W USB-C 的笔记本在游戏里**既不断电、也不白瞎性能**。

PowerGuard 是一个 Windows 上的实时功耗调控工具（C++17 / MinGW，单文件可执行）。
它每秒读取 CPU、GPU、电池的真实功率，把"插着电却还在放电"这件事本身当作负反馈，
自动、连续地调整 GPU 上限 / GPU 锁频 / CPU 最大状态，把整机峰值压到 100W 电源
能长期承受的范围以内。

---

## 一、项目背景

目标机器是一台 **i7-14650HX + RTX 5060 Laptop** 的游戏本，日常只靠一个
**100W 的 USB-C PD 适配器**供电（没有带原装大电源）。

这个组合的问题很具体：

- 100W 足够跑系统、写代码、看视频；
- 但大型游戏短时可以把整机拉到 **130W 以上**，超出 100W 的部分只能由电池补；
- 电池补得上的时候，电脑看起来一切正常——**但电量在往下掉**；
- 电池补不上的那一瞬间（尤其加载、进副本、爆炸特效），电压跌落，
  **整机直接黑屏断电**，不是蓝屏，是瞬断。

厂商的性能模式、NVIDIA 控制面板、ThrottleStop 这类工具都只能给一个**静态档位**：
要么一直很卡，要么关键时刻照样断。而游戏的负载是每秒都在变的，静态档位注定错配。

PowerGuard 的出发点就是：**别猜整机功耗，直接看电池还不还得上。**
电池在放电 = 输入功率不够，这是全机唯一一个不需要建模、不需要猜、永远真实的"超了"信号。

---

## 二、它解决的真实痛点

| 痛点 | 传统做法的困境 | PowerGuard 的做法 |
| --- | --- | --- |
| 瞬时功耗尖峰导致断电 | 全局功耗墙压得太低，平时性能白白浪费 | 只在**电池真的扛不住**时才降档，平时放性能 |
| 峰值是"一阵一阵"的，不是平均值 | 固定上限按平均功耗定，尖峰照样打穿 | 把缺口按 **Wh 累积**，瞬时尖峰被过滤，持续缺口才动作 |
| 每个游戏吃电的部位不同 | 一套参数打天下，一边过紧一边过松 | `profiles.ini` 按进程给**起点**和**优先杠杆**（CPU / GPU） |
| 同一局里菜单、城镇、战斗负载差很多 | 手动调根本跟不上 | 能量窗口 + 冷却 + 自动升降档，**局内自己找平衡点** |
| 笔记本 GPU 的 TGP 被 EC 锁死 | `nvmlDeviceSetPowerManagementLimit` 返回 `NOT_SUPPORTED`，直接放弃 | 自动退化为 **GPU 锁频**（`SetGpuLockedClocks`），照样能限功耗 |
| 崩溃/断电后限制残留，下次开机还锁着低频 | 只能手动去 NVIDIA 面板改回来 | 状态文件 + 登录恢复任务 + 单实例保护，**下次启动自动清理** |
| 调参全靠感觉，事后无法复盘 | 没有数据 | 每秒写 CSV（功率 / 电池 / 动作 / 命中的游戏），可回放分析 |
| 不想守着命令行 | 只能用 CLI | 内置本地 Web 控制台，实时曲线数值 + 参数面板 + 进程选择 |

---

## 三、核心思路：把电池当"电流表"

整套控制只有一个判据：

```
插着电  &&  电池在放电  &&  放电功率 ≥ 阈值
   →  输入功率不足  →  降档
```

剩下的都是围绕这句话做的工程化处理：

**① 缺口能量，而不是瞬时功率。**
每秒把放电功率积分成 Wh，缺口消失后按时间常数指数衰减。一个 0.5 秒的尖峰
累积不到多少能量，不会触发降档；连续几秒真的缺电，能量很快越过阈值。
阈值默认 `0.10 Wh`，相当于 10W 缺口持续约 36 秒、或 20W 持续约 18 秒——
刚好跨过"加载瞬间"这一档，又不会拖到电池掉电。

**② 冷却时间，防止过冲。**
两次降档至少间隔 15 秒。降完一档先观察电池反应，不会一口气从 1400MHz 砸到 800MHz。

**③ 双杠杆，按游戏选先后。**
- GPU 优先（默认）：先降 GPU（锁频或功耗上限），到底了再动 CPU；
- CPU 优先（`prefer = cpu`）：博德之门3 这类"缺口主要来自 CPU"的游戏，先降 CPU，GPU 兜底。

**④ 自动回升。**
缺口连续 `hold-up`（默认 300 秒）消失，就回升一档，回到菜单/过场时性能能还回来。

**⑤ 每 10 秒回写。**
厂商控制中心（性能模式、动态加速）会偷偷改功耗墙，程序定期校验并写回。

---

## 四、功能一览（以及它强在哪）

### 1. 闭环自适应控制 —— 通用底线，保证不关机

- **电池负反馈**：不依赖任何功耗模型，不需要知道游戏吃什么，只看结果。
- **缺口能量积分 + 指数衰减**：滤掉瞬时尖峰，只对"持续在补不上电"反应。
- **降档冷却**：每次只动一档，观察后再说，避免震荡。
- **迟滞（hold-down / hold-up）**：降档要缺口持续 5 秒，回升要稳定 300 秒，天然防抖。
- **双杠杆 + 可配置顺序**：GPU 功耗上限 → GPU 锁频 → CPU 最大处理器状态，按游戏选优先序。
- **杠杆跳变**：锁频值明显高于实际运行频率时（比如锁 1400 实际只有 1070），
  下一步直接跳到实际值以下，不做"锁了个寂寞"的无用功。
- **定时回写**：每 10 秒重新确认当前上限没被驱动/厂商软件改掉。

> 这一层和游戏无关，换什么游戏都生效，是"不会关机"的保险。

### 2. 分游戏参数 —— 不用每次都从 1400MHz 慢慢试错

`profiles.ini` 按进程名给每个游戏一份**起点**：

```ini
[bg3_dx11]          # 匹配 bg3_dx11.exe，大小写不敏感，可省略 .exe
gpu_clock = 900
cpu_max = 89
prefer = cpu        # 这个游戏 CPU 才是有效杠杆
```

- 只存"从哪开始"和"优先动哪个杠杆"，运行时的闭环仍会继续微调；
- 支持**多个候选进程名**（`--process "bg3.exe,bg3_dx11.exe"`），
  主进程和渲染进程不同名也不会漏；
- 用**实际命中的**那个名字查找参数段；
- 游戏启动时加载，退出后自动回到基底参数；
- 网页上改的参数会同步更新基底，不会被 ini 打回原样。

### 3. 场景内自适应 —— 同一局里的菜单 / 城镇 / 战斗

不假装能"预测"场景切换，而是让控制律自己跟上：

- **能量窗口**决定多快认定"真的缺电"；
- **冷却**决定降档节奏；
- **杠杆跳变**避免在高频锁频上做无用功；
- **自动回升**让负载回落的场景把性能还回来。

结果就是：战斗里压住，过场里放开，全程不需要人管。

### 4. 安全网 —— 崩溃、断电、强杀都不会留下烂摊子

- **状态文件**：应用前把原始值（GPU 上限、CPU 最大状态）写到
  `%LOCALAPPDATA%\PowerGuard\control_state.txt`，并记录占用它的 PID。
- **正常退出恢复**：`quit`、Ctrl+C、关窗口都会恢复原始值并删除状态文件。
- **注销/关机同步恢复**：控制台事件里先恢复再退出，不等系统强杀。
- **`--restore`**：一键恢复（`PowerGuard-restore.bat` 双击即可）。
- **`--restore-if-stale`**：只恢复"属于已退出进程"的状态文件，不会打扰正在跑的实例。
- **登录计划任务**（可选，`PowerGuard-recovery-task.bat`）：每次登录自动执行上面这一条，
  蓝屏/断电后下次开机限制自动清掉。
- **单实例互斥**（`Local\PowerGuard.SingleInstance`）：两个实例不会互相覆盖参数。
- **状态文件被占用时拒绝覆盖**：如果恢复失败，程序会**暂停控制**而不是把原始值冲掉，
  保证唯一的"原始值"副本不会丢。

### 5. 可观测性 —— 有数据才能调参

- **实时 CSV**：CPU / GPU / 电池功率、电量、AC 状态、目标是否运行、
  当前生效上限、锁频值、动作（`apply` / `step_down` / `step_up` / `restore` …），
  以及最后新增的 **`game` 列**（命中哪个进程），方便按游戏回放统计。
- **表头升级自动备份**：列变了旧 CSV 自动改名为 `.legacy`，不会新旧列混在一张表里。
- **分级日志**：`logs/powerguard_YYYY-MM-DD.log`，降档/回升/缺口/权限问题都有记录。
- **DRY-RUN 模式**：`--dry-run` 完整跑策略但不改系统，先看它想做什么。

### 6. 本地 Web 控制台 —— 浏览器里的实时面板

`--web` 启动一个只监听 `127.0.0.1` 的极简 HTTP 服务（仅依赖 Winsock，无第三方库），
打开 <http://127.0.0.1:8080> 就能看到：

- **实时功率**：CPU 封装功率、GPU 功率、电池净功率（放电为正 / 充电为负）、电量、AC 状态；
- **控制状态**：是否已应用、是否 DRY-RUN、GPU 走的是功耗上限还是锁频、
  当前生效的上限 / CPU 最大状态 / 锁定频率、目标进程与 PID、命中的参数段名；
- **参数面板**：上限、下限、步进、锁频、CPU 步进与是否参与、**降档顺序（CPU 优先）**、
  电池阈值、缺口能量阈值、降档冷却、升降档等待时间，改完点"保存并应用"；
- **进程列表**：自动列出当前进程，搜索 + 一键"设为目标"，等价于 `--process`；
- **两张开关键**："演练模式"（`--dry-run`）与"启动即生效"（`--apply-always`）；
- 灵动岛风格圆角玻璃拟态界面，深色/浅色主题跟随系统并记忆在 localStorage。

安全设计：只绑定回环地址，外部设备访问不到；接口仅本机可用。

---

## 五、快速开始

### 编译

```bash
./build.sh
```

（MinGW g++，自动收集 `src/**/*.cpp`，链接
`-lpdh -lpowrprof -lsetupapi -luuid -lws2_32 -static`。）

### 一键启动

双击 `PowerGuard.bat`，或把游戏 exe 拖上去：

```bat
PowerGuard.bat Cyberpunk2077.exe
```

脚本会**自动申请管理员权限**（GPU 锁频/功耗上限需要），默认监控
`Starfield.exe`（可在文件里改 `DEFAULT_GAME`），并以
`--profile usbc --web --process <游戏> --only-running` 运行。
启动后浏览器打开 <http://127.0.0.1:8080>。

### 命令行

```bash
./bin/PowerGuard.exe                                        # 只记录功率
./bin/PowerGuard.exe --process chrome.exe                    # 记录 + 监控进程
./bin/PowerGuard.exe --profile usbc --process "bg3.exe,bg3_dx11.exe" --only-running
./bin/PowerGuard.exe --profile usbc --dry-run --apply-always # 只看策略，不改系统
./bin/PowerGuard.exe --restore                               # 恢复原始设置
./bin/PowerGuard.exe --restore-if-stale                      # 只清理残留（登录任务用）
```

输入 `quit` 回车退出；Ctrl+C / 关窗口同样会恢复。

---

## 六、工作原理（参数级细节）

1. **应用时先备份**：读取 GPU 当前功耗上限、CPU 最大处理器状态，写入状态文件。
2. **每秒采样**电池：`ac_online=1 && discharging=1 && battery_w ≥ 阈值` 视为缺口，
   功率按 `Wh` 累积；缺口消失后按 `deficit-window` 指数衰减。
3. **满足全部条件才降档**：缺口持续 `hold-down` 秒 **且** 累计能量 ≥ `deficit-energy`
   **且** 距上次降档 ≥ `step-cooldown`：
   - 锁频模式降 `clock-step` MHz（若锁频值远高于实际频率，直接跳到实际值以下）；
   - 功耗上限模式降 `step` W；
   - 优先杠杆到底后切换到另一杠杆（CPU 可用 `--no-cpu-step` 完全排除）。
4. **缺口消失持续 `hold-up` 秒** → GPU 回升一档；GPU 回到上限后 CPU 按 `cpu-step` 回升。
5. **每 10 秒重读上限**，被驱动/厂商软件改掉时重新写入（锁频模式不做该校验）。
6. **退出/注销/关机**恢复原始值；异常情况靠状态文件 + 登录恢复任务兜底。

### 参数表

| 参数 | 默认 | 说明 |
| --- | --- | --- |
| `--profile usbc` | 关 | 启用限功耗闭环 |
| `--gpu-limit W` | 30 | GPU 初始/目标上限 |
| `--min-gpu W` | 15 | GPU 降档下限 |
| `--max-gpu W` | = gpu-limit | GPU 回升上限 |
| `--cpu-max PCT` | 99 | 最大处理器状态（99 关闭睿频） |
| `--cpu-min PCT` | 50 | CPU 降档下限 |
| `--cpu-step PCT` | 5 | CPU 每次降/升的百分点 |
| `--no-cpu-step` | 关 | 不把 CPU 当降档杠杆（只压 GPU） |
| `--prefer-cpu` | 关 | 降档先动 CPU、再动 GPU（默认 GPU 优先） |
| `--gpu-clock MHZ` | 0 | 显式锁定 GPU 核心频率（功耗上限写不进去时用） |
| `--min-gpu-clock MHZ` | 800 | 锁频降档下限 |
| `--clock-step MHZ` | 100 | 锁频每次升降幅度 |
| `--no-gpu-clock` | 关 | 关闭"功耗上限失败后自动锁频" |
| `--battery-threshold W` | 2.0 | 电池放电超过该值视为供电缺口 |
| `--deficit-energy Wh` | 0.10 | 缺口能量累计到该值才降档（滤掉瞬时尖峰） |
| `--deficit-window S` | 120 | 缺口消失后能量衰减的时间常数 |
| `--step-cooldown S` | 15 | 两次降档之间的最短间隔 |
| `--step W` | 5 | 每次降/升的 GPU 瓦数 |
| `--hold-down S` | 5 | 缺口持续多久才降档 |
| `--hold-up S` | 300 | 稳定多久才回升 |
| `--profiles FILE` | profiles.ini | 分游戏参数文件 |
| `--no-game-profiles` | 关 | 不加载分游戏参数 |
| `--apply-always` | 关 | 不等目标进程，启动即应用 |
| `--dry-run` | 关 | 只记录不修改系统 |
| `--no-gpu` / `--no-cpu` | 关 | 只控制其中一项 |
| `--web` / `--web-port N` | 关 / 8080 | 启动本地 Web 控制台 |
| `--restore` | — | 恢复上次保存的原始设置后退出 |
| `--restore-if-stale` | — | 仅当状态文件属于已退出进程时恢复 |

---

## 七、分游戏参数（profiles.ini）

不同游戏的负载形态差异很大：博德之门3 的缺口主要靠 CPU 降档才压得住；
星空则是 GPU 长期稳定在 26W 左右、几乎不需要降档。同一个静态方案必然一边过紧一边过松。

`profiles.ini`（默认当前目录，`--profiles` 可指定）格式：

```ini
[default]
# 所有目标通用，可留空

[bg3_dx11]        # 匹配 bg3_dx11.exe
gpu_clock = 900
cpu_max = 89
prefer = cpu      # 降档先动 CPU

[starfield]
gpu_clock = 1400
cpu_max = 99
prefer = gpu
```

可用键（与命令行同名）：`gpu_limit`、`min_gpu`、`max_gpu`、`cpu_max`、`cpu_min`、
`cpu_step`、`cpu_power_step`、`prefer`、`gpu_clock`、`min_gpu_clock`、`clock_step`、
`auto_clock`、`gpu_clock_auto`、`battery_threshold`、`deficit_energy`、`deficit_window`、
`step_cooldown`、`step`、`hold_down`、`hold_up`，以及 `no_gpu_clock` / `no_cpu_step`。

要点：

- 段名 = 进程名，大小写不敏感，`.exe` 可省略；`[default]` 先应用，游戏段再覆盖。
- 有多个候选进程名时，用**实际命中**的那个找段。
- 参数改动即时读到（下次游戏启动或网页切换目标后重新进入时加载）。
- 这些值只是起点，闭环仍会继续升降档。

---

## 八、权限与限制（提前说清楚）

- **GPU 功耗上限**：`nvmlDeviceSetPowerManagementLimit`，必须管理员。笔记本的 TGP
  通常由厂商 EC 托管，本机实测返回 `3 = NOT_SUPPORTED`，程序会**自动退化为锁频**
  （`nvmlDeviceSetGpuLockedClocks`，从 1400MHz 起步，退出时解除）。
- **锁频同样需要管理员**；非管理员启动会明确提示并退化为只做 CPU 控制。
- **CPU 上限**：改当前电源计划的"最大处理器状态"（AC），普通用户即可，退出时恢复。
  它限制的是频率上限，不是电压/功耗墙。
- 只支持 **NVIDIA GPU**（NVML）。
- 厂商控制中心可能反复改写上限，程序每 10 秒回写一次。
- 限功耗不是降压超频，不改硬件，重启后设置自然消失。
- 场景变化是**被动响应**的，不做帧级预测；`profiles.ini` 是起点，不会自动写回学习结果。

---

## 九、数据与 CSV

| 列 | 含义 |
| --- | --- |
| `time` | 采样时间 |
| `cpu_w` / `gpu_w` | CPU / GPU 功率（W） |
| `battery_w` | 电池功率：**正 = 放电（供电缺口）**，负 = 充电（供电余量），0 = 不充不放 |
| `battery_pct` | 电量百分比，-1 = 未知 |
| `ac_online` | 1 = 插电，0 = 电池供电，-1 = 电池不可用 |
| `discharging` | 1 = 电池正在放电，0 = 否，-1 = 电池不可用 |
| `running` | 1 = 目标进程运行中，0 = 未运行，-1 = 未配置进程监控 |
| `game` | 命中的目标进程名（不含 `.exe`），`-` = 未命中 |

启用 `--profile usbc` 后追加：

| 列 | 含义 |
| --- | --- |
| `gpu_limit_w` | 当前生效的 GPU 上限（-1 = 读不到） |
| `cpu_max_pct` | 当前生效的最大处理器状态 |
| `gpu_clock_mhz` | 锁频模式下锁定的核心频率（-1 = 未使用锁频） |
| `action` | `apply` / `restore` / `step_down` / `step_up` / `reapply` / `-` |

表头变化时旧文件自动改名为 `power_YYYY-MM-DD.csv.legacy`，避免新旧列混写。

### 数据来源

- **CPU**：`\Energy Meter(RAPL_Package0_PKG)\Power`（Intel RAPL，单位毫瓦）。
- **GPU**：`nvmlDeviceGetPowerUsage`（NVML，单位毫瓦）。
- **电池/供电**：优先用 `IOCTL_BATTERY_QUERY_STATUS`（电池类驱动，与 WMI 的
  `BatteryStatus` 同源）读取充放电方向与功率；`CallNtPowerInformation(SystemBatteryState)`
  读取电量/容量，并在电池类驱动不可用时兜底。本机实测 `SYSTEM_BATTERY_STATE` 会
  同时返回 `Charging=1` 和 `Discharging=1`，只看它会把充电误判成缺口，因此以电池类
  驱动为准。

---

## 十、目录结构

```text
PowerGuard/
├── build.sh                        # 编译脚本
├── PowerGuard.bat                  # 快捷启动（自动提权，监控默认游戏）
├── PowerGuard-restore.bat          # 快捷恢复（崩溃后还原设置）
├── PowerGuard-recovery-task.bat    # 注册/删除“登录时清理残留限制”的计划任务
├── PowerGuard-recovery-task.ps1
├── profiles.ini                    # 分游戏参数（可选）
├── README.md
├── bin/                            # 编译产物
│   └── PowerGuard.exe
├── logs/                           # 运行日志与 CSV（程序自动创建）
│   ├── powerguard_YYYY-MM-DD.log
│   └── power_YYYY-MM-DD.csv
└── src/
    ├── main.cpp                    # 入口
    ├── app/
    │   └── powerguard_app.*        # 主流程：事件循环 / CSV / 控制闭环 / 命令分发
    ├── collectors/
    │   ├── gpu_power.*             # NVIDIA NVML
    │   ├── cpu_power.*             # PDH RAPL
    │   └── battery_power.*         # 电池类驱动 IOCTL + CallNtPowerInformation
    ├── control/
    │   ├── cpu_limit.*             # 最大处理器状态
    │   ├── state_file.*            # 原始值备份与恢复
    │   ├── game_profile.*          # profiles.ini 解析与应用
    │   └── profile.h               # 限功耗参数
    ├── monitor/
    │   └── process_monitor.*       # 目标进程监控（支持多个名字）
    ├── web/
    │   └── web_server.*            # 本地 Web 控制台
    └── common/
        ├── logger.h                # 日志
        └── power_tracker.h         # 功率写入去抖
```

---

## 十一、验证建议

1. `--dry-run --apply-always`：确认参数解析、原始值读取正常，日志里能看到它"想做什么"。
2. 普通权限 `--apply-always`：CPU 设为 99%，退出后恢复 100%。
3. 管理员权限跑同一条：确认 GPU 走的是上限还是锁频，用 `nvidia-smi -q -d POWER` 对照。
4. 游戏内看 CSV：`battery_w` 应逐步降到阈值附近，`action` 列出现 `step_down` / `step_up`。
5. 任务管理器结束进程后重启：确认状态文件被识别、残留限制被恢复。
