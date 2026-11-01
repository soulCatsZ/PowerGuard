// ============================================================
// PowerChangeTracker —— 功率写入去抖 / 变化检测
//
// 保存"上一次真正写入"的 CPU/GPU 功率作为基线:
//   新值与基线偏差 >= 阈值      -> 返回 true, 写入新值并更新基线
//   偏差 < 阈值                 -> 返回 false, 基线保持不动
//                                  (小偏差会累积, 缓慢漂移最终也会触发写入)
//   超过心跳间隔没写过          -> 强制返回 true 并更新基线, 保证时间轴连续
//
// 阈值规则(可同时启用, 满足任一即算波动大):
//   absW   : 绝对阈值, 单位瓦, <=0 关闭
//   relPct : 相对阈值, 0.05 = 5%, <=0 关闭
// 两路功率中任一通道超过阈值就会写入整对 CPU/GPU 值。
// ============================================================
#pragma once

#include <windows.h>
#include <cmath>
#include <mutex>

namespace pg {

class PowerChangeTracker {
public:
    // absW: 绝对阈值(瓦); relPct: 相对阈值(小数); heartbeatMs: 心跳间隔, 0 关闭
    explicit PowerChangeTracker(double absW = 0.5,
                                double relPct = 0.0,
                                unsigned long long heartbeatMs = 60000)
        : absW_(absW), relPct_(relPct), heartbeatMs_(heartbeatMs),
          lastTick_(GetTickCount64()) {}

    // 传入新的 CPU/GPU 功率(W)。
    // 返回 true 表示波动大, 调用方应写入并可直接使用这组值;
    // 返回 false 表示波动小, 调用方跳过写入, 基线不变。
    bool shouldWrite(double cpuW, double gpuW) {
        std::lock_guard<std::mutex> lk(mu_);
        unsigned long long now = GetTickCount64();

        bool need = false;
        if (!hasBaseline_)
            need = true;                                        // 第一条必写
        else if (exceeds(cpuW, lastCpu_) || exceeds(gpuW, lastGpu_))
            need = true;                                        // 任一通道波动大
        else if (heartbeatMs_ > 0 && now - lastTick_ >= heartbeatMs_)
            need = true;                                        // 心跳补记

        if (need) {
            lastCpu_ = cpuW;
            lastGpu_ = gpuW;
            hasBaseline_ = true;
            lastTick_ = now;
        }
        return need;
    }

    void reset() {                                              // 清空基线, 下次必写
        std::lock_guard<std::mutex> lk(mu_);
        hasBaseline_ = false;
    }

    bool hasBaseline() const {
        std::lock_guard<std::mutex> lk(mu_);
        return hasBaseline_;
    }

    double lastCpu() const {
        std::lock_guard<std::mutex> lk(mu_);
        return lastCpu_;
    }

    double lastGpu() const {
        std::lock_guard<std::mutex> lk(mu_);
        return lastGpu_;
    }

private:
    // 偏差是否超过阈值(绝对/相对任一满足)
    bool exceeds(double cur, double base) const {
        double diff = std::fabs(cur - base);
        if (absW_ > 0 && diff >= absW_) return true;
        if (relPct_ > 0) {
            double rel = std::fabs(base) * relPct_;
            if (rel > 0 && diff >= rel) return true;
        }
        return false;
    }

    mutable std::mutex mu_;
    double absW_;                 // 绝对阈值(W)
    double relPct_;               // 相对阈值(比例)
    unsigned long long heartbeatMs_;   // 心跳间隔(ms), 0 = 关闭
    unsigned long long lastTick_;      // 上次写入的时刻
    double lastCpu_ = 0.0;        // 上次写入时的 CPU 功率
    double lastGpu_ = 0.0;        // 上次写入时的 GPU 功率
    bool hasBaseline_ = false;
};

} // namespace pg
