#include "control/state_file.h"

#include <windows.h>

#include <cstdlib>
#include <fstream>

#include "common/logger.h"

namespace pg {
namespace {

std::filesystem::path stateDir() {
    wchar_t buf[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH)
        return std::filesystem::path(std::wstring(buf, n)) / L"PowerGuard";
    return std::filesystem::path(L"logs");           // 环境变量异常时的兜底
}

} // namespace

std::filesystem::path controlStatePath() {
    return stateDir() / L"control_state.txt";
}

bool saveControlState(const ControlState& st) {
    std::error_code ec;
    auto dir = stateDir();
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        LOG_WARN("ctl", "创建状态目录失败: %s", ec.message().c_str());
        return false;
    }

    auto path = dir / L"control_state.txt";
    auto tmp  = dir / L"control_state.txt.tmp";

    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out) {
            LOG_WARN("ctl", "写入状态文件失败: %s", path.string().c_str());
            std::filesystem::remove(tmp, ec);
            return false;
        }
        out << "pid=" << st.pid << "\n"
            << "gpu_original_w=" << st.gpuOriginalW << "\n"
            << "cpu_original_pct=" << st.cpuOriginalPct << "\n"
            << "gpu_applied_w=" << st.gpuAppliedW << "\n"
            << "cpu_applied_pct=" << st.cpuAppliedPct << "\n"
            << "gpu_clock_mhz=" << st.gpuClockMhz << "\n"
            << "timestamp=" << st.timestamp << "\n";
    }

    // MoveFileEx 覆盖式替换: 避免"先删后改名"在两步之间留下没有任何备份的窗口。
    if (!MoveFileExW(tmp.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DWORD err = GetLastError();
        LOG_WARN("ctl", "替换状态文件失败 (GetLastError=%lu): %s",
                 (unsigned long)err, path.string().c_str());
        std::filesystem::remove(tmp, ec);
        return false;
    }
    return true;
}

bool loadControlState(ControlState& st) {
    std::ifstream in(controlStatePath());
    if (!in) return false;

    ControlState out;
    std::string line;
    while (std::getline(in, line)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);

        if (key == "pid")
            out.pid = std::strtoul(val.c_str(), nullptr, 10);
        else if (key == "gpu_original_w")
            out.gpuOriginalW = std::atoi(val.c_str());
        else if (key == "cpu_original_pct")
            out.cpuOriginalPct = std::atoi(val.c_str());
        else if (key == "gpu_applied_w")
            out.gpuAppliedW = std::atoi(val.c_str());
        else if (key == "cpu_applied_pct")
            out.cpuAppliedPct = std::atoi(val.c_str());
        else if (key == "gpu_clock_mhz")
            out.gpuClockMhz = std::atoi(val.c_str());
        else if (key == "timestamp")
            out.timestamp = val;
    }

    st = out;
    return st.pid != 0 || st.gpuOriginalW >= 0 || st.cpuOriginalPct >= 0;
}

bool removeControlState() {
    std::error_code ec;
    std::filesystem::remove(controlStatePath(), ec);
    if (ec) {
        LOG_WARN("ctl", "删除状态文件失败: %s", ec.message().c_str());
        return false;
    }
    return true;
}

bool pidAlive(unsigned long pid) {
    if (pid == 0) return false;

    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pid);
    if (!h) {
        // 可能是别的用户/受保护进程; 保守地认为还活着, 避免并发恢复
        return GetLastError() == ERROR_ACCESS_DENIED;
    }

    DWORD code = 0;
    bool alive = GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
    CloseHandle(h);
    return alive;
}

} // namespace pg
