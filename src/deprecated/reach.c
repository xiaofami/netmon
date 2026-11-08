/*
 * reach.c - 代理可达性探测实现
 *
 * 候选来自两处观测数据：
 *   1) dnsq 里 rcode != 0 的查询，按 qname 聚合；
 *   2) conns 里 byt_r = 0（发出去了但一个字节都没回来）的连接。
 * 两者都要先过 hprules_classify()，只留 HP_UNRESOLVED —— 也就是
 * 排除「广告/规则拦截」和「.lan 本地搜索域」之后仍然失败的。
 *
 * 为什么用「拿到 HTTP 状态码」而不是 2xx 判定可达：
 *   403/404/502 同样说明 TCP+TLS+HTTP 整条链路通了，只是服务端拒绝或
 *   路径不对。把它判成「不可达」会凭空制造一堆假的加规则建议。
 */
#include "reach.h"
#include "hprules.h"
#include "probe.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <time.h>

#define MAX_CAND 512
#define COOLDOWN_MS (24ULL * 3600 * 1000)

typedef struct {
    char host[256];
    long long attempts;
    long long last_ms;
    char reason[16];
} cand_t;

static cand_t g_c[MAX_CAND];
static int    g_nc;

static uint64_t wall_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000);
}

/* 只允许能安全放进 URL 的字符。候选来自数据库，仍然要挡一层。 */
static int host_ok(const char *h)
{
    if (!h || !*h) return 0;
    size_t n = strlen(h);
    if (n < 3 || n >= 200) return 0;
    if (!isalnum((unsigned char)h[0])) return 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)h[i];
        if (!(isalnum(c) || c == '.' || c == '-' || c == ':')) return 0;
    }
    /* 内网 / 本机地址不测：代理对它们没有意义，且几乎必然失败 */
    static const char *skip[] = {
        "10.", "192.168.", "127.", "172.16.", "172.17.", "172.18.",
        "172.19.", "172.2", "172.30.", "172.31.", "169.254.",
        "0.0.0.0", "255.255.255.255", NULL
    };
    for (int i = 0; skip[i]; i++)
        if (!strncmp(h, skip[i], strlen(skip[i]))) return 0;
    return 1;
}

static void cand_add(const char *h, long long n, const char *reason)
{
    if (!host_ok(h)) return;
    char rule[64];
    if (hprules_is_local(h)) return;
    if (hprules_classify(h, 3, rule, sizeof(rule)) != HP_UNRESOLVED) return;

    for (int i = 0; i < g_nc; i++) {
        if (!strcmp(g_c[i].host, h)) {
            g_c[i].attempts += n;
            return;
        }
    }
    if (g_nc >= MAX_CAND) return;
    cand_t *c = &g_c[g_nc++];
    memset(c, 0, sizeof(*c));
    snprintf(c->host, sizeof(c->host), "%s", h);
    snprintf(c->reason, sizeof(c->reason), "%s", reason);
    c->attempts = n;
}

static int dns_cb(void *ud, int argc, char **av, char **col)
{
    (void)ud; (void)col;
    if (argc < 3 || !av[0] || !av[1] || !av[2]) return 0;
    char rule[64];
    int rcode = atoi(av[2]);
    /* 归因在这里完成：拦截 / 本地域直接跳过，不进探测队列 */
    if (hprules_classify(av[0], rcode, rule, sizeof(rule)) != HP_UNRESOLVED)
        return 0;
    cand_add(av[0], atoll(av[1]), "dns_fail");
    return 0;
}

static int conn_cb(void *ud, int argc, char **av, char **col)
{
    (void)ud; (void)col;
    if (argc < 2 || !av[0] || !av[1]) return 0;
    long long n = atoll(av[1]);
    if (n < 2) return 0;                 /* 单发一次没回包很常见，不算故障 */
    cand_add(av[0], n / 4 + 1, "no_reply");
    return 0;
}

static int cool_cb(void *ud, int argc, char **av, char **col)
{
    (void)ud; (void)col;
    if (argc < 2 || !av[0] || !av[1]) return 0;
    long long t = atoll(av[1]);
    for (int i = 0; i < g_nc; i++)
        if (!strcmp(g_c[i].host, av[0]) && t > g_c[i].last_ms)
            g_c[i].last_ms = t;
    return 0;
}

static void sort_cands(void)
{
    for (int i = 1; i < g_nc; i++) {
        cand_t key = g_c[i];
        int j = i - 1;
        while (j >= 0 && g_c[j].attempts < key.attempts) {
            g_c[j + 1] = g_c[j];
            j--;
        }
        g_c[j + 1] = key;
    }
}

/*
 * 只有 2xx~4xx 才算「通了」。
 * 5xx 尤其是 502/503/504 是 sing-box 返回的网关错误页，
 * 说明代理自己也连不上目标；若把它算成可达，就会凭空生成
 * 一条「建议加代理规则」的错误结论。
 */
static int reachable(int code)
{
    return code >= 200 && code < 500;
}

static int is_ip_literal(const char *h)
{
    for (const char *p = h; *p; p++)
        if (!(isdigit((unsigned char)*p) || *p == '.' || *p == ':' ||
              (*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F')))
            return 0;
    return 1;
}

int reach_run_round(db_t *db, const char *proxy, int limit,
                    uint64_t since_ms, reach_log_fn logfn)
{
    if (!db || !proxy || !*proxy || limit <= 0) return 0;
    uint64_t now = wall_ms();
    uint64_t from = now - since_ms;
    g_nc = 0;

    char sql[1024];
    snprintf(sql, sizeof(sql),
        "SELECT qname, COUNT(*), MAX(rcode) FROM dnsq"
        " WHERE ts_ms >= %llu AND rcode IN (2,3,5)"
        " GROUP BY qname;", (unsigned long long)from);
    db_foreach(db, sql, dns_cb, NULL);

    snprintf(sql, sizeof(sql),
        "SELECT COALESCE(host,dst), COUNT(*) FROM conns"
        " WHERE first_ms >= %llu AND byt_r = 0"
        " GROUP BY 1;", (unsigned long long)from);
    db_foreach(db, sql, conn_cb, NULL);

    /* 冷却：同一个目标 24 小时内不重复测，避免持续消耗代理流量 */
    db_foreach(db, "SELECT host, MAX(ts_ms) FROM reach GROUP BY host;",
               cool_cb, NULL);

    sort_cands();

    int done = 0;
    for (int i = 0; i < g_nc && done < limit; i++) {
        cand_t *c = &g_c[i];
        if (c->last_ms && now - (uint64_t)c->last_ms < COOLDOWN_MS) continue;

        char url[512];
        snprintf(url, sizeof(url), "%s://%.200s/",
                 is_ip_literal(c->host) ? "http" : "https", c->host);

        http_result_t d, p;
        memset(&d, 0, sizeof(d));
        memset(&p, 0, sizeof(p));
        /* 两条路径都用 probe_http_proxy：
           proxy=NULL -> 直连，自动加 --interface 绕开 TUN 且 -k；
           proxy=URL  -> 走 mixed_port，绝不加 --interface。 */
        probe_http_proxy(url, NULL,  6000, 1, &d);
        probe_http_proxy(url, proxy, 10000, 1, &p);

        int dok = reachable(d.code);
        int pok = reachable(p.code);
        const char *verdict, *note;
        if (!dok && pok) {
            verdict = "need_proxy";
            note = "直连不可达、代理可达：建议在 homeproxy 的 Proxy 规则中加入该域名";
        } else if (dok && pok) {
            verdict = "both_ok";
            note = "两条路径都可达，暂不需要加规则";
        } else if (dok && !pok) {
            verdict = "direct_only";
            note = "仅直连可达，走代理反而失败";
        } else {
            verdict = "both_fail";
            note = "两条路径都不可达：可能站点下线、只允许特定客户端，或不是 HTTP 服务";
        }

        db_reach_t r;
        memset(&r, 0, sizeof(r));
        r.ts_ms      = now;
        r.host       = c->host;
        r.reason     = c->reason;
        r.attempts   = c->attempts;
        r.direct_code = d.code;
        r.direct_ms   = d.total_ms;
        r.direct_ok   = dok;
        r.proxy_code  = p.code;
        r.proxy_ms    = p.total_ms;
        r.proxy_ok    = pok;
        r.verdict     = verdict;
        r.note        = note;
        db_add_reach(db, &r);

        if (logfn)
            logfn("reach %-42s direct=%d(%.0fms) proxy=%d(%.0fms) -> %s",
                  c->host, d.code, d.total_ms, p.code, p.total_ms, verdict);
        done++;
    }
    return done;
}
