/*
 * probe.c - 主动探测实现
 *
 * 全部用非阻塞 socket + poll，靠 deadline 统一控制超时，
 * 避免 SO_RCVTIMEO 在多阶段握手（connect/TLS/TTFB）里算不清账。
 */

#define _GNU_SOURCE
#include "probe.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <time.h>
#include <math.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <netinet/ip_icmp.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

/* ---------------- 时间 ---------------- */

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

/* ---------------- 绕过代理 ---------------- */

static uint32_t g_bypass_mark = 100;   /* homeproxy 默认放行标记 */
static char     g_bypass_if[64] = "";  /* 绑定源地址，如 "203.0.113.10" */

void probe_set_bypass_mark(uint32_t mark) { g_bypass_mark = mark; }
uint32_t probe_get_bypass_mark(void) { return g_bypass_mark; }

void probe_set_bypass_iface(const char *ip_or_dev)
{
    if (!ip_or_dev) { g_bypass_if[0] = '\0'; return; }
    snprintf(g_bypass_if, sizeof(g_bypass_if), "%s", ip_or_dev);
}

const char *probe_get_bypass_iface(void) { return g_bypass_if; }

/*
 * 给 socket 打绕过标记。失败不致命（比如非 root 或内核不支持），
 * 但要能感知——调用方可以通过探测结果是否异常来判断。
 */
static void apply_mark(int fd)
{
    if (g_bypass_mark == 0 || fd < 0) return;
#ifdef SO_MARK
    uint32_t m = g_bypass_mark;
    setsockopt(fd, SOL_SOCKET, SO_MARK, &m, sizeof(m));
#else
    (void)fd;
#endif
}

/* ---------------- ICMP 校验和 ---------------- */

static uint16_t icmp_csum(const void *data, int len)
{
    const uint16_t *p = (const uint16_t *)data;
    uint32_t sum = 0;
    while (len > 1) { sum += *p++; len -= 2; }
    if (len == 1) sum += *(const uint8_t *)p;
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    return (uint16_t)~sum;
}

/* ---------------- Ping ---------------- */

int probe_ping(const char *target, int count, int timeout_ms, ping_result_t *out)
{
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    if (count <= 0) count = 3;

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;

    /* 只接受 IP 字面量：这里不解析域名，是为了把"线路质量"和"DNS质量"解耦 */
    if (inet_pton(AF_INET, target, &dst.sin_addr) != 1) {
        out->err = EINVAL;
        return -1;
    }

    int fd = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (fd < 0) {
        out->err = errno;      /* 通常是 EPERM，非 root 跑不了 */
        return -1;
    }
    apply_mark(fd);
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);

    uint16_t seq = 1;
    /* pid 混进 ICMP id，避免和系统里别的 ping 抢应答 */
    uint16_t ident = (uint16_t)(getpid() & 0xffff);

    double rtt[64];
    int got = 0;
    int sent = 0;

    double t_start_all = now_ms();

    for (int i = 0; i < count && i < 64; i++) {
        /* 构造 ICMP Echo Request */
        struct {
            struct icmphdr h;
            uint8_t payload[32];
        } pkt;

        memset(&pkt, 0, sizeof(pkt));
        pkt.h.type = ICMP_ECHO;
        pkt.h.code = 0;
        pkt.h.un.echo.id = htons(ident);
        pkt.h.un.echo.sequence = htons(seq);
        memcpy(pkt.payload, "netmon-probe", 13);
        pkt.h.checksum = 0;
        pkt.h.checksum = icmp_csum(&pkt, sizeof(pkt));

        double t_send = now_ms();
        ssize_t sn = sendto(fd, &pkt, sizeof(pkt), 0,
                            (struct sockaddr *)&dst, sizeof(dst));
        if (sn < 0) {
            /* 个别包发不出去不致命，继续下一个 */
            seq++;
            continue;
        }
        sent++;

        /* 等应答，直到这个包的 deadline */
        double deadline = t_send + timeout_ms;
        int replied = 0;

        while (!replied) {
            double left = deadline - now_ms();
            if (left <= 0) break;

            struct pollfd pfd = { .fd = fd, .events = POLLIN };
            int pr = poll(&pfd, 1, (int)left);
            if (pr <= 0) break;

            uint8_t buf[1500];
            struct sockaddr_in from;
            socklen_t fl = sizeof(from);
            ssize_t rn = recvfrom(fd, buf, sizeof(buf), 0,
                                  (struct sockaddr *)&from, &fl);
            if (rn < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
                break;
            }

            /* 解析 IP 头 + ICMP 头 */
            if (rn < (ssize_t)(sizeof(struct iphdr) + sizeof(struct icmphdr)))
                continue;

            struct iphdr *ip = (struct iphdr *)buf;
            int hl = ip->ihl * 4;
            if (hl < 20 || rn < hl + (ssize_t)sizeof(struct icmphdr)) continue;
            if (ip->protocol != IPPROTO_ICMP) continue;
            if (from.sin_addr.s_addr != dst.sin_addr.s_addr) continue;

            struct icmphdr *ic = (struct icmphdr *)(buf + hl);
            if (ic->type != ICMP_ECHOREPLY) continue;
            if (ntohs(ic->un.echo.id) != ident) continue;
            if (ntohs(ic->un.echo.sequence) != seq) continue;

            rtt[got++] = now_ms() - t_send;
            replied = 1;
        }

        seq++;

        /* 包间隔：让出 CPU，但不影响统计 */
        if (i + 1 < count) {
            double left = 200.0 - (now_ms() - t_send);
            if (left > 0) usleep((useconds_t)(left * 1000));
        }
    }

    close(fd);

    /* 收尾：最后一个包的等待时间不该算进总时长 */
    (void)t_start_all;

    out->sent = sent;
    out->ok = got;
    out->loss = sent > 0 ? (double)(sent - got) / (double)sent : 1.0;

    if (got > 0) {
        double sum = 0, mn = 1e9, mx = 0;
        for (int i = 0; i < got; i++) {
            sum += rtt[i];
            if (rtt[i] < mn) mn = rtt[i];
            if (rtt[i] > mx) mx = rtt[i];
        }
        out->rtt_min = mn;
        out->rtt_max = mx;
        out->rtt_avg = sum / got;

        if (got > 1) {
            double jsum = 0;
            for (int i = 1; i < got; i++)
                jsum += fabs(rtt[i] - rtt[i - 1]);
            out->jitter = jsum / (got - 1);
        }
    }
    return 0;
}

/*
 * TCP 连接探测。
 *
 * 对禁用 ICMP 的主机（很多 DNS、CDN 都禁）这是唯一可靠的线路质量指标。
 * 三次握手完成的耗时 ≈ ICMP RTT + 服务端 SYN-ACK 处理时间，
 * 在正常主机上两者差异可以忽略，所以数值可以直接和 ping 结果并列比较。
 */
int probe_tcp(const char *target, int port, int count, int timeout_ms,
              ping_result_t *out)
{
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    if (count <= 0) count = 3;
    if (port <= 0) port = 443;

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = htons((uint16_t)port);

    if (inet_pton(AF_INET, target, &dst.sin_addr) != 1) {
        out->err = EINVAL;
        return -1;
    }

    double rtt[64];
    int got = 0, sent = 0;

    for (int i = 0; i < count && i < 64; i++) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) { out->err = errno; break; }
        apply_mark(fd);

        /* 设小一点，非阻塞 connect 才能精确计时 */
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);

        double t0 = now_ms();
        int rc = connect(fd, (struct sockaddr *)&dst, sizeof(dst));

        if (rc == 0) {
            rtt[got++] = now_ms() - t0;
            sent++;
            close(fd);
        } else if (errno == EINPROGRESS) {
            sent++;
            struct pollfd pfd = { .fd = fd, .events = POLLOUT };
            int pr = poll(&pfd, 1, timeout_ms);

            if (pr > 0) {
                int soerr = 0;
                socklen_t len = sizeof(soerr);
                if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &len) == 0
                    && soerr == 0) {
                    rtt[got++] = now_ms() - t0;
                } else {
                    out->err = soerr;
                }
            } else {
                out->err = (pr == 0) ? ETIMEDOUT : errno;
            }
            close(fd);
        } else {
            out->err = errno;
            close(fd);
        }

        if (i + 1 < count) usleep(100 * 1000);
    }

    out->sent = sent;
    out->ok = got;
    out->loss = sent > 0 ? (double)(sent - got) / (double)sent : 1.0;

    if (got > 0) {
        double sum = 0, mn = 1e9, mx = 0;
        for (int i = 0; i < got; i++) {
            sum += rtt[i];
            if (rtt[i] < mn) mn = rtt[i];
            if (rtt[i] > mx) mx = rtt[i];
        }
        out->rtt_min = mn;
        out->rtt_max = mx;
        out->rtt_avg = sum / got;
        if (got > 1) {
            double jsum = 0;
            for (int i = 1; i < got; i++)
                jsum += fabs(rtt[i] - rtt[i - 1]);
            out->jitter = jsum / (got - 1);
        }
    }
    return 0;
}

/*
 * 站点拨测 / 带宽测速。
 *
 * 为什么用外部 curl 而不是自己实现：
 *   1) 自己实现要处理 TLS 握手、HTTP/1.1 chunked、重定向、HTTP/2，
 *      而路由器上只有 libssl.so.3 没有头文件，自建 TLS 客户端不现实。
 *   2) curl 是系统自带的，证书链、SNI、ALPN 都是现成且经过验证的。
 *   3) 探测频率低（几分钟一次），一个进程约 10ms 启动开销可忽略。
 *   4) curl 的 -w 直接给出 DNS/TCP/TLS/TTFB 各阶段耗时，
 *      比自己拆解更准，还省掉一堆计时代码。
 *
 * max_bytes > 0 时用 --max-filesize 和 -r 限制，避免拉大文件。
 */
static int http_run(const char *url, int timeout_ms, long max_bytes,
                    const char *proxy, int insecure, http_result_t *out)
{
    if (!out || !url) return -1;
    memset(out, 0, sizeof(*out));
    out->tls_ms = -1;

    /* 防御：url 里不能有 shell 元字符，因为要过 sh -c */
    if (strpbrk(url, ";&|`$<>()'\"\\\n") != NULL) {
        out->err = EINVAL;
        return -1;
    }

    int secs = (timeout_ms + 999) / 1000;
    if (secs < 1) secs = 1;

    /*
     * -o /dev/null   不存正文
     * -s -S          静默但保留错误信息
     * --max-time     总超时
     * -w             结构化输出，用 | 分隔便于 sscanf
     * --range 0-N    只取前 N 字节（带宽测速时限制体积）
     * --interface    绑定 WAN 源地址，绕过 TUN 劫持（关键）
     */
    /*
     * 两条互斥路径：
     *   直连 -> --interface 绑定 WAN 源地址，绕开 homeproxy TUN
     *   代理 -> -x 指向 mixed_port，且绝不能加 --interface
     * 走代理时额外 -k：MITM 证书 / IP 证书不匹配不代表目标不可达。
     */
    char pathopt[160];
    if (proxy && proxy[0]) {
        if (strpbrk(proxy, ";&|`$<>()'\"\\\n") != NULL) {
            out->err = EINVAL;
            return -1;
        }
        snprintf(pathopt, sizeof(pathopt), "-x '%s' ", proxy);
    } else if (g_bypass_if[0]) {
        snprintf(pathopt, sizeof(pathopt), "--interface '%s' ", g_bypass_if);
    } else {
        pathopt[0] = '\0';
    }
    const char *kopt = insecure ? "-k " : "";

    char cmd[1400];
    if (max_bytes > 0) {
        snprintf(cmd, sizeof(cmd),
            "curl -s -S -o /dev/null --max-time %d %s%s--range 0-%ld "
            "-w '%%{http_code}|%%{time_namelookup}|%%{time_connect}|"
            "%%{time_appconnect}|%%{time_starttransfer}|%%{time_total}|"
            "%%{size_download}|%%{speed_download}' '%s' 2>/dev/null",
            secs, pathopt, kopt, max_bytes - 1, url);
    } else {
        snprintf(cmd, sizeof(cmd),
            "curl -s -S -o /dev/null --max-time %d %s%s"
            "-w '%%{http_code}|%%{time_namelookup}|%%{time_connect}|"
            "%%{time_appconnect}|%%{time_starttransfer}|%%{time_total}|"
            "%%{size_download}|%%{speed_download}' '%s' 2>/dev/null",
            secs, pathopt, kopt, url);
    }

    FILE *fp = popen(cmd, "r");
    if (!fp) { out->err = errno; return -1; }

    char line[512];
    size_t n = fread(line, 1, sizeof(line) - 1, fp);
    line[n] = '\0';
    int rc = pclose(fp);

    int code = 0;
    double dns_s = 0, conn_s = 0, tls_s = 0, ttfb_s = 0, total_s = 0;
    double size_dl = 0, speed = 0;

    int fields = sscanf(line, "%d|%lf|%lf|%lf|%lf|%lf|%lf|%lf",
                        &code, &dns_s, &conn_s, &tls_s, &ttfb_s,
                        &total_s, &size_dl, &speed);

    if (fields < 6) {
        /* curl 失败时 -w 也会输出，但内容可能是 0|0|0...，这里以 rc 判断 */
        out->err = (rc == 0) ? 0 : EIO;
        out->code = code;
        return (rc == 0) ? 0 : -1;
    }

    out->code       = code;
    out->connect_ms = conn_s * 1000.0;
    out->tls_ms     = (tls_s > 0) ? (tls_s - conn_s) * 1000.0 : 0.0;
    out->ttfb_ms    = ttfb_s * 1000.0;
    out->total_ms   = total_s * 1000.0;
    out->bytes      = (long long)size_dl;

    /* speed_download 单位是字节/秒，换成千比特/秒 */
    out->kbps = speed * 8.0 / 1000.0;

    out->ok = (code >= 200 && code < 400) ? 1 : 0;
    if (rc != 0) out->err = EIO;

    return 0;
}

int probe_http(const char *url, int timeout_ms, long max_bytes,
               http_result_t *out)
{
    return http_run(url, timeout_ms, max_bytes, NULL, 0, out);
}

int probe_http_proxy(const char *url, const char *proxy,
                     int timeout_ms, long max_bytes, http_result_t *out)
{
    return http_run(url, timeout_ms, max_bytes, proxy, 1, out);
}

/* ---------------- DNS ---------------- */

int probe_dns(const char *qname, uint32_t timeout_ms, dns_result_t *out)
{
    if (!out || !qname) return -1;
    memset(out, 0, sizeof(*out));
    out->first_ip[0] = '\0';

    /*
     * getaddrinfo 没有超时参数。为了不让探测卡死，
     * 用 alarm + 信号不可重入太脏；这里选择直接调用，
     * 由调用方保证在子进程/线程里跑，或者接受最坏情况。
     *
     * 实际上 musl 的 getaddrinfo 有内部超时（默认 5 秒 × 尝试次数），
     * 不会无限阻塞，所以直接调用是可接受的。
     */
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    double t0 = now_ms();
    int rc = getaddrinfo(qname, NULL, &hints, &res);
    out->ms = now_ms() - t0;

    if (rc != 0) {
        out->ok = 0;
        out->err = rc;
        /* EAI_NONAME 近似当作 NXDOMAIN */
        out->rcode = (rc == EAI_NONAME) ? 3 : 2;
        return 0;
    }

    out->ok = 1;
    out->rcode = 0;

    int n = 0;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        if (ai->ai_family == AF_INET || ai->ai_family == AF_INET6) {
            if (n == 0) {
                void *src;
                if (ai->ai_family == AF_INET)
                    src = &((struct sockaddr_in *)ai->ai_addr)->sin_addr;
                else
                    src = &((struct sockaddr_in6 *)ai->ai_addr)->sin6_addr;
                inet_ntop(ai->ai_family, src, out->first_ip, sizeof(out->first_ip));
            }
            n++;
        }
    }
    out->answers = n;
    freeaddrinfo(res);

    (void)timeout_ms;
    return 0;
}

/* ---------------- 批量 ---------------- */

/*
 * 批量探测。
 *
 * 自动降级逻辑：ICMP 全丢时改用 TCP:443 再测一次，
 * 因为大量主机禁 ping（实测移动 DNS、百度都禁），
 * 只看 ICMP 会把"服务正常但禁 ping"误判成"线路差"。
 * 降级后如果 TCP 通，说明线路没问题，用 TCP 的 RTT 做指标。
 */
int probe_run_group(const probe_target_t *targets, size_t n,
                    int count, int timeout_ms,
                    probe_report_fn cb, void *ud)
{
    if (!targets || n == 0) return 0;

    int done = 0;
    for (size_t i = 0; i < n; i++) {
        ping_result_t r;
        if (probe_ping(targets[i].host, count, timeout_ms, &r) != 0)
            continue;

        /* ICMP 全丢 -> 试 TCP，避免误判 */
        if (r.ok == 0) {
            ping_result_t t;
            if (probe_tcp(targets[i].host, 443, count, timeout_ms, &t) == 0
                && t.ok > 0) {
                /* TCP 通了，说明主机活着，只是禁 ping */
                r = t;
            }
        }

        done++;
        if (cb) cb(&targets[i], &r, ud);
    }
    return done;
}
