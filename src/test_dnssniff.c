/*
 * test_dnssniff.c - dnssniff 单元测试
 *
 * 用构造的以太帧验证：
 *   1) 能正确解析 DNS 查询
 *   2) 能正确解析带 A 记录的应答
 *   3) 查询/应答配对能算出耗时
 *   4) 非 53 端口、非 UDP、非 IPv4 的帧被忽略
 *   5) qname 压缩指针能正确展开
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <arpa/inet.h>

/* 把 dnssniff.c 里的静态函数暴露出来测试 */
#define static
#include "dnssniff.c"
#undef static

static int g_fail = 0;
static int g_pass = 0;

#define CHECK(cond, msg) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; printf("  FAIL: %s (line %d)\n", msg, __LINE__); } \
} while (0)

static int build_frame_full(uint8_t *f, uint16_t sport, uint16_t dport,
                            const uint8_t *payload, int plen,
                            uint32_t src_ip, uint32_t dst_ip);

/* 构造一个 DNS 查询报文 */
static int build_query(uint8_t *buf, uint16_t txid, const char *qname, uint16_t qtype)
{
    memset(buf, 0, 512);
    buf[0] = txid >> 8; buf[1] = txid & 0xff;
    buf[2] = 0x01; buf[3] = 0x00;       /* 标准查询 */
    buf[4] = 0; buf[5] = 1;             /* qdcount */
    int n = 12;

    const char *p = qname;
    while (*p) {
        const char *dot = strchr(p, '.');
        int len = dot ? (int)(dot - p) : (int)strlen(p);
        buf[n++] = (uint8_t)len;
        memcpy(buf + n, p, len);
        n += len;
        if (!dot) break;
        p = dot + 1;
    }
    buf[n++] = 0;
    buf[n++] = qtype >> 8; buf[n++] = qtype & 0xff;
    buf[n++] = 0; buf[n++] = 1;         /* IN */
    return n;
}

/* 构造一个带 A 记录的 DNS 应答（qname 用压缩指针） */
static int build_reply_a(uint8_t *buf, uint16_t txid, const char *qname,
                         const uint8_t ip[4])
{
    int n = build_query(buf, txid, qname, 1);
    buf[2] = 0x81; buf[3] = 0x80;       /* 响应 + RA */
    buf[6] = 0; buf[7] = 1;             /* ancount = 1 */

    /* answer: name 用指针指回 offset 12 */
    buf[n++] = 0xc0; buf[n++] = 0x0c;
    buf[n++] = 0; buf[n++] = 1;         /* type A */
    buf[n++] = 0; buf[n++] = 1;         /* class IN */
    buf[n++] = 0; buf[n++] = 0;         /* ttl */
    buf[n++] = 0; buf[n++] = 60;
    buf[n++] = 0; buf[n++] = 4;         /* rdlength */
    memcpy(buf + n, ip, 4);
    n += 4;
    return n;
}

/* 构造一个带 AAAA 记录的 DNS 应答（qname 用压缩指针，rdlength=16） */
static int build_reply_aaaa(uint8_t *buf, uint16_t txid, const char *qname)
{
    int n = build_query(buf, txid, qname, 28);
    buf[2] = 0x81; buf[3] = 0x80;       /* 响应 + RA */
    buf[6] = 0; buf[7] = 1;             /* ancount = 1 */

    /* answer: name 用指针指回 offset 12 */
    buf[n++] = 0xc0; buf[n++] = 0x0c;
    buf[n++] = 0; buf[n++] = 28;        /* type AAAA */
    buf[n++] = 0; buf[n++] = 1;         /* class IN */
    buf[n++] = 0; buf[n++] = 0;         /* ttl */
    buf[n++] = 0; buf[n++] = 60;
    buf[n++] = 0; buf[n++] = 16;        /* rdlength = 16 */
    /* 2001:db8::1 */
    memset(buf + n, 0, 16);
    buf[n + 0] = 0x20; buf[n + 1] = 0x01;
    buf[n + 2] = 0x0d; buf[n + 3] = 0xb8;
    buf[n + 15] = 0x01;
    n += 16;
    return n;
}

/* 造一个完整以太帧：Ethernet + IPv4 + UDP + payload */
static int build_frame_ex(uint8_t *f, uint16_t sport, uint16_t dport,
                          const uint8_t *payload, int plen, uint32_t src_ip)
{
    return build_frame_full(f, sport, dport, payload, plen, src_ip,
                            htonl(0xc0a80101));
}

static int build_frame_full(uint8_t *f, uint16_t sport, uint16_t dport,
                            const uint8_t *payload, int plen,
                            uint32_t src_ip, uint32_t dst_ip)
{
    memset(f, 0, 1500);
    /* 以太头 */
    memset(f, 0x11, 6);                 /* dst mac */
    memset(f + 6, 0x22, 6);             /* src mac */
    f[12] = 0x08; f[13] = 0x00;         /* IPv4 */
    /* IP 头 */
    uint8_t *ip = f + 14;
    ip[0] = 0x45;                       /* v4, ihl=5 */
    ip[2] = 0; ip[3] = (uint8_t)(20 + 8 + plen);
    ip[8] = 64;
    ip[9] = 17;                         /* UDP */
    memcpy(ip + 12, &src_ip, 4);
    memcpy(ip + 16, &dst_ip, 4);
    /* UDP 头 */
    uint8_t *udp = ip + 20;
    uint16_t sp = sport;
    udp[0] = sp >> 8; udp[1] = sp & 0xff;
    udp[2] = dport >> 8; udp[3] = dport & 0xff;
    udp[4] = (uint8_t)((8 + plen) >> 8);
    udp[5] = (uint8_t)((8 + plen) & 0xff);
    memcpy(udp + 8, payload, plen);
    return 14 + 20 + 8 + plen;
}

/* 默认：客户端源端口 40000，发往 53 */
static int build_frame(uint8_t *f, uint16_t dport, const uint8_t *payload,
                       int plen, uint32_t src_ip)
{
    return build_frame_ex(f, 40000, dport, payload, plen, src_ip);
}

/* ---- 回调收集 ---- */
static dns_event_t g_last;
static int g_events = 0;

static void cb(const dns_event_t *e, void *ud)
{
    (void)ud;
    g_last = *e;
    g_events++;
}

static void test_query(void)
{
    printf("test: 解析 DNS 查询\n");
    uint8_t dns[512], frame[1500];
    int dl = build_query(dns, 0x1234, "www.example.com", 1);
    int fl = build_frame(frame, 53, dns, dl, htonl(0xc0a80102));

    dnssniff_t *s = calloc(1, sizeof(*s));
    size_t sz = 1024;
    s->pend = calloc(sz, sizeof(pending_t));
    s->pend_mask = sz - 1;
    s->st.frames = 0;
    g_events = 0;

    handle_frame(s, frame, fl, cb, NULL);

    CHECK(g_events == 1, "应该产生 1 个事件");
    CHECK(strcmp(g_last.qname, "www.example.com") == 0, "qname 应正确");
    CHECK(g_last.qtype == 1, "qtype 应为 1");
    CHECK(g_last.is_query == 1, "应标记为查询");
    CHECK(s->st.dns_queries == 1, "查询计数应为 1");
    CHECK(s->st.pending == 1, "应有 1 个待配对");

    free(s->pend); free(s);
}

static void test_reply_a(void)
{
    printf("test: 解析带 A 记录的应答\n");
    uint8_t dns[512], frame[1500];
    uint8_t ip[4] = { 93, 184, 216, 34 };
    int dl = build_reply_a(dns, 0x5678, "test.example.org", ip);
    int fl = build_frame(frame, 53, dns, dl, htonl(0xc0a80102));

    dnssniff_t *s = calloc(1, sizeof(*s));
    size_t sz = 1024;
    s->pend = calloc(sz, sizeof(pending_t));
    s->pend_mask = sz - 1;
    g_events = 0;

    handle_frame(s, frame, fl, cb, NULL);

    CHECK(g_events == 1, "应该产生 1 个事件");
    CHECK(g_last.is_query == 0, "应标记为应答");
    CHECK(g_last.has_ip == 1, "应解析出 IP");
    CHECK(g_last.family == 4, "应该是 IPv4");
    CHECK(g_last.ip[12] == 93 && g_last.ip[13] == 184
          && g_last.ip[14] == 216 && g_last.ip[15] == 34,
          "A 记录 IP 应正确（v4-mapped 后 4 字节）");

    free(s->pend); free(s);
}

static void test_pairing(void)
{
    printf("test: 查询/应答配对算耗时\n");
    uint8_t qdns[512], rdns[512], qf[1500], rf[1500];
    uint8_t ip[4] = { 1, 2, 3, 4 };
    int ql = build_query(qdns, 0xabcd, "pair.example", 1);
    int rl = build_reply_a(rdns, 0xabcd, "pair.example", ip);
    int qfl = build_frame(qf, 53, qdns, ql, htonl(0xc0a80102));
    /* 应答方向：源 = DNS 服务器(192.168.1.1)，目的 = 客户端(192.168.1.2) */
    int rfl = build_frame_full(rf, 53, 40000, rdns, rl,
                               htonl(0xc0a80101), htonl(0xc0a80102));

    dnssniff_t *s = calloc(1, sizeof(*s));
    size_t sz = 1024;
    s->pend = calloc(sz, sizeof(pending_t));
    s->pend_mask = sz - 1;
    g_events = 0;

    handle_frame(s, qf, qfl, cb, NULL);
    handle_frame(s, rf, rfl, cb, NULL);

    CHECK(g_events == 2, "应产生 2 个事件（查询 + 应答）");
    CHECK(g_last.is_query == 0, "第二个是应答");
    CHECK(g_last.ms >= 0.0, "应算出耗时（非负）");
    CHECK(g_last.ms < 1000.0, "耗时应该很小");
    CHECK(s->st.pending == 0, "配对后待配对数应为 0");

    free(s->pend); free(s);
}

/*
 * 回归测试：A 与 AAAA 用同一个 txid、同一个客户端端口并发查询。
 *
 * 这是 getaddrinfo 的真实行为：同一个客户端端口、几乎同时、txid 重号。
 * 修复前 pend 的 key 是 (txid, ip, port)，两条查询里第二条被当成重传只刷时间，
 * 于是两条应答必然有一条配不上 —— 线上表现为"发 AAAA 的域名应答率恰好 50%"。
 * 这个用例锁死 qtype 必须参与 key。
 */
static void test_dual_stack_same_txid(void)
{
    printf("test: A/AAAA 同 txid 并发的配对\n");
    uint8_t qa[512], qaaa[512], ra[512], raaa[512];
    uint8_t fa[1500], faaa[1500], rfa[1500], rfaaa[1500];
    uint8_t ip4[4] = { 93, 184, 216, 34 };
    uint32_t client = htonl(0xc0a80102);        /* 192.168.1.2 */
    uint32_t server = htonl(0xc0a80101);        /* 192.168.1.1 */

    /* 同一个 txid=0x7979，同一个客户端端口 40000（与 build_frame 的默认源端口一致） */
    const uint16_t cport = 40000;
    int qal  = build_query(qa,   0x7979, "dual.example", 1);
    int qaal = build_query(qaaa, 0x7979, "dual.example", 28);
    int ral  = build_reply_a(ra, 0x7979, "dual.example", ip4);
    int raal = build_reply_aaaa(raaa, 0x7979, "dual.example");

    int fal  = build_frame(fa,  53, qa,   qal,  client);
    int faal = build_frame(faaa, 53, qaaa, qaal, client);
    int rfal  = build_frame_full(rfa,  53, cport, ra,   ral,  server, client);
    int rfaal = build_frame_full(rfaaa, 53, cport, raaa, raal, server, client);

    dnssniff_t *s = calloc(1, sizeof(*s));
    size_t sz = 1024;
    s->pend = calloc(sz, sizeof(pending_t));
    s->pend_mask = sz - 1;
    g_events = 0;

    /* 两条查询并发，必须都留在表里 */
    handle_frame(s, fa,   fal,  cb, NULL);
    handle_frame(s, faaa, faal, cb, NULL);
    CHECK(s->st.pending == 2, "A 和 AAAA 应各占一个待配对槽（修复前只有 1）");

    /* A 应答先回 */
    handle_frame(s, rfa, rfal, cb, NULL);
    CHECK(g_last.is_query == 0, "A 应答事件");
    CHECK(g_last.qtype == 1, "A 应答 qtype 应为 1");
    CHECK(g_last.ms >= 0.0, "A 应答应算出耗时");

    /* AAAA 应答后回，修复前这里 ms 必然是 NULL */
    handle_frame(s, rfaaa, rfaal, cb, NULL);
    CHECK(g_last.is_query == 0, "AAAA 应答事件");
    CHECK(g_last.qtype == 28, "AAAA 应答 qtype 应为 28");
    CHECK(g_last.ms >= 0.0, "AAAA 应答也应算出耗时（修复前恒为 -1）");
    CHECK(g_last.has_ip == 1 && g_last.family == 6, "AAAA 应答应解析出 v6 地址");
    CHECK(s->st.pending == 0, "两条都配对上后待配对数应为 0");

    free(s->pend); free(s);
}

static void test_ignore(void)
{
    printf("test: 忽略无关帧\n");
    uint8_t dns[512], frame[1500];
    int dl = build_query(dns, 0x1111, "x.com", 1);

    dnssniff_t *s = calloc(1, sizeof(*s));
    size_t sz = 1024;
    s->pend = calloc(sz, sizeof(pending_t));
    s->pend_mask = sz - 1;
    g_events = 0;

    /* 非 53 端口 */
    int fl = build_frame(frame, 443, dns, dl, htonl(0xc0a80102));
    handle_frame(s, frame, fl, cb, NULL);
    CHECK(g_events == 0, "443 端口应被忽略");

    /* 非 UDP */
    build_frame(frame, 53, dns, dl, htonl(0xc0a80102));
    frame[14 + 9] = 6;                  /* 改成 TCP */
    handle_frame(s, frame, fl, cb, NULL);
    CHECK(g_events == 0, "TCP 应被忽略");

    /* ARP 帧 */
    memset(frame, 0, 60);
    frame[12] = 0x08; frame[13] = 0x06;
    handle_frame(s, frame, 60, cb, NULL);
    CHECK(g_events == 0, "ARP 应被忽略");

    free(s->pend); free(s);
}

static void test_qname_multilevel(void)
{
    printf("test: qname 解析（多级域名）\n");
    uint8_t dns[512], frame[1500];
    const char *names[] = {
        "a.b",
        "www.google.com",
        "deep.sub.domain.example.co.uk",
        "x",
    };
    dnssniff_t *s = calloc(1, sizeof(*s));
    size_t sz = 1024;
    s->pend = calloc(sz, sizeof(pending_t));
    s->pend_mask = sz - 1;

    for (int i = 0; i < 4; i++) {
        int dl = build_query(dns, (uint16_t)(0x100 + i), names[i], 1);
        int fl = build_frame(frame, 53, dns, dl, htonl(0xc0a80102));
        g_events = 0;
        handle_frame(s, frame, fl, cb, NULL);
        CHECK(g_events == 1 && strcmp(g_last.qname, names[i]) == 0,
              "多级域名应正确还原");
    }

    free(s->pend); free(s);
}

int main(void)
{
    printf("=== dnssniff 单元测试 ===\n\n");
    test_query();
    test_reply_a();
    test_pairing();
    test_dual_stack_same_txid();
    test_ignore();
    test_qname_multilevel();

    printf("\n=== 结果: %d 通过, %d 失败 ===\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
