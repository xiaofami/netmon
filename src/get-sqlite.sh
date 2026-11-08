#!/bin/sh
# 下载 SQLite amalgamation 并编译成静态库 build/sqlite/libsqlite3.a
#
# 为什么用静态库
# --------------
# 采集与报表只需要 SQLite 的核心功能。静态链接后：
#   * 目标机不用装 libsqlite3-0（省 ~700KB flash）
#   * 版本完全固定，不受发行版升级影响
#   * 没有运行时 .so 查找失败的风险
#
# 用法：
#   sh src/get-sqlite.sh            # 下载 + 编译到 build/sqlite/
#   make SQLITE_CFLAGS=-Ibuild/sqlite SQLITE_LIBS=build/sqlite/libsqlite3.a
set -e

VER=3500300                       # 3.50.3，改版本时同步改下面的 URL 与 sha256
ZIP="sqlite-amalgamation-$VER.zip"
URL="https://sqlite.org/2025/$ZIP"
SHA256=""                         # 留空则跳过校验；填上更安全

ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT="$ROOT/build/sqlite"
TMP="${TMPDIR:-$ROOT/build}/sqlite-dl"

mkdir -p "$OUT" "$TMP"

if [ ! -f "$OUT/libsqlite3.a" ]; then
    echo "==> 下载 $ZIP"
    if command -v curl >/dev/null 2>&1; then
        curl -fL --retry 3 -o "$TMP/$ZIP" "$URL"
    else
        wget -O "$TMP/$ZIP" "$URL"
    fi

    if [ -n "$SHA256" ]; then
        echo "$SHA256  $TMP/$ZIP" | sha256sum -c -
    fi

    echo "==> 解压"
    rm -rf "$TMP/src"
    mkdir -p "$TMP/src"
    # busybox unzip 也能处理标准 zip
    ( cd "$TMP/src" && unzip -q -o "$TMP/$ZIP" )

    echo "==> 编译静态库"
    SRC=$(find "$TMP/src" -name sqlite3.c | head -1)
    HDR=$(dirname "$SRC")
    ${CC:-gcc} -O2 -fPIC -DSQLITE_THREADSAFE=1 \
        -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_DEFAULT_MEMSTATUS=0 \
        -DSQLITE_OMIT_DEPRECATED -DSQLITE_DQS=0 \
        -c "$SRC" -o "$OUT/sqlite3.o"
    ar rcs "$OUT/libsqlite3.a" "$OUT/sqlite3.o"
    cp "$HDR/sqlite3.h" "$OUT/"
    echo "完成：$OUT/libsqlite3.a  ($(du -h "$OUT/libsqlite3.a" | cut -f1))"
else
    echo "已存在：$OUT/libsqlite3.a（删除后重跑可重新生成）"
fi
