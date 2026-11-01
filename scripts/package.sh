#!/usr/bin/env bash
# ============================================================
# 打包发布 zip:
#   scripts/package.sh [输出目录]     默认 dist
# 产物: dist/PowerGuard-<版本>-win64.zip
# 先跑 build.sh, 再把 exe / 配置 / 文档 / 快捷脚本放进同一目录。
# ============================================================
set -euo pipefail

cd "$(dirname "$0")/.."

VERSION="$(sed -n 's/^#define POWERGUARD_VERSION "\(.*\)"/\1/p' src/common/version.h)"
if [ -z "$VERSION" ]; then
    echo "[ERROR] 读不到 src/common/version.h 里的版本号" >&2
    exit 1
fi

OUTDIR="${1:-dist}"
NAME="PowerGuard-$VERSION"
STAGE="$OUTDIR/$NAME"
ZIP="$OUTDIR/$NAME-win64.zip"

./build.sh

rm -rf "$STAGE"
mkdir -p "$STAGE/bin"

cp bin/PowerGuard.exe "$STAGE/bin/"
cp README.md CHANGELOG.md LICENSE CONTRIBUTING.md profiles.ini "$STAGE/"
cp PowerGuard.bat PowerGuard-restore.bat \
   PowerGuard-recovery-task.bat PowerGuard-recovery-task.ps1 "$STAGE/"

rm -f "$ZIP"
if command -v zip >/dev/null 2>&1; then
    (cd "$OUTDIR" && zip -qr "$NAME-win64.zip" "$NAME")
else
    # Git Bash 的 /tmp 等路径 PowerShell 看不到, 先转成 Windows 路径
    WSTAGE="$STAGE"
    WZIP="$ZIP"
    if command -v cygpath >/dev/null 2>&1; then
        WSTAGE="$(cygpath -w "$STAGE")"
        WZIP="$(cygpath -w "$ZIP")"
    fi
    powershell -NoProfile -Command \
        "Compress-Archive -Path '$WSTAGE' -DestinationPath '$WZIP' -Force"
fi

echo "[OK] 已生成 $ZIP"
