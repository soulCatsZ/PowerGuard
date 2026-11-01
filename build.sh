#!/usr/bin/env bash
# ============================================================
# PowerGuard 编译脚本 (Git Bash / MSYS2)
# 用法:
#   ./build.sh                       编译到 bin/PowerGuard.exe
#   ./build.sh bin/PowerGuard_new.exe    编译到其他文件名(旧程序占用时可这样用)
# ============================================================
set -euo pipefail

cd "$(dirname "$0")"

OUT="${1:-bin/PowerGuard.exe}"
IMG="$(basename "$OUT")"

# 1. 检查编译器
if ! command -v g++ >/dev/null 2>&1; then
    echo "[ERROR] 找不到 g++, 请把 MinGW 的 bin 目录加入 PATH" >&2
    exit 1
fi

# 2. 目标文件正在运行时, 给出明确提示(否则只会报链接失败)
if tasklist.exe 2>/dev/null | grep -qi "^${IMG}"; then
    echo "[ERROR] ${IMG} 正在运行, 请先输入 quit 退出(或结束该进程)再编译" >&2
    echo "        也可以换个文件名: ./build.sh bin/PowerGuard_new.exe" >&2
    exit 1
fi

# 3. 收集 src 下所有源文件并编译
mapfile -t SRCS < <(find src -name '*.cpp' | sort)
mkdir -p "$(dirname "$OUT")"

echo ">>> g++ -Isrc ${SRCS[*]} -o ${OUT} -lpdh -lpowrprof -lsetupapi -luuid -lws2_32 -static -static-libgcc -static-libstdc++"
g++ -Isrc "${SRCS[@]}" -o "${OUT}" -lpdh -lpowrprof -lsetupapi -luuid -lws2_32 -static -static-libgcc -static-libstdc++

echo "[OK] 已生成 ${OUT}"
