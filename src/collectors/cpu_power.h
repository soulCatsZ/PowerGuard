// ============================================================
// CpuPower —— Intel CPU 封装功率读取
// 数据源: Windows 性能计数器 \Energy Meter(RAPL_Package0_PKG)\Power (毫瓦)
// ============================================================
#pragma once

#include <windows.h>
#include <pdh.h>

namespace pg {

class CpuPower {
public:
    CpuPower() = default;
    ~CpuPower();

    CpuPower(const CpuPower&) = delete;
    CpuPower& operator=(const CpuPower&) = delete;

    bool open();       // 绑定计数器并验证可采样; 失败时已写入详细日志
    double read();     // 返回瓦特, 失败返回 -1 并写日志(10 秒节流)

private:
    void reportFail(const char* what, long code);

    PDH_HQUERY   query_ = nullptr;
    PDH_HCOUNTER counter_ = nullptr;
    ULONGLONG    lastCollect_ = 0;
    ULONGLONG    lastErrLog_ = 0;   // 失败日志节流: 10 秒最多记一条
    int          errCount_ = 0;
};

} // namespace pg
