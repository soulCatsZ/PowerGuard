// ============================================================
// ProcessMonitor —— 目标进程生命周期监控
//   按进程名(大小写不敏感, 可省略 .exe)检测启动 / 退出 / 重启,
//   事件写入日志; 对外提供当前是否运行及 PID。
//
//   支持多个名字: --process "bg3.exe,bg3_dx11.exe"
//   命中哪个名字会记录在 matched() 里, 供分游戏参数使用。
// ============================================================
#pragma once

#include <windows.h>
#include <string>
#include <vector>

namespace pg {

struct ProcInfo {
    std::string name;      // 进程名(含 .exe)
    DWORD       pid = 0;
};

// 枚举当前所有进程(按名字排序), 供 Web 控制台选择监控目标
std::vector<ProcInfo> listProcesses();

class ProcessMonitor {
public:
    ProcessMonitor() = default;

    // 设置监控目标; 支持逗号/分号分隔多个名字; 传空串表示不监控
    void setTarget(const std::string& target);

    bool configured() const { return !targets_.empty(); }
    // 规范化后的名字(小写、无 .exe), 多个用逗号连接
    const std::string& target() const { return target_; }
    // 每个名字补上 .exe, 供日志/网页显示
    std::string targetDisplay() const;
    // 当前命中的那个进程名(规范化, 无 .exe); 未运行时为上一次命中的名字
    const std::string& matched() const { return matched_; }

    // 枚举一次进程表, 检测状态变化并写日志; 返回目标当前是否在运行
    bool refresh();

    bool  running() const { return running_; }
    DWORD pid() const { return pid_; }
    bool  changed() const { return changed_; }   // 本次 refresh 是否发生了启动/退出/重启

private:
    static std::string trimExe(std::string name);   // 去首尾空白, 转小写并去掉 .exe
    bool findProcess(DWORD& pid, std::string& matched) const;

    std::vector<std::string> targets_;   // 规范化后的进程名
    std::string target_;                 // targets_ 的逗号拼接, 供显示
    std::string matched_;                // 最近一次命中的名字
    bool        running_ = false;
    DWORD       pid_ = 0;
    bool        changed_ = false;
};

} // namespace pg
