// ============================================================
// PowerGuardApp —— 业务主流程
//   初始化日志/CSV/采样器, 事件驱动循环: 每秒采样一次,
//   功率波动超过阈值时写 CSV, 输入线程读到 quit 结束程序。
// ============================================================
#pragma once

#include <windows.h>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "collectors/gpu_power.h"
#include "collectors/cpu_power.h"
#include "collectors/battery_power.h"
#include "common/logger.h"
#include "common/power_tracker.h"
#include "control/cpu_limit.h"
#include "control/profile.h"
#include "monitor/process_monitor.h"
#include "web/web_server.h"

namespace pg {

class PowerGuardApp {
public:
    // 解析命令行: --process <名字> 指定监控进程, --only-running 只在其运行时记录
    explicit PowerGuardApp(int argc, char** argv);

    int run();

    // 控制台事件(CTRL_C/CLOSE/LOGOFF/SHUTDOWN)由静态回调转进来
    void handleConsoleCtrl(DWORD type);

private:
    void inputLoop();    // 输入线程: 读终端命令, quit 时置事件
    void sampleOnce();   // 采样一次, 按阈值决定是否写 CSV

    // ---- Web 控制台 ----
    void startWebServer();
    void stopWebServer();
    void processWebCommands();            // 主线程消费网页提交的操作
    void applyConfigChange(const ConfigChange& c);
    void publishWebStatus(double cpuW, double gpuW, const BatterySample& bat,
                          bool targetRunning);

    // ---- USB-C 100W 闭环控制 ----
    void applyControl();                                  // 应用上限并保存原始值
    void restoreControl();                                // 恢复原始值并删除状态文件
    void updateControl(bool targetRunning, const BatterySample& bat);
    bool restoreFromSavedState();                         // 供 --restore / 崩溃后恢复
    void emergencyRestore();                              // 关机/关窗时同步恢复
    void setProfileForGame(const std::string& game);      // 应用 profiles.ini
    void resetProfileToBase();                            // 回到命令行/profile 默认值
    void resolvePercentClocks(ControlProfile& p);         // 百分比 -> 实际 MHz

    GpuPower           gpu_;
    CpuPower           cpu_;
    BatteryPower       battery_;
    CpuLimit           cpuLimit_;
    ControlProfile     profile_;
    ControlProfile     baseProfile_;      // 命令行/profile 默认值(分游戏参数的基底)
    std::string        profilePath_;      // profiles.ini 路径, 空 = 不加载
    std::string        profileGame_;      // 已加载分游戏参数的进程名
    std::string        activeGameProfile_;// 命中段名, 供网页显示
    CsvLog             csv_;
    // 写入去抖: 绝对 8.0W 或 相对 5% 任一满足即写, 且最多 60s 补记一次
    PowerChangeTracker tracker_{8.0, 0.05, 60000};
    ProcessMonitor     monitor_;
    std::string        target_;
    bool               onlyRunning_ = false;
    HANDLE             quitEvent_ = nullptr;

    // 电池状态变化 / 输入不足告警的节流
    bool               haveBatteryState_ = false;
    bool               lastAcOnline_ = false;
    bool               lastDischarging_ = false;
    ULONGLONG          lastBatWarn_ = 0;

    // 控制状态
    bool               controlApplied_ = false;
    bool               gpuControlOk_ = false;
    bool               cpuControlOk_ = false;
    int                originalGpuW_ = -1;
    int                originalCpuPct_ = -1;
    int                appliedGpuW_ = -1;
    int                appliedCpuPct_ = -1;

    // GPU 锁频模式 (功耗上限被厂商 EC 锁死时使用)
    bool               usedGpuClock_ = false;
    int                appliedGpuClockMhz_ = -1;
    bool               applyGpuClock(int mhz);   // 供 applyControl 复用
    bool               controlPaused_ = false;   // 网页点过"恢复"后暂停自动应用
    ULONGLONG          batteryOverSince_ = 0;
    ULONGLONG          batteryClearSince_ = 0;
    ULONGLONG          lastReapply_ = 0;
    ULONGLONG          lastControlTick_ = 0;  // 能量积分用的上一拍
    ULONGLONG          lastStepDown_ = 0;     // 降档冷却
    ULONGLONG          lastFloorWarn_ = 0;    // 降档下限日志节流
    double             deficitEnergyWh_ = 0;  // 缺口能量积分(Wh)
    double             cpuShare_ = -1;        // CPU 占 CPU+GPU 功率的比例(平滑), -1=未知
    double             lastCpuW_ = -1;        // 上一拍 CPU 功率, 预算模式用
    double             lastGpuW_ = -1;        // 上一拍 GPU 功率, 预算模式用
    std::string        pendingAction_;      // 写入下一行 CSV 的 action 列
    bool               forceCsv_ = false;
    bool               restoreOnly_ = false;   // --restore
    bool               restoreIfStale_ = false;// --restore-if-stale
    bool               diagnose_ = false;      // --diagnose
    bool               diagnoseValidate_ = false; // --diagnose-validate
    bool               percentWarned_ = false; // 最大频率读不到只警告一次
    bool               shutdownRequested_ = false; // 控制台关机/注销事件
    std::recursive_mutex controlMu_;        // 控制写入串行化(CTRL 回调 vs 主循环)

    // ---- Web 控制台 ----
    int                webPort_ = 0;         // 0 = 未启用
    ULONGLONG          startTick_ = 0;
    std::unique_ptr<WebServer> web_;
    std::mutex         webMu_;               // 保护 webStatus_ / webCommands_
    WebStatus          webStatus_;
    std::vector<WebCommand> webCommands_;
};

} // namespace pg
