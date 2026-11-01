// ============================================================
// ControlProfile —— USB-C 100W 限功耗配置
//   默认: GPU 30W (可降到 15W), CPU 最大状态 99%,
//         电池缺口累计 >= 0.1Wh 且持续 5s 降一档,
//         稳定 5min 回升一档。
// ============================================================
#pragma once

namespace pg {

struct ControlProfile {
    bool enabled = false;        // --profile usbc
    bool dryRun = false;         // --dry-run: 只记录, 不改系统
    bool applyAlways = false;    // --apply-always: 不等游戏启动, 立即应用
    bool controlGpu = true;      // --no-gpu 关闭
    bool controlCpu = true;      // --no-cpu 关闭

    int  gpuLimitW = 30;         // 初始/目标上限
    int  minGpuW = 15;           // 降档下限
    int  maxGpuW = 0;            // 回升上限, 0 = 与 gpuLimitW 相同
    int  cpuMaxPct = 99;         // 最大处理器状态 (99 = 关睿频)
    int  cpuMinPct = 50;         // CPU 降档下限
    int  cpuStepPct = 5;         // CPU 每次降/升的百分点
    bool cpuPowerStep = true;    // CPU 是否作为降档杠杆; false = 只压 GPU
    bool preferCpu = false;      // 降档先动 CPU (适合 CPU 才是有效杠杆的游戏)
    bool preferAuto = false;     // 按近期 CPU/GPU 功率占比自动选先动哪个杠杆

    // 锁频: 功耗上限被厂商 EC 锁死时, 用锁定核心频率代替
    int  gpuClockMhz = 0;        // >0 = 显式启用锁频并以此为初始/上限频率
    int  minGpuClockMhz = 800;   // 降档下限
    int  clockStepMhz = 100;     // 每次升降的 MHz
    int  autoClockMhz = 1400;    // 功耗上限写入失败时自动锁频的起始值
    bool gpuClockAuto = true;    // --no-gpu-clock 可关闭自动降级

    // 百分比时基: 按 NVML 报告的最大核心频率换算, 换机器不用改数值。
    // 只要 >0 就优先于对应的绝对 MHz 值 (运行时由 GPU 实际最大频率换算)。
    int  gpuClockPct = 0;        // 锁频目标 (如 60 = 最大频率的 60%)
    int  minGpuClockPct = 0;     // 锁频下限
    int  autoClockPct = 0;       // 自动锁频起始值

    // 电池负反馈
    double batteryThresholdW = 2.0;  // 电池放电超过该值视为供电缺口
    double deficitEnergyWh = 0.10;   // 缺口累计到多少 Wh 才真的降档
    int  deficitWindowSec = 120;     // 缺口能量的衰减时间常数(秒)
    int  stepCooldownSec = 15;       // 两次降档之间的最短间隔(秒)
    int  stepW = 5;                  // 每次降/升的 GPU 瓦数
    int  holdDownSec = 5;            // 缺口持续多久才降档
    int  holdUpSec = 300;            // 稳定多久才回升
};

} // namespace pg
