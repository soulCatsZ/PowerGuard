// ============================================================
// CpuLimit —— 通过 Windows 电源计划限制 CPU 最大处理器状态
//   设置: SUB_PROCESSOR / PROCTHROTTLEMAX (AC)
//   99% = 关闭睿频; 80% = 更保守
//   只改当前活动方案, 不碰厂商 EC; 退出时恢复原值。
// ============================================================
#pragma once

#include <windows.h>

namespace pg {

// 只读取当前电源方案的 AC 最大处理器状态, 不保存/不写日志; 失败返回 -1
int readCpuMaxState();

class CpuLimit {
public:
    CpuLimit() = default;
    ~CpuLimit();

    CpuLimit(const CpuLimit&) = delete;
    CpuLimit& operator=(const CpuLimit&) = delete;

    // 读取当前活动电源方案和原始最大处理器状态
    bool open();

    // 释放活动方案句柄; 恢复后调用, 之后可再次 open()
    void close();

    // 写入 AC 最大处理器状态(5-100), 并读回校验
    bool setMaxState(int pct);

    // 当前 AC 最大处理器状态; 失败 -1
    int maxState() const;

    int originalMaxState() const { return originalPct_; }

    // 恢复到 open() 时保存的值
    bool restore();

private:
    GUID* scheme_ = nullptr;     // PowerGetActiveScheme 分配, LocalFree 释放
    int   originalPct_ = -1;
};

} // namespace pg
