// ============================================================
// PowerGuard 日志系统 (单头文件, C++17, 无第三方依赖)
//   - 文本日志: logs/powerguard_YYYY-MM-DD.log   按日期每天一个文件
//   - CSV 记录: logs/power_YYYY-MM-DD.csv        按日期每天一个文件
//   - 线程安全: 内部互斥锁, 多线程采样也能安全调用
//   - 控制台同步输出 (UTF-8)
// 用法:
//   pg::Logger::instance().init("logs");
//   LOG_INFO("core", "启动, GPU=%s", ok ? "可用" : "不可用");
//   pg::CsvLog csv; csv.init("logs", "time,cpu_w,gpu_w");
//   csv.writef("%s,%.2f,%.2f", pg::nowTime().c_str(), cpu, gpu);
// ============================================================
#pragma once

#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <string>
#include <mutex>
#include <fstream>
#include <filesystem>

namespace pg {

// ------------------------------------------------------------ 等级与时间
enum class Level { Debug = 0, Info, Warn, Error };

inline const char* levelName(Level lv) {
    switch (lv) {
        case Level::Debug: return "DEBUG";
        case Level::Info:  return "INFO";
        case Level::Warn:  return "WARN";
        case Level::Error: return "ERROR";
    }
    return "?";
}

inline std::string stampDate(const SYSTEMTIME& st) {
    char b[16];
    snprintf(b, sizeof b, "%04d-%02d-%02d", st.wYear, st.wMonth, st.wDay);
    return b;
}

// "HH:MM:SS.mmm", CSV 里做时间列
inline std::string stampTime(const SYSTEMTIME& st) {
    char b[24];
    snprintf(b, sizeof b, "%02d:%02d:%02d.%03d",
             st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    return b;
}

inline std::string currentDate() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    return stampDate(st);
}

inline std::string nowTime() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    return stampTime(st);
}

// ------------------------------------------------------------ 文本日志
class Logger {
public:
    static Logger& instance() {
        static Logger logger;
        return logger;
    }

    // dir: 日志目录(不存在会自动创建); minLevel: 低于该等级的日志直接丢弃
    void init(const std::string& dir = "logs", Level minLevel = Level::Info) {
        std::lock_guard<std::mutex> lk(mu_);
        dir_ = dir;
        min_ = minLevel;
        inited_ = true;
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::u8path(dir_), ec);
        SetConsoleOutputCP(CP_UTF8);                    // 控制台按 UTF-8 显示中文
        ensureFileLocked(currentDate());                // 启动即创建当天文件
    }

    void setConsole(bool on) {
        std::lock_guard<std::mutex> lk(mu_);
        console_ = on;
    }

    void setMinLevel(Level lv) {
        std::lock_guard<std::mutex> lk(mu_);
        min_ = lv;
    }

    void log(Level lv, const char* module, const char* fmt, ...) {
        SYSTEMTIME st;
        GetLocalTime(&st);

        char msg[2048];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(msg, sizeof msg, fmt, ap);
        va_end(ap);

        std::lock_guard<std::mutex> lk(mu_);
        if (lv < min_) return;
        if (inited_) ensureFileLocked(stampDate(st));   // 跨天自动换文件

        char line[2304];
        snprintf(line, sizeof line, "%s %s [%-5s] [%-5s] %s\n",
                 stampDate(st).c_str(), stampTime(st).c_str(),
                 levelName(lv), module, msg);

        if (file_.is_open()) {
            file_ << line;
            file_.flush();                              // 每行落盘, 崩溃也不丢
        }
        if (console_) fputs(line, stdout);
    }

    ~Logger() {
        std::lock_guard<std::mutex> lk(mu_);
        if (file_.is_open()) file_.close();
    }

private:
    Logger() = default;
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    void ensureFileLocked(const std::string& day) {
        if (file_.is_open() && day == day_) return;
        if (file_.is_open()) file_.close();
        day_ = day;
        std::string path = dir_ + "/powerguard_" + day + ".log";
        file_.open(std::filesystem::u8path(path), std::ios::app);
    }

    std::mutex mu_;
    std::ofstream file_;
    std::string dir_ = "logs";
    std::string day_;
    Level min_ = Level::Info;
    bool console_ = true;
    bool inited_ = false;
};

// ------------------------------------------------------------ CSV 数据记录
// 按日期分文件, 文件为空时自动写表头
class CsvLog {
public:
    CsvLog() = default;

    bool init(const std::string& dir, const std::string& header) {
        std::lock_guard<std::mutex> lk(mu_);
        dir_ = dir;
        header_ = header;
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::u8path(dir_), ec);
        return ensureFileLocked(currentDate());
    }

    void write(const std::string& row) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        std::lock_guard<std::mutex> lk(mu_);
        if (!ensureFileLocked(stampDate(st))) return;
        file_ << row << '\n';
        file_.flush();
    }

    void writef(const char* fmt, ...) {
        char row[1024];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(row, sizeof row, fmt, ap);
        va_end(ap);
        write(row);
    }

    ~CsvLog() {
        std::lock_guard<std::mutex> lk(mu_);
        if (file_.is_open()) file_.close();
    }

private:
    bool ensureFileLocked(const std::string& day) {
        if (file_.is_open() && day == day_) return true;
        if (file_.is_open()) file_.close();
        day_ = day;

        auto path = std::filesystem::u8path(dir_ + "/power_" + day + ".csv");
        std::error_code ec;

        // 旧文件表头与当前列不一致(比如新增了列)时先改名备份, 避免新旧列混写
        if (!header_.empty() && std::filesystem::exists(path, ec) &&
            std::filesystem::file_size(path, ec) > 0 && !headerMatches(path))
            backupLocked(path);

        bool fresh = !std::filesystem::exists(path, ec);
        file_.open(path, std::ios::app);
        if (file_.is_open() && fresh && !header_.empty())
            file_ << header_ << '\n';                   // 首次创建才写表头
        return file_.is_open();
    }

    bool headerMatches(const std::filesystem::path& path) const {
        std::ifstream in(path);
        std::string first;
        if (!std::getline(in, first)) return false;
        while (!first.empty() && (first.back() == '\r' || first.back() == '\n'))
            first.pop_back();
        return first == header_;
    }

    // 备份为 *.legacy, 已存在则加序号; 改名失败也不阻止后续写入
    void backupLocked(const std::filesystem::path& path) {
        std::error_code ec;
        for (int i = 0; i < 100; ++i) {
            std::string suffix = (i == 0) ? std::string(".legacy")
                                          : ".legacy" + std::to_string(i);
            auto target = path;
            target += suffix;
            if (std::filesystem::exists(target, ec)) continue;
            std::filesystem::rename(path, target, ec);
            if (ec) {
                // 多半是另一个实例还开着该文件(Windows 下 rename 会失败),
                // 此时无法写新表头, 新旧列会混在一个文件里, 至少要留下痕迹
                fprintf(stderr, "[WARN] CSV 备份失败: %s (%s), 新列会追加到旧文件中\n",
                        target.string().c_str(), ec.message().c_str());
            }
            return;
        }
    }

    std::mutex mu_;
    std::ofstream file_;
    std::string dir_ = "logs";
    std::string header_;
    std::string day_;
};

} // namespace pg

// ------------------------------------------------------------ 便捷宏
#define LOG_DEBUG(mod, ...) ::pg::Logger::instance().log(::pg::Level::Debug, mod, __VA_ARGS__)
#define LOG_INFO(mod, ...)  ::pg::Logger::instance().log(::pg::Level::Info,  mod, __VA_ARGS__)
#define LOG_WARN(mod, ...)  ::pg::Logger::instance().log(::pg::Level::Warn,  mod, __VA_ARGS__)
#define LOG_ERROR(mod, ...) ::pg::Logger::instance().log(::pg::Level::Error, mod, __VA_ARGS__)
