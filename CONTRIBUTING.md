# 参与贡献

感谢关注 PowerGuard。提交前请先阅读下面的约定。

## 环境

- Windows 10 / 11（x64）
- MinGW-w64 g++（MSYS2 的 `mingw-w64-x86_64-gcc` 即可）
- Git Bash / MSYS2 shell

## 构建

```bash
./build.sh
./bin/PowerGuard.exe --version
./bin/PowerGuard.exe --diagnose
```

`build.sh` 会自动收集 `src/**/*.cpp` 并静态链接运行时，产物为单文件
`bin/PowerGuard.exe`，不需要额外的 DLL。

## 测试

项目目前没有单元测试，提交前请至少完成：

1. `./build.sh` 编译无警告（允许平台 API 的弃用提示）。
2. `PowerGuard.exe --diagnose` 输出正常，非管理员下 GPU 显示“需要管理员”而非“不可用”。
3. `PowerGuard.exe --profile usbc --dry-run --apply-always` 启动后输入 `quit`，
   确认参数解析、原始值读取、退出恢复正常。
4. 改动控制逻辑时，额外跑一次 `--budget` 或真实电池场景，确认降档/回升日志符合预期。

涉及真实硬件写入的测试请自行承担风险，先用 `--dry-run` 验证。

## 代码约定

- C++17，单文件可执行，不引入第三方运行库；平台 API 通过 `LoadLibrary` /
  `GetProcAddress` 动态加载（如 NVML），缺失时必须优雅降级。
- 所有用户可见文本用中文，日志带模块名（`gpu` / `cpu` / `bat` / `ctl` / `web`）。
- 写系统设置前必须备份原值；新增杠杆时同步考虑 `restoreControl()`、
  状态文件和 `--restore-if-stale` 的恢复路径。
- 注释解释“为什么”，而不是重复代码在做什么。

## 提交

- 一个提交只做一件事，消息用中文或英文均可，说明动机与影响。
- 修改 `ControlProfile` 字段时，同步更新 `game_profile.cpp` 的键解析、
  参数表（README）与 `profiles.ini` 注释。
- 修改 CLI 参数时同步更新 `printUsage()` 和 README 参数表。
- 发布前更新 `src/common/version.h` 与 `CHANGELOG.md`。

## 平台支持范围

当前只支持 **NVIDIA GPU**（NVML）。AMD / Intel 显卡的控制支持欢迎贡献，
但请先通过 `--diagnose` 的降级路径保证不影响只读采集与其他杠杆。

## 安全问题

PowerGuard 会写入 GPU 功耗上限、核心频率与 CPU 最大处理器状态。请勿提交
针对特定厂商 EC 的绕过、未授权提权或破坏恢复路径的代码。安全问题请通过
仓库的私密渠道联系维护者，不要直接开公开 issue。
