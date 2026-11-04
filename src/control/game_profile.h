// ============================================================
// game_profile —— 分游戏参数 (profiles.ini)
//
// 不同游戏的瓶颈不同: 有的压 GPU 就够, 有的必须动 CPU。
// 这里用最简单的 INI 给每个 exe 存一份覆盖值:
//
//   [default]          # 所有游戏都适用
//   gpu_clock = 1400
//
//   [bg3_dx11]         # 覆盖 default, 段名 = 进程名(不含 .exe, 大小写不敏感)
//   gpu_clock = 1000
//   cpu_max = 94
//
// 未写的键沿用命令行/profile 的默认值。
// ============================================================
#pragma once

#include <string>
#include <vector>

#include "control/profile.h"

namespace pg {

// 单个段的校验摘要, 供 --check-profiles 输出
struct ProfileSectionInfo {
    std::string name;      // 归一化段名 (小写, 去掉 .exe)
    int line = 0;          // 段头所在行号 (从 1 开始)
    int validKeys = 0;
    int invalidKeys = 0;
};

struct ProfileReport {
    bool fileFound = false;                 // 文件存在且能打开
    std::vector<ProfileSectionInfo> sections;
    std::vector<std::string> warnings;      // 人类可读, 带行号/段名
};

// 读取 INI 并把 [default] + [game] 的键应用到 prof 上。
// game 为空时只应用 [default]。命中任意一段返回 true, usedSection 写入
// 最后生效的段名 (优先 game 段, 否则 "default")。
// warnings 非空时, 写入文件中所有未知键/非法值的说明 (不只是命中的段)。
bool applyGameProfile(const std::string& path, const std::string& game,
                      ControlProfile& prof, std::string& usedSection,
                      std::vector<std::string>* warnings = nullptr);

// 只解析和校验, 不修改 prof。文件能打开返回 true。
bool inspectGameProfile(const std::string& path, ProfileReport& report);

// ---- Web 控制台用的段读写 (段名 = 进程名, 大小写/.exe 不敏感) ----
// 读取段内容(不含 [section] 行); 段不存在时 found=false 但返回 true。
// normalized 输出归一化后的段名。
bool readProfileSection(const std::string& path, const std::string& name,
                        std::string& content, bool& found, std::string& normalized,
                        std::string& error);

// 校验并写入段内容。content 每行是 "key = value"(允许空行/注释)。
// 同一段有多个副本时会合并到第一处; 文件采用临时文件 + 原子替换。
bool writeProfileSection(const std::string& path, const std::string& name,
                         const std::string& content, std::string& normalized,
                         std::string& error);

// 删除段(所有同名副本); 段本来不存在也算成功。
bool removeProfileSection(const std::string& path, const std::string& name,
                          std::string& normalized, std::string& error);

// 单个键的应用(暴露出来便于测试/复用); 未知键或非法值返回 false
bool applyProfileKey(ControlProfile& prof, const std::string& key,
                     const std::string& value);

} // namespace pg
