/*
 * test_dnsmap.c - dnsmap 单元测试
 *
 * 用真实 dnsmasq 日志格式覆盖各种边角情况。
 * 编译：gcc -O2 -o test_dnsmap test_dnsmap.c dnsmap.c
 */

#define _GNU_SOURCE
#include "dnsmap.h"

#include <stdio.h>
#include <string.h>
#include <arpa/inet.h>

static int g_fail = 0;

#define CHECK(cond, fmt, ...) do {                                   \
    if (!(cond)) {                                                   \
        printf("  FAIL: " fmt "\n", ##__VA_ARGS__);                  \
        g_fail++;                                                    \
    } else {                                                         \
        printf("  ok  : " fmt "\n", ##__VA_ARGS__);                  \
    }                                                                \
} while (0)

static void to_ip4(const char *s, uint8_t out[16])
{
    struct in_addr a;
    inet_pton(AF_INET, s, &a);
    memset(out, 0, 10);
    out[10] = 0xff; out[11] = 0xff;
    memcpy(out + 12, &a, 4);
}

static void to_ip6(const char *s, uint8_t out[16])
{
    struct in6_addr a;
    inet_pton(AF_INET6, s, &a);
    memcpy(out, &a, 16);
}

/* ---- 测试 1：标准 reply 行 ---- */
static void t_basic(void)
{
    printf("[1] 标准 reply 行\n");
    dnsmap_t *m = dnsmap_new(1024);

    int r = dnsmap_feed(m, "Nov  5 21:00:00 dnsmasq[1234]: reply api.example.com is 93.184.216.34", 100);
    CHECK(r == 1, "解析成功");

    uint8_t ip[16];
    to_ip4("93.184.216.34", ip);
    const char *n = dnsmap_lookup(m, 4, ip);
    CHECK(n && strcmp(n, "api.example.com") == 0, "反查得到域名 (got %s)", n ? n : "NULL");

    dnsmap_free(m);
}

/* ---- 测试 2：带结尾点的域名 ---- */
static void t_trailing_dot(void)
{
    printf("[2] 域名带结尾点\n");
    dnsmap_t *m = dnsmap_new(1024);

    dnsmap_feed(m, "dnsmasq[1]: reply www.test.org. is 1.2.3.4", 100);

    uint8_t ip[16];
    to_ip4("1.2.3.4", ip);
    const char *n = dnsmap_lookup(m, 4, ip);
    CHECK(n && strcmp(n, "www.test.org") == 0, "点被剥掉 (got %s)", n ? n : "NULL");

    dnsmap_free(m);
}

/* ---- 测试 3：CNAME 行必须被跳过 ---- */
static void t_cname_skip(void)
{
    printf("[3] CNAME 行跳过\n");
    dnsmap_t *m = dnsmap_new(1024);

    int r = dnsmap_feed(m, "dnsmasq[1]: reply cdn.example.com is an alias for edge.example.net", 100);
    CHECK(r == 0, "CNAME 行不产生映射");

    uint8_t ip[16];
    to_ip4("1.1.1.1", ip);
    CHECK(dnsmap_lookup(m, 4, ip) == NULL, "表里查不到东西");

    dnsmap_free(m);
}

/* ---- 测试 4：IPv6 ---- */
static void t_ipv6(void)
{
    printf("[4] IPv6 应答\n");
    dnsmap_t *m = dnsmap_new(1024);

    int r = dnsmap_feed(m, "dnsmasq[1]: reply v6.example.com is 2606:4700:3030::6815:1e19", 100);
    CHECK(r == 1, "v6 解析成功");

    uint8_t ip[16];
    to_ip6("2606:4700:3030::6815:1e19", ip);
    const char *n = dnsmap_lookup(m, 6, ip);
    CHECK(n && strcmp(n, "v6.example.com") == 0, "v6 反查 (got %s)", n ? n : "NULL");

    dnsmap_free(m);
}

/* ---- 测试 5：一个 IP 多个域名 ---- */
static void t_multi_name(void)
{
    printf("[5] 同一 IP 多域名\n");
    dnsmap_t *m = dnsmap_new(1024);

    uint8_t ip[16];
    to_ip4("104.16.0.1", ip);

    dnsmap_feed(m, "dnsmasq[1]: reply a.example.com is 104.16.0.1", 100);
    dnsmap_feed(m, "dnsmasq[1]: reply b.example.com is 104.16.0.1", 101);
    dnsmap_feed(m, "dnsmasq[1]: reply c.example.com is 104.16.0.1", 102);

    const char *n = dnsmap_lookup(m, 4, ip);
    CHECK(n && strcmp(n, "c.example.com") == 0, "返回最新的 (got %s)", n ? n : "NULL");

    dnsmap_stats_t st;
    dnsmap_get_stats(m, &st);
    CHECK(st.ips == 1, "只有 1 个 IP (got %zu)", st.ips);
    CHECK(st.names == 3, "记了 3 个域名 (got %zu)", st.names);

    dnsmap_free(m);
}

/* ---- 测试 6：超过 PER_IP 上限时环形覆盖 ---- */
static void t_ring_overflow(void)
{
    printf("[6] 单 IP 域名超过上限\n");
    dnsmap_t *m = dnsmap_new(1024);

    char line[256];
    for (int i = 0; i < DNSMAP_PER_IP + 4; i++) {
        snprintf(line, sizeof(line),
                 "dnsmasq[1]: reply n%d.example.com is 104.16.0.2", i);
        dnsmap_feed(m, line, 100 + i);
    }

    dnsmap_stats_t st;
    dnsmap_get_stats(m, &st);
    CHECK(st.names == DNSMAP_PER_IP, "域名数被封顶在 %d (got %zu)",
          DNSMAP_PER_IP, st.names);

    uint8_t ip[16];
    to_ip4("104.16.0.2", ip);
    const char *n = dnsmap_lookup(m, 4, ip);
    CHECK(n != NULL, "仍能查到最新的 (got %s)", n ? n : "NULL");

    dnsmap_free(m);
}

/* ---- 测试 7：无关日志行 ---- */
static void t_irrelevant(void)
{
    printf("[7] 无关行不误判\n");
    dnsmap_t *m = dnsmap_new(1024);

    CHECK(dnsmap_feed(m, "Nov  5 21:00:00 dnsmasq[1]: query[A] a.com from 192.168.1.5", 100) == 0,
          "query 行跳过");
    CHECK(dnsmap_feed(m, "Nov  5 21:00:00 kernel: eth0 link up", 100) == 0,
          "kernel 行跳过");
    CHECK(dnsmap_feed(m, "dnsmasq[1]: reply x.com is NXDOMAIN", 100) == 0,
          "NXDOMAIN 跳过");
    /*
     * 0.0.0.0 是 DNS 拦截（广告过滤/黑洞）的返回值，不是真实地址。
     * 早前这里期望"合法应答"，结果所有被拦截的域名都写进了全零槽位，
     * 而 IPv4 反查又有归一化 bug，导致任意连接都反查到这些拦截域名。
     * 现在明确拒绝。
     */
    CHECK(dnsmap_feed(m, "dnsmasq[1]: reply x.com is 0.0.0.0", 100) == 0,
          "0.0.0.0 是拦截结果，应拒绝");

    dnsmap_free(m);
}

/* ---- 测试 8：v4 与 v6 不串味 ---- */
static void t_family_isolation(void)
{
    printf("[8] v4/v6 隔离\n");
    dnsmap_t *m = dnsmap_new(1024);

    dnsmap_feed(m, "dnsmasq[1]: reply v4only.com is 10.1.2.3", 100);
    dnsmap_feed(m, "dnsmasq[1]: reply v6only.com is ::1", 101);

    uint8_t ip4[16], ip6[16];
    to_ip4("10.1.2.3", ip4);
    to_ip6("::1", ip6);

    const char *a = dnsmap_lookup(m, 4, ip4);
    const char *b = dnsmap_lookup(m, 6, ip6);
    CHECK(a && strcmp(a, "v4only.com") == 0, "v4 正确 (got %s)", a ? a : "NULL");
    CHECK(b && strcmp(b, "v6only.com") == 0, "v6 正确 (got %s)", b ? b : "NULL");

    dnsmap_free(m);
}

/* ---- 测试 9：大表压力与命中率 ---- */
static void t_scale(void)
{
    printf("[9] 规模测试\n");
    dnsmap_t *m = dnsmap_new(200000);

    char line[256];
    int N = 50000;
    for (int i = 0; i < N; i++) {
        int a = (i >> 16) & 0xff, b = (i >> 8) & 0xff, c = i & 0xff;
        snprintf(line, sizeof(line),
                 "dnsmasq[1]: reply host%d.example.com is 10.%d.%d.%d", i, a, b, c);
        dnsmap_feed(m, line, (uint64_t)i);
    }

    /* 全部能查回来 */
    int ok = 0;
    for (int i = 0; i < N; i++) {
        int a = (i >> 16) & 0xff, b = (i >> 8) & 0xff, c = i & 0xff;
        char ips[32];
        snprintf(ips, sizeof(ips), "10.%d.%d.%d", a, b, c);
        uint8_t ip[16];
        to_ip4(ips, ip);
        if (dnsmap_lookup(m, 4, ip) != NULL) ok++;
    }
    CHECK(ok == N, "50000 条全部可查 (got %d)", ok);

    dnsmap_stats_t st;
    dnsmap_get_stats(m, &st);
    printf("       ips=%zu names=%zu evicted=%zu\n", st.ips, st.names, st.evicted);

    dnsmap_free(m);
}

/*
 * ---- 测试 10：v4 两种内存形态的归一化（回归测试）----
 *
 * 这是线上真实踩过的坑，必须锁死：
 *
 * 写入侧（dnsmap_add，来自 DNS 嗅探）用 v6-mapped：ip[10..11]=ff ff, ip[12..15]=a.b.c.d
 * 查询侧（ctparse 解析 conntrack）用裸 v4：     ip[0..3]=a.b.c.d, ip[12..15]=0
 *
 * 旧代码一律 memcpy(key+12, ip+12, 4)，查询侧永远取到 0，
 * 所有 IPv4 都归一化成 ::ffff:0.0.0.0，命中同一个槽位。
 * 症状：任意 IPv4 连接反查时拿到的都是"最近被 DNS 拦截的某个域名"，
 * 导致 203.0.113.45（萤石 RTMP）被标成 mssdk.volces.com 等 7 种无关域名。
 */
static void to_ip4_raw(const char *s, uint8_t out[16])
{
    struct in_addr a;
    inet_pton(AF_INET, s, &a);
    memset(out, 0, 16);
    memcpy(out, &a, 4);          /* 裸 v4：前 4 字节 */
}

static void t_v4_form_normalize(void)
{
    printf("[10] v4 两种内存形态归一化\n");
    dnsmap_t *m = dnsmap_new(4096);

    /* 用 v4-mapped 写入（模拟 dnsmap_add / DNS 嗅探） */
    dnsmap_feed(m, "dnsmasq[1]: reply rtmp01open.ipcam-cloud.com is 203.0.113.45", 100);
    dnsmap_feed(m, "dnsmasq[1]: reply www.baidu.com is 110.242.68.66", 101);

    /* 用裸 v4 查询（模拟 ctparse 传来的 conntrack key） */
    uint8_t raw[16];
    to_ip4_raw("203.0.113.45", raw);
    const char *a = dnsmap_lookup(m, 4, raw);
    CHECK(a && strcmp(a, "rtmp01open.ipcam-cloud.com") == 0,
          "裸 v4 能查到 v4-mapped 写入的记录 (got %s)", a ? a : "NULL");

    /* 核心回归：两个不同 IP 必须各查各的，不能串味 */
    to_ip4_raw("110.242.68.66", raw);
    const char *b = dnsmap_lookup(m, 4, raw);
    CHECK(b && strcmp(b, "www.baidu.com") == 0,
          "不同 IP 不串味 (got %s)", b ? b : "NULL");

    /* 反向兼容：裸 v4 写入、v4-mapped 查询 */
    dnsmap_t *m2 = dnsmap_new(1024);
    uint8_t raw2[16];
    to_ip4_raw("203.0.113.9", raw2);
    dnsmap_add(m2, 4, raw2, "raw-write.example", 200);
    uint8_t mapped[16];
    to_ip4("203.0.113.9", mapped);
    const char *c = dnsmap_lookup(m2, 4, mapped);
    CHECK(c && strcmp(c, "raw-write.example") == 0,
          "裸 v4 写入能被 v4-mapped 查到 (got %s)", c ? c : "NULL");
    dnsmap_free(m2);

    /* 0.0.0.0 是 DNS 拦截的返回值，必须拒绝写入 */
    uint8_t zero[16];
    to_ip4("0.0.0.0", zero);
    int r = dnsmap_add(m, 4, zero, "blocked-ad.example", 300);
    CHECK(r == 0, "0.0.0.0 拒绝写入 (ret=%d)", r);

    to_ip4_raw("0.0.0.0", raw);
    const char *z = dnsmap_lookup(m, 4, raw);
    CHECK(z == NULL, "0.0.0.0 查不到东西 (got %s)", z ? z : "NULL");

    dnsmap_free(m);
}

int main(void)
{
    t_basic();
    t_trailing_dot();
    t_cname_skip();
    t_ipv6();
    t_multi_name();
    t_ring_overflow();
    t_irrelevant();
    t_family_isolation();
    t_scale();
    t_v4_form_normalize();

    printf("\n=== %s : %d 个失败 ===\n", g_fail ? "FAILED" : "ALL PASS", g_fail);
    return g_fail ? 1 : 0;
}
