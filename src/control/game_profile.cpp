#include "control/game_profile.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace pg {
namespace {

// applyKey 的三种结果: 用来区分“键名写错”和“键对但值非法”
enum class KeyApply { Ok, Unknown, BadValue };

struct KeyValue {
    std::string key;
    std::string value;
    int line = 0;
};

struct Section {
    std::string name;      // 归一化段名, 空 = 未命名
    int line = 0;          // 段头行号
    std::vector<KeyValue> kv;
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
    int lineNo = 0;

    bool firstLine = true;
    while (std::getline(f, line)) {
        ++lineNo;
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
            cur.line = lineNo;
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
        if (!key.empty()) cur.kv.push_back({key, val, lineNo});
    }
    if (haveSection) out.push_back(cur);
    return true;
}

KeyApply applyKey(ControlProfile& p, const std::string& key,
                  const std::string& value) {
    auto setInt = [&](int& dst) {
        return parseInt(value, dst) ? KeyApply::Ok : KeyApply::BadValue;
    };
    auto setDouble = [&](double& dst) {
        return parseDouble(value, dst) ? KeyApply::Ok : KeyApply::BadValue;
    };
    auto setBool = [&](bool& dst) {
        return parseBool(value, dst) ? KeyApply::Ok : KeyApply::BadValue;
    };

    if (key == "gpu_limit")        return setInt(p.gpuLimitW);
    if (key == "min_gpu")          return setInt(p.minGpuW);
    if (key == "max_gpu")          return setInt(p.maxGpuW);
    if (key == "cpu_max")          return setInt(p.cpuMaxPct);
    if (key == "cpu_min")          return setInt(p.cpuMinPct);
    if (key == "cpu_step")         return setInt(p.cpuStepPct);
    if (key == "cpu_power_step")   return setBool(p.cpuPowerStep);

    // 开关某个杠杆: control_gpu = 0 等价于命令行的 --no-gpu
    if (key == "control_gpu") return setBool(p.controlGpu);
    if (key == "control_cpu") return setBool(p.controlCpu);
    if (key == "no_gpu") {
        bool no = false;
        if (!parseBool(value, no)) return KeyApply::BadValue;
        p.controlGpu = !no;
        return KeyApply::Ok;
    }
    if (key == "no_cpu") {
        bool no = false;
        if (!parseBool(value, no)) return KeyApply::BadValue;
        p.controlCpu = !no;
        return KeyApply::Ok;
    }

    if (key == "prefer") {
        std::string v = lower(trim(value));
        if (v == "cpu")  { p.preferCpu = true;  p.preferAuto = false; return KeyApply::Ok; }
        if (v == "gpu")  { p.preferCpu = false; p.preferAuto = false; return KeyApply::Ok; }
        if (v == "auto") { p.preferCpu = false; p.preferAuto = true;  return KeyApply::Ok; }
        return KeyApply::BadValue;
    }
    if (key == "gpu_clock")        return setInt(p.gpuClockMhz);
    if (key == "min_gpu_clock")    return setInt(p.minGpuClockMhz);
    if (key == "clock_step")       return setInt(p.clockStepMhz);
    if (key == "auto_clock")       return setInt(p.autoClockMhz);
    if (key == "gpu_clock_auto")   return setBool(p.gpuClockAuto);
    if (key == "gpu_clock_pct")     return setInt(p.gpuClockPct);
    if (key == "min_gpu_clock_pct") return setInt(p.minGpuClockPct);
    if (key == "auto_clock_pct")    return setInt(p.autoClockPct);

    if (key == "budget" || key == "input_budget")
        return setInt(p.inputBudgetW);

    if (key == "battery_threshold") return setDouble(p.batteryThresholdW);
    if (key == "deficit_energy")    return setDouble(p.deficitEnergyWh);
    if (key == "deficit_window")    return setInt(p.deficitWindowSec);
    if (key == "step_cooldown")     return setInt(p.stepCooldownSec);
    if (key == "step")              return setInt(p.stepW);
    if (key == "hold_down")         return setInt(p.holdDownSec);
    if (key == "hold_up")           return setInt(p.holdUpSec);

    // 兼容写法: no_gpu_clock = 1 等价于 gpu_clock_auto = 0
    if (key == "no_gpu_clock") {
        bool no = false;
        if (!parseBool(value, no)) return KeyApply::BadValue;
        p.gpuClockAuto = !no;
        return KeyApply::Ok;
    }
    if (key == "no_cpu_step") {
        bool no = false;
        if (!parseBool(value, no)) return KeyApply::BadValue;
        p.cpuPowerStep = !no;
        return KeyApply::Ok;
    }
    return KeyApply::Unknown;
}

void collectSectionIssues(const Section& sec, ProfileSectionInfo* info,
                          std::vector<std::string>* warnings) {
    ControlProfile scratch;
    int valid = 0;
    int invalid = 0;
    const char* label = sec.name.empty() ? "(未命名)" : sec.name.c_str();

    for (const auto& kv : sec.kv) {
        KeyApply r = applyKey(scratch, kv.key, kv.value);
        if (r == KeyApply::Ok) {
            ++valid;
            continue;
        }
        ++invalid;
        if (!warnings) continue;

        char buf[768];
        if (r == KeyApply::Unknown) {
            std::snprintf(buf, sizeof(buf), "第 %d 行 [%s]: 未知键 \"%s\"",
                          kv.line, label, kv.key.c_str());
        } else {
            std::snprintf(buf, sizeof(buf), "第 %d 行 [%s]: %s 的值 \"%s\" 无效",
                          kv.line, label, kv.key.c_str(), kv.value.c_str());
        }
        warnings->push_back(buf);
    }

    if (info) {
        info->validKeys = valid;
        info->invalidKeys = invalid;
    }
}

bool applySection(const Section& sec, ControlProfile& prof) {
    bool any = false;
    for (const auto& kv : sec.kv) {
        if (applyKey(prof, kv.key, kv.value) == KeyApply::Ok) any = true;
    }
    return any;
}

// ============================================================
// Web 控制台的段读写
//
// 不经过 parseIni: 那里会丢掉行号以外的布局, 而保存时要保留注释、BOM、
// 原始段头写法和换行风格, 只替换/追加目标段。
// ============================================================

struct LineSpan {
    size_t begin = 0;
    size_t end = 0;      // 含行尾 \n (如果有)
};

struct SectionSpan {
    size_t headerBegin = 0;
    size_t headerEnd = 0;
    size_t end = 0;      // 段体结束(下一个段头或文件末尾)
};

void splitLineSpans(const std::string& text, std::vector<LineSpan>& out) {
    out.clear();
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        LineSpan s;
        s.begin = pos;
        s.end = (nl == std::string::npos) ? text.size() : nl + 1;
        out.push_back(s);
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
}

std::string trimmedLine(const std::string& text, const LineSpan& ls) {
    return trim(text.substr(ls.begin, ls.end - ls.begin));
}

// 段头 -> 归一化段名; 不是段头返回 false
bool sectionNameFromLine(const std::string& text, const LineSpan& ls,
                         std::string& normalized) {
    std::string t = trimmedLine(text, ls);
    if (t.size() >= 3 && (unsigned char)t[0] == 0xEF &&
        (unsigned char)t[1] == 0xBB && (unsigned char)t[2] == 0xBF)
        t.erase(0, 3);
    if (t.empty() || t[0] != '[') return false;
    size_t close = t.find(']');
    if (close == std::string::npos) return false;
    normalized = normName(t.substr(1, close - 1));
    return true;
}

// 找到所有同名段的 [header 行起点, 段体结束)
std::vector<SectionSpan> findSectionSpans(const std::string& text,
                                          const std::string& wanted) {
    std::vector<LineSpan> lines;
    splitLineSpans(text, lines);

    std::vector<SectionSpan> out;
    bool inTarget = false;
    SectionSpan cur;

    for (const auto& ls : lines) {
        std::string name;
        if (sectionNameFromLine(text, ls, name)) {
            if (inTarget) {
                cur.end = ls.begin;
                out.push_back(cur);
                inTarget = false;
            }
            if (name == wanted) {
                cur = SectionSpan{};
                cur.headerBegin = ls.begin;
                cur.headerEnd = ls.end;
                cur.end = ls.end;
                inTarget = true;
            }
        }
    }
    if (inTarget) {
        cur.end = text.size();
        out.push_back(cur);
    }
    return out;
}

bool readAllText(const std::string& path, std::string& out, std::string& error) {
    out.clear();
    auto p = std::filesystem::u8path(path);
    std::ifstream in(p, std::ios::binary);
    if (!in) {
        std::error_code ec;
        // 文件不存在 = 空文件, 首次保存时创建; 存在却打不开才算错误
        if (std::filesystem::exists(p, ec)) {
            error = "无法读取 " + path;
            return false;
        }
        return true;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    if (in.bad()) {
        error = "读取 " + path + " 失败";
        return false;
    }
    out = ss.str();
    return true;
}

std::string detectEol(const std::string& text) {
    size_t nl = text.find('\n');
    if (nl == std::string::npos) return "\r\n";       // 新文件按 Windows 习惯
    return (nl > 0 && text[nl - 1] == '\r') ? "\r\n" : "\n";
}

bool atomicWriteFile(const std::string& path, const std::string& data,
                     std::string& error) {
    auto target = std::filesystem::u8path(path);
    auto tmp = target;
    tmp += ".tmp";

    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            error = "无法写入临时文件: " + tmp.string();
            return false;
        }
        out.write(data.data(), (std::streamsize)data.size());
        out.flush();
        if (!out) {
            error = "写入临时文件失败: " + tmp.string();
            std::error_code ec;
            std::filesystem::remove(tmp, ec);
            return false;
        }
    }

    // 覆盖替换而不是先删后改名, 避免留下“文件不存在”的窗口
    if (!MoveFileExW(tmp.c_str(), target.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = "替换 " + path + " 失败 (GetLastError=" +
                std::to_string((unsigned long)GetLastError()) + ")";
        std::error_code ec;
        std::filesystem::remove(tmp, ec);
        return false;
    }
    return true;
}

std::string normalizeSectionName(const std::string& name, std::string& error) {
    std::string n = normName(name);
    if (n.empty()) {
        error = "段名不能为空";
        return {};
    }
    if (n.size() > 120) {
        error = "段名太长 (最多 120 字节)";
        return {};
    }
    if (n.find_first_of("[]\r\n") != std::string::npos) {
        error = "段名不能包含 [ ] 或换行";
        return {};
    }
    return n;
}

// 校验用户提交的段内容, 同时拆成按行数组(保留注释/空行)。
// 行号相对内容本身, 和编辑器里看到的一致。
bool validateSectionContent(const std::string& normalized,
                            const std::string& content,
                            std::vector<std::string>& lines,
                            std::string& error) {
    lines.clear();
    Section sec;
    sec.name = normalized;

    size_t pos = 0;
    int lineNo = 0;
    while (pos <= content.size()) {
        size_t nl = content.find('\n', pos);
        size_t end = (nl == std::string::npos) ? content.size() : nl;
        std::string line = content.substr(pos, end - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        ++lineNo;

        std::string t = trim(line);
        if (t.empty() || t[0] == '#' || t[0] == ';') {
            lines.push_back(line);
        } else if (t[0] == '[') {
            error = "内容第 " + std::to_string(lineNo) +
                    " 行: 不要包含 [section] 行";
            return false;
        } else {
            size_t eq = t.find('=');
            if (eq == std::string::npos) {
                error = "内容第 " + std::to_string(lineNo) + " 行: 缺少 \"=\"";
                return false;
            }
            std::string key = lower(trim(t.substr(0, eq)));
            std::string val = trim(t.substr(eq + 1));
            size_t hash = val.find_first_of("#;");
            if (hash != std::string::npos) val = trim(val.substr(0, hash));
            if (val.size() >= 2 && ((val.front() == '"' && val.back() == '"') ||
                                    (val.front() == '\'' && val.back() == '\'')))
                val = val.substr(1, val.size() - 2);
            if (key.empty()) {
                error = "内容第 " + std::to_string(lineNo) + " 行: 键名为空";
                return false;
            }
            sec.kv.push_back({key, val, lineNo});
            lines.push_back(line);
        }

        if (nl == std::string::npos) break;
        pos = nl + 1;
    }

    std::vector<std::string> warnings;
    collectSectionIssues(sec, nullptr, &warnings);
    if (!warnings.empty()) {
        error.clear();
        for (const auto& w : warnings) {
            if (!error.empty()) error += '\n';
            error += w;
        }
        return false;
    }

    // 去掉末尾空行, 避免每保存一次就多出一行
    while (!lines.empty() && trim(lines.back()).empty()) lines.pop_back();
    return true;
}

} // namespace

bool applyProfileKey(ControlProfile& p, const std::string& key,
                     const std::string& value) {
    return applyKey(p, key, value) == KeyApply::Ok;
}

bool applyGameProfile(const std::string& path, const std::string& game,
                      ControlProfile& prof, std::string& usedSection,
                      std::vector<std::string>* warnings) {
    usedSection.clear();
    if (warnings) warnings->clear();
    if (path.empty()) return false;

    std::vector<Section> secs;
    if (!parseIni(path, secs)) return false;

    // 校验文件里所有段, 这样别的游戏段写错也能在日志里看到。
    if (warnings) {
        for (const auto& sec : secs)
            collectSectionIssues(sec, nullptr, warnings);
    }

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

bool inspectGameProfile(const std::string& path, ProfileReport& report) {
    report = ProfileReport{};
    if (path.empty()) return false;

    std::vector<Section> secs;
    if (!parseIni(path, secs)) return false;

    report.fileFound = true;
    for (const auto& sec : secs) {
        ProfileSectionInfo info;
        info.name = sec.name;
        info.line = sec.line;
        collectSectionIssues(sec, &info, &report.warnings);
        report.sections.push_back(info);
    }
    return true;
}

bool readProfileSection(const std::string& path, const std::string& name,
                        std::string& content, bool& found, std::string& normalized,
                        std::string& error) {
    content.clear();
    found = false;
    normalized.clear();
    error.clear();

    normalized = normalizeSectionName(name, error);
    if (normalized.empty()) return false;

    std::string text;
    if (!readAllText(path, text, error)) return false;

    const auto spans = findSectionSpans(text, normalized);
    found = !spans.empty();
    for (const auto& s : spans)
        content += text.substr(s.headerEnd, s.end - s.headerEnd);

    // 去掉末尾换行, 编辑框里更干净; 保存时会统一补回
    while (!content.empty() && (content.back() == '\n' || content.back() == '\r'))
        content.pop_back();
    return true;
}

bool writeProfileSection(const std::string& path, const std::string& name,
                         const std::string& content, std::string& normalized,
                         std::string& error) {
    normalized.clear();
    error.clear();

    normalized = normalizeSectionName(name, error);
    if (normalized.empty()) return false;

    std::string text;
    if (!readAllText(path, text, error)) return false;

    std::vector<std::string> lines;
    if (!validateSectionContent(normalized, content, lines, error)) return false;

    const std::string eol = detectEol(text);
    std::string body;
    for (const auto& line : lines) {
        body += line;
        body += eol;
    }

    const auto spans = findSectionSpans(text, normalized);
    std::string result;
    if (spans.empty()) {
        result = text;
        if (!result.empty() && result.back() != '\n') result += eol;
        if (!result.empty()) result += eol;              // 段之间留一个空行
        result += "[" + normalized + "]" + eol + body;
    } else {
        size_t pos = 0;
        bool wroteHeader = false;
        for (const auto& s : spans) {
            if (s.headerBegin < pos) continue;
            result.append(text, pos, s.headerBegin - pos);
            if (!wroteHeader) {
                // 保留原段头写法(含 BOM/大小写/行尾注释), 只换段体
                result.append(text, s.headerBegin, s.headerEnd - s.headerBegin);
                if (s.headerEnd == 0 || text[s.headerEnd - 1] != '\n') result += eol;
                result += body;
                wroteHeader = true;
            }
            pos = s.end;
        }
        result.append(text, pos, std::string::npos);
    }

    return atomicWriteFile(path, result, error);
}

bool removeProfileSection(const std::string& path, const std::string& name,
                          std::string& normalized, std::string& error) {
    normalized.clear();
    error.clear();

    normalized = normalizeSectionName(name, error);
    if (normalized.empty()) return false;

    std::string text;
    if (!readAllText(path, text, error)) return false;

    const auto spans = findSectionSpans(text, normalized);
    if (spans.empty()) return true;             // 不存在 = 已经满足

    std::string result;
    size_t pos = 0;
    for (const auto& s : spans) {
        if (s.headerBegin < pos) continue;
        result.append(text, pos, s.headerBegin - pos);
        pos = s.end;
    }
    result.append(text, pos, std::string::npos);

    return atomicWriteFile(path, result, error);
}

} // namespace pg
