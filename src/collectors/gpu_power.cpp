#include "collectors/gpu_power.h"
#include "common/logger.h"

namespace pg {
namespace {

// NVML 写入功耗上限要求进程具备管理员权限, 否则返回 NVML_ERROR_NO_PERMISSION(4)
bool processElevated() {
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok))
        return false;
    TOKEN_ELEVATION info{};
    DWORD len = 0;
    bool ok = GetTokenInformation(tok, TokenElevation, &info, sizeof(info), &len)
              && info.TokenIsElevated;
    CloseHandle(tok);
    return ok;
}

} // namespace

GpuPower::~GpuPower() {
    shutdown();
}

void GpuPower::reportFail(const char* what, long code) {
    ++errCount_;
    ULONGLONG now = GetTickCount64();
    if (lastErrLog_ && now - lastErrLog_ < 10000) return;
    LOG_WARN("gpu", "读瓦特失败: %s (code=%ld), 期间共失败 %d 次", what, code, errCount_);
    lastErrLog_ = now;
    errCount_ = 0;
}

void GpuPower::reportLimitFail(const char* what, long code) {
    ++limitErrCount_;
    ULONGLONG now = GetTickCount64();
    if (lastLimitErrLog_ && now - lastLimitErrLog_ < 10000) return;
    LOG_WARN("gpu", "读功耗上限失败: %s (code=%ld), 期间共失败 %d 次",
             what, code, limitErrCount_);
    lastLimitErrLog_ = now;
    limitErrCount_ = 0;
}

bool GpuPower::open() {
    dll_ = LoadLibraryW(L"nvml.dll");
    if (!dll_) {
        LOG_ERROR("gpu", "加载 nvml.dll 失败, GetLastError=%lu (未安装 NVIDIA 驱动?)",
                  (unsigned long)GetLastError());
        return false;
    }

    init_      = (decltype(init_))     GetProcAddress(dll_, "nvmlInit_v2");
    shutdown_  = (decltype(shutdown_)) GetProcAddress(dll_, "nvmlShutdown");
    getHandle_ = (decltype(getHandle_))GetProcAddress(dll_, "nvmlDeviceGetHandleByIndex_v2");
    getPower_  = (decltype(getPower_)) GetProcAddress(dll_, "nvmlDeviceGetPowerUsage");

    // 控制函数是可选的: 老驱动/部分笔记本不支持软设上限, 但仍可正常监测功率
    // 注意: 只用老签名的 nvmlDeviceSetPowerManagementLimit(dev, mw)。
    // _v2 版本收的是 nvmlPowerValue_v2_t*, 按旧签名调用会直接段错误。
    setLimit_           = (decltype(setLimit_))           GetProcAddress(dll_, "nvmlDeviceSetPowerManagementLimit");
    getLimit_           = (decltype(getLimit_))           GetProcAddress(dll_, "nvmlDeviceGetPowerManagementLimit");
    getEnforcedLimit_   = (decltype(getEnforcedLimit_))   GetProcAddress(dll_, "nvmlDeviceGetEnforcedPowerLimit");
    getDefaultLimit_    = (decltype(getDefaultLimit_))    GetProcAddress(dll_, "nvmlDeviceGetPowerManagementDefaultLimit");
    getLimitConstraints_= (decltype(getLimitConstraints_))GetProcAddress(dll_, "nvmlDeviceGetPowerManagementLimitConstraints");
    // 锁频: 笔记本 GPU 常把 TGP 交给 EC 管理, 设上限返回 3, 但锁频往往仍可用
    setLockedClocks_    = (decltype(setLockedClocks_))   GetProcAddress(dll_, "nvmlDeviceSetGpuLockedClocks");
    resetLockedClocks_  = (decltype(resetLockedClocks_)) GetProcAddress(dll_, "nvmlDeviceResetGpuLockedClocks");
    getClockInfo_       = (decltype(getClockInfo_))      GetProcAddress(dll_, "nvmlDeviceGetClockInfo");
    getMaxClockInfo_    = (decltype(getMaxClockInfo_))   GetProcAddress(dll_, "nvmlDeviceGetMaxClockInfo");

    elevated_ = processElevated();

    const char* missing = !init_      ? "nvmlInit_v2"
                        : !getHandle_ ? "nvmlDeviceGetHandleByIndex_v2"
                        : !getPower_  ? "nvmlDeviceGetPowerUsage"
                        : nullptr;
    if (missing) {
        LOG_ERROR("gpu", "nvml.dll 中找不到函数 %s, 驱动版本可能过旧", missing);
        return false;
    }

    int rc = init_();
    if (rc != 0) {                                         // 0 == NVML_SUCCESS
        LOG_ERROR("gpu", "nvmlInit_v2 失败, code=%d", rc);
        return false;
    }
    rc = getHandle_(0, &dev_);
    if (rc != 0) {
        LOG_ERROR("gpu", "没有可用的 NVIDIA GPU (nvmlDeviceGetHandleByIndex_v2 code=%d)", rc);
        dev_ = nullptr;
        return false;
    }
    if (!setLimit_)
        LOG_WARN("gpu", "NVML 缺少功耗上限设置函数, 本次只能监测不能限 GPU 功耗");
    else if (!elevated_)
        LOG_WARN("gpu", "当前不是管理员权限, 无法设置 GPU 功耗上限 "
                        "(NVML 会返回 4); 以管理员身份运行才能限制 GPU");
    return true;
}

double GpuPower::read() {
    if (!dev_ || !getPower_) {
        reportFail("GPU 未初始化或函数缺失", -1);
        return -1;
    }
    unsigned mw = 0;
    int rc = getPower_(dev_, &mw);
    if (rc != 0) {
        reportFail("nvmlDeviceGetPowerUsage", rc);
        return -1;
    }
    return mw / 1000.0;                                    // 毫瓦 -> 瓦
}

bool GpuPower::setPowerLimitW(unsigned watts) {
    if (!dev_ || !setLimit_) {
        LOG_ERROR("gpu", "无法设置 GPU 功耗上限: NVML 未初始化或缺少函数");
        return false;
    }
    if (!elevated_) {
        LOG_ERROR("gpu", "无法设置 GPU 功耗上限 %uW: 需要管理员权限 "
                         "(请右键以管理员身份运行)", watts);
        return false;
    }
    int rc = setLimit_(dev_, watts * 1000u);               // NVML 单位为毫瓦
    if (rc != 0) {
        const char* hint = "";
        if (rc == 4)      hint = " (需要管理员权限, 请以管理员身份运行 PowerGuard)";
        else if (rc == 3) hint = " (该笔记本/驱动不支持软件设置功耗上限)";
        else if (rc == 2) hint = " (数值超出驱动允许范围)";
        else if (rc == 8) hint = " (驱动拒绝: 供电不足)";
        LOG_ERROR("gpu", "设置 GPU 功耗上限 %uW 失败, code=%d%s", watts, rc, hint);
        return false;
    }
    LOG_INFO("gpu", "GPU 功耗上限已设为 %uW", watts);
    return true;
}

int GpuPower::requestedPowerLimitW() {
    if (!dev_ || !getLimit_) return -1;
    unsigned mw = 0;
    int rc = getLimit_(dev_, &mw);
    if (rc != 0) {
        // 笔记本 GPU 上常见返回 3(NOT_SUPPORTED), 属于正常情况, 不刷日志
        if (rc != 3) reportLimitFail("nvmlDeviceGetPowerManagementLimit", rc);
        return -1;
    }
    return (int)(mw / 1000u);
}

int GpuPower::powerLimitW() {
    int w = requestedPowerLimitW();
    if (w >= 0) return w;
    // 读不到请求值时退回到 GetEnforcedPowerLimit: 它是驱动实际生效的上限,
    // 会受 Dynamic Boost 影响, 用于"原始值"备份和展示足够。
    return enforcedPowerLimitW();
}

int GpuPower::enforcedPowerLimitW() {
    if (!dev_ || !getEnforcedLimit_) return -1;
    unsigned mw = 0;
    int rc = getEnforcedLimit_(dev_, &mw);
    if (rc != 0) {
        reportLimitFail("nvmlDeviceGetEnforcedPowerLimit", rc);
        return -1;
    }
    return (int)(mw / 1000u);
}

int GpuPower::defaultPowerLimitW() {
    if (!dev_ || !getDefaultLimit_) return -1;
    unsigned mw = 0;
    int rc = getDefaultLimit_(dev_, &mw);
    if (rc != 0) {
        reportLimitFail("nvmlDeviceGetPowerManagementDefaultLimit", rc);
        return -1;
    }
    return (int)(mw / 1000u);
}

int GpuPower::minPowerLimitW() {
    if (!dev_ || !getLimitConstraints_) return -1;
    unsigned mn = 0, mx = 0;
    int rc = getLimitConstraints_(dev_, &mn, &mx);
    if (rc != 0) {
        reportLimitFail("nvmlDeviceGetPowerManagementLimitConstraints", rc);
        return -1;
    }
    return (int)(mn / 1000u);
}

int GpuPower::maxPowerLimitW() {
    if (!dev_ || !getLimitConstraints_) return -1;
    unsigned mn = 0, mx = 0;
    int rc = getLimitConstraints_(dev_, &mn, &mx);
    if (rc != 0) {
        reportLimitFail("nvmlDeviceGetPowerManagementLimitConstraints", rc);
        return -1;
    }
    return (int)(mx / 1000u);
}

void GpuPower::saveOriginalPowerLimit() {
    if (originalLimitW_ >= 0) return;                      // 只保存第一次
    int w = powerLimitW();
    if (w >= 0) {
        originalLimitW_ = w;
        LOG_INFO("gpu", "已保存原始 GPU 功耗上限 %dW", w);
    } else {
        LOG_WARN("gpu", "读取原始 GPU 功耗上限失败, 退出时将无法自动恢复");
    }
}

// ---- 锁频 ----
// 说明: NVML_CLOCK_GRAPHICS == 0

bool GpuPower::setGpuClockLock(unsigned mhz) {
    if (!dev_ || !setLockedClocks_) {
        LOG_ERROR("gpu", "无法锁定 GPU 频率: NVML 未初始化或缺少函数");
        return false;
    }
    if (!elevated_) {
        LOG_ERROR("gpu", "无法锁定 GPU 频率 %uMHz: 需要管理员权限", mhz);
        return false;
    }
    int rc = setLockedClocks_(dev_, mhz, mhz);
    if (rc != 0) {
        const char* hint = "";
        if (rc == 3)      hint = " (该笔记本/驱动不支持锁定 GPU 频率)";
        else if (rc == 2) hint = " (频率超出驱动允许范围)";
        else if (rc == 8) hint = " (驱动拒绝: 供电不足)";
        LOG_ERROR("gpu", "锁定 GPU 频率 %uMHz 失败, code=%d%s", mhz, rc, hint);
        return false;
    }
    LOG_INFO("gpu", "GPU 核心频率已锁定为 %uMHz", mhz);
    return true;
}

bool GpuPower::resetGpuClockLock() {
    if (!dev_ || !resetLockedClocks_) return false;
    if (!elevated_) {
        LOG_ERROR("gpu", "解除 GPU 锁频需要管理员权限");
        return false;
    }
    int rc = resetLockedClocks_(dev_);
    if (rc != 0) {
        LOG_ERROR("gpu", "解除 GPU 锁频失败, code=%d (3=不支持, 4=需要管理员)", rc);
        return false;
    }
    LOG_INFO("gpu", "已解除 GPU 锁频, 恢复自动调频");
    return true;
}

int GpuPower::currentGpuClockMhz() {
    if (!dev_ || !getClockInfo_) return -1;
    unsigned mhz = 0;
    if (getClockInfo_(dev_, 0, &mhz) != 0) return -1;
    return (int)mhz;
}

int GpuPower::maxGpuClockMhz() {
    if (!dev_ || !getMaxClockInfo_) return -1;
    unsigned mhz = 0;
    if (getMaxClockInfo_(dev_, 0, &mhz) != 0) return -1;
    return (int)mhz;
}

bool GpuPower::restorePowerLimit() {
    if (originalLimitW_ < 0) {
        LOG_WARN("gpu", "没有可恢复的原始 GPU 功耗上限");
        return false;
    }
    int w = originalLimitW_;
    LOG_INFO("gpu", "恢复原始 GPU 功耗上限 %dW", w);
    if (!setPowerLimitW((unsigned)w)) {
        LOG_ERROR("gpu", "恢复 GPU 功耗上限失败, 原始值仍保留以便重试");
        return false;
    }
    originalLimitW_ = -1;                                  // 下次 apply 重新捕获
    return true;
}

void GpuPower::shutdown() {
    if (shutdown_) {
        shutdown_();
        shutdown_ = nullptr;
    }
    if (dll_) {
        FreeLibrary(dll_);
        dll_ = nullptr;
    }
    init_ = nullptr;
    getHandle_ = nullptr;
    getPower_ = nullptr;
    setLimit_ = nullptr;
    getLimit_ = nullptr;
    getEnforcedLimit_ = nullptr;
    getDefaultLimit_ = nullptr;
    getLimitConstraints_ = nullptr;
    setLockedClocks_ = nullptr;
    resetLockedClocks_ = nullptr;
    getClockInfo_ = nullptr;
    getMaxClockInfo_ = nullptr;
    dev_ = nullptr;
    elevated_ = false;
    originalLimitW_ = -1;
}

} // namespace pg
