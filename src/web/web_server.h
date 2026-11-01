// ============================================================
// WebServer —— 本地 Web 控制台
//   只监听 127.0.0.1, 提供:
//     GET  /                 内嵌的仪表盘页面
//     GET  /api/status       当前采样与控制状态 (JSON)
//     GET  /api/processes    进程列表, 用于选择游戏
//     POST /api/target       切换监控目标 {"name":"Game.exe"}
//     POST /api/config       修改限功耗参数
//     POST /api/apply        立即应用
//     POST /api/restore      恢复原始设置
//   不依赖任何第三方库, 只用 Winsock。
// ============================================================
#pragma once

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

#include "monitor/process_monitor.h"

namespace pg {

struct WebStatus {
    // ---- 实时采样 ----
    double cpuW = -1;
    double gpuW = -1;
    double batteryW = 0;
    int    batteryPct = -1;
    int    acOnline = -1;            // -1 = 电池信息不可用
    int    discharging = -1;
    int    charging = -1;
    bool   batteryValid = false;

    // ---- 控制状态 ----
    bool   profileEnabled = false;
    bool   dryRun = false;
    bool   applyAlways = false;
    bool   controlApplied = false;
    bool   paused = false;           // 网页上点过"恢复", 自动应用已暂停
    bool   gpuControlOk = false;
    bool   cpuControlOk = false;
    bool   usedGpuClock = false;     // GPU 走的是锁频而不是功耗上限
    int    gpuLimitW = -1;           // 当前生效的功耗上限
    int    cpuMaxPct = -1;           // 当前生效的最大处理器状态
    int    gpuClockMhz = -1;         // 当前锁定的频率
    int    appliedGpuW = -1;
    int    appliedCpuPct = -1;
    int    appliedGpuClockMhz = -1;

    // ---- 目标进程 ----
    std::string target;
    std::string gameProfile;         // 当前生效的分游戏参数段名, 空 = 未命中
    bool   gameRunning = false;
    DWORD  gamePid = 0;

    // ---- 可调参数 ----
    int    cfgGpuLimitW = 30;        // profile 里配置的目标上限
    int    cfgCpuMaxPct = 99;        // profile 里配置的最大处理器状态
    int    gpuClockTargetMhz = 0;
    int    minGpuW = 15;
    int    maxGpuW = 30;
    int    stepW = 5;
    int    holdDownSec = 5;
    int    holdUpSec = 300;
    int    cpuMinPct = 50;
    int    minGpuClockMhz = 800;
    int    clockStepMhz = 100;
    int    autoClockMhz = 1400;
    double batteryThresholdW = 2.0;
    bool   gpuClockAuto = true;
    int    cpuStepPct = 5;
    bool   cpuPowerStep = true;
    bool   preferCpu = false;        // 降档先动 CPU
    double deficitEnergyWh = 0.10;
    int    deficitWindowSec = 120;
    int    stepCooldownSec = 15;

    int    webPort = 0;
    unsigned long long uptimeSec = 0;
};

// 网页提交的配置修改; -1 / 负数 表示"保持原值"
struct ConfigChange {
    int    gpuLimitW = -1;
    int    maxGpuW = -1;
    int    cpuMaxPct = -1;
    int    cpuMinPct = -1;
    double batteryThresholdW = -1;
    int    stepW = -1;
    int    holdDownSec = -1;
    int    holdUpSec = -1;
    int    gpuClockMhz = -1;         // 0 = 关闭锁频模式
    int    minGpuClockMhz = -1;
    int    clockStepMhz = -1;
    int    autoClockMhz = -1;
    int    minGpuW = -1;
    int    cpuStepPct = -1;
    int    cpuPowerStep = -1;        // 0/1
    int    preferCpu = -1;           // 0/1: 降档先动 CPU
    double deficitEnergyWh = -1;
    int    deficitWindowSec = -1;
    int    stepCooldownSec = -1;
    int    dryRun = -1;              // 0/1
    int    applyAlways = -1;         // 0/1: 不等游戏启动, 立即生效
};

struct WebCommand {
    enum Kind { None, SetTarget, SetConfig, Apply, Restore };
    Kind          kind = None;
    std::string   target;
    ConfigChange  config;
};

class WebServer {
public:
    using StatusFn  = std::function<WebStatus()>;
    using ProcessFn = std::function<std::vector<ProcInfo>()>;
    using CommandFn = std::function<void(const WebCommand&)>;

    WebServer(int port, StatusFn status, ProcessFn processes, CommandFn command);
    ~WebServer();

    WebServer(const WebServer&) = delete;
    WebServer& operator=(const WebServer&) = delete;

    bool start();       // 绑定并启动后台线程; 失败已写日志
    void stop();        // 停止并关闭监听, 可重复调用
    int  port() const { return port_; }

private:
    void run();
    void handleClient(SOCKET client);
    void route(const std::string& method, const std::string& path,
               const std::string& body, std::string& out);

    int        port_;
    StatusFn   status_;
    ProcessFn  processes_;
    CommandFn  command_;

    unsigned long long listenSock_ = (unsigned long long)~0ull;  // SOCKET, 避免头文件里引 winsock
    HANDLE     thread_ = nullptr;
    volatile bool running_ = false;
};

} // namespace pg
