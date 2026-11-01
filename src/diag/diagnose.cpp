#include "diag/diagnose.h"

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#include <windows.h>
#include <powrprof.h>
#include <powersetting.h>

#include <climits>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "collectors/battery_power.h"
#include "collectors/gpu_power.h"
#include "common/logger.h"
#include "control/cpu_limit.h"
#include "control/state_file.h"

namespace pg {
namespace {

bool isElevated() {
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

std::string wideToUtf8(const wchar_t* w) {
    if (!w) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string s((size_t)n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

std::string regString(const wchar_t* sub, const wchar_t* name) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, sub, 0, KEY_READ, &key) != ERROR_SUCCESS)
        return {};
    wchar_t buf[512] = {0};
    DWORD bytes = sizeof(buf), type = 0;
    LONG rc = RegQueryValueExW(key, name, nullptr, &type, (LPBYTE)buf, &bytes);
    RegCloseKey(key);
    if (rc != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ))
        return {};
    return wideToUtf8(buf);
}

std::string activeSchemeName() {
    GUID* scheme = nullptr;
    if (PowerGetActiveScheme(nullptr, &scheme) != ERROR_SUCCESS || !scheme)
        return {};
    DWORD bytes = 0;
    std::string out;
    PowerReadFriendlyName(nullptr, scheme, nullptr, nullptr, nullptr, &bytes);
    if (bytes > 0) {
        std::vector<BYTE> buf(bytes + sizeof(wchar_t), 0);
        if (PowerReadFriendlyName(nullptr, scheme, nullptr, nullptr,
                                  buf.data(), &bytes) == ERROR_SUCCESS) {
            out = wideToUtf8(reinterpret_cast<const wchar_t*>(buf.data()));
        }
    }
    LocalFree(scheme);
    return out;
}

std::string windowsVersion() {
    static const wchar_t* kVer = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
    std::string product = regString(kVer, L"ProductName");
    std::string display = regString(kVer, L"DisplayVersion");
    std::string build   = regString(kVer, L"CurrentBuildNumber");
    if (product.empty() && build.empty()) return "未知";

    std::string out = product.empty() ? "Windows" : product;
    // Win11 的注册表 ProductName 仍写着 "Windows 10", 按 build 号修正
    if (!build.empty() && std::atoi(build.c_str()) >= 22000 &&
        out.rfind("Windows 10", 0) == 0)
        out.replace(0, 10, "Windows 11");
    if (!display.empty()) out += " " + display;
    if (!build.empty())   out += " (build " + build + ")";
    return out;
}

int logicalCpus() {
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    return (int)si.dwNumberOfProcessors;
}

int physicalCores() {
    DWORD len = 0;
    GetLogicalProcessorInformation(nullptr, &len);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || len == 0) return -1;
    if (len % sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION) != 0) return -1;

    std::vector<SYSTEM_LOGICAL_PROCESSOR_INFORMATION> buf(
        len / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION));
    if (!GetLogicalProcessorInformation(buf.data(), &len)) return -1;

    int cores = 0;
    for (const auto& e : buf)
        if (e.Relationship == RelationProcessorCore) ++cores;
    return cores;
}

const char* nvmlHint(int code) {
    switch (code) {
        case 0:  return "成功";
        case 1:  return "驱动未加载";
        case 2:  return "参数超出驱动允许范围";
        case 3:  return "该功能不受支持 (笔记本 EC 托管 TGP 时常见)";
        case 4:  return "需要管理员权限";
        case 8:  return "驱动拒绝: 供电不足";
        case -1: return "NVML 没有该函数";
        case -2: return "未以管理员身份运行";
        default: return "未知错误";
    }
}

// 判断是否有正在控制中的 PowerGuard 实例 (有则不能做写探测)
bool controllerRunning() {
    ControlState st;
    if (!loadControlState(st)) return false;
    if (st.pid == 0 || st.pid == GetCurrentProcessId()) return false;
    return pidAlive(st.pid);
}

void printBattery(const BatterySample& s, const SYSTEM_BATTERY_STATE& st,
                  bool structOk, const BatteryDesignInfo& design,
                  bool ioctl) {
    printf("\n[电池]\n");
    if (!structOk || !st.BatteryPresent) {
        printf("  未检测到电池。\n");
        printf("  没有电池缺口信号时, 闭环只能按固定上限运行, 无法判断是否超出供电。\n");
        return;
    }

    printf("  方向信号来源 : %s\n", ioctl
        ? "电池类驱动 (IOCTL_BATTERY_QUERY_STATUS), 可信"
        : "CallNtPowerInformation 兜底 (可能有充放电标志冲突)");

    const char* stateText = "空闲";
    if (s.acOnline && s.charging)         stateText = "充电中";
    else if (s.acOnline && s.discharging) stateText = "插电但放电 (输入不足)";
    else if (s.acOnline)                  stateText = "已接电源";
    else if (s.discharging)               stateText = "电池供电";
    printf("  状态         : %s (AC %s)\n", stateText, s.acOnline ? "已连接" : "未连接");
    printf("  功率         : %+.2f W  (正=放电缺口, 负=充电余量)\n", s.rateW);
    printf("  电量         : %d%% (%.1f / %.1f Wh)\n", s.percent, s.remainingWh, s.maxWh);

    if (design.valid) {
        double health = (design.designWh > 0 && design.fullWh > 0)
            ? design.fullWh * 100.0 / design.designWh : -1;
        printf("  设计/满充容量: %.1f / %.1f Wh", design.designWh, design.fullWh);
        if (health > 0) printf(" (健康度 %.0f%%)", health);
        printf("\n");
        if (design.cycles > 0) printf("  循环次数     : %d\n", design.cycles);
    }

    printf("  结构体标志   : Charging=%d Discharging=%d%s\n",
           st.Charging ? 1 : 0, st.Discharging ? 1 : 0,
           (st.Charging && st.Discharging) ? "  <- 冲突, 该来源不可用于方向判断" : "");
}

void printSuggestion(bool gpuUnverified, int gpuMaxClock,
                     bool powerWritable, bool clockUsable,
                     bool cpuUsable, bool batteryUsable) {
    auto pctMhz = [gpuMaxClock](int pct) {
        return gpuMaxClock > 0 ? gpuMaxClock * pct / 100 / 10 * 10 : 0;
    };

    printf("\n[建议] profiles.ini 起点 (闭环会在运行时自己微调):\n\n");

    if (powerWritable) {
        printf("  [default]\n");
        printf("  gpu_limit = 30\n");
        printf("  min_gpu = 15\n");
        printf("  cpu_max = 99\n");
        printf("\n  GPU 功耗上限可写, 优先用它: 步进更细, 不锁死频率, 对帧数更友好。\n");
    } else if (clockUsable) {
        printf("  [default]\n");
        if (gpuMaxClock > 0) {
            printf("  gpu_clock_pct = 50        # 最大 %dMHz 的 50%% ≈ %dMHz\n",
                   gpuMaxClock, pctMhz(50));
            printf("  min_gpu_clock_pct = 30    # 下限 ≈ %dMHz\n", pctMhz(30));
            printf("  auto_clock_pct = 45       # 自动起始 ≈ %dMHz\n", pctMhz(45));
        } else {
            printf("  gpu_clock_pct = 50\n");
            printf("  min_gpu_clock_pct = 30\n");
            printf("  auto_clock_pct = 45\n");
        }
        printf("  cpu_max = 99\n");
        printf("\n  功耗上限写不进去, 用百分比锁频; 换显卡不同的机器也不用改数值。\n");
    } else if (gpuUnverified) {
        printf("  GPU 杠杆还没实测, 先跑一次确认可写性:\n");
        printf("    PowerGuard.exe --diagnose-validate\n\n");
        printf("  确认之后二选一 (两种都建议再加 cpu_max = 99):\n\n");
        printf("  # A. 功耗上限可写\n");
        printf("  gpu_limit = 30\n");
        printf("  min_gpu = 15\n\n");
        printf("  # B. EC 托管, 功耗上限不可写 -> 百分比锁频\n");
        printf("  gpu_clock_pct = 50\n");
        printf("  min_gpu_clock_pct = 30\n");
        printf("  auto_clock_pct = 45\n");
    } else if (cpuUsable) {
        printf("  [default]\n");
        printf("  cpu_max = 99\n");
        printf("  prefer = cpu\n");
        printf("  gpu_clock_auto = 0\n");
        printf("\n  GPU 两个杠杆都不可用, 只能控制 CPU。\n");
    } else {
        printf("  # 没有可用的控制杠杆\n");
    }

    if (!batteryUsable) {
        printf("\n  注意: 电池方向信号不可用, 闭环无法判断是否超出供电预算,\n");
        printf("        只能按静态上限运行。\n");
    }
}

} // namespace

int runDiagnose(bool validate) {
    SetConsoleOutputCP(CP_UTF8);
    // 诊断表格保持干净: 模块日志只进文件, 不插进输出中间
    Logger::instance().init("logs", Level::Warn);
    Logger::instance().setConsole(false);

    const bool admin = isElevated();
    const bool controlling = controllerRunning();

    printf("============================================================\n");
    printf(" PowerGuard 能力探测 (--diagnose%s)\n", validate ? "-validate" : "");
    printf("============================================================\n");
    printf("系统   : %s\n", windowsVersion().c_str());
    printf("权限   : %s\n", admin ? "管理员" : "普通用户 (GPU 控制需要管理员)");
    printf("探测   : %s\n", validate
        ? (controlling ? "检测到正在运行的 PowerGuard, 写探测已跳过"
                       : "读写实测 (写入相同值)")
        : "只读");

    SYSTEM_POWER_STATUS sp{};
    bool haveSp = GetSystemPowerStatus(&sp) != FALSE;
    printf("供电   : %s\n", haveSp
        ? (sp.ACLineStatus == 1 ? "已接交流电" : "电池供电")
        : "未知");

    // ---------------- GPU ----------------
    GpuPower gpu;
    bool gpuOk = gpu.open();
    int gpuMaxClock = -1;
    bool gpuPowerApi = false, gpuClockApi = false;
    int powerCode = INT_MIN, lockCode = INT_MIN, resetCode = INT_MIN;

    if (gpuOk) {
        std::string name = gpu.name();
        int curClock = gpu.currentGpuClockMhz();
        gpuMaxClock = gpu.maxGpuClockMhz();
        int gpuMinClock = gpu.minGpuClockMhz();
        double watts = gpu.read();

        printf("\n[GPU] %s\n", name.empty() ? "(型号未知)" : name.c_str());
        printf("  当前核心频率 : %d MHz\n", curClock);
        printf("  最大核心频率 : %d MHz%s\n", gpuMaxClock,
               gpuMaxClock > 0 ? "  <- 百分比换算基准" : "");
        printf("  锁频范围     : %s - %s MHz%s\n",
               gpuMinClock > 0 ? std::to_string(gpuMinClock).c_str() : "?",
               gpuMaxClock > 0 ? std::to_string(gpuMaxClock).c_str() : "?",
               gpuMinClock > 0 ? "  (锁频只限上限, 低负载仍可降频)" : "");
        printf("  当前功耗     : %.1f W\n", watts);

        int req = gpu.requestedPowerLimitW();
        int enf = gpu.enforcedPowerLimitW();
        int def = gpu.defaultPowerLimitW();
        int mn  = gpu.minPowerLimitW();
        int mx  = gpu.maxPowerLimitW();
        printf("  功耗上限     : 请求 %s | 生效 %s | 默认 %s | 范围 %s-%s\n",
               req >= 0 ? (std::to_string(req) + "W").c_str() : "不可读",
               enf >= 0 ? (std::to_string(enf) + "W").c_str() : "不可读",
               def >= 0 ? (std::to_string(def) + "W").c_str() : "不可读",
               mn >= 0 ? std::to_string(mn).c_str() : "?",
               mx >= 0 ? std::to_string(mx).c_str() : "?");

        gpuPowerApi = gpu.hasLimitApi();
        gpuClockApi = gpu.hasClockControl();

        if (validate && admin && !controlling) {
            int cur = gpu.powerLimitW();
            if (cur > 0 && gpuPowerApi) powerCode = gpu.probePowerLimit((unsigned)cur);
            if (curClock > 0 && gpuClockApi) {
                lockCode = gpu.probeClockLock((unsigned)curClock);
                if (lockCode == 0) resetCode = gpu.probeClockReset();
            }
        }
    } else {
        printf("\n[GPU] NVML 不可用 (未安装 NVIDIA 驱动 / 非 NVIDIA 显卡)\n");
        printf("      详细原因见 logs/ 当天日志\n");
    }

    if (gpuOk) {
        const char* powerText;
        if (!gpuPowerApi)            powerText = "NVML 无此函数";
        else if (!admin)             powerText = "未提权 (以管理员运行才能写)";
        else if (powerCode != INT_MIN)
            powerText = powerCode == 0 ? "可写 (实测)" : "不可写 (实测)";
        else                         powerText = "未验证 (加 --diagnose-validate 实测)";

        const char* clockText;
        if (!gpuClockApi)            clockText = "NVML 无此函数";
        else if (!admin)             clockText = "API 存在, 需要管理员";
        else if (lockCode != INT_MIN)
            clockText = lockCode == 0 ? "可用 (实测)" : "不可用 (实测)";
        else                         clockText = "API 存在, 未验证";

        printf("  写功耗上限   : %s", powerText);
        if (powerCode != INT_MIN && powerCode != 0)
            printf("  [code=%d: %s]", powerCode, nvmlHint(powerCode));
        printf("\n");
        printf("  锁频能力     : %s", clockText);
        if (lockCode != INT_MIN && lockCode != 0)
            printf("  [code=%d: %s]", lockCode, nvmlHint(lockCode));
        printf("\n");
        if (resetCode != INT_MIN && resetCode != 0)
            printf("  警告         : 锁频探测后解除失败 (code=%d), 请重启或运行 --restore\n",
                   resetCode);
    }

    // ---------------- CPU ----------------
    printf("\n[CPU] %s\n",
           regString(L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                     L"ProcessorNameString").c_str());
    int cores = physicalCores();
    printf("  核心/线程    : %d / %d\n", cores, logicalCpus());
    printf("  电源方案     : %s\n", activeSchemeName().c_str());

    int acPct = readCpuMaxState();
    int dcPct = readCpuMaxStateDC();
    printf("  最大处理器状态: AC %s / DC %s\n",
           acPct >= 0 ? (std::to_string(acPct) + "%").c_str() : "不可读",
           dcPct >= 0 ? (std::to_string(dcPct) + "%").c_str() : "不可读");

    bool cpuUsable = acPct >= 0;
    bool cpuProbeDone = false, cpuWritable = false;
    if (validate && cpuUsable && !controlling) {
        CpuLimit lim;
        if (lim.open()) {
            int orig = lim.originalMaxState();
            cpuProbeDone = true;
            cpuWritable = lim.setMaxState(orig);   // 写入相同值, 无副作用
            lim.close();
        }
    }
    printf("  写 CPU 限频  : %s\n",
           cpuProbeDone ? (cpuWritable ? "可写 (实测)" : "不可写 (实测)")
                        : "未验证");

    // ---------------- 电池 ----------------
    BatteryPower bat;
    bool batOk = bat.open();
    BatterySample bs;
    SYSTEM_BATTERY_STATE st{};
    bool structOk = CallNtPowerInformation(SystemBatteryState, nullptr, 0,
                                           &st, sizeof(st)) == 0;
    if (batOk) bs = bat.read();

    BatteryDesignInfo design;
    if (batOk) bat.queryDesign(design);

    bool batteryUsable = batOk && bs.valid;
    printBattery(bs, st, structOk, design, batOk && bat.usesDeviceDriver());

    // ---------------- 结论 ----------------
    bool powerWritable = (powerCode == 0);
    bool powerBlocked  = (powerCode != INT_MIN && powerCode != 0);
    bool clockUsable   = (lockCode == 0);
    bool clockBlocked  = (lockCode != INT_MIN && lockCode != 0);
    bool gpuApiPresent = gpuPowerApi || gpuClockApi;
    // 有 API 但还没实测: 不能断言"不可用", 只能标未验证
    bool gpuUnverified = gpuOk && gpuApiPresent &&
                         (powerCode == INT_MIN || lockCode == INT_MIN);

    const char* powerState =
        !gpuOk ? "不可用 (无 NVIDIA GPU)"
        : powerWritable ? "可写 (实测)"
        : powerBlocked ? "不可写 (驱动拒绝)"
        : !gpuPowerApi ? "NVML 无此函数"
        : !admin ? "需要管理员"
        : "未验证";

    const char* clockState =
        !gpuOk ? "不可用 (无 NVIDIA GPU)"
        : clockUsable ? "可用 (实测)"
        : clockBlocked ? "不可用 (驱动拒绝)"
        : !gpuClockApi ? "NVML 无此函数"
        : !admin ? "需要管理员"
        : "未验证";

    printf("\n[能力矩阵]\n");
    printf("  GPU 功耗上限 : %s\n", powerState);
    printf("  GPU 锁频     : %s\n", clockState);
    printf("  CPU 最大状态 : %s\n",
           cpuProbeDone ? (cpuWritable ? "可读可写 (实测)" : "不可写 (实测)")
                        : (cpuUsable ? "可读" : "不可用"));
    printf("  电池缺口信号 : %s\n", batteryUsable ? "可用" : "不可用");

    bool anyLever = powerWritable || clockUsable || cpuUsable || gpuUnverified;
    if (!gpuOk && cpuUsable)
        printf("\n  -> 只控 CPU: GPU 采集/控制都不可用\n");
    else if (powerWritable)
        printf("\n  -> 杠杆: GPU 功耗上限 + CPU 最大状态\n");
    else if (clockUsable)
        printf("\n  -> 杠杆: GPU 锁频 + CPU 最大状态\n");
    else if (gpuUnverified)
        printf("\n  -> GPU 杠杆未验证: 以管理员运行 --diagnose-validate 再判定\n");
    else if (cpuUsable)
        printf("\n  -> 杠杆: 仅 CPU 最大状态\n");

    printSuggestion(gpuUnverified, gpuMaxClock,
                    powerWritable, clockUsable, cpuUsable, batteryUsable);
    printf("\n============================================================\n");

    return anyLever ? 0 : 1;
}

} // namespace pg
