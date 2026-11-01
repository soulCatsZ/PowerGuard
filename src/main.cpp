// PowerGuard 入口 —— 只负责启动业务, 具体实现都在各模块中:
//   collectors/*     功率采集(GPU: NVML, CPU: PDH / RAPL)
//   monitor/*        进程监控
//   app/*            主流程(事件循环、CSV、输入线程)
//   common/*         日志、功率去抖
// 用法: PowerGuard.exe [--process <进程名>] [--only-running]
#include "app/powerguard_app.h"

int main(int argc, char** argv) {
    pg::PowerGuardApp app(argc, argv);
    return app.run();
}
