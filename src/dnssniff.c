/*
 * dnssniff.c - 被动旁路抓取 DNS（见 dnssniff.h 顶部的安全边界说明）
 *
 * 数据来源：AF_PACKET 复制网卡上的以太帧，自己解析 IPv4/IPv6 + UDP + DNS。
 * 只读，不转发、不改包、不加任何规则。
 *
 * 配对逻辑：
 *   DNS 应答本身不带"这是哪个查询的应答"之外的信息，只有一个 16 位事务 ID
 *   + 四元组。所以用 (txid, client_ip, client_port) 做 key，收到查询时
 *   记下时间，收到应答时配对算出耗时。
 *   这个表有上限（默认 4096），满了就覆盖最旧的一条 —— 宁可丢个耗时，
 *   不能让内存无限涨。
 *
 * 性能：
 *   报文处理全部在栈上，零 malloc（除了配对表初始化一次）。
 *   解析路径不做字符串格式化，只在回调里才把 qname 拼出来。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <stdint.h>
#include <sys/socket.h>
/*
 * AF_PACKET / 经典 BPF 是 Linux 专有 API，只有路由器目标机（OpenWrt）才有。
 * 本项目在开发机（macOS）上只跑纯解析单元测试（parse_dns / handle_frame
 * 都不依赖这些头），因此把 Linux 头与用到它的 socket 初始化/统计代码
 * 用 __linux__ 隔开，让非 Linux 环境也能编译、跑单测。
 * Linux 分支的 BPF 过滤器逻辑保持原样，一行未改。
 */
#ifdef __linux__
#include <linux/if_packet.h>
#include <linux/if_ether.h>
#include <linux/filter.h>
#endif
#include <arpa/inet.h>
#include <net/if.h>

#include "dnssniff.h"

#define DEFAULT_MAX_PENDING 4096
#define PENDING_TTL_US      5000000ULL  /* 超过 5 秒的未配对查询视为过期 */

/* 待配对的查询 */
typedef struct {
    uint32_t txid;          /* 低 16 位是 DNS 事务 ID */
    /*
     * 客户端地址指纹（v4 和 v6 统一成 64 位）。
     * 为什么不分 v4/v6 两套字段：dnsmasq 同时在 IPv4 和 IPv6 上监听，
     * 双栈客户端查 A 走 v4、查 AAAA 可能走 v6，两条查询都要能配对。
     * IPv6 地址 128 位塞不进 uint32，所以统一哈希成 64 位；
     * 同端口 + 同 txid + 同 qtype 还要撞哈希的概率可以忽略。
     */
    uint64_t client_key;
    uint32_t client_port;   /* 主机字节序 */
    /*
     * qtype 必须进 key，否则 A 和 AAAA 会互相覆盖。
     * 原因：getaddrinfo 这类解析器会从**同一个客户端端口**、**几乎同一时刻**
     * 并发发出 A 和 AAAA 两条查询，而事务 ID 取自同一个自增/随机池，重号率极高。
     * 之前 key 只有 (txid, ip, port)，第二条查询被当成"重传"只刷新时间，
     * 于是两条应答里必然有一条配不上 —— 实测凡是发 AAAA 的域名应答率
     * 都正好掉到 50%（实测几个双栈域名都落在 48%~50%），
     * 而只发 A 的域名（baidu/github/cloudflare）都是 100%。
     * 耗时因此有整整一半被记成 NULL，看起来像 DNS 丢包，其实是自己的 bug。
     */
    uint16_t qtype;
    uint64_t ts_ms;
    uint8_t  used;
} pending_t;

struct dnssniff {
    int      fd;
    size_t   max_pending;
    pending_t *pend;         /* 开放寻址表，大小 2 的幂 */
    size_t   pend_mask;
    int      rcvbuf_bytes;   /* 实际生效的 SO_RCVBUF（内核给一半） */
    int      filter_ok;      /* 1 = BPF 过滤已生效，只收 udp/53 */
    dnssniff_stats_t st;
};

/*
 * 内部计时用微秒。
 *
 * 为什么不用毫秒：局域网解析器应答常在 0.1~0.5ms 量级，
 * 用毫秒取整会全部变成 0，报表上一片 0.0ms，看不出任何差异。
 * 微秒精度才能区分"隔壁应答"和"真的绕了一圈回来"。
 */
static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)(ts.tv_nsec / 1000);
}

/* 混合 hash，避免客户端端口规律导致聚簇 */
static uint32_t mix32(uint32_t x)
{
    x ^= x >> 16; x *= 0x7feb352dU;
    x ^= x >> 15; x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

/*
 * 把 16 字节 IPv6 地址混成 64 位。
 * 用 FNV-1a 走一遍，够散、够快、无分支，不会因为地址前缀相同就聚簇
 * （同一个 /64 下的设备只有后 64 位不同，简单拼接高位会全撞在一起）。
 */
static uint64_t mix64_addr(const uint8_t a[16])
{
    uint64_t h = 1469598103934665603ULL;      /* FNV offset basis */
    for (int i = 0; i < 16; i++) {
        h ^= (uint64_t)a[i];
        h *= 1099511628211ULL;                /* FNV prime */
    }
    /* 打散一次，避免低位规律影响 open addressing 的取模 */
    h ^= h >> 33; h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 29;
    return h;
}

static uint32_t pend_hash(uint32_t txid, uint64_t ckey, uint32_t port,
                          uint16_t qtype)
{
    uint32_t k = (uint32_t)(ckey ^ (ckey >> 32));
    return mix32(txid ^ mix32(k ^ mix32(port ^ (uint32_t)qtype * 0x9e3779b9U)));
}

/* 记一个待配对查询。表满时覆盖最旧的一条（线性探测到空位或同 key）。 */
static void pend_put(dnssniff_t *s, uint32_t txid, uint64_t ckey,
                     uint32_t port, uint16_t qtype, uint64_t ts)
{
    size_t i = pend_hash(txid, ckey, port, qtype) & s->pend_mask;
    size_t oldest = i;
    uint64_t oldest_ts = UINT64_MAX;

    for (size_t probe = 0; probe <= s->pend_mask; probe++) {
        pending_t *p = &s->pend[i & s->pend_mask];
        if (!p->used) {
            p->used = 1;
            p->txid = txid; p->client_key = ckey;
            p->client_port = port; p->qtype = qtype; p->ts_ms = ts;
            s->st.pending++;
            return;
        }
        if (p->txid == txid && p->client_key == ckey &&
            p->client_port == port && p->qtype == qtype) {
            /* 同一个 key 重复查询（重传），刷新时间 */
            p->ts_ms = ts;
            return;
        }
        if (p->ts_ms < oldest_ts) { oldest_ts = p->ts_ms; oldest = i & s->pend_mask; }
        i++;
    }

    /* 表满：覆盖最旧的 */
    pending_t *p = &s->pend[oldest];
    p->txid = txid; p->client_key = ckey;
    p->client_port = port; p->qtype = qtype; p->ts_ms = ts;
}

/* 找应答对应的查询，找到返回起始时间，找不到返回 0 */
static uint64_t pend_get_and_del(dnssniff_t *s, uint32_t txid,
                                uint64_t ckey, uint32_t port, uint16_t qtype)
{
    size_t i = pend_hash(txid, ckey, port, qtype) & s->pend_mask;

    for (size_t probe = 0; probe <= s->pend_mask; probe++) {
        pending_t *p = &s->pend[i & s->pend_mask];
        if (!p->used) return 0;

        if (p->txid == txid && p->client_key == ckey &&
            p->client_port == port && p->qtype == qtype) {
            uint64_t t = p->ts_ms;
            /* 不能直接清 used（会截断探测链），标记成过期让后续覆盖 */
            p->ts_ms = 0;
            if (s->st.pending) s->st.pending--;
            /* 太旧的查询不当作有效配对（超时重传场景） */
            if (t == 0 || (now_us() - t) > PENDING_TTL_US) return 0;
            return t;
        }
        i++;
    }
    return 0;
}

/*
 * 挂一个经典 BPF 过滤器，只让 udp/53 的帧进 socket。
 *
 * 为什么必须做这一步：
 *   AF_PACKET 默认把网卡上的**每一个帧**都复制一份丢给我们的 socket。
 *   局域网里视频流/P2P 能轻松跑到几万 pps，socket 缓冲区瞬间打满，
 *   内核直接丢帧（PACKET_STATISTICS 的 tp_drops）。
 *   丢了应答只是少一行记录，但**丢了查询就永远配不上对**，
 *   报表上表现为"应答比查询多、耗时大面积 NULL"，看着像 DNS 在丢包。
 *
 *   实测 8 分钟内丢 1955 帧，其中约 70 条是 DNS 查询 ——
 *   而未配对数量恰好等于这个差值，说明配对逻辑没问题，纯粹是抓包跟不上。
 *
 * 让内核在投递之前就把非 DNS 帧挡掉，缓冲区压力能降两三个数量级，
 * 从根上解决丢帧，同时明显降低用户态 CPU（不用逐帧 recv + 判断类型）。
 *
 * 用经典 BPF（SO_ATTACH_FILTER）而不是 eBPF：不需要 clang/BPF 工具链，
 * 纯汇编指令数组，内核 3.x 起就支持。而且它只挂在我们自己的 socket 上，
 * 完全不接触 netfilter/转发表，不可能影响 homeproxy 的 DNS 分流。
 *
 * 程序逻辑（pc 从 0 开始，jt/jf 是相对下一条的偏移）：
 *   0  A = ethertype
 *   1  == IPv4 ? 3 : 2
 *   2  == IPv6 ? 12 : 18(拒)
 *   3  A = ip.protocol
 *   4  == UDP ? 5 : 18(拒)
 *   5  A = frag_offset
 *   6  是分片 ? 18(拒) : 7        ; 非首片没有 UDP 头，必须排除
 *   7  X = IP 头长度
 *   8  A = sport
 *   9  == 53 ? 19(收) : 10
 *  10  A = dport
 *  11  == 53 ? 19(收) : 18(拒)
 *  12  A = ip6.next_header
 *  13  == UDP ? 14 : 18(拒)       ; 有扩展头的 IPv6 直接放弃（DNS 不会有）
 *  14  A = sport
 *  15  == 53 ? 19(收) : 16
 *  16  A = dport
 *  17  == 53 ? 19(收) : 18(拒)
 *  18  RET 0                       ; 丢弃
 *  19  RET 0x40000                 ; 接受整帧
 */
/*
 * IPv6 VLAN 帧 / 扩展头说明（§5.1.2，有意不扩展 BPF）：
 * IPv6 分支固定按「14 字节以太头 + 40 字节 v6 头」取 UDP 端口（偏移 54/56），
 * 所以既不识别带 802.1Q VLAN 标签的帧，也不跟随逐跳/分片/路由等扩展头。
 * DNS 报文正常不走扩展头，为兼容它们去扩写 BPF 反而会引入误判风险，
 * 故暂不扩展。代价只是「少收一些帧」，不影响转发，也不影响主流程。
 */
#ifdef __linux__
static int attach_dns_filter(int fd)
{
    struct sock_filter code[] = {
        /*  0 */ BPF_STMT(BPF_LD  | BPF_H   | BPF_ABS, 12),
        /*  1 */ BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K,   0x0800, 1, 0),
        /*  2 */ BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K,   0x86dd, 9, 15),

        /* ---- IPv4 ---- */
        /*  3 */ BPF_STMT(BPF_LD  | BPF_B   | BPF_ABS, 23),
        /*  4 */ BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K,   17, 0, 13),
        /*  5 */ BPF_STMT(BPF_LD  | BPF_H   | BPF_ABS, 20),
        /*  6 */ BPF_JUMP(BPF_JMP | BPF_JSET| BPF_K,   0x1fff, 11, 0),
        /*  7 */ BPF_STMT(BPF_LDX | BPF_B   | BPF_MSH, 14),
        /*  8 */ BPF_STMT(BPF_LD  | BPF_H   | BPF_IND, 14),
        /*  9 */ BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K,   53, 9, 0),
        /* 10 */ BPF_STMT(BPF_LD  | BPF_H   | BPF_IND, 16),
        /* 11 */ BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K,   53, 7, 6),

        /* ---- IPv6 ----（头固定 40 字节，UDP 端口在 54/56） */
        /* 12 */ BPF_STMT(BPF_LD  | BPF_B   | BPF_ABS, 20),
        /* 13 */ BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K,   17, 0, 4),
        /* 14 */ BPF_STMT(BPF_LD  | BPF_H   | BPF_ABS, 54),
        /* 15 */ BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K,   53, 3, 0),
        /* 16 */ BPF_STMT(BPF_LD  | BPF_H   | BPF_ABS, 56),
        /* 17 */ BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K,   53, 1, 0),

        /* 18 */ BPF_STMT(BPF_RET | BPF_K, 0),          /* 丢弃 */
        /* 19 */ BPF_STMT(BPF_RET | BPF_K, 0x40000),    /* 接受整帧 */
    };

    struct sock_fprog prog;
    prog.len    = (unsigned short)(sizeof(code) / sizeof(code[0]));
    prog.filter = code;

    return setsockopt(fd, SOL_SOCKET, SO_ATTACH_FILTER, &prog, sizeof(prog));
}
#endif  /* __linux__ —— 非 Linux 不需要也不定义该函数 */

dnssniff_t *dnssniff_open(const char *ifname, char *errbuf, size_t errlen)
{
#ifdef __linux__
    dnssniff_t *s = calloc(1, sizeof(*s));
    if (!s) {
        if (errbuf) snprintf(errbuf, errlen, "内存不足");
        return NULL;
    }

    s->max_pending = DEFAULT_MAX_PENDING;
    /* 表大小取 2 的幂，且留 2 倍余量降低冲突 */
    size_t sz = 1;
    while (sz < s->max_pending * 2) sz <<= 1;
    s->pend = calloc(sz, sizeof(pending_t));
    if (!s->pend) {
        free(s);
        if (errbuf) snprintf(errbuf, errlen, "内存不足");
        return NULL;
    }
    s->pend_mask = sz - 1;

    s->fd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    if (s->fd < 0) {
        if (errbuf) snprintf(errbuf, errlen, "AF_PACKET socket 失败: %s", strerror(errno));
        free(s->pend); free(s);
        return NULL;
    }

    /* 绑到指定网卡。NULL 表示全收。 */
    if (ifname && *ifname) {
        struct sockaddr_ll sa;
        memset(&sa, 0, sizeof(sa));
        sa.sll_family = AF_PACKET;
        sa.sll_protocol = htons(ETH_P_ALL);
        sa.sll_ifindex = (int)if_nametoindex(ifname);
        if (sa.sll_ifindex == 0) {
            if (errbuf) snprintf(errbuf, errlen, "网卡不存在: %s", ifname);
            close(s->fd); free(s->pend); free(s);
            return NULL;
        }
        if (bind(s->fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
            if (errbuf) snprintf(errbuf, errlen, "bind %s 失败: %s", ifname, strerror(errno));
            close(s->fd); free(s->pend); free(s);
            return NULL;
        }
    }

    /*
     * 先挂 BPF 过滤，再设缓冲区。
     * 顺序很重要：过滤器挂了以后只有 DNS 帧会进队列，
     * 缓冲区的实际压力能降两三个数量级（详见 attach_dns_filter 的注释）。
     *
     * 失败不致命：退化成"全量接收 + 用户态过滤"，功能照常，只是更容易丢帧。
     *
     * NETMON_DNS_NO_BPF=1 可跳过过滤，用于对比排查
     * （例如怀疑过滤器把某些报文误挡了）。
     */
    if (!getenv("NETMON_DNS_NO_BPF"))
        s->filter_ok = (attach_dns_filter(s->fd) == 0);

    /* 非阻塞：主循环用 poll 决定什么时候读 */
    int flags = fcntl(s->fd, F_GETFL, 0);
    if (flags >= 0) fcntl(s->fd, F_SETFL, flags | O_NONBLOCK);

    /*
     * 加大接收缓冲。
     *
     * 实测 br-lan 上内核丢帧高达 13 万帧（PACKET_STATISTICS 的 tp_drops），
     * 2MB 明显不够 —— 局域网里光 P2P/视频流就能把包速率推到几万 pps，
     * 而 AF_PACKET 是"每帧一份副本"全量投递，不管我们只想看 DNS。
     *
     * 尝试 32MB：内核会把 SO_RCVBUF 值翻倍后受 net.core.rmem_max 限制，
     * 读回实际值判断有没有被裁剪。设不上就退到能设多少算多少，
     * 功能不受影响，只是漏帧会多一些。
     */
    int want = 32 * 1024 * 1024;
    if (setsockopt(s->fd, SOL_SOCKET, SO_RCVBUF, &want, sizeof(want)) < 0) {
        want = 8 * 1024 * 1024;
        setsockopt(s->fd, SOL_SOCKET, SO_RCVBUF, &want, sizeof(want));
    }

    /* 读回实际生效值，供日志/排查用（内核通常给一半，且受 rmem_max 上限） */
    int got = 0;
    socklen_t gl = sizeof(got);
    if (getsockopt(s->fd, SOL_SOCKET, SO_RCVBUF, &got, &gl) == 0)
        s->rcvbuf_bytes = got;

    return s;
#else
    /* 非 Linux 没有 AF_PACKET：返回失败，由 sniff_start() 降级为警告，
       不影响监控主流程，也不影响 DNS 本身。纯解析函数仍可被单测直接调用。 */
    (void)ifname;
    if (errbuf && errlen) snprintf(errbuf, errlen, "AF_PACKET 仅在 Linux 可用");
    return NULL;
#endif
}

void dnssniff_close(dnssniff_t *s)
{
    if (!s) return;
    if (s->fd >= 0) close(s->fd);
    free(s->pend);
    free(s);
}

int dnssniff_fd(dnssniff_t *s) { return s ? s->fd : -1; }

void dnssniff_set_max_pending(dnssniff_t *s, size_t n)
{
    (void)s; (void)n;   /* 表在 open 时已分配，运行期不改 */
}

void dnssniff_get_stats(dnssniff_t *s, dnssniff_stats_t *out)
{
    if (!s || !out) return;
    *out = s->st;
}

int dnssniff_rcvbuf(dnssniff_t *s)
{
    return s ? s->rcvbuf_bytes : 0;
}

int dnssniff_filter_ok(dnssniff_t *s)
{
    return s ? s->filter_ok : 0;
}

/*
 * 解析 DNS 名字，支持压缩指针。
 *
 * 关键：out_end 返回"这个名字在原始报文里占到的下一个位置"，
 * 调用方靠它继续解析 type/class 字段。
 * 如果不返回这个位置，调用方就得自己再走一遍找结尾 ——
 * 遇到压缩指针时极容易算错（这里踩过两次坑）。
 *
 * 压缩指针只能跳一次（RFC 1035 允许链式，但实践中不会出现；
 * 为防环，深度限制 8）。
 */
static int read_name(const uint8_t *p, const uint8_t *end,
                     const uint8_t *base, char *out, size_t outsz,
                     const uint8_t **out_end)
{
    size_t n = 0;
    int depth = 0;
    const uint8_t *next = NULL;     /* 跳转前记下回程位置 */

    while (p < end) {
        if (*p == 0) {
            p++;
            if (!next) next = p;
            break;
        }

        /* 压缩指针：高两位 11 */
        if ((*p & 0xc0) == 0xc0) {
            if (p + 1 >= end || depth++ > 8) return -1;
            if (!next) next = p + 2;        /* 指针占 2 字节 */
            size_t off = ((size_t)(p[0] & 0x3f) << 8) | p[1];
            if (base + off >= end) return -1;
            p = base + off;
            continue;
        }

        size_t l = *p++;
        if (l > 63 || p + l > end) return -1;
        if (n) { if (n + 1 < outsz) out[n++] = '.'; }
        for (size_t i = 0; i < l; i++) {
            if (n + 1 < outsz) out[n++] = (char)p[i];
        }
        p += l;
    }
    if (!next) return -1;               /* 没遇到终止符，报文截断 */
    out[n] = '\0';
    if (out_end) *out_end = next;
    return (int)n;
}

/*
 * 解析一个 DNS 报文。
 * base 指向 DNS 头（用于压缩指针的相对偏移）。
 * 返回 1 表示是查询，2 表示是应答，0 表示无关/失败。
 */
static int parse_dns(const uint8_t *base, int len, dns_event_t *e)
{
    if (len < 12) return 0;

    uint16_t flags = (uint16_t)((base[2] << 8) | base[3]);
    int is_resp = (flags & 0x8000) != 0;
    uint16_t qd = (uint16_t)((base[4] << 8) | base[5]);
    uint16_t an = (uint16_t)((base[6] << 8) | base[7]);

    if (qd < 1) return 0;

    const uint8_t *p = base + 12;
    const uint8_t *end = base + len;

    e->is_query = !is_resp;
    e->txid  = (uint16_t)((base[0] << 8) | base[1]);
    e->flags = flags;
    e->qtype = 0;
    e->rcode = flags & 0x000f;
    e->n_ans = 0;
    e->an_count = 0;

    /* 第一个问题：qname + qtype + qclass */
    const uint8_t *after = NULL;
    if (read_name(p, end, base, e->qname, sizeof(e->qname), &after) < 0)
        return 0;
    if (after + 4 > end) return 0;

    e->qtype = (after[0] << 8) | after[1];
    p = after + 4;

    if (!is_resp) return 1;

    /*
     * 应答：遍历整个 answer 区。
     *
     * 早期版本遇到第一条 A/AAAA 就 break —— 双栈应答先给 A 再给 AAAA 时，
     * AAAA 结果不进 dnsmap，IPv6 目的 IP 永远反查不到域名（P4）。
     * 现在：
     *   - 每条记录都计入 an_count（应答区记录总数，含 CNAME 等）
     *   - 收集最多 DNS_MAX_ANS 条 A/AAAA 进 ans[]，各带自己的 TTL
     *   - CNAME 等其它类型跳过 rdata 但继续循环，绝不中断
     * 压缩指针解析（read_name）保持原逻辑不变。
     */
    for (uint16_t i = 0; i < an; i++) {
        char tmp[128];
        const uint8_t *nxt = NULL;
        if (read_name(p, end, base, tmp, sizeof(tmp), &nxt) < 0) break;
        p = nxt;

        if (p + 10 > end) break;
        uint16_t type  = (uint16_t)((p[0] << 8) | p[1]);
        uint32_t ttl   = ((uint32_t)p[4] << 24) | ((uint32_t)p[5] << 16)
                       | ((uint32_t)p[6] << 8)  | (uint32_t)p[7];
        uint16_t rdlen = (uint16_t)((p[8] << 8) | p[9]);
        const uint8_t *rdata = p + 10;
        if (rdata + rdlen > end) break;

        e->an_count++;

        if ((type == 1 && rdlen == 4) || (type == 28 && rdlen == 16)) {
            if (e->n_ans < DNS_MAX_ANS) {
                dns_answer_t *a = &e->ans[e->n_ans++];
                a->family = (type == 1) ? 4 : 6;
                a->ttl = ttl;
                memset(a->ip, 0, 16);
                if (type == 1) {                 /* A：v6-mapped */
                    a->ip[10] = 0xff; a->ip[11] = 0xff;
                    memcpy(a->ip + 12, rdata, 4);
                } else {                          /* AAAA：原生 16 字节 */
                    memcpy(a->ip, rdata, 16);
                }
            }
        }
        /* CNAME 等：跳过 rdata，继续下一条 */
        p = rdata + rdlen;
    }

    return 2;
}

/*
 * 把 4 字节 IPv4 地址填进 16 字节缓冲的 v6-mapped 形态。
 * 与 dnsmap 的 norm_key / ctparse 的约定一致：
 *   前 10 字节 0，ip[10]=ip[11]=0xff，后 4 字节 = a.b.c.d。
 * client / server 统一用这个形态，v6 则原样 16 字节。
 */
static void fill_v4mapped(uint8_t out[16], const uint8_t v4[4])
{
    memset(out, 0, 16);
    out[10] = 0xff; out[11] = 0xff;
    memcpy(out + 12, v4, 4);
}

/*
 * 处理一帧以太网。
 * 注意：SOCK_RAW 收到的是完整以太帧（含 14 字节以太头）。
 */
static void handle_frame(dnssniff_t *s, const uint8_t *f, int n,
                         dns_cb_t cb, void *ud)
{
    if (n < 14 + 20 + 8 + 12) return;

    /*
     * 调试开关：NETMON_DNS_DEBUG=1 时把每一帧的归属和配对结果打到 stderr。
     * 用途：线上出现"某些域名应答率只有一半"这类问题时，不用改代码重编译
     * 就能看到到底是没抓到查询、还是配不上、还是在哪一层被丢弃。
     * 默认关闭，零开销（getenv 只查一次，缓存进静态变量）。
     */
    static int dbg = -1;
    if (dbg < 0) dbg = getenv("NETMON_DNS_DEBUG") ? 1 : 0;

    uint16_t eth = (uint16_t)((f[12] << 8) | f[13]);
    int off = 14;
    int is_v4 = 0;

    if (eth == 0x0800) {
        is_v4 = 1;
    } else if (eth == 0x86dd) {
        is_v4 = 0;
    } else {
        if (dbg) fprintf(stderr, "[dnsdbg] DROP-eth type=%04x len=%d\n", eth, n);
        return;
    }

    uint32_t client_ip = 0;
    uint32_t sport, dport;
    int iphdr_len;

    if (is_v4) {
        const uint8_t *ip = f + off;
        if ((ip[0] >> 4) != 4) return;
        iphdr_len = (ip[0] & 0x0f) * 4;
        if (iphdr_len < 20 || off + iphdr_len + 8 > n) return;
        if (ip[9] != 17) {
            if (dbg) fprintf(stderr, "[dnsdbg] DROP-proto %u len=%d\n", ip[9], n);
            return;
        }
        /* 分片包不处理（DNS 基本不会分片，处理成本不值得） */
        uint16_t frag = (uint16_t)((ip[6] << 8) | ip[7]);
        if (frag & 0x3fff) {
            if (dbg) fprintf(stderr, "[dnsdbg] DROP-frag frag=%04x\n", frag);
            return;
        }

        memcpy(&client_ip, ip + 12, 4);

        const uint8_t *udp = ip + iphdr_len;
        sport = (uint32_t)((udp[0] << 8) | udp[1]);
        dport = (uint32_t)((udp[2] << 8) | udp[3]);

        if (dport != 53 && sport != 53) return;

        const uint8_t *dns = udp + 8;
        int dnslen = n - (off + iphdr_len + 8);
        if (dnslen < 12) return;

        dns_event_t e;
        memset(&e, 0, sizeof(e));

        int r = parse_dns(dns, dnslen, &e);
        if (r == 0) {
            s->st.dropped++;
            if (dbg) fprintf(stderr, "[dnsdbg] DROP-parse sp=%u dp=%u dnslen=%d\n",
                             sport, dport, dnslen);
            return;
        }

        uint32_t txid = (uint32_t)((dns[0] << 8) | dns[1]);
        uint64_t t = now_us();

        /*
         * 配对用的"客户端"身份，两个方向取值不同：
         *   查询：客户端是**源**（src=sport=客户端端口）
         *   应答：客户端是**目的**（dst=dport=客户端端口）
         * 之前在应答分支里也用了源 IP/源端口（那是 DNS 服务器），
         * 导致永远配不上、耗时恒为 NULL。
         */
        if (r == 1) {
            s->st.dns_queries++;
            e.is_query = 1;
            /* 查询帧：client=ip.src、server=ip.dst、server_port=dport */
            e.client_fam = 4; e.server_fam = 4;
            fill_v4mapped(e.client, ip + 12);
            fill_v4mapped(e.server, ip + 16);
            e.server_port = (uint16_t)dport;
            e.txid  = (uint16_t)txid;
            e.flags = (uint16_t)((dns[2] << 8) | dns[3]);
            e.client_ip = client_ip;                 /* 源 = 客户端，网络字节序 */
            /*
             * qtype 必须一起存：A 和 AAAA 会共用 txid/端口，不带类型会互相覆盖。
             * ckey 用 v4 地址本身（高 32 位补 0），与 v6 分支的哈希值域不冲突：
             * v6 结果一定是 mix64 出来的散列，v4 是原始地址，两者撞上的概率可忽略。
             */
            pend_put(s, txid, (uint64_t)client_ip, sport, (uint16_t)e.qtype, t);
            if (dbg)
                fprintf(stderr, "[dnsdbg] v4 QUERY %s qt=%d tx=%04x ck=%08x pt=%u\n",
                        e.qname, e.qtype, txid, client_ip, sport);
            e.ms = -1.0;
            if (cb) cb(&e, ud);
        } else {
            uint32_t dst_ip;
            memcpy(&dst_ip, ip + 16, 4);             /* 目的 = 客户端 */
            s->st.dns_replies++;
            e.is_query = 0;
            /* 应答帧：client=ip.dst、server=ip.src、server_port=sport */
            e.client_fam = 4; e.server_fam = 4;
            fill_v4mapped(e.client, ip + 16);
            fill_v4mapped(e.server, ip + 12);
            e.server_port = (uint16_t)sport;
            e.txid  = (uint16_t)txid;
            e.flags = (uint16_t)((dns[2] << 8) | dns[3]);
            e.client_ip = dst_ip;
            uint64_t q0 = pend_get_and_del(s, txid, (uint64_t)dst_ip, dport,
                                           (uint16_t)e.qtype);
            if (!q0) s->st.pending_unpaired++;
            if (dbg)
                fprintf(stderr, "[dnsdbg] v4 REPLY %s qt=%d tx=%04x ck=%08x pt=%u -> %s\n",
                        e.qname, e.qtype, txid, dst_ip, dport,
                        q0 ? "hit" : "MISS");
            /* 微秒 -> 毫秒（保留小数） */
            e.ms = q0 ? (double)(t - q0) / 1000.0 : -1.0;
            if (cb) cb(&e, ud);
        }
        return;
    }

    /* IPv6：固定 40 字节头，跳过扩展头太麻烦，只处理 next_header=17 的 */
    {
        const uint8_t *ip = f + off;
        if ((ip[0] >> 4) != 6) return;
        if (ip[6] != 17) return;                     /* 只处理 UDP */
        if (off + 40 + 8 + 12 > n) return;

        const uint8_t *udp = ip + 40;
        sport = (uint32_t)((udp[0] << 8) | udp[1]);
        dport = (uint32_t)((udp[2] << 8) | udp[3]);
        if (dport != 53 && sport != 53) return;

        const uint8_t *dns = udp + 8;
        int dnslen = n - (off + 40 + 8);
        if (dnslen < 12) return;

        dns_event_t e;
        memset(&e, 0, sizeof(e));

        int r = parse_dns(dns, dnslen, &e);
        if (r == 0) { s->st.dropped++; return; }

        uint32_t txid = (uint32_t)((dns[0] << 8) | dns[1]);
        uint64_t t = now_us();

        /*
         * IPv6 也要配对。
         *
         * 之前这里是"e.client_ip = 0，v6 不做配对"，
         * 结果所有走 IPv6 的查询耗时恒为 NULL。
         * 而 dnsmasq 同时监听 192.168.1.1:53 和 fda4:...::1:53 / fe80:...，
         * 双栈客户端查 AAAA 时常走 IPv6 —— 于是"凡是查 AAAA 的域名
         * 应答率正好掉一半"，看着像 DNS 丢包，其实是这里没配对。
         *
         * ckey 用 128 位地址哈希成 64 位。查询取源地址、应答取目的地址，
         * 与 v4 分支的方向约定保持一致。
         */
        const uint8_t *addr = (r == 1) ? (ip + 8) : (ip + 24);

        if (r == 1) {
            s->st.dns_queries++;
            e.is_query = 1;
            /* 查询帧：client=ip.src(8)、server=ip.dst(24)、server_port=dport */
            e.client_fam = 6; e.server_fam = 6;
            memcpy(e.client, ip + 8, 16);
            memcpy(e.server, ip + 24, 16);
            e.server_port = (uint16_t)dport;
            e.txid  = (uint16_t)txid;
            e.flags = (uint16_t)((dns[2] << 8) | dns[3]);
            /* client_ip 兼容字段仅 v4 有效，v6 保持 0 */
            pend_put(s, txid, mix64_addr(addr), sport, (uint16_t)e.qtype, t);
            if (dbg)
                fprintf(stderr, "[dnsdbg] v6 QUERY %s qt=%d tx=%04x "
                        "ck=%016llx pt=%u\n", e.qname, e.qtype, txid,
                        (unsigned long long)mix64_addr(addr), sport);
            e.ms = -1.0;
            if (cb) cb(&e, ud);
        } else {
            s->st.dns_replies++;
            e.is_query = 0;
            /* 应答帧：client=ip.dst(24)、server=ip.src(8)、server_port=sport */
            e.client_fam = 6; e.server_fam = 6;
            memcpy(e.client, ip + 24, 16);
            memcpy(e.server, ip + 8, 16);
            e.server_port = (uint16_t)sport;
            e.txid  = (uint16_t)txid;
            e.flags = (uint16_t)((dns[2] << 8) | dns[3]);
            uint64_t q0 = pend_get_and_del(s, txid, mix64_addr(addr), dport,
                                           (uint16_t)e.qtype);
            if (!q0) s->st.pending_unpaired++;
            if (dbg)
                fprintf(stderr, "[dnsdbg] v6 REPLY %s qt=%d tx=%04x "
                        "ck=%016llx pt=%u -> %s\n", e.qname, e.qtype, txid,
                        (unsigned long long)mix64_addr(addr), dport,
                        q0 ? "hit" : "MISS");
            e.ms = q0 ? (double)(t - q0) / 1000.0 : -1.0;
            if (cb) cb(&e, ud);
        }
    }
}

int dnssniff_pump(dnssniff_t *s, dns_cb_t cb, void *ud)
{
    if (!s || s->fd < 0) return -1;

    uint8_t buf[2048];
    int got = 0;

    /*
     * 每轮上限 2048 帧。
     *
     * 之前是 256，实测会漏：DEBUG 日志显示 REPLY 25 条而 QUERY 只有 18 条，
     * 少了 7 条查询 —— 说明抓包环节在丢帧，不是在配对环节。
     * 2MB 缓冲区理论上够，但 AF_PACKET 在网桥上对 ingress/egress 两侧
     * 投递时机不对等，瞬时突发时仍可能排不上队。
     * 提高单轮上限让每次 pump 尽量把缓冲排空，减少积压。
     * 主循环 200ms 一轮，2048 帧/轮 = 每秒 1 万帧处理能力，
     * 局域网 DNS 峰值远低于此，不会因此吃满 CPU。
     */
    for (int i = 0; i < 2048; i++) {
        ssize_t n = recv(s->fd, buf, sizeof(buf), 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) break;

        s->st.frames++;
        handle_frame(s, buf, (int)n, cb, ud);
        got++;
    }

    /*
     * 问内核要一次"丢了多少帧"的计数。
     * PACKET_STATISTICS 读取后会清零，所以这里是增量。
     * 这个数字能直接回答"是不是缓冲区溢出丢的" ——
     * 如果它一直是 0 却仍有 MISS，那就是别的环节的问题。
     */
#ifdef __linux__
    if (got) {
        struct tpacket_stats kst;
        socklen_t sl = sizeof(kst);
        if (getsockopt(s->fd, SOL_PACKET, PACKET_STATISTICS, &kst, &sl) == 0)
            s->st.kernel_drops += kst.tp_drops;
    }
#else
    (void)got;   /* 非 Linux 无 PACKET_STATISTICS */
#endif

    return got;
}
