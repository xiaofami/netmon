#!/bin/sh
# 验证 CIDR / 聚合表的实际数据分布。
# 造一批连接写入，然后直接查库看落库结果是否符合预期。

set -e
DB=/opt/netmon/verify.db
BIN=/opt/netmon/src/test_db

rm -f "$DB" "$DB-wal" "$DB-shm"

# 用一个临时程序造数据（复用 test_db 的写入路径）
cat > /opt/netmon/src/mkdata.c <<'EOF'
#include "db.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

int main(int argc, char **argv) {
    char e[256];
    db_t *d = db_open(argv[1], 32768, 0, e, sizeof(e));
    if (!d) { printf("open fail: %s\n", e); return 1; }
    if (db_init_schema(d, e, sizeof(e)) != 0) { printf("schema: %s\n", e); return 1; }

    uint64_t now = (uint64_t)time(NULL) * 1000;

    /* 三个不同网段，各 100 条，字节数不同便于核对 */
    struct { int a,b,c; long byt; } nets[] = {
        {104,16,0, 1000000},
        {172,64,0, 2000000},
        {188,114,96, 3000000},
    };

    for (int n = 0; n < 3; n++) {
        for (int i = 0; i < 100; i++) {
            ct_key_t k;
            memset(&k, 0, sizeof(k));
            k.family = 4; k.proto = 6; k.dport = 443;
            k.sport = 1024 + i;
            k.src[10]=0xff; k.src[11]=0xff;
            k.src[12]=10; k.src[13]=89; k.src[14]=2; k.src[15]=(unsigned char)(i+1);
            k.dst[10]=0xff; k.dst[11]=0xff;
            k.dst[12]=(unsigned char)nets[n].a;
            k.dst[13]=(unsigned char)nets[n].b;
            k.dst[14]=(unsigned char)nets[n].c;
            k.dst[15]=(unsigned char)i;

            db_add_conn(d, &k, now + i, now + i + 500,
                        10, nets[n].byt, 10, nets[n].byt * 3, 0, 0, 0, NULL);
        }
    }
    db_flush(d);

    /* 滚一次聚合 */
    uint64_t hour = (now / 3600000) * 3600000;
    db_rollup_hourly(d, hour, hour + 3600000);

    db_stats_t st;
    db_get_stats(d, &st);
    printf("写入 %lld 行，聚合 %lld 行，库 %.1f MB\n",
           st.rows_conns, st.rows_agg, st.db_bytes / 1048576.0);

    db_close(d);
    return 0;
}
EOF

cd /opt/netmon/src
gcc -O2 -Wall -I/opt/netmon/include -o mkdata mkdata.c db.c \
    /opt/netmon/sqa/sqlite-amalgamation-3500300/libsqlite3.a

./mkdata "$DB"

echo ""
echo "=== 明细表按 CIDR 分组 ==="
python3 - "$DB" <<'PY'
import sqlite3, sys
c = sqlite3.connect(sys.argv[1])
for r in c.execute("select cidr, count(*), sum(byt_o), sum(byt_r) "
                   "from conns group by cidr order by cidr"):
    print("  %-18s %4d 行  出=%.2fMB 入=%.2fMB" %
          (r[0], r[1], r[2]/1048576, r[3]/1048576))

print("")
print("=== 小时聚合表 ===")
for r in c.execute("select hour_ms, cidr, conns, bytes_o, bytes_r "
                   "from agg_hour order by cidr"):
    print("  %d  %-18s %4d 条  出=%.2fMB 入=%.2fMB" %
          (r[0], r[1], r[2], r[3]/1048576, r[4]/1048576))

print("")
print("=== 校验 ===")
ok = True
for r in c.execute("select cidr, count(*), sum(byt_o) from conns group by cidr"):
    if r[0] == '0.0.0.0/24':
        print("  FAIL: 出现 0.0.0.0/24，说明 v4-mapped 处理仍有问题")
        ok = False
    if r[1] != 100:
        print("  FAIL: %s 行数 %d != 100" % (r[0], r[1]))
        ok = False
if ok:
    print("  OK: 三个网段各 100 行，无 0.0.0.0/24")
PY

rm -f "$DB" "$DB-wal" "$DB-shm"
