#include "collectors/cpu_power.h"
#include <pdhmsg.h>
#include "common/logger.h"

#pragma comment(lib, "pdh.lib")   // MSVC 用; MinGW 用 -lpdh

namespace pg {

CpuPower::~CpuPower() {
    if (query_) PdhCloseQuery(query_);
}

void CpuPower::reportFail(const char* what, long code) {
    ++errCount_;
    ULONGLONG now = GetTickCount64();
    if (lastErrLog_ && now - lastErrLog_ < 10000) return;
    LOG_WARN("cpu", "读瓦特失败: %s (code=0x%08lX), 期间共失败 %d 次",
             what, (unsigned long)code, errCount_);
    lastErrLog_ = now;
    errCount_ = 0;
}

bool CpuPower::open() {
    PDH_STATUS rc = PdhOpenQueryW(nullptr, 0, &query_);
    if (rc != ERROR_SUCCESS) {
        LOG_ERROR("cpu", "PdhOpenQueryW 失败, code=0x%08lX", (unsigned long)rc);
        query_ = nullptr;
        return false;
    }

    // 用英文计数器名, 中文系统上也能找到
    rc = PdhAddEnglishCounterW(query_, L"\\Energy Meter(RAPL_Package0_PKG)\\Power",
                               0, &counter_);
    if (rc != ERROR_SUCCESS) {
        LOG_ERROR("cpu", "找不到计数器 \\Energy Meter(RAPL_Package0_PKG)\\Power, "
                         "code=0x%08lX (非 Intel 平台或 RAPL 未启用?)", (unsigned long)rc);
        PdhCloseQuery(query_);
        query_ = nullptr;
        return false;
    }

    rc = PdhCollectQueryData(query_);     // 建立基线, 顺便验证计数器真的能采到数
    if (rc != ERROR_SUCCESS) {
        LOG_ERROR("cpu", "计数器基线采样失败, code=0x%08lX (计数器存在但不可用)",
                  (unsigned long)rc);
        PdhCloseQuery(query_);
        query_ = nullptr;
        return false;
    }
    lastCollect_ = GetTickCount64();
    return true;
}

double CpuPower::read() {
    if (!query_) {
        reportFail("CPU 计数器未初始化", -1);
        return -1;
    }
    // "Power" 是速率计数器, 两次采样间隔太短(如刚 open 完)读数会失真
    ULONGLONG now = GetTickCount64();
    if (now - lastCollect_ < 500)
        Sleep((DWORD)(500 - (now - lastCollect_)));

    PDH_STATUS rc = PdhCollectQueryData(query_);
    if (rc != ERROR_SUCCESS) {
        reportFail("PdhCollectQueryData", rc);
        return -1;
    }
    lastCollect_ = GetTickCount64();

    PDH_FMT_COUNTERVALUE v{};
    rc = PdhGetFormattedCounterValue(counter_, PDH_FMT_DOUBLE, nullptr, &v);
    if (rc != ERROR_SUCCESS) {
        reportFail("PdhGetFormattedCounterValue", rc);
        return -1;
    }
    if (v.CStatus != PDH_CSTATUS_VALID_DATA) {
        reportFail("计数器数据无效", (long)v.CStatus);
        return -1;
    }
    return v.doubleValue / 1000.0;   // 计数器单位是毫瓦
}

} // namespace pg
