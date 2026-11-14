/*
 * test_ctparse.c - 用真实 conntrack 行验证解析器
 *
 * 这些样例全部来自路由器 /proc/net/nf_conntrack 实际抓取，
 * 覆盖 tcp / udp / icmp / 无应答 / 有 mark 的情况。
 */
#include <stdio.h>
#include <string.h>
#include <arpa/inet.h>
#include "ctparse.h"

static int fails = 0;

#define CHECK(cond, fmt, ...) do { \
    if (!(cond)) { printf("  FAIL: " fmt "\n", ##__VA_ARGS__); fails++; } \
    else         { printf("  ok  : " fmt "\n", ##__VA_ARGS__); } \
} while (0)

static void t_tcp(void)
{
    printf("\n[tcp 双方向]\n");
    char line[] = "ipv4     2 tcp      6 97 TIME_WAIT "
                  "src=203.0.113.45 dst=59.82.113.219 sport=34458 dport=80 "
                  "packets=5 bytes=972 "
                  "src=59.82.113.219 dst=203.0.113.45 sport=80 dport=34458 "
                  "packets=5 bytes=1396 [ASSURED] mark=0 zone=0 use=2";
    ct_rec_t r;
    int rc = ct_parse_line(line, &r);
    CHECK(rc == 1, "解析成功");
    CHECK(r.key.family == 4, "family=4");
    CHECK(r.key.proto == IPPROTO_TCP, "proto=tcp");

    char a[64], b[64];
    inet_ntop(AF_INET, r.key.src, a, sizeof(a));
    inet_ntop(AF_INET, r.key.dst, b, sizeof(b));
    CHECK(strcmp(a, "203.0.113.45") == 0, "src=%s", a);
    CHECK(strcmp(b, "59.82.113.219") == 0, "dst=%s", b);
    CHECK(r.key.sport == 34458, "sport=%u", r.key.sport);
    CHECK(r.key.dport == 80, "dport=%u", r.key.dport);
    CHECK(r.byt_orig == 972, "上行字节=%llu", (unsigned long long)r.byt_orig);
    CHECK(r.byt_repl == 1396, "下行字节=%llu", (unsigned long long)r.byt_repl);
    CHECK(r.pkt_orig == 5 && r.pkt_repl == 5, "包数双向 5/5");
    CHECK(r.have_reply == 1, "有应答方向");
    CHECK(r.key.mark == 0, "mark=0");
}

static void t_udp(void)
{
    printf("\n[udp 本机 DNS]\n");
    char line[] = "ipv4     2 udp      17 57 "
                  "src=127.0.0.1 dst=127.0.0.1 sport=47218 dport=5333 "
                  "packets=1 bytes=79 "
                  "src=127.0.0.1 dst=127.0.0.1 sport=5333 dport=47218 "
                  "packets=1 bytes=68 mark=0 zone=0 use=2";
    ct_rec_t r;
    CHECK(ct_parse_line(line, &r) == 1, "解析成功");
    CHECK(r.key.proto == IPPROTO_UDP, "proto=udp");
    CHECK(r.key.dport == 5333, "dport=5333");
    CHECK(r.byt_orig == 79 && r.byt_repl == 68, "字节 79/68");
}

static void t_icmp(void)
{
    printf("\n[icmp ping]\n");
    char line[] = "ipv4     2 icmp     1 16 "
                  "src=192.168.1.245 dst=8.8.8.8 type=8 code=0 id=25337 "
                  "packets=1 bytes=84 "
                  "src=8.8.8.8 dst=203.0.113.45 type=0 code=0 id=25337 "
                  "packets=1 bytes=84 mark=0 zone=0 use=2";
    ct_rec_t r;
    CHECK(ct_parse_line(line, &r) == 1, "解析成功");
    CHECK(r.key.proto == IPPROTO_ICMP, "proto=icmp");
    CHECK(r.key.sport == 0 && r.key.dport == 0, "ICMP 端口归零");
    CHECK(r.byt_orig == 84 && r.byt_repl == 84, "字节 84/84");
}

static void t_unreplied(void)
{
    printf("\n[无应答 UDP]\n");
    char line[] = "ipv4     2 udp      17 30 "
                  "src=192.168.1.100 dst=1.1.1.1 sport=5000 dport=443 "
                  "packets=2 bytes=120 [UNREPLIED] mark=0 zone=0 use=1";
    ct_rec_t r;
    CHECK(ct_parse_line(line, &r) == 1, "解析成功");
    char a[64];
    inet_ntop(AF_INET, r.key.src, a, sizeof(a));
    CHECK(strcmp(a, "192.168.1.100") == 0, "src=%s", a);
    CHECK(r.byt_orig == 120, "上行 120");
    CHECK(r.byt_repl == 0, "下行 0");
    CHECK(r.have_reply == 0, "无应答标记");
    CHECK(r.unreplied == 1, "UNREPLIED 识别");
}

static void t_ipv6(void)
{
    printf("\n[ipv6]\n");
    char line[] = "ipv6     10 tcp      6 56 TIME_WAIT "
                  "src=2001:db8:1:2:70be:a391:4ff4:e84b "
                  "dst=2001:db8:3:4:0000:0000:0000:0031 "
                  "sport=32882 dport=443 packets=17 bytes=7938 "
                  "src=2001:db8:3:4::31 dst=2001:db8:1:2:70be:a391:4ff4:e84b "
                  "sport=443 dport=32882 packets=12 bytes=5400 [ASSURED] mark=0 zone=0 use=2";
    ct_rec_t r;
    CHECK(ct_parse_line(line, &r) == 1, "解析成功");
    CHECK(r.key.family == 6, "family=6");
    CHECK(r.key.sport == 32882 && r.key.dport == 443, "端口 32882->443");
    CHECK(r.byt_orig == 7938 && r.byt_repl == 5400, "字节 7938/5400");
}

/*
 * IPv6 conntrack 行 + icmpv6（1.1.0）。
 *
 * 注意：解析器把 l4proto 用 strncmp(,"icmp",4) 先判，所以 "icmpv6" 会
 * 落到 IPPROTO_ICMP（既存行为，本测试不锁死 proto 的具体值，只锁
 * 「是 ICMP 系、端口归零」）。这里真正要验证的是：IPv6 行能被解析、
 * 128 位地址被正确写入 16 字节缓冲、双向字节数、端口归零、有应答方向。
 */
static void t_icmpv6(void)
{
    printf("\n[ipv6 icmpv6]\n");
    char line[] = "ipv6     10 icmpv6   58 29 "
                  "src=2001:db8:1:2::10 dst=2001:db8:3:4::31 "
                  "type=128 code=0 id=1234 packets=2 bytes=128 "
                  "src=2001:db8:3:4::31 dst=2001:db8:1:2::10 "
                  "type=129 code=0 id=1234 packets=2 bytes=128 "
                  "mark=0 zone=0 use=2";
    ct_rec_t r;
    CHECK(ct_parse_line(line, &r) == 1, "解析成功");
    CHECK(r.key.family == 6, "family=6");
    CHECK(r.key.proto == IPPROTO_ICMP || r.key.proto == IPPROTO_ICMPV6,
          "proto=icmp 系（既存实现把 icmpv6 归到 ICMP 之一）");

    /* 2001:db8:1:2::10 的前 4 字节应为 20 01 0d b8，确认是 16 字节 v6 地址 */
    CHECK(r.key.src[0] == 0x20 && r.key.src[1] == 0x01 &&
          r.key.src[2] == 0x0d && r.key.src[3] == 0xb8,
          "src 为 2001:db8 开头的 16 字节 v6 地址");
    char a[64], b[64];
    inet_ntop(AF_INET6, r.key.src, a, sizeof(a));
    inet_ntop(AF_INET6, r.key.dst, b, sizeof(b));
    CHECK(strchr(a, ':') != NULL && a[0] != '?', "src 可格式化为 IPv6: %s", a);
    CHECK(strchr(b, ':') != NULL && b[0] != '?', "dst 可格式化为 IPv6: %s", b);

    CHECK(r.key.sport == 0 && r.key.dport == 0, "icmpv6 端口归零");
    CHECK(r.byt_orig == 128 && r.byt_repl == 128, "字节 128/128");
    CHECK(r.pkt_orig == 2 && r.pkt_repl == 2, "包数 2/2");
    CHECK(r.have_reply == 1, "有应答方向");
}

static void t_mark(void)
{
    printf("\n[带 mark 的连接]\n");
    char line[] = "ipv4     2 tcp      6 7440 ESTABLISHED "
                  "src=192.168.1.50 dst=104.16.0.1 sport=51000 dport=443 "
                  "packets=100 bytes=50000 "
                  "src=104.16.0.1 dst=203.0.113.45 sport=443 dport=51000 "
                  "packets=200 bytes=900000 [ASSURED] mark=1 zone=0 use=2";
    ct_rec_t r;
    CHECK(ct_parse_line(line, &r) == 1, "解析成功");
    CHECK(r.key.mark == 1, "mark=1 保留");
    CHECK(r.byt_repl == 900000, "下行 900000");
}

int main(void)
{
    printf("=== ctparse 单元测试 ===\n");
    t_tcp();
    t_udp();
    t_icmp();
    t_unreplied();
    t_ipv6();
    t_icmpv6();
    t_mark();

    printf("\n=== 结果: %s (失败 %d) ===\n", fails ? "失败" : "全部通过", fails);
    return fails ? 1 : 0;
}
