#include "control/cpu_limit.h"

#include <powersetting.h>
#include <powrprof.h>

#include "common/logger.h"

namespace pg {
namespace {

// 处理器电源管理子组 / 最大处理器状态
// 54533251-82be-4824-96c1-47b60b740d00
const GUID kProcessorSubgroup =
    {0x54533251, 0x82be, 0x4824, {0x96, 0xc1, 0x47, 0xb6, 0x0b, 0x74, 0x0d, 0x00}};
// bc5038f7-23e0-4960-96da-33abaf5935ec
const GUID kProcessorThrottleMax =
    {0xbc5038f7, 0x23e0, 0x4960, {0x96, 0xda, 0x33, 0xab, 0xaf, 0x59, 0x35, 0xec}};

} // namespace

int readCpuMaxState() {
    GUID* scheme = nullptr;
    if (PowerGetActiveScheme(nullptr, &scheme) != ERROR_SUCCESS || !scheme)
        return -1;
    DWORD value = 0;
    DWORD rc = PowerReadACValueIndex(nullptr, scheme, &kProcessorSubgroup,
                                     &kProcessorThrottleMax, &value);
    LocalFree(scheme);
    return rc == ERROR_SUCCESS ? (int)value : -1;
}

int readCpuMaxStateDC() {
    GUID* scheme = nullptr;
    if (PowerGetActiveScheme(nullptr, &scheme) != ERROR_SUCCESS || !scheme)
        return -1;
    DWORD value = 0;
    DWORD rc = PowerReadDCValueIndex(nullptr, scheme, &kProcessorSubgroup,
                                     &kProcessorThrottleMax, &value);
    LocalFree(scheme);
    return rc == ERROR_SUCCESS ? (int)value : -1;
}

CpuLimit::~CpuLimit() {
    close();
}

bool CpuLimit::open() {
    close();

    DWORD rc = PowerGetActiveScheme(nullptr, &scheme_);
    if (rc != ERROR_SUCCESS) {
        LOG_ERROR("cpu", "PowerGetActiveScheme 失败, code=%lu (无法读取当前电源方案)",
                  (unsigned long)rc);
        scheme_ = nullptr;
        return false;
    }

    DWORD value = 0;
    rc = PowerReadACValueIndex(nullptr, scheme_, &kProcessorSubgroup,
                               &kProcessorThrottleMax, &value);
    if (rc != ERROR_SUCCESS) {
        LOG_ERROR("cpu", "读取最大处理器状态失败, code=%lu (电源方案可能不包含该设置)",
                  (unsigned long)rc);
        close();
        return false;
    }

    originalPct_ = (int)value;
    LOG_INFO("cpu", "当前最大处理器状态 %d%% (原始值已保存)", originalPct_);
    return true;
}

void CpuLimit::close() {
    if (scheme_) {
        LocalFree(scheme_);
        scheme_ = nullptr;
    }
    originalPct_ = -1;
}

int CpuLimit::maxState() const {
    if (!scheme_) return -1;
    DWORD value = 0;
    DWORD rc = PowerReadACValueIndex(nullptr, scheme_, &kProcessorSubgroup,
                                     &kProcessorThrottleMax, &value);
    if (rc != ERROR_SUCCESS) return -1;
    return (int)value;
}

bool CpuLimit::setMaxState(int pct) {
    if (!scheme_) {
        LOG_ERROR("cpu", "CPU 限频未初始化 (未读取活动电源方案)");
        return false;
    }
    if (pct < 5) pct = 5;
    if (pct > 100) pct = 100;

    DWORD rc = PowerWriteACValueIndex(nullptr, scheme_, &kProcessorSubgroup,
                                      &kProcessorThrottleMax, (DWORD)pct);
    if (rc != ERROR_SUCCESS) {
        LOG_ERROR("cpu", "设置最大处理器状态 %d%% 失败, code=%lu",
                  pct, (unsigned long)rc);
        return false;
    }

    rc = PowerSetActiveScheme(nullptr, scheme_);
    if (rc != ERROR_SUCCESS) {
        // 写入本身可能已经生效, 继续读回校验
        LOG_WARN("cpu", "PowerSetActiveScheme 失败, code=%lu (设置可能稍后生效)",
                 (unsigned long)rc);
    }

    int now = maxState();
    if (now != pct) {
        LOG_WARN("cpu", "写入后读回 %d%%, 期望 %d%% (可能被系统/厂商策略覆盖)",
                 now, pct);
        return false;
    }

    LOG_INFO("cpu", "最大处理器状态已设为 %d%%", pct);
    return true;
}

bool CpuLimit::restore() {
    if (originalPct_ < 0) {
        LOG_WARN("cpu", "没有可恢复的原始最大处理器状态");
        return false;
    }
    int pct = originalPct_;
    LOG_INFO("cpu", "恢复原始最大处理器状态 %d%%", pct);
    return setMaxState(pct);
}

} // namespace pg
