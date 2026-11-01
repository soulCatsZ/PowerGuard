// ============================================================
// BatteryPower —— 电池 / 供电状态读取
// 数据源:
//   1. 电池类驱动 (IOCTL_BATTERY_QUERY_STATUS): 充放电方向/功率
//      与 WMI 的 BatteryStatus 同源, 在部分笔记本上这是唯一可靠的
//      方向来源。本机实测 CallNtPowerInformation 会同时返回
//      Charging=1 && Discharging=1, 导致"充电"被误判成"缺口"。
//   2. CallNtPowerInformation(SystemBatteryState)（powrprof）:
//      电量/容量, 以及 IOCTL 不可用时的兜底。
//   - AcOnLine / Charging / Discharging
//   - Rate: 充放电功率(mW)
//   - RemainingCapacity / MaxCapacity
//
// 用途: 判断 USB-C 100W 供电是否足够。
//       插着电却在放电 => 输入功率不足，电池正在补缺口。
// ============================================================
#pragma once

#include <windows.h>

namespace pg {

// 设计容量 / 满充容量 / 循环次数 (来自电池类驱动, --diagnose 用)
struct BatteryDesignInfo {
    bool   valid = false;
    double designWh = -1.0;
    double fullWh = -1.0;
    int    cycles = -1;
};

struct BatterySample {
    bool   valid = false;
    bool   acOnline = false;
    bool   batteryPresent = false;
    bool   charging = false;
    bool   discharging = false;
    double rateW = 0.0;        // 正 = 放电(缺口), 负 = 充电(余量)
    double remainingWh = 0.0;
    double maxWh = 0.0;
    int    percent = -1;       // 0-100, -1 = 未知
};

class BatteryPower {
public:
    BatteryPower() = default;
    ~BatteryPower();

    BatteryPower(const BatteryPower&) = delete;
    BatteryPower& operator=(const BatteryPower&) = delete;

    // 验证电池 API 可用; 失败写日志并返回 false（程序可继续只采 CPU/GPU）
    bool open();

    // 读取一次; 失败返回 valid=false 的结构（10 秒节流写日志）
    BatterySample read();

    // 方向信号是否来自电池类驱动 (false = 回退到 CallNtPowerInformation)
    bool usesDeviceDriver() const { return dev_ != INVALID_HANDLE_VALUE; }

    // 读取设计容量/满充容量/循环次数; 需要电池类驱动
    bool queryDesign(BatteryDesignInfo& out);

private:
    void reportFail(long status);

    // 电池类驱动 (setupapi + IOCTL)
    bool openDevice();
    void closeDevice();
    bool readDevice(BatterySample& s);

    bool      inited_ = false;
    ULONGLONG lastErrLog_ = 0;   // 失败日志节流: 10 秒最多一条
    int       errCount_ = 0;

    HANDLE        dev_ = INVALID_HANDLE_VALUE;
    unsigned long tag_ = 0;
};

} // namespace pg
