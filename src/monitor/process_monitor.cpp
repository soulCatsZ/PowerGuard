#include "monitor/process_monitor.h"

#include <tlhelp32.h>
#include <algorithm>
#include <cctype>

#include "common/logger.h"

namespace pg {

std::vector<ProcInfo> listProcesses() {
    std::vector<ProcInfo> out;

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            if (pe.th32ProcessID == 0 || pe.th32ProcessID == 4) continue;   // 系统进程
            char name[512] = {};
            // Web 页面用 UTF-8, 这里也转成 UTF-8 以免中文进程名乱码
            WideCharToMultiByte(CP_UTF8, 0, pe.szExeFile, -1, name, sizeof(name),
                                nullptr, nullptr);
            if (!name[0]) continue;
            out.push_back({name, pe.th32ProcessID});
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);

    std::sort(out.begin(), out.end(), [](const ProcInfo& a, const ProcInfo& b) {
        return _stricmp(a.name.c_str(), b.name.c_str()) < 0;
    });
    return out;
}

std::string ProcessMonitor::trimExe(std::string name) {
    auto notSpace = [](unsigned char c) { return !std::isspace(c); };
    name.erase(name.begin(), std::find_if(name.begin(), name.end(), notSpace));
    name.erase(std::find_if(name.rbegin(), name.rend(), notSpace).base(), name.end());
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    if (name.size() > 4 && name.compare(name.size() - 4, 4, ".exe") == 0)
        name.erase(name.size() - 4);
    return name;
}

void ProcessMonitor::setTarget(const std::string& target) {
    targets_.clear();

    size_t start = 0;
    while (start <= target.size()) {
        size_t sep = target.find_first_of(",;", start);
        std::string part = target.substr(start, sep == std::string::npos
                                                   ? std::string::npos : sep - start);
        std::string name = trimExe(part);
        if (!name.empty() &&
            std::find(targets_.begin(), targets_.end(), name) == targets_.end())
            targets_.push_back(name);
        if (sep == std::string::npos) break;
        start = sep + 1;
    }

    target_.clear();
    for (size_t i = 0; i < targets_.size(); ++i) {
        if (i) target_ += ',';
        target_ += targets_[i];
    }

    running_ = false;
    pid_ = 0;
    matched_.clear();
    changed_ = false;
}

std::string ProcessMonitor::targetDisplay() const {
    std::string out;
    for (size_t i = 0; i < targets_.size(); ++i) {
        if (i) out += ',';
        out += targets_[i] + ".exe";
    }
    return out;
}

bool ProcessMonitor::findProcess(DWORD& pid, std::string& matched) const {
    pid = 0;
    matched.clear();
    if (targets_.empty()) return false;

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        LOG_WARN("proc", "CreateToolhelp32Snapshot 失败, GetLastError=%lu",
                 (unsigned long)GetLastError());
        return false;
    }

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);

    bool found = false;
    if (Process32FirstW(snap, &pe)) {
        do {
            // MinGW 只声明宽字符版, 转成当前代码页的窄字符串再比较
            char name[512] = {};
            WideCharToMultiByte(CP_ACP, 0, pe.szExeFile, -1, name, sizeof(name), nullptr, nullptr);
            std::string norm = trimExe(name);
            if (std::find(targets_.begin(), targets_.end(), norm) != targets_.end()) {
                pid = pe.th32ProcessID;
                matched = norm;
                found = true;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return found;
}

bool ProcessMonitor::refresh() {
    if (targets_.empty()) return false;

    DWORD pid = 0;
    std::string matched;
    bool nowRunning = findProcess(pid, matched);
    changed_ = false;

    std::string last = matched_.empty() ? targetDisplay() : matched_ + ".exe";
    if (!running_ && nowRunning) {
        LOG_INFO("proc", "目标进程 %s 已启动 (PID %lu)",
                 (matched + ".exe").c_str(), (unsigned long)pid);
        changed_ = true;
    } else if (running_ && !nowRunning) {
        LOG_INFO("proc", "目标进程 %s 已退出 (原 PID %lu)",
                 last.c_str(), (unsigned long)pid_);
        changed_ = true;
    } else if (running_ && nowRunning && pid != pid_) {
        LOG_INFO("proc", "目标进程 %s 已重启 (PID %lu -> %lu)",
                 (matched + ".exe").c_str(), (unsigned long)pid_, (unsigned long)pid);
        changed_ = true;
    }

    running_ = nowRunning;
    pid_ = nowRunning ? pid : 0;
    if (nowRunning) matched_ = matched;
    return running_;
}

} // namespace pg
