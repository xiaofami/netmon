/*
 * dnssniff.h - 被动旁路抓取 DNS 查询/应答
 *
 * ⚠️ 安全边界（极其重要，改动前务必读完）
 *
 *   本模块是 **只读旁路监听**，在 DNS 通路上不占任何位置：
 *     - 用 AF_PACKET 在网卡上复制一份报文，内核协议栈照常转发；
 *     - 不修改 dnsmasq / smartdns 任何配置；
 *     - 不添加任何 iptables / nftables / eBPF 规则；
 *     - 不碰 /etc/resolv.conf，不换监听端口，不做代理转发；
 *     - 不发任何报文，只收。
 *
 *   因此：本模块崩溃、退出、被 kill -9，DNS 分流与解析 **完全不受影响**。
 *   这是它相比 eBPF（挂错位置会真的拦包/改包）最关键的优点。
 *
 * 为什么不用 eBPF：
 *   验证过内核对 eBPF 的支持（6.12.103，bpffs 已挂载，tracepoint/xdp/kprobe
 *   等 program type 都可用），但：
 *     1) 这版内核 kprobe 的 available_filter_functions 是空的，kprobe 不可用；
 *     2) 现有 tracepoint 里没有能拿到 DNS 报文内容的（udp 只有 fail_queue_rcv_skb，
 *        sock 只有 set_state 等）；
 *     3) 唯一可行路径是 XDP/TC + 自己解析，而 TC/XDP 程序必须挂在转发路径上，
 *        一旦出错就是真的断网；
 *     4) 编译需要 clang + libbpf，apk 仓库没有 clang 包。
 *   结论：AF_PACKET 旁路能做到完全相同的数据，风险却是零。
 */

#ifndef NETMON_DNSSNIFF_H
#define NETMON_DNSSNIFF_H

#include <stdint.h>
#include <stddef.h>

typedef struct dnssniff dnssniff_t;

/*
 * 一次 DNS 观测（1.1.0 升级）。
 *
 * v4 地址一律用 16 字节缓冲的 v6-mapped 形态存放
 *   （前 10 字节 0，ip[10]=ip[11]=0xff，后 4 字节为 a.b.c.d），
 * 与 dnsmap / ctparse 的归一约定一致；v6 原生 16 字节。
 *
 * 查询帧：client = ip.src、server = ip.dst、server_port = dport；
 * 应答帧：client = ip.dst、server = ip.src、server_port = sport。
 * 两条协议族分支都按此填充，IPv6 不再丢失客户端身份。
 */
#define DNS_MAX_ANS 8

typedef struct {
    uint8_t  family;        /* 该应答地址协议族 4 或 6 */
    uint8_t  ip[16];         /* v4 统一存 v6-mapped，与 dnsmap 一致 */
    uint32_t ttl;            /* 该记录的 TTL（秒） */
} dns_answer_t;

typedef struct {
    char     qname[128];
    uint16_t txid;           /* DNS 事务 ID */
    int      qtype, rcode;   /* 1=A, 28=AAAA；rcode 仅应答有效 */
    uint16_t flags;          /* DNS header flags，供 AA/RA/TC 判定 */
    double   ms;             /* -1 = 未配对 */
    uint8_t  is_query;
    uint8_t  client_fam, server_fam;
    uint8_t  client[16], server[16];   /* 全地址，v4 用 v6-mapped */
    uint16_t server_port;
    uint32_t client_ip;      /* 兼容旧字段：仅 v4 有效（网络字节序，取后 4 字节） */
    uint8_t  n_ans;          /* 收集到的 A/AAAA 条数（≤ DNS_MAX_ANS） */
    dns_answer_t ans[DNS_MAX_ANS];
    uint16_t an_count;      /* 应答区记录总数（含 CNAME 等） */
} dns_event_t;

typedef void (*dns_cb_t)(const dns_event_t *e, void *ud);

/*
 * 打开监听。
 * ifname : 网卡名，NULL 表示监听所有网卡
 * 返回 NULL 表示失败（权限不足 / 网卡不存在），原因写 errbuf。
 *
 * 注意：需要 CAP_NET_RAW（root 即可）。
 */
dnssniff_t *dnssniff_open(const char *ifname, char *errbuf, size_t errlen);
void        dnssniff_close(dnssniff_t *s);

/*
 * 取底层 fd，交给主循环做 poll。
 * 返回 -1 表示已关闭。
 */
int         dnssniff_fd(dnssniff_t *s);

/*
 * 处理一批已就绪的报文。非阻塞。
 * 返回本轮解析出的事件数，-1 表示致命错误。
 */
int         dnssniff_pump(dnssniff_t *s, dns_cb_t cb, void *ud);

/* 统计 */
typedef struct {
    uint64_t frames;        /* 收到的帧总数 */
    uint64_t dns_queries;
    uint64_t dns_replies;
    uint64_t dropped;       /* 解析失败/丢弃 */
    uint64_t pending;       /* 当前待配对的查询数 */
    /*
     * 应答里没能找到对应查询的条数（累计）。
     * 这是排查"应答率只有一半"的关键指标：
     *   - 若它 ≈ dns_replies，说明查询根本没抓到（监听网卡/方向不对）
     *   - 若它 ≈ dns_replies/2，说明 key 撞了（qtype/端口/txid 组合有问题）
     */
    uint64_t pending_unpaired;

    /*
     * 内核因缓冲区满而丢弃的帧数（PACKET_STATISTICS 增量累计）。
     * 排查"查询抓不全"时先看这个：
     *   - 持续增长 → 缓冲区不够，加大 SO_RCVBUF 或提高单轮处理上限
     *   - 始终为 0 → 不是缓冲区问题，要看是不是根本没经过这块网卡
     */
    uint64_t kernel_drops;
} dnssniff_stats_t;

void dnssniff_get_stats(dnssniff_t *s, dnssniff_stats_t *out);

/*
 * 实际生效的 SO_RCVBUF 字节数（内核一般给设定值的一半）。
 * 排查"抓包跟不上"时用：太小说明缓冲区是瓶颈。
 */
int  dnssniff_rcvbuf(dnssniff_t *s);

/*
 * BPF 过滤是否生效。1 = 内核只投递 udp/53，丢帧风险大幅降低；
 * 0 = 退化为全量接收（功能正常，但流量大时更容易丢帧）。
 */
int  dnssniff_filter_ok(dnssniff_t *s);

/* 待配对查询表的容量上限，默认 4096。0 表示用默认值。 */
void dnssniff_set_max_pending(dnssniff_t *s, size_t n);

#endif /* NETMON_DNSSNIFF_H */
