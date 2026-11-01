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

#include "control/profile.h"

namespace pg {

// 读取 INI 并把 [default] + [game] 的键应用到 prof 上。
// game 为空时只应用 [default]。命中任意一段返回 true, usedSection 写入
// 最后生效的段名 (优先 game 段, 否则 "default")。
bool applyGameProfile(const std::string& path, const std::string& game,
                      ControlProfile& prof, std::string& usedSection);

// 单个键的应用(暴露出来便于测试/复用); 未知键返回 false
bool applyProfileKey(ControlProfile& prof, const std::string& key,
                     const std::string& value);

} // namespace pg
