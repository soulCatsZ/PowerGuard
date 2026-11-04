// ============================================================
// PowerGuard 纯逻辑单元测试
//   覆盖: PowerChangeTracker / profiles.ini 解析与校验 / 参数归一
//   不依赖 NVML、电池或管理员权限, 可在 CI 中重复运行。
// ============================================================
#include <windows.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "common/power_tracker.h"
#include "control/game_profile.h"
#include "control/profile.h"

namespace {

int g_pass = 0;
int g_fail = 0;

#define CHECK(cond) do {                                                     \
    if (cond) { ++g_pass; }                                                  \
    else {                                                                   \
        std::printf("[FAIL] %s:%d  %s\n", __FILE__, __LINE__, #cond);        \
        ++g_fail;                                                            \
    }                                                                        \
} while (0)

#define CHECK_EQ_INT(exp, act) do {                                          \
    int e_ = (exp); int a_ = (act);                                          \
    if (e_ == a_) { ++g_pass; }                                              \
    else {                                                                   \
        std::printf("[FAIL] %s:%d  %s == %s (expected %d, got %d)\n",        \
                    __FILE__, __LINE__, #exp, #act, e_, a_);                 \
        ++g_fail;                                                            \
    }                                                                        \
} while (0)

#define CHECK_EQ_DBL(exp, act) do {                                          \
    double e_ = (exp); double a_ = (act);                                    \
    if (std::fabs(e_ - a_) < 1e-9) { ++g_pass; }                             \
    else {                                                                   \
        std::printf("[FAIL] %s:%d  %s == %s (expected %.6f, got %.6f)\n",    \
                    __FILE__, __LINE__, #exp, #act, e_, a_);                 \
        ++g_fail;                                                            \
    }                                                                        \
} while (0)

#define CHECK_EQ_STR(exp, act) do {                                          \
    std::string e_ = (exp); std::string a_ = (act);                          \
    if (e_ == a_) { ++g_pass; }                                              \
    else {                                                                   \
        std::printf("[FAIL] %s:%d  %s == %s (expected \"%s\", got \"%s\")\n",\
                    __FILE__, __LINE__, #exp, #act,                          \
                    e_.c_str(), a_.c_str());                                 \
        ++g_fail;                                                            \
    }                                                                        \
} while (0)

// ------------------------------------------------------------
// PowerChangeTracker
// ------------------------------------------------------------
void testPowerChangeTracker() {
    // 绝对阈值: 8W; 基线只在真正写入时移动
    {
        pg::PowerChangeTracker t(8.0, 0.0, 0);
        CHECK(!t.hasBaseline());
        CHECK(t.shouldWrite(10.0, 20.0));          // 第一条必写
        CHECK(t.hasBaseline());
        CHECK_EQ_DBL(10.0, t.lastCpu());
        CHECK_EQ_DBL(20.0, t.lastGpu());
        CHECK(!t.shouldWrite(14.0, 24.0));         // 4W < 8W
        CHECK(!t.shouldWrite(17.0, 27.0));         // 7W < 8W
        CHECK(t.shouldWrite(18.0, 28.0));          // 8W >= 8W
        CHECK_EQ_DBL(18.0, t.lastCpu());
        CHECK_EQ_DBL(28.0, t.lastGpu());
        CHECK(!t.shouldWrite(20.0, 30.0));         // 距新基线 2W
        t.reset();
        CHECK(!t.hasBaseline());
        CHECK(t.shouldWrite(200.0, 300.0));        // reset 后必写
    }

    // 相对阈值: 5%; 任一通道超阈值就写整对
    {
        pg::PowerChangeTracker t(0.0, 0.05, 0);
        CHECK(t.shouldWrite(100.0, 50.0));
        CHECK(!t.shouldWrite(104.0, 50.0));        // 4W < 5W
        CHECK(t.shouldWrite(106.0, 50.0));         // 6W >= 5W
        CHECK(t.shouldWrite(106.0, 60.0));         // GPU 10W >= 2.5W
    }

    // 绝对 + 相对同时启用
    {
        pg::PowerChangeTracker t(8.0, 0.05, 0);
        CHECK(t.shouldWrite(100.0, 100.0));
        CHECK(!t.shouldWrite(104.0, 104.0));       // 4W, 低于两者
        CHECK(t.shouldWrite(109.0, 104.0));        // 相对 5W, 9W 触发
    }

    // 心跳: 即使功率不变也补写一条
    {
        pg::PowerChangeTracker t(100.0, 0.0, 1);
        CHECK(t.shouldWrite(1.0, 1.0));
        CHECK(!t.shouldWrite(1.5, 1.5));
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        CHECK(t.shouldWrite(1.5, 1.5));
    }
}

// ------------------------------------------------------------
// profiles.ini 单键解析
// ------------------------------------------------------------
void testApplyProfileKey() {
    pg::ControlProfile p;

    CHECK(pg::applyProfileKey(p, "gpu_limit", "45"));
    CHECK_EQ_INT(45, p.gpuLimitW);
    CHECK(!pg::applyProfileKey(p, "gpu_limit", "abc"));
    CHECK_EQ_INT(45, p.gpuLimitW);                 // 非法值不改动
    CHECK(!pg::applyProfileKey(p, "gpu_limit", ""));
    CHECK_EQ_INT(45, p.gpuLimitW);
    CHECK(!pg::applyProfileKey(p, "gpu_limit", "46x"));
    CHECK_EQ_INT(45, p.gpuLimitW);
    CHECK(pg::applyProfileKey(p, "gpu_limit", " 46 "));
    CHECK_EQ_INT(46, p.gpuLimitW);

    CHECK(pg::applyProfileKey(p, "min_gpu", "12"));
    CHECK_EQ_INT(12, p.minGpuW);
    CHECK(pg::applyProfileKey(p, "max_gpu", "60"));
    CHECK_EQ_INT(60, p.maxGpuW);
    CHECK(pg::applyProfileKey(p, "cpu_max", "93"));
    CHECK_EQ_INT(93, p.cpuMaxPct);
    CHECK(pg::applyProfileKey(p, "cpu_min", "55"));
    CHECK_EQ_INT(55, p.cpuMinPct);
    CHECK(pg::applyProfileKey(p, "cpu_step", "3"));
    CHECK_EQ_INT(3, p.cpuStepPct);

    CHECK(pg::applyProfileKey(p, "cpu_power_step", "off"));
    CHECK(!p.cpuPowerStep);
    CHECK(pg::applyProfileKey(p, "cpu_power_step", "ON"));
    CHECK(p.cpuPowerStep);

    // 杠杆开关: 两种写法等价于 --no-gpu / --no-cpu
    CHECK(p.controlGpu && p.controlCpu);
    CHECK(pg::applyProfileKey(p, "no_gpu", "1"));
    CHECK(!p.controlGpu);
    CHECK(pg::applyProfileKey(p, "control_gpu", "true"));
    CHECK(p.controlGpu);
    CHECK(pg::applyProfileKey(p, "control_cpu", "0"));
    CHECK(!p.controlCpu);
    CHECK(pg::applyProfileKey(p, "no_cpu", "off"));
    CHECK(p.controlCpu);
    CHECK(!pg::applyProfileKey(p, "no_gpu", "sometimes"));
    CHECK(p.controlGpu);                              // 非法值不改动

    CHECK(pg::applyProfileKey(p, "prefer", "cpu"));
    CHECK(p.preferCpu && !p.preferAuto);
    CHECK(pg::applyProfileKey(p, "prefer", "Auto"));
    CHECK(!p.preferCpu && p.preferAuto);
    CHECK(pg::applyProfileKey(p, "prefer", "gpu"));
    CHECK(!p.preferCpu && !p.preferAuto);
    CHECK(!pg::applyProfileKey(p, "prefer", "bogus"));
    CHECK(!p.preferCpu && !p.preferAuto);           // 非法值不改动

    CHECK(pg::applyProfileKey(p, "gpu_clock", "1400"));
    CHECK_EQ_INT(1400, p.gpuClockMhz);
    CHECK(pg::applyProfileKey(p, "min_gpu_clock", "700"));
    CHECK_EQ_INT(700, p.minGpuClockMhz);
    CHECK(pg::applyProfileKey(p, "clock_step", "50"));
    CHECK_EQ_INT(50, p.clockStepMhz);
    CHECK(pg::applyProfileKey(p, "auto_clock", "1200"));
    CHECK_EQ_INT(1200, p.autoClockMhz);
    CHECK(pg::applyProfileKey(p, "gpu_clock_auto", "0"));
    CHECK(!p.gpuClockAuto);

    CHECK(pg::applyProfileKey(p, "gpu_clock_pct", "55"));
    CHECK_EQ_INT(55, p.gpuClockPct);
    CHECK(pg::applyProfileKey(p, "min_gpu_clock_pct", "30"));
    CHECK_EQ_INT(30, p.minGpuClockPct);
    CHECK(pg::applyProfileKey(p, "auto_clock_pct", "45"));
    CHECK_EQ_INT(45, p.autoClockPct);

    CHECK(pg::applyProfileKey(p, "budget", "70"));
    CHECK_EQ_INT(70, p.inputBudgetW);
    CHECK(pg::applyProfileKey(p, "input_budget", "80"));
    CHECK_EQ_INT(80, p.inputBudgetW);

    CHECK(pg::applyProfileKey(p, "battery_threshold", "1.5"));
    CHECK_EQ_DBL(1.5, p.batteryThresholdW);
    CHECK(pg::applyProfileKey(p, "deficit_energy", "0.25"));
    CHECK_EQ_DBL(0.25, p.deficitEnergyWh);
    CHECK(pg::applyProfileKey(p, "deficit_window", "60"));
    CHECK_EQ_INT(60, p.deficitWindowSec);
    CHECK(pg::applyProfileKey(p, "step_cooldown", "7"));
    CHECK_EQ_INT(7, p.stepCooldownSec);
    CHECK(pg::applyProfileKey(p, "step", "3"));
    CHECK_EQ_INT(3, p.stepW);
    CHECK(pg::applyProfileKey(p, "hold_down", "4"));
    CHECK_EQ_INT(4, p.holdDownSec);
    CHECK(pg::applyProfileKey(p, "hold_up", "120"));
    CHECK_EQ_INT(120, p.holdUpSec);

    CHECK(pg::applyProfileKey(p, "no_gpu_clock", "1"));
    CHECK(!p.gpuClockAuto);
    CHECK(pg::applyProfileKey(p, "no_gpu_clock", "0"));
    CHECK(p.gpuClockAuto);
    CHECK(!pg::applyProfileKey(p, "no_gpu_clock", "maybe"));
    CHECK(p.gpuClockAuto);

    CHECK(pg::applyProfileKey(p, "no_cpu_step", "yes"));
    CHECK(!p.cpuPowerStep);
    CHECK(pg::applyProfileKey(p, "no_cpu_step", "no"));
    CHECK(p.cpuPowerStep);

    CHECK(!pg::applyProfileKey(p, "unknown_key", "1"));
    CHECK(!pg::applyProfileKey(p, "GPU_LIMIT", "10"));  // 键由解析器统一小写
}

// ------------------------------------------------------------
// profiles.ini 文件解析
// ------------------------------------------------------------
std::filesystem::path tempDir() {
    std::error_code ec;
    auto p = std::filesystem::temp_directory_path(ec);
    return ec ? std::filesystem::current_path(ec) : p;
}

bool writeFile(const std::filesystem::path& path, const std::string& body) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(body.data(), (std::streamsize)body.size());
    return f.good();
}

void testApplyGameProfile() {
    auto dir = tempDir();
    auto file = dir / ("pg_profiles_" +
                       std::to_string(GetCurrentProcessId()) + ".ini");

    // 故意把游戏段放在 [default] 前面: 语义上仍应先应用 default 再覆盖。
    const std::string ini =
        "\xEF\xBB\xBF"                       // UTF-8 BOM
        "# leading comment\r\n"
        "\r\n"
        "[ MyGame.EXE ]\r\n"
        "gpu_limit = \"40\"\r\n"
        "gpu_clock = '1400'\r\n"
        "cpu_max = 85\r\n"
        "prefer = cpu\r\n"
        "budget = 70\r\n"
        "\r\n"
        "[mygame]\r\n"
        "cpu_step = 3\r\n"
        "no_gpu = 1\r\n"
        "\r\n"
        "[default]\r\n"
        "gpu_limit = 33\r\n"
        "cpu_max = 90  # inline comment\r\n"
        "prefer = gpu\r\n"
        "no_cpu = 1\r\n"
        "battery_threshold = 3.5 ; another comment\r\n";

    CHECK(writeFile(file, ini));

    {
        pg::ControlProfile p;
        std::string used;
        CHECK(pg::applyGameProfile(file.string(), "mygame", p, used));
        CHECK_EQ_STR("mygame", used);
        CHECK_EQ_INT(40, p.gpuLimitW);          // 游戏段覆盖 default
        CHECK_EQ_INT(85, p.cpuMaxPct);
        CHECK_EQ_INT(3, p.cpuStepPct);          // 第二个 [mygame] 段
        CHECK_EQ_INT(1400, p.gpuClockMhz);      // 单引号
        CHECK(p.preferCpu && !p.preferAuto);    // 游戏段覆盖 default
        CHECK_EQ_INT(70, p.inputBudgetW);       // budget 别名
        CHECK_EQ_DBL(3.5, p.batteryThresholdW);
        CHECK(!p.controlGpu);                   // 游戏段 no_gpu = 1
        CHECK(!p.controlCpu);                   // default no_cpu = 1
    }

    {
        pg::ControlProfile p;
        std::string used;
        CHECK(pg::applyGameProfile(file.string(), "MYGAME.EXE", p, used));
        CHECK_EQ_STR("mygame", used);           // 大小写/.exe 不敏感
        CHECK_EQ_INT(40, p.gpuLimitW);
    }

    {
        pg::ControlProfile p;
        std::string used;
        CHECK(pg::applyGameProfile(file.string(), "not_present.exe", p, used));
        CHECK_EQ_STR("default", used);          // 只应用 default
        CHECK_EQ_INT(33, p.gpuLimitW);
        CHECK_EQ_INT(90, p.cpuMaxPct);
        CHECK(!p.preferCpu && !p.preferAuto);
        CHECK_EQ_DBL(3.5, p.batteryThresholdW);
        CHECK(p.controlGpu);                    // default 没关 GPU
        CHECK(!p.controlCpu);                   // default 关了 CPU
    }

    {
        pg::ControlProfile p;
        std::string used;
        CHECK(pg::applyGameProfile(file.string(), "", p, used));
        CHECK_EQ_STR("default", used);
        CHECK_EQ_INT(33, p.gpuLimitW);
    }

    // 文件不存在 / 路径为空
    {
        pg::ControlProfile p;
        std::string used = "sentinel";
        CHECK(!pg::applyGameProfile((dir / "pg_missing.ini").string(),
                                    "x", p, used));
        CHECK(used.empty());
        CHECK_EQ_INT(30, p.gpuLimitW);          // 保持默认值
        CHECK_EQ_INT(99, p.cpuMaxPct);

        used = "sentinel";
        CHECK(!pg::applyGameProfile("", "x", p, used));
        CHECK(used.empty());
    }

    // 只有未知键 / 没有段
    {
        auto unknown = dir / ("pg_profiles_unknown_" +
                              std::to_string(GetCurrentProcessId()) + ".ini");
        CHECK(writeFile(unknown,
            "[default]\r\n"
            "junk = 1\r\n"
            "[only_unknown]\r\n"
            "junk2 = 2\r\n"));
        pg::ControlProfile p;
        std::string used;
        CHECK(!pg::applyGameProfile(unknown.string(), "only_unknown", p, used));
        CHECK(used.empty());
        std::error_code ec;
        std::filesystem::remove(unknown, ec);
    }

    {
        auto comments = dir / ("pg_profiles_comments_" +
                               std::to_string(GetCurrentProcessId()) + ".ini");
        CHECK(writeFile(comments, "# no sections\r\n\r\n; still nothing\r\n"));
        pg::ControlProfile p;
        std::string used;
        CHECK(!pg::applyGameProfile(comments.string(), "x", p, used));
        CHECK(used.empty());
        std::error_code ec;
        std::filesystem::remove(comments, ec);
    }

    std::error_code ec;
    std::filesystem::remove(file, ec);
}

// ------------------------------------------------------------
// profiles.ini 校验 (未知键 / 非法值, 带行号)
// ------------------------------------------------------------
void testInspectGameProfile() {
    auto dir = tempDir();
    auto file = dir / ("pg_profiles_check_" +
                       std::to_string(GetCurrentProcessId()) + ".ini");

    const std::string ini =
        "\xEF\xBB\xBF# comment\n"      // 1 (BOM 在第一行)
        "\n"                           // 2
        "[default]\n"                  // 3
        "gpu_limit = 35\n"             // 4
        "junk = 1\n"                   // 5
        "cpu_max = abc\n"              // 6
        "[Game.exe]\n"                 // 7
        "cpu_step = 3\n"               // 8
        "prefer = sometimes\n"         // 9
        "bogus = 2\n"                  // 10
        "[empty]\n";                   // 11

    CHECK(writeFile(file, ini));

    {
        pg::ProfileReport r;
        CHECK(pg::inspectGameProfile(file.string(), r));
        CHECK(r.fileFound);
        CHECK_EQ_INT(3, (int)r.sections.size());

        CHECK_EQ_STR("default", r.sections[0].name);
        CHECK_EQ_INT(3, r.sections[0].line);
        CHECK_EQ_INT(1, r.sections[0].validKeys);
        CHECK_EQ_INT(2, r.sections[0].invalidKeys);

        CHECK_EQ_STR("game", r.sections[1].name);   // 大小写/.exe 已归一化
        CHECK_EQ_INT(7, r.sections[1].line);
        CHECK_EQ_INT(1, r.sections[1].validKeys);
        CHECK_EQ_INT(2, r.sections[1].invalidKeys);

        CHECK_EQ_STR("empty", r.sections[2].name);
        CHECK_EQ_INT(11, r.sections[2].line);
        CHECK_EQ_INT(0, r.sections[2].validKeys);
        CHECK_EQ_INT(0, r.sections[2].invalidKeys);

        CHECK_EQ_INT(4, (int)r.warnings.size());
        auto has = [&](const char* needle) {
            for (const auto& w : r.warnings)
                if (w.find(needle) != std::string::npos) return true;
            return false;
        };
        CHECK(has("第 5 行 [default]: 未知键 \"junk\""));
        CHECK(has("第 6 行 [default]: cpu_max 的值 \"abc\" 无效"));
        CHECK(has("第 9 行 [game]: prefer 的值 \"sometimes\" 无效"));
        CHECK(has("第 10 行 [game]: 未知键 \"bogus\""));
    }

    // applyGameProfile 也应能带出警告, 且命中段的合法值照常生效
    {
        pg::ControlProfile p;
        std::string used;
        std::vector<std::string> warnings;
        CHECK(pg::applyGameProfile(file.string(), "game.exe", p, used, &warnings));
        CHECK_EQ_STR("game", used);
        CHECK_EQ_INT(35, p.gpuLimitW);      // default
        CHECK_EQ_INT(3, p.cpuStepPct);      // game
        CHECK_EQ_INT(4, (int)warnings.size());
    }

    // 文件不存在 / 路径为空
    {
        pg::ProfileReport r;
        CHECK(!pg::inspectGameProfile((dir / "pg_check_missing.ini").string(), r));
        CHECK(!r.fileFound);
        CHECK(r.warnings.empty());

        CHECK(!pg::inspectGameProfile("", r));
        CHECK(!r.fileFound);
    }

    std::error_code ec;
    std::filesystem::remove(file, ec);
}

// ------------------------------------------------------------
// 参数归一
// ------------------------------------------------------------
void testNormalizeProfile() {
    // 默认值 (也是命令行未指定时的行为)
    {
        pg::ControlProfile p;
        CHECK_EQ_INT(30, p.gpuLimitW);
        CHECK_EQ_INT(15, p.minGpuW);
        CHECK_EQ_INT(0, p.maxGpuW);
        CHECK_EQ_INT(99, p.cpuMaxPct);
        CHECK_EQ_INT(50, p.cpuMinPct);
        CHECK_EQ_INT(5, p.cpuStepPct);
        CHECK(p.cpuPowerStep);
        CHECK(!p.preferCpu && !p.preferAuto);
        CHECK_EQ_INT(0, p.gpuClockMhz);
        CHECK_EQ_INT(800, p.minGpuClockMhz);
        CHECK_EQ_INT(100, p.clockStepMhz);
        CHECK_EQ_INT(1400, p.autoClockMhz);
        CHECK(p.gpuClockAuto);
        CHECK_EQ_INT(0, p.inputBudgetW);
        CHECK_EQ_DBL(2.0, p.batteryThresholdW);
        CHECK_EQ_DBL(0.10, p.deficitEnergyWh);
        CHECK_EQ_INT(120, p.deficitWindowSec);
        CHECK_EQ_INT(15, p.stepCooldownSec);
        CHECK_EQ_INT(5, p.stepW);
        CHECK_EQ_INT(5, p.holdDownSec);
        CHECK_EQ_INT(300, p.holdUpSec);

        pg::normalizeProfile(p);
        CHECK_EQ_INT(30, p.gpuLimitW);
        CHECK_EQ_INT(15, p.minGpuW);
        CHECK_EQ_INT(30, p.maxGpuW);            // 0 -> gpuLimitW
        CHECK_EQ_INT(99, p.cpuMaxPct);
        CHECK_EQ_INT(50, p.cpuMinPct);
        CHECK_EQ_INT(800, p.minGpuClockMhz);
        CHECK_EQ_INT(1400, p.autoClockMhz);
    }

    // 低于下限
    {
        pg::ControlProfile p;
        p.gpuLimitW = 1;       p.minGpuW = 0;      p.maxGpuW = -5;
        p.cpuMaxPct = 1;       p.cpuMinPct = 0;    p.cpuStepPct = 0;
        p.inputBudgetW = -5;   p.batteryThresholdW = -1.0;
        p.deficitEnergyWh = -0.5; p.deficitWindowSec = 1;
        p.stepCooldownSec = -1; p.stepW = 0;
        p.holdDownSec = 0;     p.holdUpSec = 0;
        p.gpuClockMhz = -5;    p.minGpuClockMhz = 1;
        p.clockStepMhz = 1;    p.autoClockMhz = 50;
        p.gpuClockPct = -1;    p.minGpuClockPct = -2; p.autoClockPct = -3;

        pg::normalizeProfile(p);
        CHECK_EQ_INT(5, p.gpuLimitW);
        CHECK_EQ_INT(5, p.minGpuW);
        CHECK_EQ_INT(5, p.maxGpuW);
        CHECK_EQ_INT(5, p.cpuMaxPct);
        CHECK_EQ_INT(5, p.cpuMinPct);
        CHECK_EQ_INT(1, p.cpuStepPct);
        CHECK_EQ_INT(0, p.inputBudgetW);
        CHECK_EQ_DBL(0.0, p.batteryThresholdW);
        CHECK_EQ_DBL(0.0, p.deficitEnergyWh);
        CHECK_EQ_INT(5, p.deficitWindowSec);
        CHECK_EQ_INT(0, p.stepCooldownSec);
        CHECK_EQ_INT(1, p.stepW);
        CHECK_EQ_INT(1, p.holdDownSec);
        CHECK_EQ_INT(1, p.holdUpSec);
        CHECK_EQ_INT(0, p.gpuClockMhz);
        CHECK_EQ_INT(100, p.minGpuClockMhz);
        CHECK_EQ_INT(10, p.clockStepMhz);
        CHECK_EQ_INT(100, p.autoClockMhz);      // 被抬到 minGpuClockMhz
        CHECK_EQ_INT(0, p.gpuClockPct);
        CHECK_EQ_INT(0, p.minGpuClockPct);
        CHECK_EQ_INT(0, p.autoClockPct);
    }

    // 高于上限
    {
        pg::ControlProfile p;
        p.gpuLimitW = 999;     p.minGpuW = 999;    p.maxGpuW = 1;
        p.cpuMaxPct = 200;     p.cpuMinPct = 150;  p.cpuStepPct = 99;
        p.inputBudgetW = 5000; p.deficitWindowSec = 0;
        p.stepW = 0;           p.holdDownSec = 0;  p.holdUpSec = 0;
        p.gpuClockMhz = 1000;  p.minGpuClockMhz = 1200;
        p.clockStepMhz = 5;    p.autoClockMhz = 900;
        p.gpuClockPct = 150;   p.minGpuClockPct = 200; p.autoClockPct = 300;

        pg::normalizeProfile(p);
        CHECK_EQ_INT(250, p.gpuLimitW);
        CHECK_EQ_INT(250, p.minGpuW);
        CHECK_EQ_INT(250, p.maxGpuW);
        CHECK_EQ_INT(100, p.cpuMaxPct);
        CHECK_EQ_INT(100, p.cpuMinPct);
        CHECK_EQ_INT(50, p.cpuStepPct);
        CHECK_EQ_INT(1000, p.inputBudgetW);
        CHECK_EQ_INT(5, p.deficitWindowSec);
        CHECK_EQ_INT(1, p.stepW);
        CHECK_EQ_INT(1, p.holdDownSec);
        CHECK_EQ_INT(1, p.holdUpSec);
        CHECK_EQ_INT(100, p.gpuClockPct);
        CHECK_EQ_INT(100, p.minGpuClockPct);
        CHECK_EQ_INT(100, p.autoClockPct);
        CHECK_EQ_INT(1000, p.gpuClockMhz);
        CHECK_EQ_INT(1000, p.minGpuClockMhz);   // 不超过显式目标
        CHECK_EQ_INT(10, p.clockStepMhz);
        CHECK_EQ_INT(1200, p.autoClockMhz);     // 先抬到 min, 再压低 min
    }

    // 交叉约束
    {
        pg::ControlProfile p;
        p.gpuLimitW = 40; p.maxGpuW = 30;
        pg::normalizeProfile(p);
        CHECK_EQ_INT(40, p.maxGpuW);            // 不小于 gpuLimitW
    }
    {
        pg::ControlProfile p;
        p.gpuLimitW = 40; p.minGpuW = 50;
        pg::normalizeProfile(p);
        CHECK_EQ_INT(40, p.minGpuW);            // 不大于 gpuLimitW
    }
    {
        pg::ControlProfile p;
        p.cpuMaxPct = 80; p.cpuMinPct = 90;
        pg::normalizeProfile(p);
        CHECK_EQ_INT(80, p.cpuMinPct);          // 不大于 cpuMaxPct
    }
    {
        pg::ControlProfile p;
        p.gpuClockPct = 50; p.minGpuClockPct = 60;
        pg::normalizeProfile(p);
        CHECK_EQ_INT(50, p.minGpuClockPct);     // 不大于 gpuClockPct
    }
    {
        pg::ControlProfile p;
        p.minGpuClockMhz = 900; p.autoClockMhz = 500;
        pg::normalizeProfile(p);
        CHECK_EQ_INT(900, p.autoClockMhz);
    }
    {
        pg::ControlProfile p;
        p.gpuClockMhz = 900; p.minGpuClockMhz = 1200;
        pg::normalizeProfile(p);
        CHECK_EQ_INT(900, p.minGpuClockMhz);
    }

    // 正常值不应被改动
    {
        pg::ControlProfile p;
        p.gpuLimitW = 60;      p.minGpuW = 20;      p.maxGpuW = 80;
        p.cpuMaxPct = 90;      p.cpuMinPct = 40;    p.cpuStepPct = 5;
        p.inputBudgetW = 65;   p.batteryThresholdW = 1.5;
        p.deficitEnergyWh = 0.2; p.deficitWindowSec = 120;
        p.stepCooldownSec = 15; p.stepW = 5;
        p.holdDownSec = 5;     p.holdUpSec = 300;
        p.gpuClockMhz = 1500;  p.minGpuClockMhz = 900;
        p.clockStepMhz = 100;  p.autoClockMhz = 1400;
        p.gpuClockPct = 60;    p.minGpuClockPct = 40; p.autoClockPct = 50;

        pg::normalizeProfile(p);
        CHECK_EQ_INT(60, p.gpuLimitW);
        CHECK_EQ_INT(20, p.minGpuW);
        CHECK_EQ_INT(80, p.maxGpuW);
        CHECK_EQ_INT(90, p.cpuMaxPct);
        CHECK_EQ_INT(40, p.cpuMinPct);
        CHECK_EQ_INT(5, p.cpuStepPct);
        CHECK_EQ_INT(65, p.inputBudgetW);
        CHECK_EQ_DBL(1.5, p.batteryThresholdW);
        CHECK_EQ_DBL(0.2, p.deficitEnergyWh);
        CHECK_EQ_INT(120, p.deficitWindowSec);
        CHECK_EQ_INT(15, p.stepCooldownSec);
        CHECK_EQ_INT(5, p.stepW);
        CHECK_EQ_INT(5, p.holdDownSec);
        CHECK_EQ_INT(300, p.holdUpSec);
        CHECK_EQ_INT(1500, p.gpuClockMhz);
        CHECK_EQ_INT(900, p.minGpuClockMhz);
        CHECK_EQ_INT(100, p.clockStepMhz);
        CHECK_EQ_INT(1400, p.autoClockMhz);
        CHECK_EQ_INT(60, p.gpuClockPct);
        CHECK_EQ_INT(40, p.minGpuClockPct);
        CHECK_EQ_INT(50, p.autoClockPct);
    }
}

// ------------------------------------------------------------
// Web 控制台的段读写: 读 / 写 / 删, 保留文件布局
// ------------------------------------------------------------
void testProfileSectionCrud() {
    auto dir = tempDir();
    const std::string suffix = std::to_string(GetCurrentProcessId());
    auto file  = dir / ("pg_profiles_crud_" + suffix + ".ini");
    auto file2 = dir / ("pg_profiles_crud2_" + suffix + ".ini");
    auto file3 = dir / ("pg_profiles_crud3_" + suffix + ".ini");
    auto file4 = dir / ("pg_profiles_crud4_" + suffix + ".ini");

    auto readAll = [](const std::filesystem::path& p) {
        std::ifstream f(p, std::ios::binary);
        std::string s;
        char buf[512];
        while (f) {
            f.read(buf, sizeof buf);
            s.append(buf, (size_t)f.gcount());
        }
        return s;
    };

    const std::string ini =
        "\xEF\xBB\xBF# top\r\n"
        "[default]\r\n"
        "gpu_limit = 35\r\n"
        "\r\n"
        "; game section\r\n"
        "[Game.exe]\r\n"
        "cpu_max = 90\r\n";
    CHECK(writeFile(file, ini));

    std::string content, norm, err;
    bool found = false;

    // 段名大小写 / .exe 不敏感, 返回段体(不含段头)
    CHECK(pg::readProfileSection(file.string(), "game.EXE", content, found, norm, err));
    CHECK(found);
    CHECK_EQ_STR("game", norm);
    CHECK_EQ_STR("cpu_max = 90", content);

    // 段不存在: 返回成功, found=false
    content = "sentinel";
    CHECK(pg::readProfileSection(file.string(), "missing", content, found, norm, err));
    CHECK(!found);
    CHECK(content.empty());

    // 覆盖已有段: 保留 BOM/注释/其他段, 并沿用文件原有的 CRLF
    CHECK(pg::writeProfileSection(file.string(), "GAME.exe",
                                  "gpu_clock = 900\ncpu_max = 89\n", norm, err));
    CHECK_EQ_STR("game", norm);
    std::string after = readAll(file);
    CHECK(after.find("\xEF\xBB\xBF# top\r\n") == 0);
    CHECK(after.find("[default]\r\n") != std::string::npos);
    CHECK(after.find("; game section\r\n[Game.exe]\r\n"
                     "gpu_clock = 900\r\ncpu_max = 89\r\n") != std::string::npos);
    CHECK(after.find("cpu_max = 90") == std::string::npos);
    CHECK(after.find('\n') != std::string::npos && after.find("\r\n") != std::string::npos);

    // 保存后的文件仍能被解析/应用
    {
        pg::ControlProfile p;
        std::string used;
        CHECK(pg::applyGameProfile(file.string(), "game", p, used));
        CHECK_EQ_STR("game", used);
        CHECK_EQ_INT(35, p.gpuLimitW);
        CHECK_EQ_INT(900, p.gpuClockMhz);
        CHECK_EQ_INT(89, p.cpuMaxPct);
    }

    // 内容校验失败时文件必须保持不变
    const std::string before = readAll(file);
    CHECK(!pg::writeProfileSection(file.string(), "game", "junk = 1\n", norm, err));
    CHECK(err.find("未知键") != std::string::npos);
    CHECK(!pg::writeProfileSection(file.string(), "game", "cpu_max = abc\n", norm, err));
    CHECK(err.find("无效") != std::string::npos);
    CHECK(!pg::writeProfileSection(file.string(), "game", "cpu_max 90\n", norm, err));
    CHECK(err.find("缺少") != std::string::npos);
    CHECK(!pg::writeProfileSection(file.string(), "game", "[other]\nx = 1\n", norm, err));
    CHECK(err.find("不要包含") != std::string::npos);
    CHECK_EQ_STR(before, readAll(file));

    // 非法段名
    CHECK(!pg::writeProfileSection(file.string(), "bad[name", "cpu_max = 90\n",
                                   norm, err));
    CHECK(norm.empty());
    CHECK(!pg::readProfileSection(file.string(), "", content, found, norm, err));

    // 追加新段: 原文件末尾无换行时补齐, 段与段之间空一行
    CHECK(writeFile(file2, "[a]\r\ncpu_max = 80"));
    CHECK(pg::writeProfileSection(file2.string(), "New.EXE", "gpu_limit = 40\n",
                                  norm, err));
    CHECK_EQ_STR("new", norm);
    CHECK_EQ_STR("[a]\r\ncpu_max = 80\r\n\r\n[new]\r\ngpu_limit = 40\r\n",
                 readAll(file2));

    // 文件不存在: 读取算空文件, 保存时创建
    std::error_code ec;
    std::filesystem::remove(file3, ec);
    CHECK(pg::readProfileSection(file3.string(), "x", content, found, norm, err));
    CHECK(!found);
    CHECK(pg::writeProfileSection(file3.string(), "x", "cpu_max = 70\n", norm, err));
    CHECK_EQ_STR("[x]\r\ncpu_max = 70\r\n", readAll(file3));

    // 同名段有多个副本: 读到全部; 保存合并到第一处; 删除全部
    CHECK(writeFile(file4,
        "[g]\r\ncpu_max = 80\r\n"
        "[k]\r\ncpu_max = 85\r\n"
        "[g]\r\ncpu_max = 90\r\n"));
    CHECK(pg::readProfileSection(file4.string(), "g", content, found, norm, err));
    CHECK(found);
    CHECK_EQ_STR("cpu_max = 80\r\ncpu_max = 90", content);

    CHECK(pg::writeProfileSection(file4.string(), "g", "cpu_max = 70\n", norm, err));
    std::string s4 = readAll(file4);
    size_t firstG = s4.find("[g]");
    CHECK(firstG != std::string::npos);
    CHECK(firstG == s4.rfind("[g]"));
    CHECK(s4.find("[k]\r\ncpu_max = 85\r\n") != std::string::npos);
    CHECK(s4.find("cpu_max = 70") != std::string::npos);

    CHECK(pg::removeProfileSection(file4.string(), "G.exe", norm, err));
    std::string s4b = readAll(file4);
    CHECK(s4b.find("[g]") == std::string::npos);
    CHECK(s4b.find("[k]\r\ncpu_max = 85\r\n") != std::string::npos);
    CHECK(pg::removeProfileSection(file4.string(), "g", norm, err));  // 幂等

    std::filesystem::remove(file, ec);
    std::filesystem::remove(file2, ec);
    std::filesystem::remove(file3, ec);
    std::filesystem::remove(file4, ec);
}

} // namespace

int main() {
    std::printf("PowerGuard unit tests\n");
    std::printf("---------------------\n");

    testPowerChangeTracker();
    testApplyProfileKey();
    testApplyGameProfile();
    testInspectGameProfile();
    testNormalizeProfile();
    testProfileSectionCrud();

    std::printf("---------------------\n");
    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
