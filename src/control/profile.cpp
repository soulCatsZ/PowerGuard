#include "control/profile.h"

namespace pg {

void normalizeProfile(ControlProfile& p) {
    // 参数校验/归一下
    if (p.gpuLimitW < 5)   p.gpuLimitW = 5;
    if (p.gpuLimitW > 250) p.gpuLimitW = 250;
    if (p.minGpuW < 5)     p.minGpuW = 5;
    if (p.minGpuW > p.gpuLimitW) p.minGpuW = p.gpuLimitW;
    if (p.maxGpuW <= 0)    p.maxGpuW = p.gpuLimitW;
    if (p.maxGpuW < p.gpuLimitW) p.maxGpuW = p.gpuLimitW;
    if (p.cpuMaxPct < 5)   p.cpuMaxPct = 5;
    if (p.cpuMaxPct > 100) p.cpuMaxPct = 100;
    if (p.cpuMinPct < 5)   p.cpuMinPct = 5;
    if (p.cpuMinPct > p.cpuMaxPct) p.cpuMinPct = p.cpuMaxPct;
    if (p.cpuStepPct < 1)  p.cpuStepPct = 1;
    if (p.cpuStepPct > 50) p.cpuStepPct = 50;
    if (p.inputBudgetW < 0)   p.inputBudgetW = 0;
    if (p.inputBudgetW > 1000) p.inputBudgetW = 1000;
    if (p.batteryThresholdW < 0) p.batteryThresholdW = 0;
    if (p.deficitEnergyWh < 0) p.deficitEnergyWh = 0;
    if (p.deficitWindowSec < 5) p.deficitWindowSec = 5;
    if (p.stepCooldownSec < 0) p.stepCooldownSec = 0;
    if (p.stepW < 1)       p.stepW = 1;
    if (p.holdDownSec < 1) p.holdDownSec = 1;
    if (p.holdUpSec < 1)   p.holdUpSec = 1;
    if (p.gpuClockMhz < 0) p.gpuClockMhz = 0;
    if (p.minGpuClockMhz < 100) p.minGpuClockMhz = 100;
    if (p.clockStepMhz < 10)    p.clockStepMhz = 10;
    if (p.autoClockMhz < p.minGpuClockMhz)
        p.autoClockMhz = p.minGpuClockMhz;
    if (p.gpuClockMhz > 0 && p.minGpuClockMhz > p.gpuClockMhz)
        p.minGpuClockMhz = p.gpuClockMhz;

    // 百分比时基: 0-100; 下限不能高于目标
    if (p.gpuClockPct < 0)     p.gpuClockPct = 0;
    if (p.gpuClockPct > 100)   p.gpuClockPct = 100;
    if (p.minGpuClockPct < 0)  p.minGpuClockPct = 0;
    if (p.minGpuClockPct > 100) p.minGpuClockPct = 100;
    if (p.autoClockPct < 0)    p.autoClockPct = 0;
    if (p.autoClockPct > 100)  p.autoClockPct = 100;
    if (p.gpuClockPct > 0 && p.minGpuClockPct > p.gpuClockPct)
        p.minGpuClockPct = p.gpuClockPct;
}

} // namespace pg
