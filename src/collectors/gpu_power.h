// ============================================================
// GpuPower —— NVIDIA GPU 功率读取 (NVML, 毫瓦 -> 瓦)
// 不依赖 nvml.h / nvml.lib, 运行时动态加载 nvml.dll(驱动自带)
// ============================================================
#pragma once

#include <windows.h>
#include <string>

namespace pg {

typedef struct nvmlDevice_st* nvmlDevice_t;   // 不依赖 nvml.h, 自己声明

class GpuPower {
public:
    GpuPower() = default;
    ~GpuPower();

    GpuPower(const GpuPower&) = delete;
    GpuPower& operator=(const GpuPower&) = delete;

    bool open();       // 加载 nvml.dll 并初始化; 失败时已写入详细日志
    double read();     // 返回瓦特, 失败返回 -1 并写日志(10 秒节流)
    void shutdown();   // 关闭 NVML 并释放 DLL, 可重复调用

    std::string name();     // GPU 型号; 读不到返回空串 (--diagnose 用)

    // ---- 功耗上限控制 (NVML 的 limit 单位是毫瓦, 这里统一用瓦) ----
    // 驱动是否导出了设置函数; 部分笔记本驱动导出但拒绝写入
    bool hasLimitApi() const { return dev_ && setLimit_; }
    bool isElevated() const { return elevated_; }
    // 可以真正写入: 有函数 + 已提权 (NVML 设置上限要求管理员, 否则返回 4)
    bool hasPowerLimitControl() const { return hasLimitApi() && elevated_; }
    bool setPowerLimitW(unsigned watts);   // 需要管理员权限; 失败写日志
    int  powerLimitW();                    // 展示用 (读不到请求值时退回 enforced), 失败 -1
    int  requestedPowerLimitW();           // 驱动记录的请求上限; 不支持返回 -1
    int  enforcedPowerLimitW();            // 驱动实际生效的上限, 失败 -1
    int  defaultPowerLimitW();             // 默认上限, 失败 -1
    int  minPowerLimitW();                 // 允许的最小上限, 失败 -1
    int  maxPowerLimitW();                 // 允许的最大上限, 失败 -1

    // 只保存第一次读到的"请求上限"; 读不到则保持 -1 (不用 enforced 代替)
    void saveOriginalPowerLimit();
    int  originalPowerLimitW() const { return originalLimitW_; }
    bool restorePowerLimit();              // 恢复到 saveOriginal 时的值

    // ---- GPU 锁频 (功耗上限被厂商 EC 锁死时的替代杠杆) ----
    bool hasClockControl() const { return dev_ && setLockedClocks_ && resetLockedClocks_; }
    // 把核心频率上限设为 mhz, 下限尽量交给驱动 (允许低负载降频); 需要管理员
    bool setGpuClockLock(unsigned mhz);
    bool resetGpuClockLock();              // 解除锁定, 恢复驱动自动调频
    int  currentGpuClockMhz();             // 当前核心频率, 失败 -1
    int  maxGpuClockMhz();                 // 允许的最大核心频率, 失败 -1
    int  minGpuClockMhz();                 // 允许的最低核心频率 (P0), 失败 -1

    // ---- 能力探测 (--diagnose) ----
    // 返回 NVML 原始错误码: 0 = 成功, 正数 = NVML 错误, -1 = 无 API, -2 = 未提权。
    // 探测不写日志, 由调用方决定怎么展示。
    int probePowerLimit(unsigned watts);
    int probeClockLock(unsigned mhz);
    int probeClockReset();

private:
    void reportFail(const char* what, long code);
    void reportLimitFail(const char* what, long code);

    HMODULE dll_ = nullptr;
    int  (*init_)(void) = nullptr;
    int  (*shutdown_)(void) = nullptr;
    int  (*getHandle_)(unsigned, nvmlDevice_t*) = nullptr;
    int  (*getPower_)(nvmlDevice_t, unsigned*) = nullptr;

    int  (*setLimit_)(nvmlDevice_t, unsigned) = nullptr;
    int  (*getLimit_)(nvmlDevice_t, unsigned*) = nullptr;
    int  (*getEnforcedLimit_)(nvmlDevice_t, unsigned*) = nullptr;
    int  (*getDefaultLimit_)(nvmlDevice_t, unsigned*) = nullptr;
    int  (*getLimitConstraints_)(nvmlDevice_t, unsigned*, unsigned*) = nullptr;
    int  (*setLockedClocks_)(nvmlDevice_t, unsigned, unsigned) = nullptr;
    int  (*resetLockedClocks_)(nvmlDevice_t) = nullptr;
    int  (*getClockInfo_)(nvmlDevice_t, int, unsigned*) = nullptr;
    int  (*getMaxClockInfo_)(nvmlDevice_t, int, unsigned*) = nullptr;
    int  (*getMinMaxClockOfPState_)(nvmlDevice_t, int, int, unsigned*, unsigned*) = nullptr;
    int  (*getName_)(nvmlDevice_t, char*, unsigned) = nullptr;

    nvmlDevice_t dev_ = nullptr;
    bool elevated_ = false;      // 是否以管理员身份运行 (设置上限的前提)
    int originalLimitW_ = -1;    // -1 = 尚未保存/不可用

    ULONGLONG lastErrLog_ = 0;   // 失败日志节流: 10 秒最多记一条
    int errCount_ = 0;
    ULONGLONG lastLimitErrLog_ = 0;
    int limitErrCount_ = 0;
};

} // namespace pg
