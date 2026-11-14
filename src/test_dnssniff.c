/*
 * test_dnssniff.c - dnssniff 单元测试
 *
 * 用构造的以太帧验证：
 *   1) DNS 查询 / 应答（A、AAAA）解析、查询应答配对算耗时
 *   2) 非 53 端口（v4/v6）、非 UDP、ARP 帧被忽略
 *   3) qname 压缩指针、多级域名
 *   4) 1.1.0 事件结构：完整 16 字节 client/server 地址、协议族、
 *      server_port、txid、flags，v4 与 v6 两条分支都填充
 *   5) 1.1.0 parse_dns：遍历整个 answer 区，A/AAAA 多答案 + 各自 TTL，
 *      CNAME 跳过但继续统计 an_count（不再遇第一条 A 就 break）
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

/* 2001:db8::1（文档保留前缀，占位用） */
static const uint8_t AAAA_DEFAULT[16] = {
    0x20,0x01,0x0d,0xb8,0,0,0,0,0,0,0,0,0,0,0,0x01
};

static int build_frame_full(uint8_t *f, uint16_t sport, uint16_t dport,
                            const uint8_t *payload, int plen,
                            uint32_t src_ip, uint32_t dst_ip);

/* 构造一个 DNS 查询报文（flags=0x0100 标准查询） */
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

/* 构造一个带 A 记录的 DNS 应答（qname 用压缩指针，TTL 可指定） */
static int build_reply_a(uint8_t *buf, uint16_t txid, const char *qname,
                         const uint8_t ip[4], uint32_t ttl)
{
    int n = build_query(buf, txid, qname, 1);
    buf[2] = 0x81; buf[3] = 0x80;       /* 响应 + RA，flags=0x8180 */
    buf[6] = 0; buf[7] = 1;             /* ancount = 1 */

    buf[n++] = 0xc0; buf[n++] = 0x0c;   /* name 指针指回 offset 12 */
    buf[n++] = 0; buf[n++] = 1;         /* type A */
    buf[n++] = 0; buf[n++] = 1;         /* class IN */
    buf[n++] = (ttl >> 24) & 0xff; buf[n++] = (ttl >> 16) & 0xff;
    buf[n++] = (ttl >> 8) & 0xff; buf[n++] = ttl & 0xff;
    buf[n++] = 0; buf[n++] = 4;         /* rdlength */
    memcpy(buf + n, ip, 4); n += 4;
    return n;
}

/* 构造一个带 AAAA 记录的 DNS 应答（qname 压缩指针，TTL/地址可指定） */
static int build_reply_aaaa(uint8_t *buf, uint16_t txid, const char *qname,
                            const uint8_t ip[16], uint32_t ttl)
{
    int n = build_query(buf, txid, qname, 28);
    buf[2] = 0x81; buf[3] = 0x80;
    buf[6] = 0; buf[7] = 1;

    buf[n++] = 0xc0; buf[n++] = 0x0c;
    buf[n++] = 0; buf[n++] = 28;        /* type AAAA */
    buf[n++] = 0; buf[n++] = 1;
    buf[n++] = (ttl >> 24) & 0xff; buf[n++] = (ttl >> 16) & 0xff;
    buf[n++] = (ttl >> 8) & 0xff; buf[n++] = ttl & 0xff;
    buf[n++] = 0; buf[n++] = 16;        /* rdlength = 16 */
    memcpy(buf + n, ip, 16); n += 16;
    return n;
}

/*
 * 构造多答案应答：CNAME + A + AAAA（ancount=3，flags=0x8400 带 AA）。
 * 用来验证 parse_dns 遍历整个 answer 区：CNAME 跳过但继续，
 * A 与 AAAA 都收集（不再遇第一条 A 就 break），各自 TTL 正确。
 */
static int build_reply_multi(uint8_t *buf, uint16_t txid, const char *qname)
{
    int n = build_query(buf, txid, qname, 1);
    buf[2] = 0x84; buf[3] = 0x00;       /* QR + AA，flags=0x8400 */
    buf[6] = 0; buf[7] = 3;             /* ancount = 3 */

    /* 1) CNAME（type 5），应被跳过但计入 an_count */
    buf[n++] = 0xc0; buf[n++] = 0x0c;
    buf[n++] = 0; buf[n++] = 5;
    buf[n++] = 0; buf[n++] = 1;
    buf[n++] = 0; buf[n++] = 0; buf[n++] = 0x01; buf[n++] = 0x2c; /* ttl=300 */
    buf[n++] = 0; buf[n++] = 1;         /* rdlen=1，rdata=根 */
    buf[n++] = 0x00;

    /* 2) A，ttl=120 */
    buf[n++] = 0xc0; buf[n++] = 0x0c;
    buf[n++] = 0; buf[n++] = 1;
    buf[n++] = 0; buf[n++] = 1;
    buf[n++] = 0; buf[n++] = 0; buf[n++] = 0; buf[n++] = 120;
    buf[n++] = 0; buf[n++] = 4;
    buf[n++] = 93; buf[n++] = 184; buf[n++] = 216; buf[n++] = 34;

    /* 3) AAAA，ttl=60 */
    buf[n++] = 0xc0; buf[n++] = 0x0c;
    buf[n++] = 0; buf[n++] = 28;
    buf[n++] = 0; buf[n++] = 1;
    buf[n++] = 0; buf[n++] = 0; buf[n++] = 0; buf[n++] = 60;
    buf[n++] = 0; buf[n++] = 16;
    memcpy(buf + n, AAAA_DEFAULT, 16); n += 16;
    return n;
}

/* 造一个完整 v4 以太帧：Ethernet + IPv4 + UDP + payload */
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
    memset(f, 0x11, 6);                 /* dst mac */
    memset(f + 6, 0x22, 6);            /* src mac */
    f[12] = 0x08; f[13] = 0x00;        /* IPv4 */
    uint8_t *ip = f + 14;
    ip[0] = 0x45;                      /* v4, ihl=5 */
    ip[2] = 0; ip[3] = (uint8_t)(20 + 8 + plen);
    ip[8] = 64;
    ip[9] = 17;                        /* UDP */
    memcpy(ip + 12, &src_ip, 4);
    memcpy(ip + 16, &dst_ip, 4);
    uint8_t *udp = ip + 20;
    udp[0] = sport >> 8; udp[1] = sport & 0xff;
    udp[2] = dport >> 8; udp[3] = dport & 0xff;
    udp[4] = (uint8_t)((8 + plen) >> 8);
    udp[5] = (uint8_t)((8 + plen) & 0xff);
    memcpy(udp + 8, payload, plen);
    return 14 + 20 + 8 + plen;
}

/* 造一个完整 v6 以太帧：Ethernet + IPv6(固定 40 字节头) + UDP + payload */
static int build_frame_v6(uint8_t *f, uint16_t sport, uint16_t dport,
                          const uint8_t *payload, int plen,
                          const uint8_t src[16], const uint8_t dst[16])
{
    memset(f, 0, 1500);
    memset(f, 0x11, 6);
    memset(f + 6, 0x22, 6);
    f[12] = 0x86; f[13] = 0xdd;        /* IPv6 */
    uint8_t *ip = f + 14;
    ip[0] = 0x60;                      /* version 6 */
    ip[4] = 0; ip[5] = (uint8_t)(8 + plen);  /* payload length（可选，仅占位） */
    ip[6] = 17;                        /* next header = UDP */
    memcpy(ip + 8, src, 16);
    memcpy(ip + 24, dst, 16);
    uint8_t *udp = ip + 40;
    udp[0] = sport >> 8; udp[1] = sport & 0xff;
    udp[2] = dport >> 8; udp[3] = dport & 0xff;
    udp[4] = (uint8_t)((8 + plen) >> 8);
    udp[5] = (uint8_t)((8 + plen) & 0xff);
    memcpy(udp + 8, payload, plen);
    return 14 + 40 + 8 + plen;
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

static dnssniff_t *new_sniffer(void)
{
    dnssniff_t *s = calloc(1, sizeof(*s));
    size_t sz = 1024;
    s->pend = calloc(sz, sizeof(pending_t));
    s->pend_mask = sz - 1;
    return s;
}

static void test_query(void)
{
    printf("test: 解析 DNS 查询（v4，含全地址字段）\n");
    uint8_t dns[512], frame[1500];
    int dl = build_query(dns, 0x1234, "www.example.com", 1);
    int fl = build_frame(frame, 53, dns, dl, htonl(0xc0a80102));

    dnssniff_t *s = new_sniffer();
    s->st.frames = 0;
    g_events = 0;

    handle_frame(s, frame, fl, cb, NULL);

    CHECK(g_events == 1, "应该产生 1 个事件");
    CHECK(strcmp(g_last.qname, "www.example.com") == 0, "qname 应正确");
    CHECK(g_last.qtype == 1, "qtype 应为 1");
    CHECK(g_last.is_query == 1, "应标记为查询");
    CHECK(g_last.txid == 0x1234, "txid 应回填");
    CHECK(g_last.flags == 0x0100, "flags 应回填（标准查询 RD）");
    CHECK(g_last.client_fam == 4 && g_last.server_fam == 4,
          "v4 客户端/服务端协议族都为 4");
    CHECK(g_last.client[10] == 0xff && g_last.client[11] == 0xff,
          "v4 client 用 v6-mapped 形态");
    CHECK(g_last.client[12] == 0xc0 && g_last.client[13] == 0xa8 &&
          g_last.client[14] == 0x01 && g_last.client[15] == 0x02,
          "查询帧 client=ip.src");
    CHECK(g_last.server[12] == 0xc0 && g_last.server[13] == 0xa8 &&
          g_last.server[14] == 0x01 && g_last.server[15] == 0x01,
          "查询帧 server=ip.dst");
    CHECK(g_last.server_port == 53, "查询帧 server_port=dport=53");
    CHECK(s->st.dns_queries == 1, "查询计数应为 1");
    CHECK(s->st.pending == 1, "应有 1 个待配对");

    free(s->pend); free(s);
}

static void test_reply_a(void)
{
    printf("test: 解析带 A 记录的应答（新 ans[] 字段 + TTL + flags）\n");
    uint8_t dns[512], frame[1500];
    uint8_t ip[4] = { 93, 184, 216, 34 };
    int dl = build_reply_a(dns, 0x5678, "test.example.org", ip, 60);
    int fl = build_frame(frame, 53, dns, dl, htonl(0xc0a80102));

    dnssniff_t *s = new_sniffer();
    g_events = 0;

    handle_frame(s, frame, fl, cb, NULL);

    CHECK(g_events == 1, "应该产生 1 个事件");
    CHECK(g_last.is_query == 0, "应标记为应答");
    CHECK(g_last.n_ans == 1, "应解析出 1 条 A/AAAA");
    CHECK(g_last.ans[0].family == 4, "A 记录 family=4");
    CHECK(g_last.ans[0].ttl == 60, "A 记录 TTL=60");
    CHECK(g_last.ans[0].ip[10] == 0xff && g_last.ans[0].ip[11] == 0xff,
          "A 记录用 v6-mapped 形态");
    CHECK(g_last.ans[0].ip[12] == 93 && g_last.ans[0].ip[13] == 184
          && g_last.ans[0].ip[14] == 216 && g_last.ans[0].ip[15] == 34,
          "A 记录 IP 应正确（v4-mapped 后 4 字节）");
    CHECK(g_last.an_count == 1, "an_count=1");
    CHECK(g_last.flags == 0x8180, "应答 flags=0x8180");

    free(s->pend); free(s);
}

static void test_pairing(void)
{
    printf("test: 查询/应答配对算耗时\n");
    uint8_t qdns[512], rdns[512], qf[1500], rf[1500];
    uint8_t ip[4] = { 1, 2, 3, 4 };
    int ql = build_query(qdns, 0xabcd, "pair.example", 1);
    int rl = build_reply_a(rdns, 0xabcd, "pair.example", ip, 60);
    int qfl = build_frame(qf, 53, qdns, ql, htonl(0xc0a80102));
    /* 应答方向：源 = DNS 服务器，目的 = 客户端 */
    int rfl = build_frame_full(rf, 53, 40000, rdns, rl,
                               htonl(0xc0a80101), htonl(0xc0a80102));

    dnssniff_t *s = new_sniffer();
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
 * 锁死 qtype 必须参与配对 key（修复前 AAAA 应答恒配不上）。
 */
static void test_dual_stack_same_txid(void)
{
    printf("test: A/AAAA 同 txid 并发的配对\n");
    uint8_t qa[512], qaaa[512], ra[512], raaa[512];
    uint8_t fa[1500], faaa[1500], rfa[1500], rfaaa[1500];
    uint8_t ip4[4] = { 93, 184, 216, 34 };
    uint32_t client = htonl(0xc0a80102);
    uint32_t server = htonl(0xc0a80101);

    const uint16_t cport = 40000;
    int qal  = build_query(qa,   0x7979, "dual.example", 1);
    int qaal = build_query(qaaa, 0x7979, "dual.example", 28);
    int ral  = build_reply_a(ra, 0x7979, "dual.example", ip4, 60);
    int raal = build_reply_aaaa(raaa, 0x7979, "dual.example", AAAA_DEFAULT, 60);

    int fal  = build_frame(fa,  53, qa,   qal,  client);
    int faal = build_frame(faaa, 53, qaaa, qaal, client);
    int rfal  = build_frame_full(rfa,  53, cport, ra,   ral,  server, client);
    int rfaal = build_frame_full(rfaaa, 53, cport, raaa, raal, server, client);

    dnssniff_t *s = new_sniffer();
    g_events = 0;

    handle_frame(s, fa,   fal,  cb, NULL);
    handle_frame(s, faaa, faal, cb, NULL);
    CHECK(s->st.pending == 2, "A 和 AAAA 应各占一个待配对槽（修复前只有 1）");

    handle_frame(s, rfa, rfal, cb, NULL);
    CHECK(g_last.is_query == 0, "A 应答事件");
    CHECK(g_last.qtype == 1, "A 应答 qtype 应为 1");
    CHECK(g_last.ms >= 0.0, "A 应答应算出耗时");

    handle_frame(s, rfaaa, rfaal, cb, NULL);
    CHECK(g_last.is_query == 0, "AAAA 应答事件");
    CHECK(g_last.qtype == 28, "AAAA 应答 qtype 应为 28");
    CHECK(g_last.ms >= 0.0, "AAAA 应答也应算出耗时（修复前恒为 -1）");
    CHECK(g_last.n_ans == 1 && g_last.ans[0].family == 6,
          "AAAA 应答应解析出 v6 地址");
    CHECK(s->st.pending == 0, "两条都配对上后待配对数应为 0");

    free(s->pend); free(s);
}

/* 1.1.0：v6 查询帧，验证 client/server 地址、fam、server_port、txid、flags */
static void test_v6_query(void)
{
    printf("test: IPv6 查询帧（全地址字段）\n");
    uint8_t dns[512], frame[1500];
    int dl = build_query(dns, 0x2468, "v6.example", 28);

    uint8_t src[16], dst[16];
    memset(src, 0, 16); memset(dst, 0, 16);
    src[0] = 0x20; src[1] = 0x01; src[2] = 0x0d; src[3] = 0xb8; src[15] = 0x02;
    dst[0] = 0x20; dst[1] = 0x01; dst[2] = 0x0d; dst[3] = 0xb8; dst[15] = 0x01;
    int fl = build_frame_v6(frame, 40000, 53, dns, dl, src, dst);

    dnssniff_t *s = new_sniffer();
    g_events = 0;
    handle_frame(s, frame, fl, cb, NULL);

    CHECK(g_events == 1, "应产生 1 个事件");
    CHECK(g_last.is_query == 1, "查询");
    CHECK(g_last.qtype == 28, "qtype=AAAA");
    CHECK(g_last.client_fam == 6 && g_last.server_fam == 6, "v6 协议族");
    CHECK(memcmp(g_last.client, src, 16) == 0, "查询帧 client=ip.src（原生 16 字节）");
    CHECK(memcmp(g_last.server, dst, 16) == 0, "查询帧 server=ip.dst");
    CHECK(g_last.server_port == 53, "查询帧 server_port=dport=53");
    CHECK(g_last.txid == 0x2468, "txid 应回填");
    CHECK(g_last.flags == 0x0100, "flags 应回填");
    CHECK(s->st.dns_queries == 1, "v6 查询应计数");
    CHECK(s->st.pending == 1, "v6 查询应进入配对表");

    free(s->pend); free(s);
}

/* 1.1.0：v6 应答帧，验证 client/server 方向、AAAA 答案、TTL、flags */
static void test_v6_reply(void)
{
    printf("test: IPv6 应答帧（AAAA + TTL + flags）\n");
    uint8_t dns[512], frame[1500];
    uint8_t aaaa[16];
    memcpy(aaaa, AAAA_DEFAULT, 16);
    aaaa[15] = 0xab;                  /* 2001:db8::ab */
    int dl = build_reply_aaaa(dns, 0x1357, "v6reply.example", aaaa, 90);

    uint8_t server[16], client[16];    /* 应答：ip.src=DNS 服务器, ip.dst=客户端 */
    memset(server, 0, 16); memset(client, 0, 16);
    server[0] = 0x20; server[1] = 0x01; server[2] = 0x0d; server[3] = 0xb8;
    server[15] = 0x01;
    client[0] = 0x20; client[1] = 0x01; client[2] = 0x0d; client[3] = 0xb8;
    client[15] = 0x02;
    int fl = build_frame_v6(frame, 53, 40000, dns, dl, server, client);

    dnssniff_t *s = new_sniffer();
    g_events = 0;
    handle_frame(s, frame, fl, cb, NULL);

    CHECK(g_events == 1, "应产生 1 个事件");
    CHECK(g_last.is_query == 0, "应答");
    CHECK(g_last.client_fam == 6 && g_last.server_fam == 6, "v6 协议族");
    CHECK(memcmp(g_last.client, client, 16) == 0, "应答帧 client=ip.dst");
    CHECK(memcmp(g_last.server, server, 16) == 0, "应答帧 server=ip.src");
    CHECK(g_last.server_port == 53, "应答帧 server_port=sport=53");
    CHECK(g_last.txid == 0x1357, "txid 应回填");
    CHECK(g_last.flags == 0x8180, "应答 flags 应回填");
    CHECK(g_last.n_ans == 1, "1 条 AAAA");
    CHECK(g_last.ans[0].family == 6, "AAAA family=6");
    CHECK(g_last.ans[0].ttl == 90, "AAAA TTL=90");
    CHECK(memcmp(g_last.ans[0].ip, aaaa, 16) == 0, "AAAA 地址应正确");
    CHECK(g_last.an_count == 1, "an_count=1");

    free(s->pend); free(s);
}

/* 1.1.0：遍历整个 answer 区，CNAME 跳过继续，A 与 AAAA 都收集 */
static void test_multi_answer(void)
{
    printf("test: 多答案应答（CNAME + A + AAAA，TTL / an_count / AA）\n");
    uint8_t dns[512], frame[1500];
    int dl = build_reply_multi(dns, 0x2468, "multi.example");
    int fl = build_frame(frame, 53, dns, dl, htonl(0xc0a80102));

    dnssniff_t *s = new_sniffer();
    g_events = 0;
    handle_frame(s, frame, fl, cb, NULL);

    CHECK(g_events == 1, "应产生 1 个事件");
    CHECK(g_last.is_query == 0, "应答");
    CHECK(g_last.flags == 0x8400, "flags=0x8400");
    CHECK((g_last.flags & 0x0400) != 0, "AA 位置位");
    CHECK(g_last.txid == 0x2468, "txid");
    CHECK(g_last.an_count == 3, "an_count=3（CNAME 也计入总记录数）");
    CHECK(g_last.n_ans == 2, "收集到 2 条 A/AAAA（不再遇第一条 A 就 break）");
    CHECK(g_last.ans[0].family == 4 && g_last.ans[0].ttl == 120,
          "A 记录：family=4 ttl=120");
    CHECK(g_last.ans[0].ip[12] == 93 && g_last.ans[0].ip[13] == 184 &&
          g_last.ans[0].ip[14] == 216 && g_last.ans[0].ip[15] == 34,
          "A 记录地址");
    CHECK(g_last.ans[1].family == 6 && g_last.ans[1].ttl == 60,
          "AAAA 记录：family=6 ttl=60");
    CHECK(memcmp(g_last.ans[1].ip, AAAA_DEFAULT, 16) == 0, "AAAA 记录地址");

    free(s->pend); free(s);
}

static void test_ignore(void)
{
    printf("test: 忽略无关帧（含 v6 非 53 端口）\n");
    uint8_t dns[512], frame[1500];
    int dl = build_query(dns, 0x1111, "x.com", 1);

    dnssniff_t *s = new_sniffer();
    g_events = 0;

    /* v4 非 53 端口 */
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

    /* v6 非 53 端口 */
    uint8_t src[16], dst[16];
    memset(src, 0, 16); memset(dst, 0, 16);
    src[0] = 0x20; src[1] = 0x01; src[2] = 0x0d; src[3] = 0xb8;
    dst[0] = 0x20; dst[1] = 0x01; dst[2] = 0x0d; dst[3] = 0xb8;
    int v6fl = build_frame_v6(frame, 40000, 5353, dns, dl, src, dst);
    g_events = 0;
    handle_frame(s, frame, v6fl, cb, NULL);
    CHECK(g_events == 0, "v6 非 53 端口应被忽略");

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
    dnssniff_t *s = new_sniffer();

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
    test_v6_query();
    test_v6_reply();
    test_multi_answer();
    test_ignore();
    test_qname_multilevel();

    printf("\n=== 结果: %d 通过, %d 失败 ===\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
