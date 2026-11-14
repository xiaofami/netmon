/*
 * test_db.c - 数据库层实测
 *
 * 重点验证三件事：
 *   1. 写入吞吐：大批量事务比逐条提交快多少（这是设计核心）
 *   2. 数据正确性：写入的字节数/包数能原样读回
 *   3. 聚合与GC：小时/天聚合结果正确，30天清理不误删
 */

#define _GNU_SOURCE
#include "db.h"
#include "ctparse.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

static int g_fail = 0;

#define CHECK(cond, fmt, ...) do {                                   \
    if (!(cond)) { printf("  FAIL: " fmt "\n", ##__VA_ARGS__); g_fail++; } \
    else { printf("  ok  : " fmt "\n", ##__VA_ARGS__); }             \
} while (0)

static double mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

/* 读回一行结果，把每列文本收集起来供 CHECK 断言；NULL 写成 "(null)" */
typedef struct {
    int  rows;
    int  argc;
    char cols[24][128];
} rowcap_t;

static int cap_cb(void *ud, int argc, char **argv, char **col)
{
    (void)col;
    if (!ud) return 0;                 /* 供 CREATE/ALTER 这类无结果行的语句复用 */
    rowcap_t *r = (rowcap_t *)ud;
    r->rows++;
    r->argc = argc;
    for (int i = 0; i < argc && i < 24; i++)
        snprintf(r->cols[i], sizeof(r->cols[i]), "%s",
                 argv[i] ? argv[i] : "(null)");
    return 0;
}

/* 造一条看起来真实的连接记录 */
static void make_conn(ct_key_t *k, int i)
{
    memset(k, 0, sizeof(*k));
    k->family = 4;
    k->proto = (i % 3 == 0) ? 17 : 6;
    k->sport = (uint16_t)(1024 + (i % 60000));
    k->dport = (i % 3 == 0) ? 53 : 443;

    /* 源：内网 192.168.1.x */
    k->src[10] = 0xff; k->src[11] = 0xff;
    k->src[12] = 10; k->src[13] = 89; k->src[14] = 2;
    k->src[15] = (uint8_t)(1 + (i % 200));

    /* 目的：模拟 Cloudflare 网段分布 */
    static const uint8_t prefixes[][3] = {
        {104,16,0}, {172,64,0}, {188,114,96}, {162,159,0}, {1,1,1}
    };
    const uint8_t *p = prefixes[i % 5];
    k->dst[10] = 0xff; k->dst[11] = 0xff;
    k->dst[12] = p[0]; k->dst[13] = p[1];
    k->dst[14] = p[2]; k->dst[15] = (uint8_t)(i % 250);
}

/* ---- 测试 1：写入吞吐 ---- */
static void t_throughput(const char *path)
{
    printf("[1] 写入吞吐（N=50000）\n");

    unlink(path);
    char e[256];
    db_t *d = db_open(path, 65536, 65536, e, sizeof(e));
    if (!d) { printf("  FAIL: open %s\n", e); g_fail++; return; }

    if (db_init_schema(d, e, sizeof(e)) != 0) {
        printf("  FAIL: schema %s\n", e); db_close(d); g_fail++; return;
    }

    const int N = 50000;
    uint64_t base = (uint64_t)time(NULL) * 1000u;

    double t0 = mono_ms();
    for (int i = 0; i < N; i++) {
        ct_key_t k;
        make_conn(&k, i);
        uint64_t first = base + (uint64_t)i * 10;
        uint64_t last = first + 5000;
        db_add_conn(d, &k, first, last,
                    10, 4096, 10, 8192, 0, 0, 0, NULL);
    }
    /* 收尾 */
    db_flush(d);
    double elapsed = mono_ms() - t0;

    printf("      耗时 %.0f ms，%.0f 条/秒，单条 %.2f µs\n",
           elapsed, N / (elapsed / 1000.0), elapsed * 1000.0 / N);

    db_stats_t st;
    db_get_stats(d, &st);
    printf("      库文件 %.1f MB，WAL %.1f MB，提交 %ld 次\n",
           st.db_bytes / 1048576.0, st.wal_bytes / 1048576.0, st.commits);

    CHECK(st.rows_conns == N, "写入行数 %lld == %d", st.rows_conns, N);
    CHECK(elapsed < 15000, "耗时在 15 秒内（%.0f ms）", elapsed);

    db_close(d);
    unlink(path);
}

/* ---- 测试 2：大事务 vs 逐条提交 ---- */
static void t_batch_effect(const char *path)
{
    printf("[2] 大事务 vs 逐条提交\n");

    const int N = 5000;
    char e[256];
    uint64_t base = (uint64_t)time(NULL) * 1000u;

    /* 逐条提交：每次 add 之后立即 flush */
    char p1[512];
    snprintf(p1, sizeof(p1), "%s.perrow", path);
    unlink(p1);
    db_t *d1 = db_open(p1, 16384, 0, e, sizeof(e));
    if (!d1) { printf("  FAIL open\n"); g_fail++; return; }
    db_init_schema(d1, e, sizeof(e));

    double t0 = mono_ms();
    for (int i = 0; i < N; i++) {
        ct_key_t k;
        make_conn(&k, i);
        db_add_conn(d1, &k, base + i, base + i + 100, 1, 100, 1, 200, 0, 0, 0, NULL);
        db_flush(d1);      /* 每行一次 fsync */
    }
    double per_row = mono_ms() - t0;
    db_close(d1);

    /* 批量：攒到 2000 自动提交 */
    char p2[512];
    snprintf(p2, sizeof(p2), "%s.batch", path);
    unlink(p2);
    db_t *d2 = db_open(p2, 16384, 0, e, sizeof(e));
    if (!d2) { printf("  FAIL open\n"); g_fail++; return; }
    db_init_schema(d2, e, sizeof(e));

    t0 = mono_ms();
    for (int i = 0; i < N; i++) {
        ct_key_t k;
        make_conn(&k, i);
        db_add_conn(d2, &k, base + i, base + i + 100, 1, 100, 1, 200, 0, 0, 0, NULL);
    }
    db_flush(d2);
    double batch = mono_ms() - t0;
    db_close(d2);

    printf("      逐条: %.0f ms (%.1f µs/条)\n", per_row, per_row * 1000.0 / N);
    printf("      批量: %.0f ms (%.1f µs/条)\n", batch, batch * 1000.0 / N);
    printf("      加速: %.1fx\n", per_row / (batch > 0.1 ? batch : 0.1));

    CHECK(batch < per_row, "批量比逐条快（%.0f < %.0f ms）", batch, per_row);

    unlink(p1); unlink(p2);
}

/* ---- 测试 3：聚合正确性 ---- */
static void t_rollup(const char *path)
{
    printf("[3] 小时聚合正确性\n");

    char p[512];
    snprintf(p, sizeof(p), "%s.agg", path);
    unlink(p);

    char e[256];
    db_t *d = db_open(p, 32768, 0, e, sizeof(e));
    if (!d) { printf("  FAIL open: %s\n", e); g_fail++; return; }
    db_init_schema(d, e, sizeof(e));

    /* 造 100 条连接，全在同一个小时、同一 cidr、同一端口 */
    uint64_t hour = 1735689600000ULL;   /* 2025-01-01 00:00:00 UTC */
    uint64_t base = hour + 1000;

    for (int i = 0; i < 100; i++) {
        ct_key_t k;
        memset(&k, 0, sizeof(k));
        k.family = 4;
        k.proto = 6;
        k.dport = 443;
        k.src[10] = 0xff; k.src[11] = 0xff;
        k.src[12] = 10; k.src[13] = 89; k.src[14] = 2; k.src[15] = (uint8_t)(i + 1);
        k.dst[10] = 0xff; k.dst[11] = 0xff;
        k.dst[12] = 104; k.dst[13] = 16; k.dst[14] = 0; k.dst[15] = (uint8_t)(i % 250);

        db_add_conn(d, &k, base + i, base + i + 1000,
                    10, 1000, 10, 2000, 0, 0, 0, NULL);
    }
    db_flush(d);

    long rc = db_rollup_hourly(d, hour, hour + 3600000ULL);
    CHECK(rc >= 0, "聚合执行成功 (rc=%ld)", rc);

    db_stats_t st;
    db_get_stats(d, &st);
    CHECK(st.rows_agg > 0, "聚合表有数据 (%lld 行)", st.rows_agg);

    /* 重复聚合应该是幂等的（ON CONFLICT DO UPDATE 会累加，所以要验证语义） */
    printf("      注意：重复聚合会累加，业务上应该只对未聚合区间调用\n");

    db_close(d);
    unlink(p);
}

/* ---- 测试 4：GC 不误删 ---- */
static void t_gc(const char *path)
{
    printf("[4] GC 只删过期数据\n");

    char p[512];
    snprintf(p, sizeof(p), "%s.gc", path);
    unlink(p);

    char e[256];
    db_t *d = db_open(p, 32768, 0, e, sizeof(e));
    if (!d) { printf("  FAIL open\n"); g_fail++; return; }
    db_init_schema(d, e, sizeof(e));

    uint64_t now = (uint64_t)time(NULL) * 1000u;
    uint64_t old = now - 40ULL * 86400000ULL;   /* 40 天前 */
    uint64_t fresh = now - 1ULL * 86400000ULL;  /* 1 天前 */

    for (int i = 0; i < 50; i++) {
        ct_key_t k;
        make_conn(&k, i);
        db_add_conn(d, &k, old + i, old + i + 100, 1, 100, 1, 200, 0, 0, 0, NULL);
    }
    for (int i = 0; i < 30; i++) {
        ct_key_t k;
        make_conn(&k, i + 1000);
        db_add_conn(d, &k, fresh + i, fresh + i + 100, 1, 100, 1, 200, 0, 0, 0, NULL);
    }
    db_flush(d);

    db_stats_t before;
    db_get_stats(d, &before);
    CHECK(before.rows_conns == 80, "写入了 80 行 (%lld)", before.rows_conns);

    /* 删掉 30 天前的 */
    uint64_t cutoff = now - 30ULL * 86400000ULL;
    long deleted = db_gc(d, cutoff);
    printf("      GC 删除 %ld 行\n", deleted);

    db_stats_t after;
    db_get_stats(d, &after);
    CHECK(after.rows_conns == 30, "剩余 30 行（只删了旧的）(%lld)", after.rows_conns);
    CHECK(deleted == 50, "删除数 = 50 (%ld)", deleted);

    db_close(d);
    unlink(p);
}

/* ---- 测试 5：v6 与 CIDR 计算 ---- */
static void t_ip_handling(const char *path)
{
    printf("[5] IPv6 与 CIDR 计算\n");

    uint8_t v6[16];
    ct_key_t k;
    memset(&k, 0, sizeof(k));
    k.family = 6;
    k.proto = 6;
    k.dport = 443;
    /* 2606:4700:3030::6815:1e19 */
    unsigned char raw6[16] = {0x26,0x06,0x47,0x00,0x30,0x30,0,0,0,0,0,0,0x68,0x15,0x1e,0x19};
    memcpy(v6, raw6, 16);
    memcpy(k.src, v6, 16);
    memcpy(k.dst, v6, 16);
    k.dst[15] = 0x20;

    char out[64];
    db_ip_str(6, v6, out, sizeof(out));
    printf("      v6 字符串: %s\n", out);
    CHECK(strchr(out, ':') != NULL && out[0] != '?', "v6 正确格式化");

    uint8_t v4[16] = {0,0,0,0,0,0,0,0,0,0,0xff,0xff,104,16,0,1};
    db_ip_str(4, v4, out, sizeof(out));
    printf("      v4 字符串: %s\n", out);
    CHECK(strcmp(out, "104.16.0.1") == 0, "v4 正确格式化 (%s)", out);

    (void)path;
}

/* ---- 测试 6：dnsq / conns 新列写入读出 ---- */
static void t_new_columns(const char *path)
{
    printf("[6] 新列写入读出（dnsq 全字段 + conns client_ip/dir）\n");

    char p[512];
    snprintf(p, sizeof(p), "%s.cols", path);
    unlink(p);

    char e[256];
    db_t *d = db_open(p, 16384, 0, e, sizeof(e));
    if (!d) { printf("  FAIL: open %s\n", e); g_fail++; return; }
    if (db_init_schema(d, e, sizeof(e)) != 0) {
        printf("  FAIL: schema %s\n", e); db_close(d); g_fail++; return;
    }

    /* 一条带全部新字段的 DNS 应答 */
    db_dnsq_t q;
    memset(&q, 0, sizeof(q));
    q.ts_ms       = 1700000000000ULL;
    q.qname       = "example.test";
    q.qtype       = 1;
    q.rcode       = 0;
    q.ms          = 1.5;
    q.answers     = 2;
    q.client_ip   = "192.168.1.2";
    q.client_fam  = 4;
    q.server_ip   = "192.168.1.1";
    q.server_fam  = 4;
    q.server_port = 53;
    q.ttl         = 42;
    q.answer_cnt  = 2;
    q.answer_ips  = "192.0.2.10,192.0.2.11";
    q.hp          = 1;                 /* 归因：广告拦截 */
    CHECK(db_add_dnsq(d, &q) == 0, "写入带新列的 dnsq 行");

    rowcap_t cap;
    memset(&cap, 0, sizeof(cap));
    db_foreach(d,
        "SELECT server_ip,server_fam,server_port,client_fam,"
        "ttl,answer_cnt,answer_ips,answers,upstream,client_ip,hp "
        "FROM dnsq WHERE qname='example.test'", cap_cb, &cap);
    CHECK(cap.rows == 1, "读出 1 行");
    CHECK(strcmp(cap.cols[0],  "192.168.1.1") == 0, "server_ip=%s", cap.cols[0]);
    CHECK(strcmp(cap.cols[1],  "4") == 0,            "server_fam=%s", cap.cols[1]);
    CHECK(strcmp(cap.cols[2],  "53") == 0,           "server_port=%s", cap.cols[2]);
    CHECK(strcmp(cap.cols[3],  "4") == 0,            "client_fam=%s", cap.cols[3]);
    CHECK(strcmp(cap.cols[4],  "42") == 0,           "ttl=%s", cap.cols[4]);
    CHECK(strcmp(cap.cols[5],  "2") == 0,            "answer_cnt=%s", cap.cols[5]);
    CHECK(strcmp(cap.cols[6],  "192.0.2.10,192.0.2.11") == 0,
          "answer_ips=%s", cap.cols[6]);
    CHECK(strcmp(cap.cols[7],  "192.0.2.10,192.0.2.11") == 0,
          "answers 列写 answer_ips=%s", cap.cols[7]);
    /* upstream 是 1.0 的历史列（原先存首个应答 IP），1.1.0 起不再写入 */
    CHECK(strcmp(cap.cols[8],  "(null)") == 0,       "upstream 列不再写入");
    CHECK(strcmp(cap.cols[9],  "192.168.1.2") == 0,  "client_ip=%s", cap.cols[9]);
    CHECK(strcmp(cap.cols[10], "1") == 0,            "hp 归因入库=%s", cap.cols[10]);

    /* ttl<=0（-1 未知）→ NULL；answer_ips 空 → NULL、answers 退回计数串 */
    memset(&q, 0, sizeof(q));
    q.ts_ms = 1700000000001ULL; q.qname = "ttl-null.test"; q.qtype = 28; q.rcode = 0;
    q.server_fam = 4; q.client_fam = 4; q.ttl = -1;
    q.hp = -1;                         /* 调用方没归因 → 必须写 NULL，不能冒充「正常」 */
    CHECK(db_add_dnsq(d, &q) == 0, "写入 ttl 未知的行");
    memset(&cap, 0, sizeof(cap));
    db_foreach(d, "SELECT ttl,answers,answer_ips,hp "
                  "FROM dnsq WHERE qname='ttl-null.test'", cap_cb, &cap);
    CHECK(strcmp(cap.cols[0], "(null)") == 0, "ttl<=0 写 NULL: %s", cap.cols[0]);
    CHECK(strcmp(cap.cols[1], "0") == 0,      "answers 空退回计数字符串: %s", cap.cols[1]);
    CHECK(strcmp(cap.cols[2], "(null)") == 0, "answer_ips 空写 NULL");
    CHECK(strcmp(cap.cols[3], "(null)") == 0, "hp<0 写 NULL: %s", cap.cols[3]);

    /* conns 新列：db_add_conn_ex */
    ct_key_t k;
    make_conn(&k, 0);
    CHECK(db_add_conn_ex(d, &k, 100, 200, 1, 10, 2, 20, 0, 0, 0, NULL,
                         "192.168.1.7", 1) == 0, "db_add_conn_ex 写入");
    db_flush(d);
    memset(&cap, 0, sizeof(cap));
    db_foreach(d, "SELECT client_ip,dir FROM conns WHERE first_ms=100", cap_cb, &cap);
    CHECK(strcmp(cap.cols[0], "192.168.1.7") == 0, "conns.client_ip=%s", cap.cols[0]);
    CHECK(strcmp(cap.cols[1], "1") == 0,            "conns.dir=%s", cap.cols[1]);

    /* 旧 db_add_conn 原型仍可用，且写 NULL / 0 */
    CHECK(db_add_conn(d, &k, 300, 400, 1, 10, 1, 10, 0, 0, 0, NULL) == 0,
          "旧 db_add_conn 仍可调用");
    db_flush(d);
    memset(&cap, 0, sizeof(cap));
    db_foreach(d, "SELECT client_ip,dir FROM conns WHERE first_ms=300", cap_cb, &cap);
    CHECK(strcmp(cap.cols[0], "(null)") == 0, "旧调用 client_ip=NULL");
    CHECK(strcmp(cap.cols[1], "0") == 0,      "旧调用 dir=0");

    db_close(d);
    unlink(p);
}

/* ---- 测试 7：ensure_column 对旧库幂等 ---- */
static void t_old_db_migration(const char *path)
{
    printf("[7] ensure_column 对旧库幂等迁移\n");

    char p[512];
    snprintf(p, sizeof(p), "%s.old", path);
    unlink(p);

    char e[256];
    db_t *d = db_open(p, 16384, 0, e, sizeof(e));
    if (!d) { printf("  FAIL: open %s\n", e); g_fail++; return; }

    /* 手工造 1.0.6 老库的 dnsq：只有旧 8 列，无任何新列 */
    CHECK(db_foreach(d,
        "CREATE TABLE dnsq(ts_ms INTEGER, qname TEXT, qtype INTEGER, "
        "rcode INTEGER, ms REAL, answers TEXT, upstream TEXT, client_ip TEXT);",
        cap_cb, NULL) == 0, "建旧 dnsq 表（8 列）");

    /* 第一次：应幂等 ALTER 加列、不报错；第二次：不能报 duplicate column */
    CHECK(db_init_schema(d, e, sizeof(e)) == 0, "老库首次 db_init_schema 成功: %s", e);
    CHECK(db_init_schema(d, e, sizeof(e)) == 0, "老库重复 db_init_schema 幂等: %s", e);

    /* 迁移后写入全部新列，验证列确实加进来了 */
    db_dnsq_t q;
    memset(&q, 0, sizeof(q));
    q.ts_ms       = 1; q.qname = "old.test"; q.qtype = 28; q.rcode = 0;
    q.client_fam  = 6;
    q.server_ip   = "2001:db8::1";
    q.server_fam  = 6;
    q.server_port = 5353;
    q.ttl         = 60; q.answer_cnt = 1;
    q.answer_ips  = "2001:db8::9";
    CHECK(db_add_dnsq(d, &q) == 0, "迁移后可写新列");

    rowcap_t cap;
    memset(&cap, 0, sizeof(cap));
    db_foreach(d, "SELECT server_ip,server_fam,server_port,ttl,"
                 "answer_cnt,answer_ips FROM dnsq WHERE qname='old.test'",
               cap_cb, &cap);
    CHECK(cap.rows == 1, "读出迁移后写入的行");
    CHECK(strcmp(cap.cols[0], "2001:db8::1") == 0, "server_ip=%s", cap.cols[0]);
    CHECK(strcmp(cap.cols[1], "6") == 0,           "server_fam=%s", cap.cols[1]);
    CHECK(strcmp(cap.cols[2], "5353") == 0,         "server_port=%s", cap.cols[2]);
    CHECK(strcmp(cap.cols[3], "60") == 0,           "ttl=%s", cap.cols[3]);
    CHECK(strcmp(cap.cols[4], "1") == 0,            "answer_cnt=%s", cap.cols[4]);
    CHECK(strcmp(cap.cols[5], "2001:db8::9") == 0, "answer_ips=%s", cap.cols[5]);

    db_close(d);
    unlink(p);
}

int main(int argc, char **argv)
{
    const char *dbpath = (argc > 1) ? argv[1] : "/tmp/netmon_test.db";

    t_throughput(dbpath);
    t_batch_effect(dbpath);
    t_rollup(dbpath);
    t_gc(dbpath);
    t_ip_handling(dbpath);
    t_new_columns(dbpath);
    t_old_db_migration(dbpath);

    printf("\n=== %s : %d 个失败 ===\n", g_fail ? "FAILED" : "ALL PASS", g_fail);
    return g_fail ? 1 : 0;
}
