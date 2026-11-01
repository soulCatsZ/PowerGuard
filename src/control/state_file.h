// ============================================================
// 控制状态文件 —— 记录原始 GPU/CPU 设置, 用于异常退出后的恢复
//   位置: %LOCALAPPDATA%\PowerGuard\control_state.txt
//   正常退出会删除; 崩溃/被杀后保留, 下次启动或 --restore 恢复。
// ============================================================
#pragma once

#include <filesystem>
#include <string>

namespace pg {

struct ControlState {
    unsigned long pid = 0;
    int gpuOriginalW = -1;
    int cpuOriginalPct = -1;
    int gpuAppliedW = -1;
    int cpuAppliedPct = -1;
    int gpuClockMhz = -1;        // 已锁定的核心频率, -1 = 未使用锁频
    std::string timestamp;
};

std::filesystem::path controlStatePath();

bool saveControlState(const ControlState& st);
bool loadControlState(ControlState& st);
bool removeControlState();

// 进程是否仍存活; 无权限查询时按"存活"处理, 避免误恢复
bool pidAlive(unsigned long pid);

} // namespace pg
