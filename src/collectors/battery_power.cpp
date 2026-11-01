#include "collectors/battery_power.h"

#include <powrprof.h>
#include <setupapi.h>
#include <devguid.h>
#include <poclass.h>

#include <cmath>
#include <vector>

#include "common/logger.h"

namespace pg {

void BatteryPower::reportFail(long status) {
    ++errCount_;
    ULONGLONG now = GetTickCount64();
    if (lastErrLog_ && now - lastErrLog_ < 10000) return;
    LOG_WARN("bat", "读取电池状态失败 (status=0x%08lX), 期间共失败 %d 次",
             (unsigned long)status, errCount_);
    lastErrLog_ = now;
    errCount_ = 0;
}

BatteryPower::~BatteryPower() {
    closeDevice();
}

void BatteryPower::closeDevice() {
    if (dev_ != INVALID_HANDLE_VALUE) {
        CloseHandle(dev_);
        dev_ = INVALID_HANDLE_VALUE;
        tag_ = 0;
    }
}

// 枚举电池类设备并打开第一个。失败返回 false, 调用方回退到
// CallNtPowerInformation(SystemBatteryState)。
bool BatteryPower::openDevice() {
    closeDevice();

    HDEVINFO devs = SetupDiGetClassDevsA(&GUID_DEVCLASS_BATTERY, nullptr, nullptr,
                                         DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devs == INVALID_HANDLE_VALUE) return false;

    bool opened = false;
    SP_DEVICE_INTERFACE_DATA did{};
    did.cbSize = sizeof(did);

    for (DWORD i = 0;
         SetupDiEnumDeviceInterfaces(devs, nullptr, &GUID_DEVCLASS_BATTERY, i, &did);
         ++i) {
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailA(devs, &did, nullptr, 0, &need, nullptr);
        if (need == 0 || need > 4096) continue;

        std::vector<char> buf(need);
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_A*>(buf.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);
        if (!SetupDiGetDeviceInterfaceDetailA(devs, &did, detail, need, &need, nullptr))
            continue;

        HANDLE h = CreateFileA(detail->DevicePath, GENERIC_READ | GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;

        DWORD waitMs = 0, tag = 0, bytes = 0;
        if (DeviceIoControl(h, IOCTL_BATTERY_QUERY_TAG, &waitMs, sizeof(waitMs),
                            &tag, sizeof(tag), &bytes, nullptr)) {
            dev_ = h;
            tag_ = tag;
            opened = true;
            break;
        }
        CloseHandle(h);
    }

    SetupDiDestroyDeviceInfoList(devs);
    return opened;
}

// 用电池类驱动读取方向/功率。成功时覆盖 s 的供电字段。
bool BatteryPower::readDevice(BatterySample& s) {
    if (dev_ == INVALID_HANDLE_VALUE) return false;

    auto query = [&](BATTERY_STATUS& bs) {
        BATTERY_WAIT_STATUS bws{};
        bws.BatteryTag = tag_;
        DWORD bytes = 0;
        return DeviceIoControl(dev_, IOCTL_BATTERY_QUERY_STATUS, &bws, sizeof(bws),
                               &bs, sizeof(bs), &bytes, nullptr) != FALSE;
    };

    BATTERY_STATUS bs{};
    if (!query(bs)) {
        // 设备被移除/重新枚举时 tag 会失效, 重开一次再试
        closeDevice();
        if (!openDevice() || !query(bs)) return false;
    }

    const bool onLine = (bs.PowerState & BATTERY_POWER_ON_LINE) != 0;
    const bool charging = (bs.PowerState & BATTERY_CHARGING) != 0;
    const bool discharging = (bs.PowerState & BATTERY_DISCHARGING) != 0;

    LONG raw = bs.Rate;
    if (raw == (LONG)BATTERY_UNKNOWN_RATE) raw = 0;
    double rate = std::fabs((double)raw) / 1000.0;
    if (rate > 500.0) rate = 0.0;

    s.acOnline = onLine;
    s.charging = charging;
    s.discharging = discharging;
    if (discharging && !charging)       s.rateW = rate;    // 放电
    else if (charging && !discharging)  s.rateW = -rate;   // 充电
    else if (charging && discharging)   s.rateW = (raw < 0) ? rate : -rate;  // 标志冲突时按符号
    else                                s.rateW = 0.0;
    return true;
}

bool BatteryPower::queryDesign(BatteryDesignInfo& out) {
    out = BatteryDesignInfo{};
    if (dev_ == INVALID_HANDLE_VALUE && !openDevice()) return false;

    BATTERY_QUERY_INFORMATION q{};
    q.BatteryTag = tag_;
    q.InformationLevel = BatteryInformation;

    BATTERY_INFORMATION bi{};
    DWORD bytes = 0;
    if (!DeviceIoControl(dev_, IOCTL_BATTERY_QUERY_INFORMATION, &q, sizeof(q),
                         &bi, sizeof(bi), &bytes, nullptr)) {
        return false;
    }

    out.valid = true;
    if (bi.DesignedCapacity != BATTERY_UNKNOWN_CAPACITY)
        out.designWh = bi.DesignedCapacity / 1000.0;
    if (bi.FullChargedCapacity != BATTERY_UNKNOWN_CAPACITY)
        out.fullWh = bi.FullChargedCapacity / 1000.0;
    out.cycles = (int)bi.CycleCount;
    return true;
}

bool BatteryPower::open() {
    closeDevice();

    SYSTEM_BATTERY_STATE st{};
    NTSTATUS rc = CallNtPowerInformation(SystemBatteryState, nullptr, 0,
                                         &st, sizeof(st));
    if (rc != 0) {
        LOG_WARN("bat", "CallNtPowerInformation(SystemBatteryState) 失败, status=0x%08lX",
                 (unsigned long)rc);
        return false;
    }
    if (!st.BatteryPresent) {
        LOG_WARN("bat", "系统未检测到电池 (台式机或电池被禁用)");
        return false;
    }

    if (openDevice()) {
        LOG_INFO("bat", "电池方向来源: 电池类驱动 (IOCTL_BATTERY_QUERY_STATUS)");
    } else {
        LOG_WARN("bat", "电池类驱动不可用, 回退到 CallNtPowerInformation "
                        "(充放电标志可能不准)");
    }

    inited_ = true;
    return true;
}

BatterySample BatteryPower::read() {
    BatterySample s;
    if (!inited_) return s;

    SYSTEM_BATTERY_STATE st{};
    NTSTATUS rc = CallNtPowerInformation(SystemBatteryState, nullptr, 0,
                                         &st, sizeof(st));
    if (rc != 0) {
        reportFail((long)rc);
        return s;
    }

    s.valid          = true;
    s.batteryPresent = st.BatteryPresent != FALSE;

    // 方向/功率优先用电池类驱动 (与 WMI 同源)。
    if (!readDevice(s)) {
        s.acOnline    = st.AcOnLine != FALSE;
        s.charging    = st.Charging != FALSE;
        s.discharging = st.Discharging != FALSE;

        // 本机实测这两个标志可能同时为真; 用 GetSystemPowerStatus 的
        // "正在充电" 位消歧, 否则会把充电误判成缺口。
        if (s.charging && s.discharging) {
            SYSTEM_POWER_STATUS sp{};
            if (GetSystemPowerStatus(&sp) && (sp.BatteryFlag & 0x08))
                s.discharging = false;
        }

        // Rate 单位毫瓦。驱动在放电时可能写入负值, 先按 32 位有符号解释。
        double rate = std::fabs((double)(LONG)st.Rate) / 1000.0;
        if (rate > 500.0) rate = 0.0;
        if (s.discharging && !s.charging)      s.rateW = rate;
        else if (s.charging && !s.discharging) s.rateW = -rate;
        else if (s.charging && s.discharging)  s.rateW = ((LONG)st.Rate < 0) ? rate : -rate;
        else                                   s.rateW = 0.0;
    }

    s.remainingWh = st.RemainingCapacity / 1000.0;
    s.maxWh       = st.MaxCapacity / 1000.0;
    if (st.MaxCapacity > 0)
        s.percent = (int)(((unsigned long long)st.RemainingCapacity * 100) / st.MaxCapacity);
    return s;
}

} // namespace pg
