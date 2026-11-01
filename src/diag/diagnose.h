// ============================================================
// diagnose —— 机器能力探测 (--diagnose)
//
// 换一台机器时, 先跑一遍探测, 再决定用哪个杠杆:
//   GPU 功耗上限可写 -> gpu_limit
//   EC 托管不可写     -> gpu_clock / gpu_clock_pct
//   都没有            -> 只控 CPU
//
// 默认只读, 不需要管理员; --diagnose-validate 会做"写入相同值"
// 的无副作用实测 (需要管理员), 用来确认功耗上限/锁频到底能不能写。
// ============================================================
#pragma once

namespace pg {

// 返回 0 = 至少有一个可用杠杆; 1 = GPU/CPU 都不可控
int runDiagnose(bool validate);

} // namespace pg
