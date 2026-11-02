#!/usr/bin/env bash
# 编译并运行纯逻辑单元测试 (不需要管理员 / NVML / 电池)。
set -euo pipefail
cd "$(dirname "$0")/.."

OUT="tests/pg_tests.exe"
echo ">>> 编译单元测试"
g++ -std=c++17 -Wall -Wextra -Isrc \
    tests/test_main.cpp \
    src/control/game_profile.cpp \
    src/control/profile.cpp \
    -o "$OUT" \
    -static -static-libgcc -static-libstdc++

echo ">>> 运行单元测试"
"$OUT"
