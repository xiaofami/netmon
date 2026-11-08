/*
 * probe.h - 主动探测：延迟/丢包/带宽/DNS质量/站点可用性
 *
 * 与 conns 表的分工：
 *   conns 是被动观测——用户实际访问了什么，被动记录。
 *   probe 是主动探测——网络当前质量如何，定时测量。
 *   两者结合才能回答"为什么这个时段访问慢"。
 *
 * 探测维度（对应选定的四项）：
 *   1. 出国带宽 + 延迟 + 丢包   -> probe_ping / probe_http(max_bytes 大)
 *   2. DNS 解析质量             -> probe_dns
 *   3. 站点可用性拨测           -> probe_http(max_bytes 小)
 *   4. 分运营商线路质量         -> 结果带 tag 区分
 *
 * 实现原则：
 *   - 全部带超时保护，绝不能让探测卡住主循环。
 *   - 探测频率低（几分钟一次），对网络零影响。
 *   - 每个目标独立超时，一个挂了不影响其他。
 *
 * 注：带宽测速和站点拨测本质是同一个动作（HTTP GET），
 *     区别只在 max_bytes 和是否关心速度，所以共用 probe_http。
 */

#ifndef NETMON_PROBE_H
#define NETMON_PROBE_H

#include <stdint.h>
#include <stddef.h>

/* ---------------- 绕过代理 ---------------- */

/*
 * 探测流量的绕过标记。
 *
 * 实测发现的问题：homeproxy 以 TUN 模式运行，装了 19 条 redirect/dnat 规则。
 * 对 211.137.32.178 做 TCP:443 探测时测出 0.2ms——这是本机回环速度，
 * 说明连接根本没出网，在本地就被 sing-box 接管了。
 * 这样的数据毫无意义，会得出"线路极好"的错误结论。
 *
 * 解法：nft 规则里 `meta mark 0x64 return` 会直接放行带该 mark 的流量。
 * 给探测 socket 设 SO_MARK=100，走的就是真实物理线路。
 *
 * 该值可被配置文件覆盖，默认 100（对应 homeproxy 默认放行标记）。
 */
void probe_set_bypass_mark(uint32_t mark);

/* 当前生效的绕过标记，0 表示不绕过 */
uint32_t probe_get_bypass_mark(void);

/*
 * curl 用的绕过方式：绑定源地址。
 *
 * curl 不支持 SO_MARK，但可以用 --interface 绑定 WAN 的源 IP。
 * 绑定后连接不经过 TUN 的 iifname 匹配，直接走物理网卡出网。
 * 实测 --interface 203.0.113.45 把 connect 从 201ms 降到 2ms，
 * 确认生效。传 NULL 或空串表示不绑定。
 */
void probe_set_bypass_iface(const char *ip_or_dev);
const char *probe_get_bypass_iface(void);

/* ---------------- Ping ---------------- */

typedef struct {
    int      ok;          /* 收到应答的包数 */
    int      sent;        /* 发出的包数 */
    double   rtt_min;
    double   rtt_avg;
    double   rtt_max;
    double   jitter;      /* 相邻 RTT 差的平均绝对值 */
    double   loss;        /* 0.0 ~ 1.0 */
    int      err;         /* 失败时的 errno，0 表示无错 */
} ping_result_t;

/*
 * ICMP ping（IPv4）。target 必须是 IP 字面量，不做 DNS 解析
 * ——这样可以独立测量"到某个 IP 的线路质量"，不受解析影响。
 * 需要 root 权限（原始 socket）。
 * 返回 0 表示探测流程执行完毕（不代表网络通），结果看 out。
 */
int probe_ping(const char *target, int count, int timeout_ms, ping_result_t *out);

/*
 * TCP 连接探测（SYN -> 握手完成）。
 *
 * 为什么必须有它：
 *   实测发现 211.137.32.178（移动DNS）、180.101.50.242（百度）都是
 *   100% ICMP 丢包，但服务本身完全正常——它们禁 ping。
 *   只看 ICMP 会把这些主机误判成"线路差"，进而污染统计。
 *   所以对 ICMP 不通的目标，必须能降级到 TCP 探测。
 *
 * port 传 0 时默认 443。
 */
int probe_tcp(const char *target, int port, int count, int timeout_ms,
              ping_result_t *out);

/* ---------------- HTTP ---------------- */

typedef struct {
    int      ok;
    int      code;            /* HTTP 状态码，0 表示没拿到 */
    double   connect_ms;      /* TCP 握手耗时 */
    double   tls_ms;          /* TLS 握手耗时，http 为 0 */
    double   ttfb_ms;         /* 发完请求到收到第一个字节 */
    double   total_ms;
    long long bytes;          /* 实际收到的字节数 */
    double   kbps;            /* 传输阶段平均速率，千比特/秒 */
    int      err;
} http_result_t;

/*
 * 一次 HTTP GET。
 *
 * max_bytes 控制下载上限：
 *   - 站点拨测：传 1（只要首字节和状态码）
 *   - 带宽测速：传 8MB 之类，读满或超时即止
 * 传 0 表示不限（不推荐，可能拉巨大文件）。
 *
 * url 形如 http://host/path 或 https://host/path。
 * 不支持重定向跟随超过 3 次。
 */
int probe_http(const char *url, int timeout_ms, long max_bytes,
               http_result_t *out);

/*
 * 同 probe_http，但强制走指定的 HTTP/SOCKS 混合代理（homeproxy mixed_port）。
 *
 * 用途：当某个域名直连失败时，测一次「开代理能否访问」，用来判断
 * 是否该把它加进 homeproxy 的 Proxy 规则。走代理时不加 --interface，
 * 否则会绕过 TUN 变成直连，测出来的结论是错的。
 * 代理路径默认 -k：代理 MITM / IP 证书不匹配不应被当成「不可达」。
 * proxy 形如 http://127.0.0.1:5330，NULL 表示直连。
 */
int probe_http_proxy(const char *url, const char *proxy,
                     int timeout_ms, long max_bytes, http_result_t *out);

/* ---------------- DNS ---------------- */

typedef struct {
    int      ok;              /* 拿到了应答（无论 rcode） */
    uint32_t rcode;           /* 0=NOERROR, 3=NXDOMAIN ... */
    double   ms;              /* 端到端耗时 */
    int      answers;         /* 应答记录数（0 表示 NXDOMAIN/过滤） */
    char     first_ip[64];
    int      err;
} dns_result_t;

/*
 * 用系统解析器（getaddrinfo）查 qname。
 *
 * 为什么不用自建 DNS 报文：
 *   getaddrinfo 走的就是系统实际使用的解析链路（含 homeproxy 的 DNS 劫持、
 *   缓存、上游），测出来的才是"用户真实感受到的解析耗时"。
 *   自建报文得自己处理上游选择，反而偏离真实路径。
 *   代价是拿不到精确的 rcode，用 answers==0 近似。
 */
int probe_dns(const char *qname, uint32_t timeout_ms, dns_result_t *out);

/* ---------------- 批量 ---------------- */

typedef struct {
    const char *tag;          /* "telecom" / "unicom" / "mobile" / "oversea" ... */
    const char *host;         /* IP 字面量 */
} probe_target_t;

typedef void (*probe_report_fn)(const probe_target_t *t,
                               const ping_result_t *r, void *ud);

/* 顺序跑一组 ping 目标。总耗时上限 = n * count * timeout_ms，
 * 调用方要选好参数别让一轮探测太久。 */
int probe_run_group(const probe_target_t *targets, size_t n,
                    int count, int timeout_ms,
                    probe_report_fn cb, void *ud);

#endif /* NETMON_PROBE_H */
