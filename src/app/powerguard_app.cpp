#include "app/powerguard_app.h"

#include <cstdio>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <string>
#include <thread>
#include <algorithm>
#include <iostream>

#include "control/game_profile.h"
#include "control/state_file.h"
#include "diag/diagnose.h"

namespace pg {
namespace {

PowerGuardApp* g_appForCtrl = nullptr;

// 控制台关闭/注销/关机时 Windows 只给几秒: 在这里同步恢复，避免限制被留在系统里。
// CTRL_C/CTRL_BREAK 走正常退出路径(置事件), 由主循环恢复并删除状态文件。
BOOL WINAPI consoleCtrlHandler(DWORD type) {
    if (!g_appForCtrl) return FALSE;
    switch (type) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
        case CTRL_LOGOFF_EVENT:
        case CTRL_SHUTDOWN_EVENT:
            g_appForCtrl->handleConsoleCtrl(type);
            return type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT;
        default:
            return FALSE;
    }
}

void printUsage() {
    printf(
        "用法: PowerGuard.exe [选项]\n"
        "  --process <名字>        监控目标进程 (可省略 .exe)\n"
        "  --only-running          仅在目标进程运行时记录 CSV\n"
        "  --profile usbc          启用 USB-C 100W 限功耗闭环\n"
        "  --gpu-limit <W>         GPU 初始/目标上限 (默认 30)\n"
        "  --min-gpu <W>           GPU 降档下限 (默认 15)\n"
        "  --max-gpu <W>           GPU 回升上限 (默认等于 --gpu-limit)\n"
        "  --cpu-max <%%>          最大处理器状态 (默认 99, 99=关睿频)\n"
        "  --cpu-min <%%>          CPU 降档下限 (默认 50)\n"
        "  --cpu-step <%%>         CPU 每次降档的百分点 (默认 5)\n"
        "  --no-cpu-step           不把 CPU 当作降档杠杆(只压 GPU)\n"
        "  --prefer-cpu            降档先动 CPU, 再动 GPU (默认反过来)\n"
        "  --gpu-clock <MHz>       锁定 GPU 核心频率 (功耗上限不可用时的替代)\n"
        "  --min-gpu-clock <MHz>   锁频降档下限 (默认 800)\n"
        "  --gpu-clock-pct <%%>     按最大核心频率的百分比锁频 (换机器不用改数值)\n"
        "  --min-gpu-clock-pct <%%> 锁频下限的百分比\n"
        "  --auto-clock-pct <%%>    自动锁频起始值的百分比\n"
        "  --clock-step <MHz>      每次升降档幅度 (默认 100)\n"
        "  --no-gpu-clock          关闭\"功耗上限失败后自动锁频\"\n"
        "  --battery-threshold <W> 电池放电阈值 (默认 2.0)\n"
        "  --deficit-energy <Wh>   缺口累计到多少 Wh 才降档 (默认 0.10)\n"
        "  --deficit-window <秒>   缺口能量衰减时间常数 (默认 120)\n"
        "  --step-cooldown <秒>    两次降档最短间隔 (默认 15)\n"
        "  --step <W>              每次升降档幅度 (默认 5)\n"
        "  --hold-down <秒>        供电缺口持续多久降档 (默认 5)\n"
        "  --hold-up <秒>          供电稳定多久回升 (默认 300)\n"
        "  --profiles <文件>       分游戏参数文件 (默认 profiles.ini)\n"
        "  --no-game-profiles      不加载分游戏参数\n"
        "  --apply-always          不等待游戏启动, 立即应用\n"
        "  --no-gpu / --no-cpu     关闭对应控制\n"
        "  --dry-run               只记录动作, 不修改系统\n"
        "  --restore               恢复上次异常退出保存的原始值\n"
        "  --restore-if-stale      仅当状态文件属于已退出的进程时恢复(登录任务用)\n"
        "  --diagnose              探测本机能力: GPU 杠杆/CPU/电池信号 (只读)\n"
        "  --diagnose-validate     同上, 并实测功耗上限/锁频是否可写 (需管理员)\n"
        "  --web                   启动本机 Web 控制台 (http://127.0.0.1:8080)\n"
        "  --web-port <端口>       指定 Web 端口\n"
        "  --help                  显示本帮助\n"
        "\n"
        "提示: GPU 限功耗需要管理员权限, 并以管理员身份运行; 普通权限下只控制 CPU。\n");
}

} // namespace

PowerGuardApp::PowerGuardApp(int argc, char** argv) {
    // 同时支持 "--opt value" 和 "--opt=value"
    auto matchOpt = [&](int& idx, const std::string& a, const char* name,
                        std::string& value) -> bool {
        std::string p = std::string("--") + name;
        if (a == p) {
            if (idx + 1 < argc) {
                value = argv[++idx];
            } else {
                fprintf(stderr, "[WARN] %s 需要参数\n", p.c_str());
            }
            return true;
        }
        if (a.rfind(p + "=", 0) == 0) {
            value = a.substr(p.size() + 1);
            return true;
        }
        return false;
    };
    auto parseInt = [](const std::string& s, int def) -> int {
        if (s.empty()) return def;
        char* end = nullptr;
        long v = std::strtol(s.c_str(), &end, 10);
        return (end && *end == '\0') ? (int)v : def;
    };
    auto parseDouble = [](const std::string& s, double def) -> double {
        if (s.empty()) return def;
        char* end = nullptr;
        double v = std::strtod(s.c_str(), &end);
        return (end && *end == '\0') ? v : def;
    };

    bool gameProfilesEnabled = true;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        std::string v;

        if (a == "--only-running") {
            onlyRunning_ = true;
        } else if (a == "--dry-run") {
            profile_.dryRun = true;
        } else if (a == "--restore") {
            restoreOnly_ = true;
        } else if (a == "--restore-if-stale") {
            restoreIfStale_ = true;
        } else if (a == "--diagnose") {
            diagnose_ = true;
        } else if (a == "--diagnose-validate") {
            diagnose_ = true;
            diagnoseValidate_ = true;
        } else if (a == "--apply-always") {
            profile_.applyAlways = true;
        } else if (a == "--web") {
            webPort_ = 8080;
        } else if (matchOpt(i, a, "web-port", v)) {
            webPort_ = parseInt(v, 8080);
        } else if (a == "--no-gpu") {
            profile_.controlGpu = false;
        } else if (a == "--no-cpu") {
            profile_.controlCpu = false;
        } else if (a == "--help" || a == "-h") {
            printUsage();
            std::exit(0);
        } else if (matchOpt(i, a, "process", v)) {
            target_ = v;
        } else if (matchOpt(i, a, "profile", v)) {
            if (v == "usbc") {
                profile_.enabled = true;
            } else if (!v.empty()) {
                fprintf(stderr, "[WARN] 未知 profile: %s (目前只支持 usbc)\n", v.c_str());
            }
        } else if (matchOpt(i, a, "gpu-limit", v)) {
            profile_.gpuLimitW = parseInt(v, profile_.gpuLimitW);
        } else if (matchOpt(i, a, "min-gpu", v)) {
            profile_.minGpuW = parseInt(v, profile_.minGpuW);
        } else if (matchOpt(i, a, "max-gpu", v)) {
            profile_.maxGpuW = parseInt(v, profile_.maxGpuW);
        } else if (matchOpt(i, a, "cpu-max", v)) {
            profile_.cpuMaxPct = parseInt(v, profile_.cpuMaxPct);
        } else if (matchOpt(i, a, "cpu-min", v)) {
            profile_.cpuMinPct = parseInt(v, profile_.cpuMinPct);
        } else if (matchOpt(i, a, "cpu-step", v)) {
            profile_.cpuStepPct = parseInt(v, profile_.cpuStepPct);
        } else if (a == "--no-cpu-step") {
            profile_.cpuPowerStep = false;
        } else if (a == "--prefer-cpu") {
            profile_.preferCpu = true;
        } else if (matchOpt(i, a, "gpu-clock", v)) {
            profile_.gpuClockMhz = parseInt(v, profile_.gpuClockMhz);
        } else if (matchOpt(i, a, "min-gpu-clock", v)) {
            profile_.minGpuClockMhz = parseInt(v, profile_.minGpuClockMhz);
        } else if (matchOpt(i, a, "gpu-clock-pct", v)) {
            profile_.gpuClockPct = parseInt(v, profile_.gpuClockPct);
        } else if (matchOpt(i, a, "min-gpu-clock-pct", v)) {
            profile_.minGpuClockPct = parseInt(v, profile_.minGpuClockPct);
        } else if (matchOpt(i, a, "auto-clock-pct", v)) {
            profile_.autoClockPct = parseInt(v, profile_.autoClockPct);
        } else if (matchOpt(i, a, "clock-step", v)) {
            profile_.clockStepMhz = parseInt(v, profile_.clockStepMhz);
        } else if (a == "--no-gpu-clock") {
            profile_.gpuClockAuto = false;
        } else if (matchOpt(i, a, "battery-threshold", v)) {
            profile_.batteryThresholdW = parseDouble(v, profile_.batteryThresholdW);
        } else if (matchOpt(i, a, "deficit-energy", v)) {
            profile_.deficitEnergyWh = parseDouble(v, profile_.deficitEnergyWh);
        } else if (matchOpt(i, a, "deficit-window", v)) {
            profile_.deficitWindowSec = parseInt(v, profile_.deficitWindowSec);
        } else if (matchOpt(i, a, "step-cooldown", v)) {
            profile_.stepCooldownSec = parseInt(v, profile_.stepCooldownSec);
        } else if (matchOpt(i, a, "profiles", v)) {
            profilePath_ = v;
        } else if (a == "--no-game-profiles") {
            gameProfilesEnabled = false;
        } else if (matchOpt(i, a, "step", v)) {
            profile_.stepW = parseInt(v, profile_.stepW);
        } else if (matchOpt(i, a, "hold-down", v)) {
            profile_.holdDownSec = parseInt(v, profile_.holdDownSec);
        } else if (matchOpt(i, a, "hold-up", v)) {
            profile_.holdUpSec = parseInt(v, profile_.holdUpSec);
        } else {
            fprintf(stderr, "[WARN] 未识别的参数: %s\n", a.c_str());
        }
    }

    if (!gameProfilesEnabled) {
        profilePath_.clear();
    } else if (profilePath_.empty()) {
        profilePath_ = "profiles.ini";
    }

    normalizeProfile(profile_);
    baseProfile_ = profile_;
    monitor_.setTarget(target_);
}

void PowerGuardApp::normalizeProfile(ControlProfile& p) {
    // 参数校验/归一下
    if (p.gpuLimitW < 5)   p.gpuLimitW = 5;
    if (p.gpuLimitW > 250) p.gpuLimitW = 250;
    if (p.minGpuW < 5)     p.minGpuW = 5;
    if (p.minGpuW > p.gpuLimitW) p.minGpuW = p.gpuLimitW;
    if (p.maxGpuW <= 0)    p.maxGpuW = p.gpuLimitW;
    if (p.maxGpuW < p.gpuLimitW) p.maxGpuW = p.gpuLimitW;
    if (p.cpuMaxPct < 5)   p.cpuMaxPct = 5;
    if (p.cpuMaxPct > 100) p.cpuMaxPct = 100;
    if (p.cpuMinPct < 5)   p.cpuMinPct = 5;
    if (p.cpuMinPct > p.cpuMaxPct) p.cpuMinPct = p.cpuMaxPct;
    if (p.cpuStepPct < 1)  p.cpuStepPct = 1;
    if (p.cpuStepPct > 50) p.cpuStepPct = 50;
    if (p.batteryThresholdW < 0) p.batteryThresholdW = 0;
    if (p.deficitEnergyWh < 0) p.deficitEnergyWh = 0;
    if (p.deficitWindowSec < 5) p.deficitWindowSec = 5;
    if (p.stepCooldownSec < 0) p.stepCooldownSec = 0;
    if (p.stepW < 1)       p.stepW = 1;
    if (p.holdDownSec < 1) p.holdDownSec = 1;
    if (p.holdUpSec < 1)   p.holdUpSec = 1;
    if (p.gpuClockMhz < 0) p.gpuClockMhz = 0;
    if (p.minGpuClockMhz < 100) p.minGpuClockMhz = 100;
    if (p.clockStepMhz < 10)    p.clockStepMhz = 10;
    if (p.autoClockMhz < p.minGpuClockMhz)
        p.autoClockMhz = p.minGpuClockMhz;
    if (p.gpuClockMhz > 0 && p.minGpuClockMhz > p.gpuClockMhz)
        p.minGpuClockMhz = p.gpuClockMhz;

    // 百分比时基: 1-100; 下限不能高于目标
    if (p.gpuClockPct < 0)     p.gpuClockPct = 0;
    if (p.gpuClockPct > 100)   p.gpuClockPct = 100;
    if (p.minGpuClockPct < 0)  p.minGpuClockPct = 0;
    if (p.minGpuClockPct > 100) p.minGpuClockPct = 100;
    if (p.autoClockPct < 0)    p.autoClockPct = 0;
    if (p.autoClockPct > 100)  p.autoClockPct = 100;
    if (p.gpuClockPct > 0 && p.minGpuClockPct > p.gpuClockPct)
        p.minGpuClockPct = p.gpuClockPct;
}

// ============================================================
// Web 控制台
// ============================================================

void PowerGuardApp::startWebServer() {
    if (webPort_ <= 0 || web_) return;

    web_ = std::make_unique<WebServer>(webPort_,
        [this]() {
            std::lock_guard<std::mutex> lk(webMu_);
            return webStatus_;
        },
        []() { return listProcesses(); },
        [this](const WebCommand& cmd) {
            std::lock_guard<std::mutex> lk(webMu_);
            if (webCommands_.size() < 32) webCommands_.push_back(cmd);
        });

    if (web_->start())
        LOG_INFO("web", "Web 控制台: http://127.0.0.1:%d (仅本机可访问)", webPort_);
    else {
        LOG_ERROR("web", "Web 控制台启动失败 (端口 %d 可能被占用), 其余功能不受影响",
                  webPort_);
        web_.reset();
    }
}

void PowerGuardApp::stopWebServer() {
    if (!web_) return;
    web_->stop();
    web_.reset();
}

void PowerGuardApp::processWebCommands() {
    std::vector<WebCommand> cmds;
    {
        std::lock_guard<std::mutex> lk(webMu_);
        cmds.swap(webCommands_);
    }

    for (const auto& cmd : cmds) {
        switch (cmd.kind) {
            case WebCommand::SetTarget:
                target_ = cmd.target;
                monitor_.setTarget(cmd.target);
                profileGame_.clear();        // 换目标后重新匹配分游戏参数
                activeGameProfile_.clear();
                tracker_.reset();
                LOG_INFO("web", "监控目标已切换为 %s",
                         monitor_.configured() ? monitor_.targetDisplay().c_str() : "(无)");
                break;
            case WebCommand::SetConfig:
                controlPaused_ = false;      // 用户改参数 = 想用新参数
                applyConfigChange(cmd.config);
                break;
            case WebCommand::Apply:
                controlPaused_ = false;
                if (profile_.enabled && !controlApplied_) applyControl();
                break;
            case WebCommand::Restore:
                if (profile_.enabled && controlApplied_) restoreControl();
                controlPaused_ = true;       // 恢复后不再自动套用, 直到再次应用
                LOG_INFO("web", "已暂停自动控制, 点\"立即应用\"可恢复");
                break;
            default:
                break;
        }
    }
}

void PowerGuardApp::applyConfigChange(const ConfigChange& c) {
    // 先还原旧值, 避免新的 max 小于旧的 applied 时状态文件记错原始值
    if (controlApplied_) restoreControl();

    // 网页上改的参数同时成为“基底”，否则换游戏重载 profiles.ini 时会被打回原样
    auto apply = [&](ControlProfile& p) {
        if (c.gpuLimitW >= 0)         p.gpuLimitW = c.gpuLimitW;
        if (c.maxGpuW >= 0)           p.maxGpuW = c.maxGpuW;
        if (c.minGpuW >= 0)           p.minGpuW = c.minGpuW;
        if (c.cpuMaxPct >= 0)         p.cpuMaxPct = c.cpuMaxPct;
        if (c.cpuMinPct >= 0)         p.cpuMinPct = c.cpuMinPct;
        if (c.cpuStepPct >= 0)        p.cpuStepPct = c.cpuStepPct;
        if (c.cpuPowerStep >= 0)      p.cpuPowerStep = c.cpuPowerStep != 0;
        if (c.preferCpu >= 0)         p.preferCpu = c.preferCpu != 0;
        if (c.batteryThresholdW >= 0) p.batteryThresholdW = c.batteryThresholdW;
        if (c.deficitEnergyWh >= 0)   p.deficitEnergyWh = c.deficitEnergyWh;
        if (c.deficitWindowSec >= 0)  p.deficitWindowSec = c.deficitWindowSec;
        if (c.stepCooldownSec >= 0)   p.stepCooldownSec = c.stepCooldownSec;
        if (c.stepW >= 0)             p.stepW = c.stepW;
        if (c.holdDownSec >= 0)       p.holdDownSec = c.holdDownSec;
        if (c.holdUpSec >= 0)         p.holdUpSec = c.holdUpSec;
        // 网页给的是绝对 MHz 时，清掉对应的百分比，免得重载时被百分比覆盖
        if (c.gpuClockMhz >= 0) {
            p.gpuClockMhz = c.gpuClockMhz;
            p.gpuClockPct = 0;
        }
        if (c.minGpuClockMhz >= 0) {
            p.minGpuClockMhz = c.minGpuClockMhz;
            p.minGpuClockPct = 0;
        }
        if (c.clockStepMhz >= 0)      p.clockStepMhz = c.clockStepMhz;
        if (c.autoClockMhz >= 0) {
            p.autoClockMhz = c.autoClockMhz;
            p.autoClockPct = 0;
        }
        if (c.dryRun >= 0)            p.dryRun = c.dryRun != 0;
        if (c.applyAlways >= 0)       p.applyAlways = c.applyAlways != 0;
    };
    apply(profile_);
    apply(baseProfile_);
    normalizeProfile(profile_);
    normalizeProfile(baseProfile_);

    LOG_INFO("web", "参数已更新: GPU %dW / 锁频 %dMHz, CPU %d%% (下限 %d%%, 步进 %d%%), "
                    "电池阈值 %.1fW / 缺口 %.2fWh, 步进 %dW/%dMHz, 等待 %ds/%ds%s",
             profile_.gpuLimitW, profile_.gpuClockMhz, profile_.cpuMaxPct,
             profile_.cpuMinPct, profile_.cpuStepPct,
             profile_.batteryThresholdW, profile_.deficitEnergyWh,
             profile_.stepW, profile_.clockStepMhz,
             profile_.holdDownSec, profile_.holdUpSec,
             profile_.dryRun ? " [DRY-RUN]" : "");
}

void PowerGuardApp::publishWebStatus(double c, double g, const BatterySample& bat,
                                     bool targetRunning) {
    WebStatus s;
    s.cpuW = c;
    s.gpuW = g;

    s.batteryValid = bat.valid;
    if (bat.valid) {
        s.batteryW    = bat.rateW;
        s.batteryPct  = bat.percent;
        s.acOnline    = bat.acOnline ? 1 : 0;
        s.discharging = bat.discharging ? 1 : 0;
        s.charging    = bat.charging ? 1 : 0;
    }

    s.profileEnabled = profile_.enabled;
    s.dryRun         = profile_.dryRun;
    s.applyAlways    = profile_.applyAlways;
    s.controlApplied = controlApplied_;
    s.paused         = controlPaused_;
    s.gpuControlOk   = gpuControlOk_;
    s.cpuControlOk   = cpuControlOk_;
    s.usedGpuClock   = usedGpuClock_;

    s.gpuLimitW   = (gpuControlOk_ && !usedGpuClock_) ? appliedGpuW_ : gpu_.powerLimitW();
    s.cpuMaxPct   = cpuControlOk_ ? cpuLimit_.maxState() : readCpuMaxState();
    s.gpuClockMhz = usedGpuClock_ ? appliedGpuClockMhz_ : -1;

    s.appliedGpuW        = appliedGpuW_;
    s.appliedCpuPct      = appliedCpuPct_;
    s.appliedGpuClockMhz = appliedGpuClockMhz_;

    s.target      = monitor_.targetDisplay();
    s.gameProfile = activeGameProfile_;
    s.gameRunning = targetRunning;
    s.gamePid     = monitor_.pid();

    s.cfgGpuLimitW      = profile_.gpuLimitW;
    s.cfgCpuMaxPct      = profile_.cpuMaxPct;
    s.gpuClockTargetMhz = profile_.gpuClockMhz;
    s.minGpuW           = profile_.minGpuW;
    s.maxGpuW           = profile_.maxGpuW;
    s.stepW             = profile_.stepW;
    s.holdDownSec       = profile_.holdDownSec;
    s.holdUpSec         = profile_.holdUpSec;
    s.cpuMinPct         = profile_.cpuMinPct;
    s.minGpuClockMhz    = profile_.minGpuClockMhz;
    s.clockStepMhz      = profile_.clockStepMhz;
    s.autoClockMhz      = profile_.autoClockMhz;
    s.batteryThresholdW = profile_.batteryThresholdW;
    s.gpuClockAuto      = profile_.gpuClockAuto;
    s.cpuStepPct        = profile_.cpuStepPct;
    s.cpuPowerStep      = profile_.cpuPowerStep;
    s.preferCpu         = profile_.preferCpu;
    s.deficitEnergyWh   = profile_.deficitEnergyWh;
    s.deficitWindowSec  = profile_.deficitWindowSec;
    s.stepCooldownSec   = profile_.stepCooldownSec;

    s.webPort   = webPort_;
    s.uptimeSec = (GetTickCount64() - startTick_) / 1000;

    std::lock_guard<std::mutex> lk(webMu_);
    webStatus_ = s;
}

int PowerGuardApp::run() {
    // 能力探测是独立工具: 不碰控制、不占单实例锁、不初始化日志文件
    if (diagnose_) return runDiagnose(diagnoseValidate_);

    Logger::instance().init("logs", Level::Info);
    startTick_ = GetTickCount64();
    LOG_INFO("core", "PowerGuard 启动");
    LOG_DEBUG("core", "日志等级已设为 DEBUG 时可看到逐次采样");

    // 单实例: 两个实例会互相覆盖状态文件, 恢复正常值就全乱了。
    // --restore / --restore-if-stale 只是恢复工具, 不占这个锁。
    HANDLE instanceMutex = nullptr;
    if (!restoreOnly_ && !restoreIfStale_) {
        instanceMutex = CreateMutexW(nullptr, TRUE, L"Local\\PowerGuard.SingleInstance");
        if (!instanceMutex || GetLastError() == ERROR_ALREADY_EXISTS) {
            LOG_ERROR("core", "已有 PowerGuard 实例在运行, 本实例退出 (避免状态文件互相覆盖)");
            if (instanceMutex) CloseHandle(instanceMutex);
            return 1;
        }
    }

    if (restoreOnly_) {
        LOG_INFO("ctl", "恢复模式: 读取上次保存的原始设置");
        gpu_.open();                       // GPU 失败也能继续恢复 CPU
        if (restoreFromSavedState()) {
            LOG_INFO("ctl", "恢复完成");
            return 0;
        }
        LOG_ERROR("ctl", "没有可恢复的状态, 或恢复失败");
        return 1;
    }

    // 登录任务用: 只在“状态文件确实属于已退出的进程”时恢复, 不打扰正在跑的实例。
    if (restoreIfStale_) {
        gpu_.open();
        ControlState st;
        if (!loadControlState(st)) {
            LOG_INFO("ctl", "没有残留状态, 无需恢复");
            return 0;
        }
        if (st.pid != 0 && st.pid != GetCurrentProcessId() && pidAlive(st.pid)) {
            LOG_INFO("ctl", "状态文件属于正在运行的 PID %lu, 跳过恢复", st.pid);
            return 0;
        }
        bool ok = restoreFromSavedState();
        return ok ? 0 : 1;
    }

    // 启用控制时 CSV 追加当前上限/动作列
    // battery_w 正=放电(供电缺口), 负=充电(供电余量)
    const char* header = profile_.enabled
        ? "time,cpu_w,gpu_w,battery_w,battery_pct,ac_online,discharging,running,gpu_limit_w,cpu_max_pct,gpu_clock_mhz,action,game"
        : "time,cpu_w,gpu_w,battery_w,battery_pct,ac_online,discharging,running,game";
    if (!csv_.init("logs", header))
        LOG_ERROR("csv", "CSV 文件初始化失败");

    if (monitor_.configured()) {
        LOG_INFO("proc", "监控目标进程: %s", monitor_.targetDisplay().c_str());
        if (!monitor_.refresh())               // 启动时先探测一次当前状态
            LOG_INFO("proc", "目标进程当前未运行, 等待启动");
    }
    if (onlyRunning_ && !monitor_.configured())
        LOG_WARN("core", "--only-running 需要配合 --process 使用, 已忽略");

    // 打开失败的具体原因由 GpuPower/CpuPower/BatteryPower 内部写入日志(含错误码)
    if (gpu_.open()) LOG_INFO("gpu", "NVML 就绪, GPU 功率可读");
    if (cpu_.open()) LOG_INFO("cpu", "Energy Meter 计数器就绪");
    if (battery_.open()) LOG_INFO("bat", "电池状态可读 (放电功率 / 剩余电量)");

    // 百分比时基要用 GPU 报告的最大频率, NVML 打开之后才能换算。
    // baseProfile_ 不在这里换算: 每次 setProfileForGame 复制后都会重新算一遍。
    // 立即生效/无进程监控时 setProfileForGame 紧接着就会做, 这里不必重复。
    if (profile_.enabled && !(profile_.applyAlways || !monitor_.configured()))
        resolvePercentClocks(profile_);

    // 上次崩溃/被杀留下的限制不要等到游戏启动才清: 开机后系统就可能带着限频。
    if (profile_.enabled && !profile_.dryRun) {
        ControlState st;
        if (loadControlState(st) && st.pid != GetCurrentProcessId() && !pidAlive(st.pid)) {
            LOG_WARN("ctl", "发现上次异常退出留下的状态 (PID %lu), 启动时先恢复", st.pid);
            restoreFromSavedState();
        }
    }

    // 不监控进程 / 立即生效时, 先把 profiles.ini 的 [default] 段套上
    if (profile_.enabled && (profile_.applyAlways || !monitor_.configured()))
        setProfileForGame("");

    if (profile_.enabled) {
        if (!profilePath_.empty())
            LOG_INFO("ctl", "分游戏参数: %s (段名 = 进程名, 不含 .exe)",
                     profilePath_.c_str());
        LOG_INFO("ctl", "profile=usbc 启用%s: GPU %dW (范围 %d-%dW), CPU %d%% (最低 %d%%), "
                        "电池阈值 %.1fW, 降档 %dW/%ds, 回升 %dW/%ds, %s",
                 profile_.dryRun ? " [DRY-RUN]" : "",
                 profile_.gpuLimitW, profile_.minGpuW, profile_.maxGpuW,
                 profile_.cpuMaxPct, profile_.cpuMinPct,
                 profile_.batteryThresholdW, profile_.stepW, profile_.holdDownSec,
                 profile_.stepW, profile_.holdUpSec,
                 (profile_.applyAlways || !monitor_.configured())
                     ? "立即生效" : "仅目标进程运行时生效");

        LOG_INFO("ctl", "降档杠杆: %s优先%s, CPU 步进 %d%%",
                 profile_.preferCpu ? "CPU" : "GPU",
                 profile_.cpuPowerStep ? "" : " (CPU 不参与降档)",
                 profile_.cpuStepPct);

        if (profile_.controlGpu) {
            if (profile_.gpuClockMhz > 0)
                LOG_INFO("gpu", "GPU 锁频模式: 目标 %dMHz (下限 %dMHz, 步进 %dMHz)",
                         profile_.gpuClockMhz, profile_.minGpuClockMhz,
                         profile_.clockStepMhz);
            else if (gpu_.hasPowerLimitControl()) {
                int def = gpu_.defaultPowerLimitW();
                int mn  = gpu_.minPowerLimitW();
                int mx  = gpu_.maxPowerLimitW();
                if (def >= 0)
                    LOG_INFO("gpu", "GPU 功耗上限: 当前 %dW, 默认 %dW, 范围 %d-%dW",
                             gpu_.powerLimitW(), def, mn, mx);
            } else if (profile_.gpuClockAuto && gpu_.hasClockControl()) {
                LOG_INFO("gpu", "功耗上限不可写时, 将自动锁定 GPU 频率 %dMHz 起步",
                         profile_.autoClockMhz);
            }
        }
    }

    LOG_INFO("core", "输入 quit 并回车结束程序");

    // ---- 事件驱动 ----
    // 主线程: 等 quitEvent(超时 1s) 就采集一次; 输入线程: 读到 quit 就置事件
    quitEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!quitEvent_) {
        LOG_ERROR("core", "CreateEventW 失败, GetLastError=%lu", (unsigned long)GetLastError());
        if (instanceMutex) CloseHandle(instanceMutex);
        return 1;
    }

    // 关窗口 / 注销 / 关机时同步恢复限制 (正常情况下主循环来不及跑完)
    g_appForCtrl = this;
    SetConsoleCtrlHandler(consoleCtrlHandler, TRUE);

    startWebServer();
    std::thread inputThread(&PowerGuardApp::inputLoop, this);

    printf("> ");
    fflush(stdout);

    while (WaitForSingleObject(quitEvent_, 1000) == WAIT_TIMEOUT)
        sampleOnce();

    stopWebServer();
    inputThread.detach();   // 读线程可能阻塞在 getline, 分离后随进程退出
    if (profile_.enabled && controlApplied_) {
        LOG_INFO("core", "退出前恢复控制设置");
        restoreControl();
    }
    LOG_INFO("core", "收到退出命令, PowerGuard 退出");

    SetConsoleCtrlHandler(consoleCtrlHandler, FALSE);
    g_appForCtrl = nullptr;
    CloseHandle(quitEvent_);
    quitEvent_ = nullptr;
    if (instanceMutex) CloseHandle(instanceMutex);
    return 0;
}

void PowerGuardApp::handleConsoleCtrl(DWORD type) {
    switch (type) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
            // 走正常退出路径, 让主循环恢复并删状态文件
            LOG_INFO("core", "收到 Ctrl+C, 准备恢复设置后退出");
            if (quitEvent_) SetEvent(quitEvent_);
            break;
        case CTRL_CLOSE_EVENT:
        case CTRL_LOGOFF_EVENT:
        case CTRL_SHUTDOWN_EVENT:
            LOG_WARN("core", "控制台关闭/注销/关机, 立即恢复设置");
            emergencyRestore();
            break;
        default:
            break;
    }
}

void PowerGuardApp::emergencyRestore() {
    shutdownRequested_ = true;
    if (quitEvent_) SetEvent(quitEvent_);
    std::lock_guard<std::recursive_mutex> lk(controlMu_);
    if (profile_.enabled && controlApplied_ && !profile_.dryRun)
        restoreControl();
}

void PowerGuardApp::resetProfileToBase() {
    profile_ = baseProfile_;
    normalizeProfile(profile_);
    activeGameProfile_.clear();
}

// 百分比 -> 实际 MHz (按 NVML 报告的最大核心频率)。>0 的百分比优先于绝对 MHz。
void PowerGuardApp::resolvePercentClocks(ControlProfile& p) {
    if (p.gpuClockPct <= 0 && p.minGpuClockPct <= 0 && p.autoClockPct <= 0) return;

    int maxMhz = gpu_.maxGpuClockMhz();
    if (maxMhz <= 0) {
        if (!percentWarned_) {
            LOG_WARN("gpu", "读不到 GPU 最大核心频率, 百分比锁频无法换算, 沿用绝对 MHz 值");
            percentWarned_ = true;
        }
        return;
    }

    auto toMhz = [maxMhz](int pct) -> int {
        long long mhz = (long long)maxMhz * pct / 100;
        return (int)(mhz / 10 * 10);            // 10MHz 对齐
    };

    if (p.gpuClockPct > 0) {
        int mhz = toMhz(p.gpuClockPct);
        if (mhz > 0) {
            LOG_INFO("gpu", "百分比锁频: 目标 %d%% x 最大 %dMHz = %dMHz",
                     p.gpuClockPct, maxMhz, mhz);
            p.gpuClockMhz = mhz;
        }
    }
    if (p.minGpuClockPct > 0) {
        int mhz = toMhz(p.minGpuClockPct);
        if (mhz > 0) {
            LOG_INFO("gpu", "百分比锁频: 下限 %d%% x 最大 %dMHz = %dMHz",
                     p.minGpuClockPct, maxMhz, mhz);
            p.minGpuClockMhz = mhz;
        }
    }
    if (p.autoClockPct > 0) {
        int mhz = toMhz(p.autoClockPct);
        if (mhz > 0) {
            LOG_INFO("gpu", "百分比锁频: 自动起始 %d%% x 最大 %dMHz = %dMHz",
                     p.autoClockPct, maxMhz, mhz);
            p.autoClockMhz = mhz;
        }
    }
}

void PowerGuardApp::setProfileForGame(const std::string& game) {
    profile_ = baseProfile_;
    activeGameProfile_.clear();

    std::string used;
    if (!profilePath_.empty() && applyGameProfile(profilePath_, game, profile_, used)) {
        activeGameProfile_ = used;
    }

    resolvePercentClocks(profile_);
    normalizeProfile(profile_);

    if (!activeGameProfile_.empty()) {
        std::string gpuText = profile_.gpuClockMhz > 0
            ? std::to_string(profile_.gpuClockMhz) + "MHz"
            : std::to_string(profile_.gpuLimitW) + "W";
        LOG_INFO("ctl", "已加载分游戏参数 [%s]%s%s: GPU %s, CPU %d%%, 降档优先 %s",
                 used.c_str(), game.empty() ? "" : " 对应 ",
                 game.empty() ? "" : (game + ".exe").c_str(),
                 gpuText.c_str(), profile_.cpuMaxPct,
                 profile_.preferCpu ? "CPU" : "GPU");
    }
}

void PowerGuardApp::inputLoop() {
    std::string line;
    while (std::getline(std::cin, line)) {
        // 去首尾空白并转小写, 兼容 "quit"/"QUIT"/" quit "
        auto notSpace = [](unsigned char ch) { return !std::isspace(ch); };
        line.erase(line.begin(), std::find_if(line.begin(), line.end(), notSpace));
        line.erase(std::find_if(line.rbegin(), line.rend(), notSpace).base(), line.end());
        for (auto& ch : line) ch = (char)std::tolower((unsigned char)ch);

        if (line == "quit") {
            SetEvent(quitEvent_);
            return;
        }
        if (!line.empty())
            LOG_WARN("input", "未知命令 \"%s\", 输入 quit 退出", line.c_str());

        printf("> ");
        fflush(stdout);
    }
    // 输入流结束(EOF/Ctrl+Z)也退出, 避免程序卡在等待
    LOG_INFO("input", "输入流已结束, 自动退出");
    SetEvent(quitEvent_);
}

void PowerGuardApp::sampleOnce() {
    processWebCommands();

    bool targetRunning = monitor_.refresh();   // 同时检测启动/退出/重启并写日志
    if (monitor_.changed())
        tracker_.reset();                      // 状态变化后强制记录一条基线

    // 供电状态: 插电却在放电，说明输入功率不足（USB-C 100W 场景）
    BatterySample bat = battery_.read();
    if (bat.valid) {
        if (!haveBatteryState_ || bat.acOnline != lastAcOnline_ ||
            bat.discharging != lastDischarging_) {
            tracker_.reset();                  // 供电状态变化也强制记录一条基线
            haveBatteryState_ = true;
            lastAcOnline_ = bat.acOnline;
            lastDischarging_ = bat.discharging;
        }
        if (bat.acOnline && bat.discharging && bat.rateW > 5.0) {
            ULONGLONG now = GetTickCount64();
            if (!lastBatWarn_ || now - lastBatWarn_ >= 10000) {
                LOG_WARN("bat", "插电但电池放电 %.1fW (剩余 %d%%)，输入功率不足",
                         bat.rateW, bat.percent);
                lastBatWarn_ = now;
            }
        }
    }

    // 闭环控制必须在 --only-running 提前返回之前执行:
    // 游戏退出时也要能恢复设置。
    updateControl(targetRunning, bat);

    double c = cpu_.read();
    double g = gpu_.read();
    publishWebStatus(c, g, bat, targetRunning);

    // --only-running: 目标不在跑就不记录功率;
    // 但 apply/restore 动作要写一行, 方便对时间轴。
    bool actionRow = forceCsv_;
    if (onlyRunning_ && monitor_.configured() && !targetRunning && !actionRow) {
        LOG_DEBUG("proc", "目标进程未运行, 跳过本次记录");
        return;
    }

    if (c < 0 || g < 0) {
        LOG_DEBUG("power", "本次采样不完整(CPU=%.2f, GPU=%.2f), 跳过 CSV", c, g);
        return;
    }

    if (forceCsv_) {
        tracker_.reset();                      // 动作行强制写入
        forceCsv_ = false;
    }

    if (tracker_.shouldWrite(c, g)) {
        int running = monitor_.configured() ? (targetRunning ? 1 : 0) : -1;
        LOG_DEBUG("power", "写入 CSV: CPU=%.2fW GPU=%.2fW running=%d", c, g, running);

        if (profile_.enabled) {
            // 优先记录驱动记录的请求上限; 读不到且我们正在控制时记录设定值;
            // 都没有才退回 enforced (含 Dynamic Boost, 仅作参考)。
            int gpuLimit = gpu_.requestedPowerLimitW();
            if (gpuLimit < 0)
                gpuLimit = gpuControlOk_ ? appliedGpuW_ : gpu_.enforcedPowerLimitW();
            int cpuMax   = cpuLimit_.maxState();
            int gpuClock = usedGpuClock_ ? appliedGpuClockMhz_ : -1;
            std::string action = pendingAction_.empty() ? "-" : pendingAction_;
            std::string game = monitor_.matched().empty() ? "-" : monitor_.matched();

            if (bat.valid) {
                csv_.writef("%s,%.2f,%.2f,%.2f,%d,%d,%d,%d,%d,%d,%d,%s,%s",
                            nowTime().c_str(), c, g, bat.rateW, bat.percent,
                            bat.acOnline ? 1 : 0, bat.discharging ? 1 : 0,
                            running, gpuLimit, cpuMax, gpuClock, action.c_str(),
                            game.c_str());
            } else {
                csv_.writef("%s,%.2f,%.2f,-1.00,-1,-1,-1,%d,%d,%d,%d,%s,%s",
                            nowTime().c_str(), c, g, running,
                            gpuLimit, cpuMax, gpuClock, action.c_str(), game.c_str());
            }
        } else {
            std::string game = monitor_.matched().empty() ? "-" : monitor_.matched();
            if (bat.valid) {
                csv_.writef("%s,%.2f,%.2f,%.2f,%d,%d,%d,%d,%s",
                            nowTime().c_str(), c, g, bat.rateW, bat.percent,
                            bat.acOnline ? 1 : 0, bat.discharging ? 1 : 0, running,
                            game.c_str());
            } else {
                csv_.writef("%s,%.2f,%.2f,-1.00,-1,-1,-1,%d,%s",
                            nowTime().c_str(), c, g, running, game.c_str());
            }
        }
        pendingAction_.clear();
    } else {
        LOG_DEBUG("power", "波动小, 跳过写入: CPU=%.2fW GPU=%.2fW", c, g);
    }
}

bool PowerGuardApp::applyGpuClock(int mhz) {
    if (!gpu_.hasClockControl()) {
        LOG_WARN("ctl", "NVML 不支持锁定 GPU 频率, 无法限制 GPU");
        return false;
    }
    if (profile_.dryRun) {
        // 演练模式不写系统, 也就不要求管理员权限
        LOG_INFO("ctl", "[DRY-RUN] 将 GPU 核心频率锁定为 %dMHz (当前 %dMHz)",
                 mhz, gpu_.currentGpuClockMhz());
    } else if (!gpu_.isElevated()) {
        LOG_WARN("ctl", "锁定 GPU 频率需要管理员权限");
        return false;
    } else if (!gpu_.setGpuClockLock((unsigned)mhz)) {
        return false;
    }
    usedGpuClock_ = true;
    gpuControlOk_ = true;
    appliedGpuClockMhz_ = mhz;
    return true;
}

void PowerGuardApp::applyControl() {
    std::lock_guard<std::recursive_mutex> lk(controlMu_);
    if (!profile_.enabled || controlApplied_) return;

    // 上次异常退出留下的状态: 先恢复, 避免把原始值覆盖掉
    if (!profile_.dryRun) {
        ControlState old;
        if (loadControlState(old)) {
            if (old.pid != GetCurrentProcessId() && pidAlive(old.pid)) {
                LOG_WARN("ctl", "另一个 PowerGuard 实例 (PID %lu) 正在控制, 本实例不应用",
                         old.pid);
                return;
            }
            LOG_WARN("ctl", "发现上次异常退出留下的状态文件 (PID %lu), 先恢复原始设置",
                     old.pid);
            if (!restoreFromSavedState()) {
                // 继续应用会用“被限制后的值”覆盖原始值，状态文件也再也恢复不了。
                // 宁可暂停，也不要把原始值丢了。
                LOG_ERROR("ctl", "遗留状态恢复失败, 为保护原始值本次不应用; "
                                 "请先以管理员身份运行 --restore");
                controlPaused_ = true;
                return;
            }
        }
    }

    controlApplied_ = true;
    deficitEnergyWh_ = 0;
    lastControlTick_ = 0;
    lastStepDown_ = 0;
    lastFloorWarn_ = 0;
    gpuControlOk_ = false;
    cpuControlOk_ = false;
    originalGpuW_ = -1;
    originalCpuPct_ = -1;
    appliedGpuW_ = -1;
    appliedCpuPct_ = -1;
    usedGpuClock_ = false;
    appliedGpuClockMhz_ = -1;

    // ---- GPU ----
    if (profile_.controlGpu) {
        bool done = false;

        if (profile_.gpuClockMhz > 0) {
            done = applyGpuClock(profile_.gpuClockMhz);        // 显式锁频
        } else if (!gpu_.hasPowerLimitControl()) {
            if (gpu_.hasLimitApi() && !gpu_.isElevated())
                LOG_WARN("ctl", "未提权, 无法限制 GPU (需以管理员身份运行)");
            else
                LOG_WARN("ctl", "NVML 不支持设置 GPU 功耗上限");
        } else {
            gpu_.saveOriginalPowerLimit();
            originalGpuW_ = gpu_.originalPowerLimitW();
            if (originalGpuW_ < 0) {
                LOG_WARN("ctl", "读取原始 GPU 功耗上限失败");
            } else if (profile_.dryRun) {
                LOG_INFO("ctl", "[DRY-RUN] 将 GPU 上限 %dW -> %dW",
                         originalGpuW_, profile_.gpuLimitW);
                gpuControlOk_ = true;
                appliedGpuW_ = originalGpuW_;
                done = true;
            } else if (gpu_.setPowerLimitW((unsigned)profile_.gpuLimitW)) {
                gpuControlOk_ = true;
                appliedGpuW_ = profile_.gpuLimitW;
                done = true;
            }
        }

        // 功耗上限写不进去(笔记本 TGP 常由 EC 托管, 返回 3)时, 退化为锁频
        if (!done && profile_.gpuClockMhz == 0 && profile_.gpuClockAuto
            && gpu_.hasClockControl() && gpu_.isElevated()) {
            LOG_WARN("ctl", "GPU 功耗上限不可用, 自动退化为锁定核心频率 %dMHz",
                     profile_.autoClockMhz);
            done = applyGpuClock(profile_.autoClockMhz);
        }

        if (!done && !gpuControlOk_)
            LOG_WARN("ctl", "GPU 控制不可用, 本局只做 CPU 控制");
    }

    // ---- CPU ----
    if (profile_.controlCpu) {
        if (!cpuLimit_.open()) {
            LOG_WARN("ctl", "CPU 电源设置不可用, 跳过 CPU 控制");
        } else {
            originalCpuPct_ = cpuLimit_.originalMaxState();
            if (profile_.dryRun) {
                LOG_INFO("ctl", "[DRY-RUN] 将 CPU 最大状态 %d%% -> %d%%",
                         originalCpuPct_, profile_.cpuMaxPct);
                cpuControlOk_ = true;
                appliedCpuPct_ = originalCpuPct_;
            } else if (cpuLimit_.setMaxState(profile_.cpuMaxPct)) {
                cpuControlOk_ = true;
                appliedCpuPct_ = profile_.cpuMaxPct;
            } else {
                LOG_WARN("ctl", "CPU 限频失败, 本局只做 GPU 控制");
            }
        }
    }

    if (!gpuControlOk_ && !cpuControlOk_) {
        if (profile_.dryRun) {
            LOG_INFO("ctl", "[DRY-RUN] GPU/CPU 均不可控制");
        } else {
            LOG_ERROR("ctl", "GPU/CPU 控制均失败, 本次未应用任何设置");
            controlApplied_ = false;
            return;
        }
    } else if (!profile_.dryRun) {
        ControlState st;
        st.pid = GetCurrentProcessId();
        st.gpuOriginalW = originalGpuW_;
        st.cpuOriginalPct = originalCpuPct_;
        st.gpuAppliedW = (gpuControlOk_ && !usedGpuClock_) ? appliedGpuW_ : -1;
        st.cpuAppliedPct = cpuControlOk_ ? appliedCpuPct_ : -1;
        st.gpuClockMhz = (gpuControlOk_ && usedGpuClock_) ? appliedGpuClockMhz_ : -1;
        st.timestamp = currentDate() + " " + nowTime();
        saveControlState(st);
    }

    std::string gpuText = !gpuControlOk_ ? "未控制"
        : usedGpuClock_ ? std::to_string(appliedGpuClockMhz_) + "MHz"
                        : std::to_string(appliedGpuW_) + "W";
    std::string cpuText = cpuControlOk_
        ? std::to_string(appliedCpuPct_) + "%" : "未控制";
    LOG_INFO("ctl", "控制已应用: GPU %s, CPU %s", gpuText.c_str(), cpuText.c_str());

    pendingAction_ = "apply";
    forceCsv_ = true;
    lastReapply_ = GetTickCount64();
}

void PowerGuardApp::restoreControl() {
    std::lock_guard<std::recursive_mutex> lk(controlMu_);
    if (!profile_.enabled || !controlApplied_) return;

    bool ok = true;
    if (!profile_.dryRun) {
        if (gpuControlOk_ && usedGpuClock_) {
            if (!gpu_.resetGpuClockLock()) ok = false;
        } else if (gpuControlOk_ && originalGpuW_ >= 0) {
            if (!gpu_.restorePowerLimit()) ok = false;
        }
        if (cpuControlOk_ && originalCpuPct_ >= 0) {
            if (!cpuLimit_.restore()) ok = false;
        }
        if (ok) {
            removeControlState();
        } else {
            LOG_ERROR("ctl", "恢复未完全成功, 保留状态文件, 可用 --restore 重试");
        }
    }

    std::string gpuText = !gpuControlOk_ ? "未控制"
        : usedGpuClock_ ? "解除锁频"
                        : std::to_string(originalGpuW_) + "W";
    std::string cpuText = originalCpuPct_ >= 0
        ? std::to_string(originalCpuPct_) + "%" : "未控制";

    cpuLimit_.close();
    controlApplied_ = false;
    gpuControlOk_ = false;
    cpuControlOk_ = false;
    appliedGpuW_ = -1;
    appliedCpuPct_ = -1;
    usedGpuClock_ = false;
    appliedGpuClockMhz_ = -1;
    batteryOverSince_ = 0;
    batteryClearSince_ = 0;
    deficitEnergyWh_ = 0;
    lastControlTick_ = 0;
    lastStepDown_ = 0;

    pendingAction_ = "restore";
    forceCsv_ = true;

    LOG_INFO("ctl", "控制已恢复: GPU %s, CPU %s", gpuText.c_str(), cpuText.c_str());
}

void PowerGuardApp::updateControl(bool targetRunning, const BatterySample& bat) {
    std::lock_guard<std::recursive_mutex> lk(controlMu_);
    if (!profile_.enabled) return;
    if (controlPaused_) return;            // 网页上手动恢复后暂停, 直到再次应用
    if (shutdownRequested_) return;        // 正在关机/注销, 不要再把限制套回去

    // 游戏切换: 先恢复旧游戏, 换上新游戏的 profiles.ini 参数, 再应用
    if (targetRunning && monitor_.configured()) {
        const std::string& game = monitor_.matched();
        if (game != profileGame_) {
            bool wasApplied = controlApplied_;
            if (wasApplied) restoreControl();
            setProfileForGame(game);
            profileGame_ = game;
            lastControlTick_ = 0;
            deficitEnergyWh_ = 0;
            if (wasApplied || profile_.applyAlways) applyControl();
            return;
        }
    }

    if (!controlApplied_) {
        lastControlTick_ = 0;
        if (profile_.applyAlways || !monitor_.configured() || targetRunning)
            applyControl();
        return;
    }

    // 游戏退出后恢复; --apply-always 时保持到程序退出
    if (!profile_.applyAlways && monitor_.configured() && !targetRunning) {
        restoreControl();
        if (!profileGame_.empty()) {
            profileGame_.clear();
            resetProfileToBase();
        }
        return;
    }

    if (profile_.dryRun) return;

    ULONGLONG now = GetTickCount64();

    // 每 10 秒校验一次, 防止厂商 EC/控制中心把上限改回去
    if (now - lastReapply_ >= 10000) {
        lastReapply_ = now;
        // 只用驱动记录的"请求上限"做校验: enforced 值含 Dynamic Boost,
        // 拿它比较会在每个周期都误判为"被外部修改"并反复回写。
        if (!usedGpuClock_ && gpuControlOk_ && appliedGpuW_ > 0) {
            int cur = gpu_.requestedPowerLimitW();
            if (cur > 0 && cur != appliedGpuW_) {
                LOG_WARN("ctl", "GPU 上限被外部改为 %dW, 重新设为 %dW", cur, appliedGpuW_);
                if (gpu_.setPowerLimitW((unsigned)appliedGpuW_)) {
                    pendingAction_ = "reapply";
                    forceCsv_ = true;
                } else {
                    gpuControlOk_ = false;
                    LOG_ERROR("ctl", "重新应用 GPU 上限失败, 本局停止 GPU 控制");
                }
            }
        }
        if (cpuControlOk_ && appliedCpuPct_ > 0) {
            int cur = cpuLimit_.maxState();
            if (cur > 0 && cur != appliedCpuPct_) {
                LOG_WARN("ctl", "CPU 最大状态被外部改为 %d%%, 重新设为 %d%%",
                         cur, appliedCpuPct_);
                if (cpuLimit_.setMaxState(appliedCpuPct_)) {
                    pendingAction_ = "reapply";
                    forceCsv_ = true;
                } else {
                    cpuControlOk_ = false;
                    LOG_ERROR("ctl", "重新应用 CPU 上限失败, 本局停止 CPU 控制");
                }
            }
        }
    }

    // 缺口能量积分: 电池读数本身是量化/跳变的, 单看瞬时功率容易误降档。
    // 持续缺口能量按 Wh 累积, 消失后按 deficit_window 指数衰减。
    double dt = 1.0;
    if (lastControlTick_ != 0) {
        dt = (double)(now - lastControlTick_) / 1000.0;
        if (dt < 0) dt = 0;
        if (dt > 5.0) dt = 5.0;          // 睡眠/卡顿后不补记一大段
    }
    lastControlTick_ = now;

    // 电池反馈只在插电时有效; 拔电/读不到电池时不做任何升降档
    if (!bat.valid || !bat.acOnline) {
        batteryOverSince_ = 0;
        batteryClearSince_ = 0;
        deficitEnergyWh_ = 0;
        return;
    }

    bool deficit = bat.discharging && bat.rateW >= profile_.batteryThresholdW;
    if (deficit) {
        deficitEnergyWh_ += bat.rateW * dt / 3600.0;
    } else {
        double tau = profile_.deficitWindowSec > 0 ? profile_.deficitWindowSec : 1.0;
        deficitEnergyWh_ *= std::exp(-dt / tau);
        if (deficitEnergyWh_ < 0.001) deficitEnergyWh_ = 0;
    }

    if (deficit) {
        batteryClearSince_ = 0;
        if (batteryOverSince_ == 0) {
            batteryOverSince_ = now;
        } else if (now - batteryOverSince_ >= (ULONGLONG)profile_.holdDownSec * 1000) {
            bool enough = deficitEnergyWh_ >= profile_.deficitEnergyWh;
            bool cooled = lastStepDown_ == 0 ||
                          now - lastStepDown_ >= (ULONGLONG)profile_.stepCooldownSec * 1000;

            if (enough && cooled) {
                auto stepGpu = [&]() -> bool {
                    if (usedGpuClock_ && appliedGpuClockMhz_ > profile_.minGpuClockMhz) {
                        int next = appliedGpuClockMhz_ - profile_.clockStepMhz;
                        // 杠杆效率: 锁频值明显高于实际运行频率时, 小步降频只是原地打转。
                        // 直接降到实际频率之下, 才能真的改变功耗。
                        int cur = gpu_.currentGpuClockMhz();
                        if (cur > 0 && cur + 50 < appliedGpuClockMhz_) {
                            int jump = cur - profile_.clockStepMhz;
                            if (jump < next) {
                                next = jump;
                                LOG_INFO("ctl", "实际频率 %dMHz 低于锁定值 %dMHz, 直接跳到 %dMHz",
                                         cur, appliedGpuClockMhz_, next);
                            }
                        }
                        if (next < profile_.minGpuClockMhz) next = profile_.minGpuClockMhz;
                        if (gpu_.setGpuClockLock((unsigned)next)) {
                            appliedGpuClockMhz_ = next;
                            LOG_WARN("ctl", "缺口能量 %.3fWh (补电 %.1fW), GPU 频率降到 %dMHz",
                                     deficitEnergyWh_, bat.rateW, next);
                            return true;
                        }
                    }
                    if (!usedGpuClock_ && gpuControlOk_ && appliedGpuW_ > profile_.minGpuW) {
                        int next = appliedGpuW_ - profile_.stepW;
                        if (next < profile_.minGpuW) next = profile_.minGpuW;
                        if (gpu_.setPowerLimitW((unsigned)next)) {
                            appliedGpuW_ = next;
                            LOG_WARN("ctl", "缺口能量 %.3fWh (补电 %.1fW), GPU 上限降到 %dW",
                                     deficitEnergyWh_, bat.rateW, next);
                            return true;
                        }
                    }
                    return false;
                };

                auto stepCpu = [&]() -> bool {
                    if (!profile_.cpuPowerStep || !cpuControlOk_ ||
                        appliedCpuPct_ <= profile_.cpuMinPct)
                        return false;
                    int next = appliedCpuPct_ - profile_.cpuStepPct;
                    if (next < profile_.cpuMinPct) next = profile_.cpuMinPct;
                    if (!cpuLimit_.setMaxState(next)) return false;
                    appliedCpuPct_ = next;
                    LOG_WARN("ctl", "缺口能量 %.3fWh (补电 %.1fW), CPU 最大状态降到 %d%%",
                             deficitEnergyWh_, bat.rateW, next);
                    return true;
                };

                // 默认 GPU 优先; profiles.ini 的 prefer=cpu 或网页开关可改成 CPU 优先。
                bool stepped = false;
                if (profile_.preferCpu) {
                    stepped = stepCpu();
                    if (!stepped) stepped = stepGpu();
                } else {
                    stepped = stepGpu();
                    if (!stepped) stepped = stepCpu();
                }

                if (stepped) {
                    pendingAction_ = "step_down";
                    forceCsv_ = true;
                    lastStepDown_ = now;
                    deficitEnergyWh_ = 0;     // 重新累积, 避免同一缺口连着狂降
                    LOG_INFO("ctl", "已降档, 下次降档至少等 %ds",
                             profile_.stepCooldownSec);
                }
            }

            if (!enough || !cooled) {
                LOG_DEBUG("ctl", "缺口 %.3fWh/%.2fWh, 冷却剩余 %ds, 暂不降档",
                          deficitEnergyWh_, profile_.deficitEnergyWh,
                          cooled ? 0 : (int)(((ULONGLONG)profile_.stepCooldownSec * 1000 -
                                              (now - lastStepDown_)) / 1000));
            } else {
                bool atFloor =
                    (usedGpuClock_ ? appliedGpuClockMhz_ <= profile_.minGpuClockMhz
                                   : (!gpuControlOk_ || appliedGpuW_ <= profile_.minGpuW)) &&
                    (!profile_.cpuPowerStep || !cpuControlOk_ ||
                     appliedCpuPct_ <= profile_.cpuMinPct);
                if (atFloor && (!lastFloorWarn_ || now - lastFloorWarn_ >= 60000)) {
                    LOG_WARN("ctl", "补电 %.1fW, 缺口 %.3fWh, 但已到降档下限 "
                                    "(GPU %s, CPU %s)", bat.rateW, deficitEnergyWh_,
                             usedGpuClock_ ? "锁频下限" : "下限",
                             profile_.cpuPowerStep ? "下限" : "未参与");
                    lastFloorWarn_ = now;
                }
            }
            batteryOverSince_ = now;      // 仍缺电则继续每 holdDownSec 检查一次
        }
    } else {
        batteryOverSince_ = 0;
        if (batteryClearSince_ == 0) {
            batteryClearSince_ = now;
        } else if (now - batteryClearSince_ >= (ULONGLONG)profile_.holdUpSec * 1000) {
            bool stepped = false;

            if (usedGpuClock_) {
                int target = profile_.gpuClockMhz > 0 ? profile_.gpuClockMhz
                                                      : profile_.autoClockMhz;
                if (appliedGpuClockMhz_ > 0 && appliedGpuClockMhz_ < target) {
                    int next = appliedGpuClockMhz_ + profile_.clockStepMhz;
                    if (next > target) next = target;
                    if (gpu_.setGpuClockLock((unsigned)next)) {
                        appliedGpuClockMhz_ = next;
                        stepped = true;
                        LOG_INFO("ctl", "供电稳定, GPU 频率回升到 %dMHz", next);
                    }
                }
            } else if (gpuControlOk_ && appliedGpuW_ < profile_.maxGpuW) {
                int next = appliedGpuW_ + profile_.stepW;
                if (next > profile_.maxGpuW) next = profile_.maxGpuW;
                if (gpu_.setPowerLimitW((unsigned)next)) {
                    appliedGpuW_ = next;
                    stepped = true;
                    LOG_INFO("ctl", "供电稳定, GPU 上限回升到 %dW", next);
                }
            }
            if (!stepped && profile_.cpuPowerStep && cpuControlOk_ &&
                appliedCpuPct_ < profile_.cpuMaxPct) {
                int next = appliedCpuPct_ + profile_.cpuStepPct;
                if (next > profile_.cpuMaxPct) next = profile_.cpuMaxPct;
                if (cpuLimit_.setMaxState(next)) {
                    appliedCpuPct_ = next;
                    stepped = true;
                    LOG_INFO("ctl", "供电稳定, CPU 最大状态回升到 %d%%", next);
                }
            }

            if (stepped) {
                pendingAction_ = "step_up";
                forceCsv_ = true;
            }
            batteryClearSince_ = now;
        }
    }
}

bool PowerGuardApp::restoreFromSavedState() {
    std::lock_guard<std::recursive_mutex> lk(controlMu_);
    ControlState st;
    if (!loadControlState(st)) {
        LOG_INFO("ctl", "没有找到状态文件: %s", controlStatePath().string().c_str());
        return false;
    }

    if (st.pid != 0 && st.pid != GetCurrentProcessId() && pidAlive(st.pid)) {
        LOG_WARN("ctl", "状态文件属于正在运行的 PID %lu, 跳过恢复", st.pid);
        return false;
    }

    bool ok = true;

    if (st.gpuAppliedW >= 0 && st.gpuOriginalW < 0) {
        LOG_WARN("ctl", "状态文件缺少 GPU 原始值, 无法安全恢复 GPU");
        ok = false;
    }
    if (st.cpuAppliedPct >= 0 && st.cpuOriginalPct < 0) {
        LOG_WARN("ctl", "状态文件缺少 CPU 原始值, 无法安全恢复 CPU");
        ok = false;
    }

    // 只有确实改过 GPU 才恢复, 避免动到未控制的设置
    if (st.gpuClockMhz >= 0) {
        if (!gpu_.hasClockControl() || !gpu_.isElevated()) {
            LOG_WARN("ctl", "无法解除 GPU 锁频 (未提权或驱动不支持), 请以管理员身份重试");
            ok = false;
        } else if (!gpu_.resetGpuClockLock()) {
            ok = false;
        }
    } else if (st.gpuOriginalW >= 0 && st.gpuAppliedW >= 0) {
        if (!gpu_.hasPowerLimitControl()) {
            LOG_WARN("ctl", "NVML 不支持设置 GPU 上限, 无法恢复 GPU");
            ok = false;
        } else if (!gpu_.setPowerLimitW((unsigned)st.gpuOriginalW)) {
            ok = false;
        }
    }

    if (st.cpuOriginalPct >= 0 && st.cpuAppliedPct >= 0) {
        if (!cpuLimit_.open()) {
            ok = false;
        } else if (!cpuLimit_.setMaxState(st.cpuOriginalPct)) {
            ok = false;
        }
    }

    if (ok) {
        removeControlState();
        if (st.gpuClockMhz >= 0)
            LOG_INFO("ctl", "已恢复原始设置: GPU 解除锁频 (原锁 %dMHz), CPU %d%%",
                     st.gpuClockMhz, st.cpuOriginalPct);
        else
            LOG_INFO("ctl", "已恢复原始设置: GPU %dW, CPU %d%%",
                     st.gpuOriginalW, st.cpuOriginalPct);
    } else {
        LOG_ERROR("ctl", "恢复失败, 状态文件保留以便重试");
    }

    cpuLimit_.close();
    return ok;
}

} // namespace pg
