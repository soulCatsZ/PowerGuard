#include "control/game_profile.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace pg {
namespace {

struct Section {
    std::string name;
    std::vector<std::pair<std::string, std::string>> kv;
};

std::string trim(std::string s) {
    auto notSpace = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
    s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
    return s;
}

std::string lower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

// 段名和进程名同规则: 小写, 去 .exe
std::string normName(std::string s) {
    s = lower(trim(s));
    if (s.size() > 4 && s.compare(s.size() - 4, 4, ".exe") == 0)
        s.erase(s.size() - 4);
    return s;
}

bool parseBool(const std::string& v, bool& out) {
    std::string s = lower(trim(v));
    if (s == "1" || s == "true" || s == "on" || s == "yes")  { out = true;  return true; }
    if (s == "0" || s == "false" || s == "off" || s == "no") { out = false; return true; }
    return false;
}

bool parseInt(const std::string& v, int& out) {
    std::string s = trim(v);
    if (s.empty()) return false;
    char* end = nullptr;
    long n = std::strtol(s.c_str(), &end, 10);
    if (!end || *end != '\0') return false;
    out = (int)n;
    return true;
}

bool parseDouble(const std::string& v, double& out) {
    std::string s = trim(v);
    if (s.empty()) return false;
    char* end = nullptr;
    double n = std::strtod(s.c_str(), &end);
    if (!end || *end != '\0') return false;
    out = n;
    return true;
}

bool parseIni(const std::string& path, std::vector<Section>& out) {
    std::ifstream f(path.c_str());
    if (!f) return false;

    Section cur;
    cur.name = "";                       // 第一个 [section] 之前的内容忽略
    bool haveSection = false;
    std::string line;

    bool firstLine = true;
    while (std::getline(f, line)) {
        if (firstLine) {
            firstLine = false;
            // 去掉 UTF-8 BOM
            if (line.size() >= 3 && (unsigned char)line[0] == 0xEF &&
                (unsigned char)line[1] == 0xBB && (unsigned char)line[2] == 0xBF)
                line.erase(0, 3);
        }
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::string t = trim(line);
        if (t.empty() || t[0] == '#' || t[0] == ';') continue;

        if (t[0] == '[') {
            if (haveSection) out.push_back(cur);
            size_t close = t.find(']');
            std::string name = t.substr(1, close == std::string::npos ? std::string::npos
                                                                     : close - 1);
            cur = Section{};
            cur.name = normName(name);
            haveSection = true;
            continue;
        }

        size_t eq = t.find('=');
        if (eq == std::string::npos) continue;
        std::string key = lower(trim(t.substr(0, eq)));
        std::string val = trim(t.substr(eq + 1));
        // 允许行尾注释: gpu_clock = 900  # 备注
        size_t hash = val.find_first_of("#;");
        if (hash != std::string::npos) val = trim(val.substr(0, hash));
        // 允许值两侧带引号
        if (val.size() >= 2 && ((val.front() == '"' && val.back() == '"') ||
                                (val.front() == '\'' && val.back() == '\'')))
            val = val.substr(1, val.size() - 2);
        if (!key.empty()) cur.kv.emplace_back(key, val);
    }
    if (haveSection) out.push_back(cur);
    return true;
}

bool applySection(const Section& sec, ControlProfile& prof) {
    bool any = false;
    for (const auto& kv : sec.kv) {
        if (applyProfileKey(prof, kv.first, kv.second)) any = true;
    }
    return any;
}

} // namespace

bool applyProfileKey(ControlProfile& p, const std::string& key,
                     const std::string& value) {
    if (key == "gpu_limit")        return parseInt(value, p.gpuLimitW);
    if (key == "min_gpu")          return parseInt(value, p.minGpuW);
    if (key == "max_gpu")          return parseInt(value, p.maxGpuW);
    if (key == "cpu_max")          return parseInt(value, p.cpuMaxPct);
    if (key == "cpu_min")          return parseInt(value, p.cpuMinPct);
    if (key == "cpu_step")         return parseInt(value, p.cpuStepPct);
    if (key == "cpu_power_step")   return parseBool(value, p.cpuPowerStep);
    if (key == "prefer") {
        std::string v = lower(trim(value));
        if (v == "cpu") { p.preferCpu = true;  return true; }
        if (v == "gpu") { p.preferCpu = false; return true; }
        return false;
    }
    if (key == "gpu_clock")        return parseInt(value, p.gpuClockMhz);
    if (key == "min_gpu_clock")    return parseInt(value, p.minGpuClockMhz);
    if (key == "clock_step")       return parseInt(value, p.clockStepMhz);
    if (key == "auto_clock")       return parseInt(value, p.autoClockMhz);
    if (key == "gpu_clock_auto")   return parseBool(value, p.gpuClockAuto);
    if (key == "gpu_clock_pct")     return parseInt(value, p.gpuClockPct);
    if (key == "min_gpu_clock_pct") return parseInt(value, p.minGpuClockPct);
    if (key == "auto_clock_pct")    return parseInt(value, p.autoClockPct);

    if (key == "battery_threshold") return parseDouble(value, p.batteryThresholdW);
    if (key == "deficit_energy")    return parseDouble(value, p.deficitEnergyWh);
    if (key == "deficit_window")    return parseInt(value, p.deficitWindowSec);
    if (key == "step_cooldown")     return parseInt(value, p.stepCooldownSec);
    if (key == "step")              return parseInt(value, p.stepW);
    if (key == "hold_down")         return parseInt(value, p.holdDownSec);
    if (key == "hold_up")           return parseInt(value, p.holdUpSec);

    // 兼容写法: no_gpu_clock = 1 等价于 gpu_clock_auto = 0
    if (key == "no_gpu_clock") {
        bool no = false;
        if (!parseBool(value, no)) return false;
        p.gpuClockAuto = !no;
        return true;
    }
    if (key == "no_cpu_step") {
        bool no = false;
        if (!parseBool(value, no)) return false;
        p.cpuPowerStep = !no;
        return true;
    }
    return false;
}

bool applyGameProfile(const std::string& path, const std::string& game,
                      ControlProfile& prof, std::string& usedSection) {
    usedSection.clear();
    if (path.empty()) return false;

    std::vector<Section> secs;
    if (!parseIni(path, secs)) return false;

    bool any = false;
    std::string gameNorm = normName(game);

    for (const auto& sec : secs) {
        if (sec.name == "default" && applySection(sec, prof)) {
            any = true;
            usedSection = "default";
        }
    }
    if (!gameNorm.empty()) {
        for (const auto& sec : secs) {
            if (sec.name == gameNorm && applySection(sec, prof)) {
                any = true;
                usedSection = sec.name;
            }
        }
    }
    return any;
}

} // namespace pg
