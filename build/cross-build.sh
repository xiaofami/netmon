#!/bin/sh
# 用 OpenWrt/ImmortalWrt SDK 的工具链交叉编译 netmon 与 netmon-report。
#
# 为什么不直接 `make package/netmon/compile`
# -----------------------------------------
# 那需要把仓库塞进 SDK 的 package/ 目录并按 OpenWrt 的包框架走一遍
# （feeds 更新、defconfig、依赖解析），在 CI 里既慢又容易因为上游仓库
# 变动而红。这里只做一件事：用 SDK 的交叉编译器 + 目标机 musl 头文件
# 编出两个二进制，再交给 build/build-apk.py 打包 —— 包结构与本地
# 从路由器取二进制的那条路**完全一致**，安装/卸载脚本不会跑偏。
#
# 用法：
#   SDK_DIR=/path/to/sdk ./build/cross-build.sh [输出目录]
#   ARCH=x86_64 SDK_DIR=... ./build/cross-build.sh out/
#
# 产物：<输出目录>/netmon、<输出目录>/netmon-report（musl ELF）
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${1:-"$ROOT/build/out"}
SDK_DIR=${SDK_DIR:?请设置 SDK_DIR 指向 ImmortalWrt/OpenWrt SDK 根目录}

TC=$(ls -d "$SDK_DIR"/staging_dir/toolchain-* 2>/dev/null | head -1)
[ -n "$TC" ] || { echo "SDK 里找不到 toolchain-* 目录：$SDK_DIR" >&2; exit 1; }

# 从 SDK 里自己找编译器，不要求调用方知道 triple 前缀
# （x86-64 是 x86_64-openwrt-linux，armv8 是 aarch64-openwrt-linux，
#   填错了报错信息很难懂，不如自动探测）。
CC=$(ls "$TC"/bin/*-openwrt-linux-gcc 2>/dev/null | head -1)
[ -x "$CC" ] || { echo "找不到交叉编译器（$TC/bin/*-openwrt-linux-gcc）" >&2; exit 1; }
TRIPLE=$(basename "$CC" -gcc)
echo "==> 工具链：$TRIPLE ($($CC -dumpversion))"

# 工具链找 sysroot 靠这个变量；不设会在链接阶段报 cannot find crt1.o
STAGING_DIR="$SDK_DIR/staging_dir"
export STAGING_DIR

mkdir -p "$OUT" "$ROOT/build/ci"

# ── 1. SQLite 静态库（目标架构）────────────────────────────────────────────
# 仓库不带二进制 .a（不同架构不同），每次现编。amalgamation 单文件，
# 交叉编译一份约 20 秒，加进 CI 缓存不值得，简单可靠优先。
SQL_VER=3500300
SQL_A="$ROOT/build/ci/libsqlite3-$SQL_VER.a"
SQL_SRC="$ROOT/build/ci/sqlite-src"

# 三个来源，优先级从高到低：
#   1. 已经有编好的静态库 → 直接跳过
#   2. SQLITE_SRC_DIR 指向已解压的 amalgamation（CI 缓存 / 离线环境）
#   3. 从 sqlite.org 下载 zip
if [ -f "$SQL_A" ]; then
    echo "==> 复用已有 libsqlite3.a"
else
    if [ -n "${SQLITE_SRC_DIR:-}" ] && [ -f "$SQLITE_SRC_DIR/sqlite3.c" ]; then
        echo "==> 使用现成的 amalgamation：$SQLITE_SRC_DIR"
        rm -rf "$SQL_SRC"; mkdir -p "$SQL_SRC"
        cp "$SQLITE_SRC_DIR"/sqlite3.c "$SQLITE_SRC_DIR"/sqlite3.h "$SQL_SRC/"
    else
        ZIP="$ROOT/build/ci/sqlite-amalgamation-$SQL_VER.zip"
        # 下载到 .part 再改名，并校验 zip 结构：直连 sqlite.org 偶尔会中途截断，
        # 留下「存在但残缺」的 zip，下次运行看到文件在就跳过下载，于是一直用坏文件
        # （本地踩过：解压报 end-of-central-directory）。
        if [ ! -f "$ZIP" ] || ! unzip -tqq "$ZIP" >/dev/null 2>&1; then
            echo "==> 下载 SQLite amalgamation $SQL_VER"
            URL="https://sqlite.org/2025/sqlite-amalgamation-$SQL_VER.zip"
            rm -f "$ZIP" "$ZIP.part"
            if command -v curl >/dev/null 2>&1; then
                curl -fsSL --retry 3 --retry-delay 2 -o "$ZIP.part" "$URL"
            else
                wget -q -O "$ZIP.part" "$URL"
            fi
            unzip -tqq "$ZIP.part" >/dev/null 2>&1 || {
                echo "下载的 zip 不完整：$ZIP.part" >&2; exit 1; }
            mv "$ZIP.part" "$ZIP"
        fi
        rm -rf "$SQL_SRC"; mkdir -p "$SQL_SRC"
        ( cd "$SQL_SRC" && unzip -q -o "$ZIP" )
        # zip 里是 sqlite-amalgamation-XXXX/ 子目录，摊平一层方便后面引用
        found=$(find "$SQL_SRC" -name sqlite3.c | head -1)
        [ -n "$found" ] || { echo "解压后找不到 sqlite3.c" >&2; exit 1; }
        cp "$found" "$(dirname "$found")/sqlite3.h" "$SQL_SRC"/
    fi

    SRC="$SQL_SRC/sqlite3.c"
    [ -f "$SRC" ] || { echo "缺少 $SRC" >&2; exit 1; }

    echo "==> 交叉编译 libsqlite3.a"
    "$CC" -O2 -fPIC -DSQLITE_THREADSAFE=1 \
        -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_DEFAULT_MEMSTATUS=0 \
        -DSQLITE_OMIT_DEPRECATED -DSQLITE_DQS=0 \
        -c "$SRC" -o "$ROOT/build/ci/sqlite3.o"
    "$(dirname "$CC")/$TRIPLE-ar" rcs "$SQL_A" "$ROOT/build/ci/sqlite3.o"
    cp "$SQL_SRC/sqlite3.h" "$ROOT/build/ci/sqlite3.h"
fi

# ── 2. 两个二进制 ──────────────────────────────────────────────────────────
# -I 指向刚才那份 sqlite3.h，避免用到主机 /usr/include 里的 glibc 版本
CFLAGS="-O2 -Wall -Wextra -s -I$ROOT/build/ci -I$ROOT/src"

echo "==> 编译 netmon"
# shellcheck disable=SC2086
"$CC" $CFLAGS -o "$OUT/netmon" \
    "$ROOT/src/netmon.c" "$ROOT/src/db.c" "$ROOT/src/ctsample.c" \
    "$ROOT/src/ctparse.c" "$ROOT/src/dnsmap.c" "$ROOT/src/dnssniff.c" \
    "$ROOT/src/hprules.c" "$SQL_A" -lm

echo "==> 编译 netmon-report"
# shellcheck disable=SC2086
"$CC" $CFLAGS -o "$OUT/netmon-report" \
    "$ROOT/src/netmon-report.c" "$ROOT/src/hprules.c" "$ROOT/src/detail.c" \
    "$SQL_A" -lm

echo "==> 校验产物"
for f in "$OUT/netmon" "$OUT/netmon-report"; do
    size=$(wc -c < "$f")
    [ "$size" -gt 100000 ] || { echo "$f 太小（$size B），编译有问题" >&2; exit 1; }
    # 必须是目标机的 musl ELF；打出 glibc 动态链接的二进制装到路由器上是跑不起来的
    if ! grep -qa "ld-musl" "$f"; then
        echo "$f 没有 musl 解释器，疑似用错了编译器" >&2; exit 1
    fi
    echo "  $(basename "$f")  $size bytes"
done

echo "完成：$OUT"
