---
name: Bug 报告
about: 控制不生效、报错、断电、参数行为异常
title: '[Bug] '
labels: bug
---

## 现象

<!-- 发生了什么，和期望的有什么差别 -->

## 复现步骤

1.
2.
3.

## 环境

- PowerGuard 版本（`PowerGuard.exe --version`）:
- Windows 版本:
- 笔记本型号 / 台式机:
- GPU 型号:
- 是否以管理员身份运行:
- 是否有电池 / 是否用 `--budget`:

## 诊断信息

请贴 `PowerGuard.exe --diagnose` 的输出（去掉个人路径即可）：

```text
（贴这里）
```

如果问题与控制效果有关，再补几行 CSV（`logs/power_YYYY-MM-DD.csv`），
尤其是 `battery_w` 和 `action` 两列：

```text
（贴这里）
```

## 附加信息

<!-- 有无杀软拦截、厂商驱动版本、是否插着原装电源等 -->
