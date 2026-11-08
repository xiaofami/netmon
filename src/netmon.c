/*
 * netmon - 网络监控主程序
 *
 * 架构：
 *
 *   conntrack (/proc/net/nf_conntrack)
 *        |
 *        | 每 5 秒增量采样（ctsample）
 *        v
 *   连接生命周期跟踪（哈希表，内存）
 *        |
 *        | 连接消亡时记录 起止时间 + 双向字节/包数
 *        v
 *   写库缓冲（db.c，攒够批量提交）
 *        |
 *        v
 *   SQLite（30 天明细 + 永久小时/天聚合）
 *
 *
 *   dnsmasq 日志 --(tail)--> dnsmap（IP -> 域名）
 *                                 |
 *                                 v 链接时补上 host 字段
 *
 *
 *   probe（每 5 分钟）--> 延迟/丢包/带宽/DNS质量/站点可用性 --> probe 表
 *
 *
 * 设计原则（对应"性能要强，不能劣化现有网络"）：
 *   1. 采样只读 /proc，不挂 netfilter hook，零数据面开销
 *   2. 单条连接只写一行，不是每采样点一行
 *   3. 写库走大事务，把 fsync 摊薄到几千条一次
 *   4. 主动探测频率低、数据量小，且绕过代理标记
 *   5. 常驻内存仅哈希表（每条 ~120 字节），万级连接约 1.2MB
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <math.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <syslog.h>
#include <sys/types.h>

#include "ctparse.h"
#include "ctsample.h"
#include "db.h"
#include "dnsmap.h"
#include "dnssniff.h"
#include "hprules.h"

/* ---------------- 配置 ---------------- */

typedef struct {
    /* 采样 */
    int      sample_ms;         /* conntrack 采样间隔，默认 5000 */
    /* 写库 */
    const char *db_path;
    int      cache_kb;          /* SQLite page cache */
    int      mmap_kb;
    int      flush_ms;          /* 最长多久刷一次盘 */
    /* 保留 */
    int      retain_days;       /* 明细保留天数，默认 30 */
    /* DNS 日志（旧方案，需改 dnsmasq，默认关闭） */
    const char *dns_log;        /* dnsmasq 日志路径，空表示不追踪 */
    /* DNS 被动旁路嗅探（新方案，AF_PACKET 只读，不碰 dnsmasq） */
    int      dns_sniff;         /* 1 = 启用，默认 1 */
    const char *sniff_if;       /* 监听哪个网卡，NULL = 全部 */
    /* 其它 */
    int      verbose;
} cfg_t;

static cfg_t g_cfg = {
    .sample_ms   = 5000,
    .db_path     = "/opt/netmon/data/netmon.db",
    .cache_kb    = 262144,      /* 256MB：3.9G 内存，留足余量 */
    .mmap_kb     = 262144,
    .flush_ms    = 30000,
    .retain_days = 30,
    .dns_log     = "",          /* 空 = 不读 dnsmasq 日志 */
    .dns_sniff   = 1,           /* 默认启用被动嗅探（只读，零风险） */
    .sniff_if    = "br-lan",    /* 只监听 LAN 侧，不碰 WAN */
    .verbose     = 0,
};

/* ---------------- 全局状态 ---------------- */

static volatile sig_atomic_t g_stop = 0;
static db_t   *g_db = NULL;
static dnsmap_t *g_dnsmap = NULL;
static int     g_dns_fd = -1;
static dnssniff_t *g_sniff = NULL;
static time_t  g_last_gc = 0;
static time_t  g_last_rollup = 0;

/* 运行统计 */
static struct {
    unsigned long samples;
    unsigned long conns_written;
    unsigned long parse_fail;
    unsigned long ignored;            /* 被过滤的噪音连接 */
    unsigned long preexisting_skip;   /* 启动前就存在、被跳过的连接 */
    uint64_t      last_sample_us;
    uint64_t      total_sample_us;
    unsigned long dns_lines;
    unsigned long dns_maps;
    /* DNS 嗅探 */
    unsigned long dns_queries;
    unsigned long dns_replies;
    unsigned long dns_mapped;
} g_stat;

/* ---------------- 工具 ---------------- */

static uint64_t mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static uint64_t wall_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

/*
 * 日志：直接写 syslog，带正确的优先级。
 *
 * 之前所有输出都走 stderr，而 procd 把整个服务的 stderr 当成一个流交给 logd，
 * 没有优先级的原始行一律被标成 daemon.err。结果 logread 里
 * 「本地网段 192.168.1.0/24」这种正常信息也显示为错误，真出故障时反而淹没在
 * 一堆 err 里分不出来。
 *
 * 现在按严重程度分四级写 LOG_DAEMON：
 *   LOG_DEBUG   心跳统计、采样明细。默认不落盘 —— 每分钟一条，一天 1440 条，
 *               会把 logd 那点环形缓冲里别的服务的日志挤掉。加 -v 才输出。
 *   LOG_INFO    启停、网段识别、规则同步
 *   LOG_WARNING 非致命降级（嗅探起不来、homeproxy 没装）
 *   LOG_ERR     真故障（库打不开、建表失败、刷盘失败）
 *
 * 前台手跑（stderr 是终端）时同时打到终端，方便调试；被 procd 接管后
 * stderr 不是终端，就只走 syslog，同一条日志不会记两遍。
 */
static int g_stderr_tty;    /* stderr 是不是终端，main 里初始化 */

static void vlog(int pri, const char *fmt, va_list ap)
{
    char buf[1024];
    vsnprintf(buf, sizeof(buf), fmt, ap);

    /* 注意用 "%s" 而不是把 buf 当格式串：日志里有 % 会被误解析 */
    syslog(pri, "%s", buf);

    if (g_stderr_tty) {
        time_t t = time(NULL);
        struct tm tm;
        localtime_r(&t, &tm);
        fprintf(stderr, "%04d-%02d-%02d %02d:%02d:%02d %s\n",
                tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                tm.tm_hour, tm.tm_min, tm.tm_sec, buf);
    }
}

static void logd(const char *fmt, ...)      /* debug：只在 -v 时落盘 */
{
    if (!g_cfg.verbose) return;
    va_list ap; va_start(ap, fmt);
    vlog(LOG_DEBUG, fmt, ap);
    va_end(ap);
}

static void logi(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    vlog(LOG_INFO, fmt, ap);
    va_end(ap);
}

static void logw(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    vlog(LOG_WARNING, fmt, ap);
    va_end(ap);
}

static void loge(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    vlog(LOG_ERR, fmt, ap);
    va_end(ap);
}

/* ---------------- 连接消亡 -> 写库 ---------------- */

/*
 * 这是整个系统的核心数据入口。
 *
 * 每条连接在消亡时被调用一次，写入一行记录：
 *   - 起止时间、双向包数/字节数
 *   - 反查出的域名（如果 dnsmap 里有）
 *
 * 为什么在消亡时才写：
 *   活着的时候写会变成"每条连接 × 每次采样"一行，
 *   30 天下来是十亿级数据量。消亡时写只需一行，
 *   而且此时累计值才是完整的。
 */
/*
 * 判断这条连接是否值得记录。
 *
 * 过滤掉三类噪音（实测中它们占了相当比例，且完全不含用户上网信息）：
 *   1. 回环：127.0.0.0/8、::1 —— 本机进程间通信
 *   2. 局域网内部：源和目的都在**本机有地址的同一个网段**
 *      —— 这些是设备之间/容器之间的流量，不是"上网"
 *   3. 广播/组播：.255、224.0.0.0/4
 *
 * 第 2 条不写死网段：LAN 可能是 192.168.1.0/24，也可能是 10.x、172.16.x，
 * 还可能有多个 VLAN、docker/podman 网桥。启动时枚举本机所有接口地址与掩码，
 * 记成若干网段，运行时按掩码比对。这样换网段不需要改代码。
 *
 * 但不能反过来「把私有地址全过滤掉」：PPPoE 拿到的 WAN IP 往往也是 10.x，
 * 那是真实出网流量，必须留。判据是「本机在该网段有地址」而不是「是私有地址」。
 */
#define MAX_LOCAL_NET 16
#define MAX_SELF_IP   32

typedef struct { uint32_t net; uint32_t mask; } local_net_t;

static local_net_t g_lnet[MAX_LOCAL_NET];
static int         g_lnet_n;

/*
 * 本机自己所有接口的 IPv4 地址（含 WAN、pppoe、tun、podman 网桥等）。
 *
 * 为什么要单独记：开了 tun 代理（sing-box / homeproxy）之后，同一份用户流量
 * 在 conntrack 里会出现两次 ——
 *     src=<客户端>     dst=<目标>  ← 客户端发出，被 tun 收走
 *     src=<本机 WAN 地址> dst=<目标>  ← 代理进程从 WAN 重新发出
 * 两条的 bytes 几乎相同。不处理的话：域名总量翻倍，WAN 地址还会被当成一台
 * 「未命名设备」和真设备并列出现在设备榜里。
 *
 * 判据取「src 是本机任何一个接口地址」：设备永远是别的机器，本机地址不可能是设备。
 * 这条也顺带解决「路由器自己下载东西算成一台设备」的观感问题。
 */
static uint32_t g_self_ip[MAX_SELF_IP];
static int      g_self_n;

/* 同理记一份 IPv6。tun 代理同样会让 v6 流量出现两次。 */
static uint8_t  g_self_v6[MAX_SELF_IP][16];
static int      g_self_v6_n;

/* 把本机所有接口的 IPv4 地址折算成 (network, mask)，忽略点对点与 /32。 */
static void load_local_nets(void)
{
    struct ifaddrs *ifa, *p;
    if (getifaddrs(&ifa) != 0) {
        logw("getifaddrs 失败: %s（LAN 内互访不过滤）", strerror(errno));
        return;
    }
    for (p = ifa; p; p = p->ifa_next) {
        if (!p->ifa_addr) continue;

        /* --- IPv6：只收 global，跳过 link-local（fe80 每口一个，无意义）--- */
        if (p->ifa_addr->sa_family == AF_INET6) {
            const uint8_t *b = ((struct sockaddr_in6 *)p->ifa_addr)->sin6_addr.s6_addr;
            if (b[0] == 0xfe && b[1] == 0x80) continue;         /* link-local */
            int is_lo6 = !memcmp(b, "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\1", 16);
            if (is_lo6) continue;
            if (g_self_v6_n < MAX_SELF_IP)
                memcpy(g_self_v6[g_self_v6_n++], b, 16);
            continue;
        }

        if (p->ifa_addr->sa_family != AF_INET) continue;
        uint32_t a = ntohl(((struct sockaddr_in *)p->ifa_addr)->sin_addr.s_addr);

        /* 本机地址全收（含 /32 的 PPPoE 对端、tun 的 /30），用于排除自身 */
        if (a != 0x7f000001u && g_self_n < MAX_SELF_IP) {
            int dup = 0;
            for (int i = 0; i < g_self_n; i++)
                if (g_self_ip[i] == a) { dup = 1; break; }
            if (!dup) g_self_ip[g_self_n++] = a;
        }

        if (!p->ifa_netmask) continue;
        uint32_t m = ntohl(((struct sockaddr_in *)p->ifa_netmask)->sin_addr.s_addr);
        if (m == 0 || m == 0xffffffffu) continue;   /* 无掩码 / 单主机路由 */
        if (!strcmp(p->ifa_name, "lo")) continue;
        if (g_lnet_n >= MAX_LOCAL_NET) continue;

        g_lnet[g_lnet_n].net  = a & m;
        g_lnet[g_lnet_n].mask = m;
        g_lnet_n++;
    }
    freeifaddrs(ifa);

    for (int i = 0; i < g_lnet_n; i++) {
        char ip[INET_ADDRSTRLEN];
        struct in_addr n = { htonl(g_lnet[i].net) };
        inet_ntop(AF_INET, &n, ip, sizeof ip);
        /* 掩码转前缀长度：255.255.255.0 → 24 */
        int bits = 0;
        for (uint32_t m = g_lnet[i].mask; m; m <<= 1) bits++;
        logi("本地网段 %s/%d（内部互访不计流量）", ip, bits);
    }
    for (int i = 0; i < g_self_n; i++) {
        char ip[INET_ADDRSTRLEN];
        struct in_addr a = { htonl(g_self_ip[i]) };
        inet_ntop(AF_INET, &a, ip, sizeof ip);
        logi("本机地址 %s（不作为设备统计）", ip);
    }
}

static int is_self_ip(const uint8_t *p)
{
    uint32_t a = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                 ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
    for (int i = 0; i < g_self_n; i++)
        if (g_self_ip[i] == a) return 1;
    return 0;
}

static int is_self_ip6(const uint8_t *p)
{
    for (int i = 0; i < g_self_v6_n; i++)
        if (!memcmp(g_self_v6[i], p, 16)) return 1;
    return 0;
}

static int in_same_local_net(const uint8_t *s, const uint8_t *d)
{
    uint32_t a = ((uint32_t)s[0] << 24) | ((uint32_t)s[1] << 16) |
                 ((uint32_t)s[2] << 8)  |  (uint32_t)s[3];
    uint32_t b = ((uint32_t)d[0] << 24) | ((uint32_t)d[1] << 16) |
                 ((uint32_t)d[2] << 8)  |  (uint32_t)d[3];
    for (int i = 0; i < g_lnet_n; i++) {
        uint32_t m = g_lnet[i].mask;
        if ((a & m) == g_lnet[i].net && (b & m) == g_lnet[i].net) return 1;
    }
    return 0;
}

static int ignore_conn(const ct_key_t *k)
{
    static const uint8_t V4MAP[12] = {0,0,0,0,0,0,0,0,0,0,0xff,0xff};

    /*
     * IPv6 只做一件事：源是本机地址就丢弃（tun 代理导致的重复计量）。
     * 网段内互访、广播、组播这些 v4 规则不套用到 v6：
     *   - 运营商给的是 /64，同网段判断等于「全世界的 v6 都算内网」
     *   - v6 没有广播，组播地址段也不同
     */
    if (k->family == 6)
        return is_self_ip6(k->src);

    if (k->family != 4) return 0;

    const uint8_t *s = k->src, *d = k->dst;
    if (memcmp(s, V4MAP, 12) == 0) s += 12;
    if (memcmp(d, V4MAP, 12) == 0) d += 12;

    /* 回环 */
    if (s[0] == 127 && d[0] == 127) return 1;

    /* 广播 */
    if (d[3] == 255) return 1;

    /* 组播 224.0.0.0/4 */
    if (d[0] >= 224 && d[0] <= 239) return 1;

    /* 同网段互访（LAN 内设备之间、容器之间）：不是上网流量 */
    if (in_same_local_net(s, d)) return 1;

    /*
     * 源是本机某个接口地址：不是「设备」在上网。
     * 典型是 tun 代理把同一份流量从 WAN 重新发出（见 g_self_ip 注释），
     * 不过滤就会让域名总量翻倍、WAN 地址冒充一台设备。
     */
    if (is_self_ip(s)) return 1;

    return 0;
}

static void on_conn_gone(const ct_key_t *k, uint64_t seen_ms,
                         uint64_t last_ms, uint64_t pkt_o, uint64_t byt_o,
                         uint64_t pkt_r, uint64_t byt_r, uint8_t preexisting,
                         void *ud)
{
    (void)ud;
    if (!g_db) return;

    /* 全零的空连接直接丢掉，别污染数据 */
    if (pkt_o == 0 && byt_o == 0 && pkt_r == 0 && byt_r == 0)
        return;

    if (ignore_conn(k)) {
        g_stat.ignored++;
        return;
    }

    /*
     * 启动前就存在的连接：起点未知，dur 不可信。
     * 这类记录直接丢弃——实测中会一次性涌入几百条 dur=0 的垃圾，
     * 把统计搞乱。它们后续的新连接会正常记录。
     */
    if (preexisting) {
        g_stat.preexisting_skip++;
        return;
    }

    /* 反查域名：拿不到就写 NULL，别硬编个占位符 */
    const char *host = g_dnsmap ? dnsmap_lookup(g_dnsmap, k->family, k->dst)
                                : NULL;

    int rc = db_add_conn(g_db, k, seen_ms, last_ms,
                         pkt_o, byt_o, pkt_r, byt_r,
                         0, k->zone, k->mark, host);
    if (rc == 0)
        g_stat.conns_written++;
}

static void on_conn_seen(const ct_key_t *k, const ct_rec_t *cur,
                         uint64_t d_pkt_o, uint64_t d_byt_o,
                         uint64_t d_pkt_r, uint64_t d_byt_r, void *ud)
{
    (void)k; (void)cur;
    (void)d_pkt_o; (void)d_byt_o; (void)d_pkt_r; (void)d_byt_r;
    (void)ud;
    /* 采样期间不做任何事——所有落库动作都在消亡回调里。
     * 这是"一条连接只写一行"的关键：活着的时候不写。 */
}

/* ---------------- DNS 日志追踪 ---------------- */

/*
 * 打开 dnsmasq 日志并 seek 到末尾。
 *
 * 为什么要 seek 到末尾：dnsmasq 的日志是持续追加的，
 * 从头读会把启动前的历史全部重放一遍，既慢又没意义。
 */
static int dns_log_open(void)
{
    if (!g_cfg.dns_log || g_cfg.dns_log[0] == '\0') return -1;

    int fd = open(g_cfg.dns_log, O_RDONLY | O_NONBLOCK);
    if (fd < 0) return -1;

    off_t end = lseek(fd, 0, SEEK_END);
    if (end < 0) { close(fd); return -1; }

    return fd;
}

/*
 * 读日志新增内容，喂给 dnsmap。
 *
 * 用非阻塞读 + 自己维护行缓冲，避免 fgets 在长行时的问题。
 * 每次最多处理 200 行，防止一次性处理太多拖慢主循环。
 */
static void dns_log_pump(void)
{
    if (g_dns_fd < 0) {
        g_dns_fd = dns_log_open();
        if (g_dns_fd < 0) return;
    }

    static char buf[65536];
    static size_t blen = 0;

    int lines = 0;
    while (lines < 200) {
        ssize_t n = read(g_dns_fd, buf + blen, sizeof(buf) - blen - 1);
        if (n <= 0) break;

        blen += (size_t)n;
        buf[blen] = '\0';

        /* 逐行处理，最后一行可能不完整，留到下次 */
        char *start = buf;
        char *nl;
        while ((nl = memchr(start, '\n', blen - (size_t)(start - buf))) != NULL) {
            *nl = '\0';
            g_stat.dns_lines++;
            if (dnsmap_feed(g_dnsmap, start, (uint64_t)time(NULL)))
                g_stat.dns_maps++;
            start = nl + 1;
            lines++;
            if (lines >= 200) break;
        }

        /* 把剩余部分挪到开头 */
        size_t left = blen - (size_t)(start - buf);
        if (left > 0 && start != buf)
            memmove(buf, start, left);
        blen = left;

        if (blen > sizeof(buf) - 2) blen = 0;   /* 防御：单行超长，丢弃 */
        if (n < (ssize_t)(sizeof(buf) - blen - 1)) break;
    }
}

/* ---------------- DNS 被动嗅探 ---------------- */

/*
 * DNS 事件回调：把嗅探到的映射喂给 dnsmap，把查询质量写进 dnsq 表。
 *
 * ⚠️ 安全：这里只做「记录」，不发包、不改包。
 *    嗅探器本身是 AF_PACKET 只读复制，DNS 处理流程完全不依赖它。
 *    本函数出错最多是少一条记录，不可能影响解析。
 */
static void on_dns_event(const dns_event_t *e, void *ud)
{
    (void)ud;
    if (!e || !e->qname[0]) return;

    uint64_t now = wall_ms();

    if (e->is_query) {
        /*
         * 查询：写 dnsq 表记录解析质量。
         * 应答到达时才算得出耗时，所以查询阶段 ms 写 NULL，
         * 由应答那条记录补上 —— 但为了避免一个名字两条记录，
         * 这里只在应答时落库（见下面）。
         */
        g_stat.dns_queries++;
        return;
    }

    /* 应答：有解析结果就更新反查表 */
    if (e->has_ip && g_dnsmap) {
        dnsmap_add(g_dnsmap, e->family, e->ip, e->qname, (uint64_t)time(NULL));
        g_stat.dns_mapped++;
    }

    /*
     * 落库：每个应答写一行 dnsq。
     * 频率约为每秒几次，对写入量无影响（相比 conntrack 的几千条/分钟）。
     *
     * 只记 A/AAAA（qtype 1/28），其他记录类型对"解析质量"没意义。
     */
    if (e->qtype != 1 && e->qtype != 28) return;

    /* 反查：拿第一个 A 记录的 IP 文本 */
    char ipbuf[64] = "";
    if (e->has_ip) {
        if (e->family == 4)
            snprintf(ipbuf, sizeof(ipbuf), "%u.%u.%u.%u",
                     e->ip[12], e->ip[13], e->ip[14], e->ip[15]);
        else {
            char t[64];
            if (inet_ntop(AF_INET6, e->ip, t, sizeof(t)))
                snprintf(ipbuf, sizeof(ipbuf), "%s", t);
        }
    }

    db_dnsq_t q;
    memset(&q, 0, sizeof(q));
    q.ts_ms   = now;
    q.qname   = e->qname;
    q.qtype   = e->qtype;
    q.rcode   = e->rcode;
    q.ms      = (e->ms >= 0.0) ? e->ms : NAN;
    q.answers = e->has_ip ? 1 : 0;
    q.first_ip = ipbuf[0] ? ipbuf : NULL;

    /*
     * 客户端 IP：DNS 报文里唯一能拿到的"是谁发的"信息。
     * client_ip 是网络字节序的 IPv4，收到 0 表示没取到（IPv6 查询或本机）。
     */
    char ipcbuf[24] = "";
    if (e->client_ip) {
        uint32_t h = ntohl(e->client_ip);
        snprintf(ipcbuf, sizeof(ipcbuf), "%u.%u.%u.%u",
                 (h >> 24) & 0xff, (h >> 16) & 0xff,
                 (h >> 8) & 0xff, h & 0xff);
    }
    q.client_ip = ipcbuf[0] ? ipcbuf : NULL;

    db_add_dnsq(g_db, &q);

    /*
     * 这里必须累加 dns_lines，否则日志里永远显示 "dns 0 行"。
     * 之前 dns_lines 只在旧的 dnsmasq 日志路径（read_dns_log）里自增，
     * 改用 AF_PACKET 嗅探后那条路径不再走，计数就一直是 0 ——
     * 而缓存里明明有几百个 IP，看着像嗅探没工作，其实是显示的数错了。
     */
    g_stat.dns_lines++;

    g_stat.dns_replies++;
}

/* 启动嗅探器。失败不致命 —— 没它只是少了域名，其他功能照常。 */
static void sniff_start(void)
{
    if (!g_cfg.dns_sniff) {
        logi("DNS 嗅探未启用（dns_sniff=0）");
        return;
    }

    char err[256] = "";
    g_sniff = dnssniff_open(g_cfg.sniff_if, err, sizeof(err));
    if (!g_sniff) {
        /*
         * 只警告，不退出。这是旁路功能，缺了不影响监控主流程，
         * 更不影响 DNS 本身。
         */
        logw("DNS 嗅探启动失败（不影响 DNS 与监控）: %s", err);
        return;
    }

    /* 把实际生效的接收缓冲区打出来：太小就是抓不全的元凶 */
    int rb = dnssniff_rcvbuf(g_sniff);
    logi("DNS 嗅探已启动: 监听 %s（AF_PACKET 只读旁路，不参与转发）"
           " 接收缓冲=%dKB BPF过滤=%s",
           g_cfg.sniff_if ? g_cfg.sniff_if : "全部网卡", rb / 1024,
           dnssniff_filter_ok(g_sniff) ? "已启用(仅udp/53)"
                                       : "未启用(退化为全量接收)");
}

static void sniff_stop(void)
{
    if (g_sniff) {
        dnssniff_close(g_sniff);
        g_sniff = NULL;
    }
}

/* ---------------- 主动探测 ---------------- */

static void do_rollup_and_gc(void)
{
    time_t now = time(NULL);

    /* 每小时滚一次聚合 */
    if (now - g_last_rollup >= 3600) {
        uint64_t nowms = wall_ms();
        uint64_t from = nowms - 2ULL * 3600000ULL;   /* 滚最近两小时，覆盖边界 */
        long n = db_rollup_hourly(g_db, from, nowms);
        if (n >= 0)
            logi("rollup: %ld 行", n);
        g_last_rollup = now;
    }

    /* 每天检查一次 GC，避免频繁执行 */
    if (now - g_last_gc >= 86400) {
        uint64_t cutoff = wall_ms() - (uint64_t)g_cfg.retain_days * 86400000ULL;
        long n = db_gc(g_db, cutoff);
        if (n >= 0)
            logi("gc: 删除 %ld 行（保留 %d 天）", n, g_cfg.retain_days);
        else
            loge("gc 失败");
        g_last_gc = now;
    }
}

static void print_stats(void)
{
    db_stats_t ds;
    db_get_stats(g_db, &ds);
    dnsmap_stats_t dm;
    dnsmap_get_stats(g_dnsmap, &dm);

    double avg_us = g_stat.samples ?
        (double)g_stat.total_sample_us / g_stat.samples : 0;

    /*
     * DNS 嗅探的查询/应答/待配对计数必须打出来。
     * 之前只看 "dns N 行"，看不出应答率为什么只有一半 ——
     * 分不清是"查询没抓到"还是"应答没配上"。
     * 现在一眼能看出：q 和 r 差多少、有多少查询悬着没配上。
     */
    char dnsbuf[128] = "dns -";
    if (g_sniff) {
        dnssniff_stats_t st;
        dnssniff_get_stats(g_sniff, &st);
        double pair = st.dns_replies ?
            (double)(st.dns_replies - st.pending_unpaired) / st.dns_replies * 100.0 : 0.0;
        snprintf(dnsbuf, sizeof(dnsbuf),
                 "dns q=%llu r=%llu 未配对=%llu(%.0f%%) 待处理=%llu 内核丢帧=%llu",
                 (unsigned long long)st.dns_queries,
                 (unsigned long long)st.dns_replies,
                 (unsigned long long)st.pending_unpaired, pair,
                 (unsigned long long)st.pending,
                 (unsigned long long)st.kernel_drops);
    }

    logd("stat: 采样 %lu 次(均 %.0fus) 连接 %lu 条 | "
           "明细 %lld 行 聚合 %lld 行 库 %.1fMB wal %.1fMB | "
           "dns %lu 行/%lu 映射 缓存 %zu IP | %s | 提交 %ld 次",
           g_stat.samples, avg_us, g_stat.conns_written,
           ds.rows_conns, ds.rows_agg,
           ds.db_bytes / 1048576.0, ds.wal_bytes / 1048576.0,
           g_stat.dns_lines, g_stat.dns_mapped + g_stat.dns_maps,
           dm.ips, dnsbuf, ds.commits);
}

/* ---------------- 主循环 ---------------- */

static int parse_args(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--db") == 0) {
            if (++i < argc) g_cfg.db_path = argv[i];
        } else if (strcmp(argv[i], "-i") == 0 || strcmp(argv[i], "--interval") == 0) {
            if (++i < argc) g_cfg.sample_ms = atoi(argv[i]);
        } else if (strcmp(argv[i], "-r") == 0 || strcmp(argv[i], "--retain") == 0) {
            if (++i < argc) g_cfg.retain_days = atoi(argv[i]);
        } else if (strcmp(argv[i], "--dns-log") == 0) {
            if (++i < argc) g_cfg.dns_log = argv[i];
        } else if (strcmp(argv[i], "--sniff-if") == 0) {
            /*
             * 嗅探哪个网卡。传 "any" 表示不 bind —— 收所有网卡的帧。
             * 排查"某些查询抓不到"时用：如果 any 模式能抓到而 br-lan 抓不到，
             * 说明那些报文走了别的接口（lo / singtun0 等）。
             */
            if (++i < argc)
                g_cfg.sniff_if = (!strcmp(argv[i], "any") || !argv[i][0])
                                 ? NULL : argv[i];
        } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            g_cfg.verbose = 1;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printf(
                "netmon - 网络监控\n"
                "用法: netmon [选项]\n"
                "  -d, --db <路径>        数据库路径 (默认 %s)\n"
                "  -i, --interval <ms>    采样间隔 (默认 5000)\n"
                "  -r, --retain <天>      明细保留天数 (默认 30)\n"
                "      --dns-log <路径>   dnsmasq 日志 (默认 %s)\n"
                "      --sniff-if <网卡>  DNS 嗅探接口 (默认 br-lan，any=全部)\n"
                "  -v, --verbose          输出更多信息\n",
                g_cfg.db_path, g_cfg.dns_log);
            exit(0);
        } else {
            fprintf(stderr, "未知参数: %s\n", argv[i]);
            return -1;
        }
    }
    return 0;
}

/* 递归创建目录（mkdir -p 的等价实现） */
static int mkdir_p(const char *path, mode_t mode)
{
    char tmp[512];
    size_t len = strlen(path);
    if (len == 0 || len >= sizeof(tmp)) return -1;

    snprintf(tmp, sizeof(tmp), "%s", path);

    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        if (mkdir(tmp, mode) != 0 && errno != EEXIST) return -1;
        *p = '/';
    }
    if (mkdir(tmp, mode) != 0 && errno != EEXIST) return -1;
    return 0;
}

int main(int argc, char **argv)
{
    if (parse_args(argc, argv) != 0) return 1;

    /* 前台手跑时 stderr 是终端，日志同时打到屏幕；被 procd 接管后不是终端，
     * 就只走 syslog，避免同一条日志被记两遍（procd 会把 stderr 也收进 logd）。 */
    g_stderr_tty = isatty(STDERR_FILENO);
    openlog("netmon", LOG_PID, LOG_DAEMON);

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    /* 确保数据库所在目录存在 */
    {
        char dir[512];
        snprintf(dir, sizeof(dir), "%s", g_cfg.db_path);
        char *slash = strrchr(dir, '/');
        if (slash && slash != dir) {
            *slash = '\0';
            if (mkdir_p(dir, 0755) != 0) {
                loge("无法创建目录 %s: %s", dir, strerror(errno));
                return 1;
            }
        }
    }

    /* 本地网段表：同网段互访不记流量。必须在开始采样前准备好。 */
    load_local_nets();

    /* 打开数据库 */
    char err[512];
    g_db = db_open(g_cfg.db_path, g_cfg.cache_kb, g_cfg.mmap_kb,
                   err, sizeof(err));
    if (!g_db) {
        loge("数据库打开失败: %s", err);
        return 1;
    }
    if (db_init_schema(g_db, err, sizeof(err)) != 0) {
        loge("建表失败: %s", err);
        db_close(g_db);
        return 1;
    }

    /* DNS 映射表 */
    g_dnsmap = dnsmap_new(131072);
    if (!g_dnsmap) {
        loge("dnsmap 分配失败");
        db_close(g_db);
        return 1;
    }

    /* DNS 被动嗅探：只读旁路，失败不影响任何东西 */
    sniff_start();

    /*
     * homeproxy 规则同步。首次会 fork 一次 sing-box rule-set decompile，
     * 把当前 cache.db 里的 AdsAll 二进制规则集反编译成明文。
     * 之后每 30 秒检查指纹，配置或规则集一变就自动重建。
     */
    if (hprules_init("/var/run/homeproxy/cache.db",
                     "/etc/config/homeproxy", "/opt/netmon/data") == 0 &&
        hprules_available()) {
        const hp_version_t *v = hprules_version();
        logi("homeproxy 规则已同步: %s", v->detail);
        db_hpevent_t e;
        memset(&e, 0, sizeof(e));
        e.ts_ms = wall_ms(); e.fp = v->fp; e.detail = v->detail;
        e.rules = v->rules;  e.patterns = v->patterns;
        db_add_hpevent(g_db, &e);
    } else {
        logw("%s（可选依赖，采集与其它功能不受影响）", hprules_reason());
    }

    logi("netmon 启动: db=%s 采样=%dms 保留=%d天",
           g_cfg.db_path, g_cfg.sample_ms, g_cfg.retain_days);

    uint64_t next_sample = mono_ms();
    uint64_t next_stats  = mono_ms() + 60000;
    uint64_t next_hpchk  = 0;      /* 立刻检查一次规则指纹 */

    while (!g_stop) {
        uint64_t now = mono_ms();

        /* ---- 采样 ---- */
        if (now >= next_sample) {
            uint64_t t0 = mono_ms();
            cts_stats_t st;
            cts_sample(wall_ms(), on_conn_seen, on_conn_gone, NULL, &st);

            uint64_t cost = mono_ms() - t0;
            g_stat.samples++;
            g_stat.total_sample_us += cost;
            g_stat.last_sample_us = cost;
            g_stat.parse_fail += st.unknown;

            if (g_cfg.verbose)
                logd("sample: %zu 行 %zu 存活 %zu 消亡 cost=%ums",
                       st.parsed, st.alive, st.gone, (unsigned)cost);

            next_sample = now + (uint64_t)g_cfg.sample_ms;
        }

        /* ---- DNS 日志（旧方案） ---- */
        dns_log_pump();

        /* ---- DNS 被动嗅探 ---- */
        if (g_sniff) {
            if (dnssniff_pump(g_sniff, on_dns_event, NULL) < 0) {
                /* 致命错误（比如 fd 被关）：停掉，不重试，避免刷日志 */
                logw("DNS 嗅探异常退出，后续不再反查域名");
                sniff_stop();
            }
        }

        /* ---- 刷盘 ---- */
        if (db_should_flush(g_db, now)) {
            long n = db_flush(g_db);
            if (n < 0) loge("刷盘失败");
            else if (g_cfg.verbose)
                logd("flush: %ld 行", n);
        }

        /* ---- homeproxy 规则指纹（配置 / 规则集变化自动跟进） ---- */
        if (now >= next_hpchk) {
            int changed = 0;
            if (hprules_refresh(&changed) == 0 && changed) {
                const hp_version_t *v = hprules_version();
                db_hpevent_t e;
                memset(&e, 0, sizeof(e));
                e.ts_ms = wall_ms(); e.fp = v->fp; e.detail = v->detail;
                e.rules = v->rules;  e.patterns = v->patterns;
                db_add_hpevent(g_db, &e);
                logi("homeproxy 规则变更: %s", v->detail);
            }
            next_hpchk = now + 30000;
        }

        /* ---- 统计输出 ---- */
        if (now >= next_stats) {
            print_stats();
            next_stats = now + 60000;
        }

        /* ---- 聚合与 GC ---- */
        do_rollup_and_gc();

        usleep(200 * 1000);   /* 200ms 一轮，足够响应，又不占 CPU */
    }

    /* ---- 退出清理 ---- */
    logi("收到退出信号，清理中…");
    long flushed = db_flush(g_db);
    logi("最后刷盘 %ld 行", flushed);
    print_stats();

    if (g_dns_fd >= 0) close(g_dns_fd);
    sniff_stop();
    dnsmap_free(g_dnsmap);
    db_close(g_db);

    logi("已退出");
    closelog();
    return 0;
}
