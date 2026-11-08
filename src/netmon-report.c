/*
 * netmon-report.c - netmon 数据查询/报表工具
 *
 * 为什么单独做一个二进制而不是塞进 netmon：
 *   1) 采集进程必须极致精简，任何 report 代码都不该出现在它的热路径上；
 *   2) 查询可能很慢（全表扫 30 天明细），不能影响采样；
 *   3) 只读打开（immutable=1 不够安全，因为 WAL 里可能还有未 checkpoint 的数据，
 *      所以用普通只读 + busy_timeout，让它能读到 WAL）。
 *
 * 子命令：
 *   top       流量 Top N（按目的网段/域名/端口）
 *   trend     时间趋势（按小时/天，看流量和连接数曲线）
 *   probe     探测质量（延迟、丢包、带宽，按时段统计）
 *   dns       DNS 解析质量
 *   devices   按设备看 DNS 行为
 *   client    单台设备完整详情（前端点开设备时调用）
 *   status    数据库概况
 *   prune     清理指定目标的历史探测记录
 *   dashboard 前端用的完整 JSON（含设备、归因、规则版本、代理探测结果）
 *
 * dashboard / client 会先调 hprules_refresh() 检查 homeproxy 规则指纹，
 * 规则一变本次响应就按新规则重新归因；变更事件由采集进程写 hp_event。
 *
 * 输出默认是给人看的分栏文本；加 -j 输出 JSON 便于脚本消费。
 * dashboard 与 client 始终输出 JSON。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <math.h>
#include <ctype.h>
#include <sys/statvfs.h>
#include <sqlite3.h>
#include "hprules.h"
#include "detail.h"

/* 归因辅助定义在 dashboard 一节，但 client 命令在它之前也要用 */
static void cls_json(const char *q, int rcode, char *out, size_t n);

/* detail.c 通过回调拿设备名；实现放在设备目录区之后（类型在那里定义） */
static const char *detail_name_cb(const char *ip, char *buf, size_t n);

static sqlite3 *g_db;
static int      g_json;
static int      g_rw;      /* 1 = 可写（prune 用），默认只读 */

/* ---------------- 通用 ---------------- */

static void die(const char *msg)
{
    fprintf(stderr, "netmon-report: %s\n", msg);
    exit(2);
}

static int open_db(const char *path, int readonly)
{
    /*
     * 默认只读：查询不该有写权限，避免手滑改坏数据。
     * prune 子命令才用可写打开（g_rw=1）。
     */
    int rc = sqlite3_open_v2(path, &g_db,
                             (readonly ? SQLITE_OPEN_READONLY : SQLITE_OPEN_READWRITE)
                             | SQLITE_OPEN_URI, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "netmon-report: 打不开 %s: %s\n",
                path, sqlite3_errmsg(g_db));
        return -1;
    }
    /* 只读库也要等锁，否则主进程 checkpoint 时会报 busy */
    sqlite3_busy_timeout(g_db, 5000);

    return 0;
}

/* "2026-11-05 22:20:06" —— 本地时区 */
static void ts_str(uint64_t ms, char *out, size_t n)
{
    time_t t = (time_t)(ms / 1000);
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(out, n, "%Y-%m-%d %H:%M:%S", &tm);
}

/* "11-05 22:20" —— 报表里够用且省宽度 */
static void ts_short(uint64_t ms, char *out, size_t n)
{
    time_t t = (time_t)(ms / 1000);
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(out, n, "%m-%d %H:%M", &tm);
}

/* 人类可读字节数 */
static void human_bytes(double b, char *out, size_t n)
{
    static const char *u[] = { "B", "K", "M", "G", "T" };
    int i = 0;
    while (b >= 1024.0 && i < 4) { b /= 1024.0; i++; }
    if (i == 0) snprintf(out, n, "%.0f%s", b, u[i]);
    else        snprintf(out, n, "%.2f%s", b, u[i]);
}

/* 单次查询：把每行拼成一行文本，或输出 JSON 数组里的一个对象 */
static sqlite3_stmt *prep(const char *sql)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(g_db, sql, -1, &st, NULL) != SQLITE_OK)
        die(sqlite3_errmsg(g_db));
    return st;
}

/* JSON 字符串转义 */
static void json_esc(const char *s, char *out, size_t n)
{
    size_t o = 0;
    if (!s) { if (n) out[0] = '\0'; return; }
    for (; *s && o + 8 < n; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = (char)c; }
        else if (c < 0x20)         { o += (size_t)snprintf(out + o, n - o, "\\u%04x", c); }
        else                       { out[o++] = (char)c; }
    }
    out[o] = '\0';
}

/* ---------------- 参数 ---------------- */

/* 解析 "--since 24h" / "--since 7d" / "--since 30m" 成毫秒时长 */
static uint64_t parse_span(const char *s)
{
    char *end = NULL;
    double v = strtod(s, &end);
    if (!end || v <= 0) die("时间跨度格式错，例：24h / 7d / 30m");

    uint64_t mult;
    switch (*end) {
    case 'm': mult = 60ULL * 1000;                  break;
    case 'h': mult = 3600ULL * 1000;                break;
    case 'd': mult = 24ULL * 3600 * 1000;           break;
    case 'w': mult = 7ULL * 24 * 3600 * 1000;       break;
    case '\0': mult = 60ULL * 1000;                 break;   /* 默认分钟 */
    default: die("时间单位只支持 m/h/d/w");
    }
    return (uint64_t)(v * (double)mult);
}

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000);
}

static void usage(void)
{
    printf(
"用法: netmon-report <命令> [选项]\n"
"\n"
"命令:\n"
"  top     [--since 24h] [-n 20] [--by cidr|host|port|proto]\n"
"          流量排行。默认按目的网段。\n"
"  trend   [--since 7d] [--unit hour|day]\n"
"          流量与连接数时间趋势。\n"
"  probe   [--since 24h] [--target IP]\n"
"          探测质量：延迟/丢包/带宽，按目标统计。\n"
"  dns     [--since 24h]\n"
"          DNS 解析质量。\n"
"  devices [--since 24h]\n"
"          按设备（客户端 IP）看 DNS 行为：查了多少、查什么、应答率。\n"
"  client  --ip IP [--since 24h]\n"
"          单台设备完整详情（概况/活动曲线/Top 域名/查询日志/流量），始终输出 JSON。\n"
"  status\n"
"          数据库概况（行数、体积、时间覆盖范围）。\n"
"  dashboard [--since 24h]\n"
"          给 Web 前端用的一次性完整 JSON。\n"
"  prune   --target IP [--before 7d] [--dry-run]\n"
"          删除指定目标的探测历史，或删除早于某时间的探测记录。\n"
"          只动 probe 表，不碰连接明细。\n"
"\n"
"通用选项:\n"
"  --ip IP        client 命令指定客户端 IP\n"
"  -j, --json     输出 JSON\n"
"  -d, --db PATH  指定数据库（默认 /opt/netmon/data/netmon.db）\n"
"  -h, --help     帮助\n");
}

/* ---------------- top ---------------- */

static int cmd_top(uint64_t since_ms, int n, const char *by)
{
    uint64_t from = now_ms() - since_ms;

    const char *col, *label;
    if      (!strcmp(by, "cidr"))  { col = "cidr";          label = "目的网段"; }
    else if (!strcmp(by, "host"))  { col = "IFNULL(host,dst)"; label = "域名/目的IP"; }
    else if (!strcmp(by, "port"))  { col = "dport";         label = "目的端口"; }
    else if (!strcmp(by, "proto")) { col = "proto";         label = "协议"; }
    else { fprintf(stderr, "未知 --by=%s\n", by); return 2; }

    char sql[1024];
    snprintf(sql, sizeof(sql),
        "SELECT %s AS k,"
        "       COUNT(*)          AS conns,"
        "       SUM(byt_o)        AS bo,"
        "       SUM(byt_r)        AS br,"
        "       SUM(byt_o+byt_r)  AS bt"
        "  FROM conns"
        " WHERE first_ms >= %llu"
        " GROUP BY k"
        " ORDER BY bt DESC"
        " LIMIT %d;",
        col, (unsigned long long)from, n);

    sqlite3_stmt *st = prep(sql);

    if (g_json) printf("{\"by\":\"%s\",\"since_ms\":%llu,\"rows\":[",
                       by, (unsigned long long)from);

    if (!g_json) {
        printf("\n流量排行（%s，最近 %.1f 小时）\n", label,
               since_ms / 3600000.0);
        printf("%-24s %10s %12s %12s %12s\n",
               label, "连接数", "上行", "下行", "合计");
        printf("--------------------------------------------------------------------------\n");
    }

    int row = 0;
    int rc;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        char key[128];
        const unsigned char *kt = sqlite3_column_text(st, 0);

        if (!kt) snprintf(key, sizeof(key), "(空)");
        else {
            /* 端口/协议是数字，转成人能懂的名字 */
            int coltype = sqlite3_column_type(st, 0);
            if (!strcmp(by, "port") && coltype == SQLITE_INTEGER) {
                int p = sqlite3_column_int(st, 0);
                const char *nm = "";
                switch (p) {
                case 443:  nm = " https";  break;
                case 80:   nm = " http";   break;
                case 53:   nm = " dns";    break;
                case 22:   nm = " ssh";    break;
                case 123:  nm = " ntp";    break;
                case 853:  nm = " dot";    break;
                case 5228: nm = " gcm";    break;
                default: break;
                }
                snprintf(key, sizeof(key), "%d%s", p, nm);
            } else if (!strcmp(by, "proto") && coltype == SQLITE_INTEGER) {
                int p = sqlite3_column_int(st, 0);
                snprintf(key, sizeof(key), "%d (%s)", p,
                         p == 6 ? "tcp" : p == 17 ? "udp" : p == 1 ? "icmp" : "?");
            } else {
                snprintf(key, sizeof(key), "%s", (const char *)kt);
            }
        }

        long long conns = sqlite3_column_int64(st, 1);
        double bo = sqlite3_column_double(st, 2);
        double br = sqlite3_column_double(st, 3);
        double bt = sqlite3_column_double(st, 4);

        if (g_json) {
            char ek[256];
            json_esc(key, ek, sizeof(ek));
            printf("%s{\"key\":\"%s\",\"conns\":%lld,\"bytes_out\":%.0f,"
                   "\"bytes_in\":%.0f,\"bytes_total\":%.0f}",
                   row ? "," : "", ek, conns, bo, br, bt);
        } else {
            char hb_o[24], hb_r[24], hb_t[24];
            human_bytes(bo, hb_o, sizeof(hb_o));
            human_bytes(br, hb_r, sizeof(hb_r));
            human_bytes(bt, hb_t, sizeof(hb_t));
            printf("%-24s %10lld %12s %12s %12s\n", key, conns, hb_o, hb_r, hb_t);
        }
        row++;
    }
    if (rc != SQLITE_DONE) die(sqlite3_errmsg(g_db));
    sqlite3_finalize(st);

    if (g_json) printf("]}\n");
    else if (!row) printf("（该时段没有数据）\n");
    else {
        printf("--------------------------------------------------------------------------\n");
        printf("共 %d 行\n", row);
    }
    return 0;
}

/* ---------------- trend ---------------- */

static int cmd_trend(uint64_t since_ms, const char *unit)
{
    uint64_t from = now_ms() - since_ms;
    int is_day = !strcmp(unit, "day");
    uint64_t bucket = is_day ? 86400000ULL : 3600000ULL;

    char sql[1024];
    snprintf(sql, sizeof(sql),
        "SELECT (first_ms / %llu) * %llu AS bucket,"
        "       COUNT(*)         AS conns,"
        "       SUM(byt_o)       AS bo,"
        "       SUM(byt_r)       AS br"
        "  FROM conns"
        " WHERE first_ms >= %llu"
        " GROUP BY bucket"
        " ORDER BY bucket ASC;",
        (unsigned long long)bucket, (unsigned long long)bucket,
        (unsigned long long)from);

    sqlite3_stmt *st = prep(sql);

    if (g_json) printf("{\"unit\":\"%s\",\"rows\":[", unit);
    else {
        printf("\n时间趋势（按%s）\n", is_day ? "天" : "小时");
        printf("%-14s %10s %12s %12s\n", "时间", "连接数", "上行", "下行");
        printf("------------------------------------------------\n");
    }

    int row = 0;
    int rc;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        uint64_t bk = (uint64_t)sqlite3_column_int64(st, 0);
        long long conns = sqlite3_column_int64(st, 1);
        double bo = sqlite3_column_double(st, 2);
        double br = sqlite3_column_double(st, 3);

        if (g_json) {
            printf("%s{\"ts_ms\":%llu,\"conns\":%lld,\"bytes_out\":%.0f,"
                   "\"bytes_in\":%.0f}",
                   row ? "," : "", (unsigned long long)bk, conns, bo, br);
        } else {
            char tb[32], ho[24], hr[24];
            ts_short(bk, tb, sizeof(tb));
            human_bytes(bo, ho, sizeof(ho));
            human_bytes(br, hr, sizeof(hr));
            printf("%-14s %10lld %12s %12s\n", tb, conns, ho, hr);
        }
        row++;
    }
    if (rc != SQLITE_DONE) die(sqlite3_errmsg(g_db));
    sqlite3_finalize(st);

    if (g_json) printf("]}\n");
    else if (!row) printf("（该时段没有数据）\n");
    return 0;
}

/* ---------------- probe ---------------- */

/*
 * 探测统计要点：
 *   - 丢包率按"有 loss 字段的行"平均，ok=0 的行 loss 也参与（正是要看的）
 *   - 带宽只统计 ok=1 且有 bw_kbps 的，失败的不算 0 混进去
 *   - 每个目标给出：样本数、成功率、平均/最差延迟、平均丢包、平均带宽
 */
static int cmd_probe(uint64_t since_ms, const char *only_target)
{
    uint64_t from = now_ms() - since_ms;

    char sql[1400];
    if (only_target)
        snprintf(sql, sizeof(sql),
            "SELECT target, tag, kind,"
            "       COUNT(*),"
            "       SUM(ok),"
            "       AVG(CASE WHEN rtt_ms IS NOT NULL THEN rtt_ms END),"
            "       MAX(CASE WHEN rtt_ms IS NOT NULL THEN rtt_ms END),"
            "       AVG(CASE WHEN loss IS NOT NULL THEN loss END),"
            "       AVG(CASE WHEN ok=1 AND bw_kbps IS NOT NULL THEN bw_kbps END),"
            "       MAX(ts_ms)"
            "  FROM probe WHERE ts_ms >= %llu AND target = '%s'"
            " GROUP BY target, tag, kind ORDER BY tag, target;",
            (unsigned long long)from, only_target);
    else
        snprintf(sql, sizeof(sql),
            "SELECT target, tag, kind,"
            "       COUNT(*),"
            "       SUM(ok),"
            "       AVG(CASE WHEN rtt_ms IS NOT NULL THEN rtt_ms END),"
            "       MAX(CASE WHEN rtt_ms IS NOT NULL THEN rtt_ms END),"
            "       AVG(CASE WHEN loss IS NOT NULL THEN loss END),"
            "       AVG(CASE WHEN ok=1 AND bw_kbps IS NOT NULL THEN bw_kbps END),"
            "       MAX(ts_ms)"
            "  FROM probe WHERE ts_ms >= %llu"
            " GROUP BY target, tag, kind ORDER BY tag, target;",
            (unsigned long long)from);

    sqlite3_stmt *st = prep(sql);

    if (g_json) printf("{\"rows\":[");
    else {
        printf("\n探测质量（最近 %.1f 小时）\n", since_ms / 3600000.0);
        printf("%-18s %-9s %6s %7s %9s %9s %8s %10s\n",
               "目标", "分组", "样本", "成功率", "均延迟", "最差", "丢包", "带宽");
        printf("--------------------------------------------------------------------------------------\n");
    }

    int row = 0;
    int rc;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        const char *target = (const char *)sqlite3_column_text(st, 0);
        const char *tag    = (const char *)sqlite3_column_text(st, 1);
        const char *kind   = (const char *)sqlite3_column_text(st, 2);
        long long n      = sqlite3_column_int64(st, 3);
        long long okn    = sqlite3_column_int64(st, 4);
        double avg_rtt   = sqlite3_column_double(st, 5);
        double max_rtt   = sqlite3_column_double(st, 6);
        double avg_loss  = sqlite3_column_double(st, 7);
        double avg_bw    = sqlite3_column_double(st, 8);

        if (sqlite3_column_type(st, 5) == SQLITE_NULL) avg_rtt = NAN;
        if (sqlite3_column_type(st, 6) == SQLITE_NULL) max_rtt = NAN;
        if (sqlite3_column_type(st, 7) == SQLITE_NULL) avg_loss = NAN;
        if (sqlite3_column_type(st, 8) == SQLITE_NULL) avg_bw = NAN;

        double rate = n ? (double)okn / (double)n : 0.0;

        if (g_json) {
            /* SQL NULL 映射成 JSON null，不能输出 0（会把"没测"当成"测得 0"） */
            char jr[64], jx[64], jl[64], jb[64];
            if (isnan(avg_rtt))  snprintf(jr, sizeof(jr), "null");
            else                 snprintf(jr, sizeof(jr), "%.2f", avg_rtt);
            if (isnan(max_rtt))  snprintf(jx, sizeof(jx), "null");
            else                 snprintf(jx, sizeof(jx), "%.2f", max_rtt);
            if (isnan(avg_loss)) snprintf(jl, sizeof(jl), "null");
            else                 snprintf(jl, sizeof(jl), "%.4f", avg_loss);
            if (isnan(avg_bw))   snprintf(jb, sizeof(jb), "null");
            else                 snprintf(jb, sizeof(jb), "%.1f", avg_bw);

            printf("%s{\"target\":\"%s\",\"tag\":\"%s\",\"kind\":\"%s\","
                   "\"samples\":%lld,\"ok\":%lld,\"ok_rate\":%.4f,"
                   "\"rtt_avg_ms\":%s,\"rtt_max_ms\":%s,"
                   "\"loss_avg\":%s,\"bw_avg_kbps\":%s}",
                   row ? "," : "",
                   target ? target : "", tag ? tag : "", kind ? kind : "",
                   n, okn, rate, jr, jx, jl, jb);
        } else {
            char sr[16], ar[16], mr[16], lr[16], br[24];
            snprintf(sr, sizeof(sr), "%.0f%%", rate * 100);
            if (isnan(avg_rtt))  snprintf(ar, sizeof(ar), "-");
            else                 snprintf(ar, sizeof(ar), "%.1f", avg_rtt);
            if (isnan(max_rtt))  snprintf(mr, sizeof(mr), "-");
            else                 snprintf(mr, sizeof(mr), "%.1f", max_rtt);
            if (isnan(avg_loss)) snprintf(lr, sizeof(lr), "-");
            else                 snprintf(lr, sizeof(lr), "%.0f%%", avg_loss * 100);
            if (isnan(avg_bw))   snprintf(br, sizeof(br), "-");
            else                 snprintf(br, sizeof(br), "%.0f kbps", avg_bw);

            printf("%-18s %-9s %6lld %7s %9s %9s %8s %10s\n",
                   target ? target : "?", tag ? tag : "-",
                   n, sr, ar, mr, lr, br);
        }
        row++;
    }
    if (rc != SQLITE_DONE) die(sqlite3_errmsg(g_db));
    sqlite3_finalize(st);

    if (g_json) printf("]}\n");
    else if (!row) printf("（该时段没有探测数据）\n");
    else printf("--------------------------------------------------------------------------------------\n");
    return 0;
}

/* ---------------- dns ---------------- */

/*
 * DNS 统计的三个维度必须分开看，混在一起会得出错误结论：
 *
 *   1) replied  = ms IS NOT NULL   —— 有没有收到应答报文（链路质量）。
 *      收不到 = 超时/丢包，这才是真正的"DNS 坏了"。
 *   2) rcode=0                     —— 应答是不是 NOERROR（协议层成功）。
 *   3) rcode=3 (NXDOMAIN)          —— 域名不存在，属于被查方的问题。
 *      rcode=5 (REFUSED)            —— 服务端拒答，通常是分流策略的正常行为。
 *
 * 之所以拆开：之前把 rcode!=0 一律算成"失败"，导致 iot.isp-gateway.cn 这类
 * 设备反复查询不存在域名的场景显示成 ok=0，看起来像 DNS 故障；
 * 其实链路 0 丢包、平均 0.008ms，完全是设备侧在问一个不存在的域名。
 * 把"链路通"和"域名存在"混为一个成功率，是在制造假故障。
 */
static int cmd_dns(uint64_t since_ms)
{
    uint64_t from = now_ms() - since_ms;

    char sql[1536];
    snprintf(sql, sizeof(sql),
        "SELECT qname, COUNT(*),"
        "       SUM(CASE WHEN ms IS NOT NULL THEN 1 ELSE 0 END),"   /* 收到应答 */
        "       SUM(CASE WHEN rcode=0 THEN 1 ELSE 0 END),"           /* NOERROR  */
        "       SUM(CASE WHEN rcode=3 THEN 1 ELSE 0 END),"           /* NXDOMAIN */
        "       SUM(CASE WHEN rcode=5 THEN 1 ELSE 0 END),"           /* REFUSED  */
        "       AVG(ms), MAX(ms),"
        "       SUM(CASE WHEN ms > 1000 THEN 1 ELSE 0 END),"
        /*
         * A 和 AAAA 分开统计配对情况。
         * 为什么要分开：实测发现部分设备（比如 Windows 上跑代理/加密 DNS 的机器）
         * 的 AAAA 查询报文抓不到，但 AAAA 应答能抓到。混在一起统计时，
         * 这部分缺失会把整体的"应答率"拉低到 50% 左右，看起来像 DNS 在丢包，
         * 实际 A 查询的配对是完整的。分开显示才能一眼看出问题出在哪个类型。
         */
        "       SUM(CASE WHEN qtype=1  AND ms IS NOT NULL THEN 1 ELSE 0 END),"  /* A 配对 */
        "       SUM(CASE WHEN qtype=1  THEN 1 ELSE 0 END),"                     /* A 总数 */
        "       SUM(CASE WHEN qtype=28 AND ms IS NOT NULL THEN 1 ELSE 0 END),"  /* AAAA 配对 */
        "       SUM(CASE WHEN qtype=28 THEN 1 ELSE 0 END)"                      /* AAAA 总数 */
        "  FROM dnsq WHERE ts_ms >= %llu"
        " GROUP BY qname ORDER BY COUNT(*) DESC;",
        (unsigned long long)from);

    sqlite3_stmt *st = prep(sql);

    if (g_json) printf("{\"rows\":[");
    else {
        printf("\nDNS 解析质量（最近 %.1f 小时）\n", since_ms / 3600000.0);
        printf("应答率 = 收到应答/总次数（衡量链路）；NX = 域名不存在；REF = 服务端拒答\n");
        printf("A/AAAA 列是两类查询各自的配对率：AAAA 偏低多为抓包侧限制（部分设备走加密 DNS）\n");
        printf("%-26s %6s %7s %7s %7s %6s %6s %11s %11s\n",
               "域名", "次数", "应答", "应答率", "NOERROR", "NX", "REF",
               "A/AAAA配对", "均耗时");
        printf("-------------------------------------------------------------------------------------------------\n");
    }

    int row = 0;
    int rc;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        const char *q   = (const char *)sqlite3_column_text(st, 0);
        long long n     = sqlite3_column_int64(st, 1);
        long long rp    = sqlite3_column_int64(st, 2);
        long long okn   = sqlite3_column_int64(st, 3);
        long long nx    = sqlite3_column_int64(st, 4);
        long long ref   = sqlite3_column_int64(st, 5);
        int  has_avg    = sqlite3_column_type(st, 6) != SQLITE_NULL;
        int  has_max    = sqlite3_column_type(st, 7) != SQLITE_NULL;
        double avg      = has_avg ? sqlite3_column_double(st, 6) : 0.0;
        double mx       = has_max ? sqlite3_column_double(st, 7) : 0.0;
        long long slow  = sqlite3_column_int64(st, 8);
        long long a_ok  = sqlite3_column_int64(st, 9);
        long long a_all = sqlite3_column_int64(st, 10);
        long long q_ok  = sqlite3_column_int64(st, 11);
        long long q_all = sqlite3_column_int64(st, 12);
        double rate     = n ? (double)rp / (double)n : 0.0;

        if (g_json) {
            char eq[256], ja[32] = "null", jm[32] = "null";
            json_esc(q, eq, sizeof(eq));
            /* 没收到应答时耗时必须输出 null，不能写 0 —— 否则"没测到"
               会被前端画成"测到 0ms"，趋势图全是假的谷底。 */
            if (has_avg) snprintf(ja, sizeof(ja), "%.2f", avg);
            if (has_max) snprintf(jm, sizeof(jm), "%.2f", mx);
            printf("%s{\"qname\":\"%s\",\"count\":%lld,\"replied\":%lld,"
                   "\"ok_rate\":%.4f,\"noerror\":%lld,\"nxdomain\":%lld,"
                   "\"refused\":%lld,\"ms_avg\":%s,\"ms_max\":%s,"
                   "\"slow_gt_1s\":%lld,\"a_paired\":%lld,\"a_total\":%lld,"
                   "\"aaaa_paired\":%lld,\"aaaa_total\":%lld}",
                   row ? "," : "", eq, n, rp, rate, okn, nx, ref, ja, jm,
                   slow, a_ok, a_all, q_ok, q_all);
        } else {
            /* A / AAAA 用 12/14 这种紧凑写法，避免表格太宽 */
            char pair[64] = "-";
            if (a_all || q_all)
                snprintf(pair, sizeof(pair), "%lld/%lld·%lld/%lld",
                         a_ok, a_all, q_ok, q_all);
            char ja[32] = "    -   ";
            if (has_avg) snprintf(ja, sizeof(ja), "%8.1fms", avg);
            printf("%-26s %6lld %7lld %6.1f%% %7lld %6lld %6lld %12s %11s\n",
                   q ? q : "?", n, rp, rate * 100.0, okn, nx, ref, pair, ja);
        }
        row++;
    }
    if (rc != SQLITE_DONE) die(sqlite3_errmsg(g_db));
    sqlite3_finalize(st);

    if (g_json) printf("]}\n");
    else if (!row) printf("（该时段没有 DNS 数据）\n");
    else printf("-------------------------------------------------------------------------------------------------\n");
    return 0;
}

/* ---------------- 设备目录 ---------------- */

/*
 * 设备身份的三层来源，按可信度排序：
 *   0) /etc/config/dhcp 的 config host 段 —— 用户在 LuCI「静态地址分配」里
 *      亲手填的名字。静态 IP 设备（NAS、摄像头、服务器、AP）全在这里，
 *      而且它们**不出现在 /tmp/dhcp.leases 里**（infinite 租约不续租）。
 *   1) /tmp/dhcp.leases —— dnsmasq 动态租约，覆盖手机、笔记本。
 *   2) 都没有 → 生成兜底名，保证面板上每台设备都有可读标识。
 *
 * 旧版只读租约表，于是 192.168.1.101 明明在静态分配里叫 NAS-1，
 * 面板却显示「未登记设备」。这次彻底修掉。
 *
 * 自己解析文本而不 fork `uci show dhcp`：CGI 每 10 秒跑一次，
 * 省掉一次进程创建，也避免 uci 不在 PATH 时静默失效。
 */
#define MAX_DEV 256
enum { DEV_STATIC = 0, DEV_LEASE = 1, DEV_MAC = 2, DEV_AUTO = 3 };

typedef struct {
    char ip[46];
    char name[96];      /* 原始名 */
    char display[96];   /* 面板显示名（超长型号会简化） */
    char mac[20];
    char vendor[24];
    char kind[16];
    char canon[46];     /* 归并后的规范 IP（以新静态地址为准） */
    char aliases[320];  /* 同设备其它 IP，逗号分隔 */
    char peers[160];    /* 同一台设备其它网卡的 IP，逗号分隔 */
    int  multi_nic;     /* 同名不同 MAC：一块有线 + 一块无线之类的多网卡 */
    int  source;
} devrec_t;

static devrec_t g_dev[MAX_DEV];
static int   g_ndev;
static int   g_dev_loaded;

/* 取 UCI 行里 '...' / "..." 中的值 */
static int uci_val(const char *line, char *out, size_t n)
{
    const char *a = strchr(line, '\'');
    if (!a) a = strchr(line, '"');
    if (!a) return 0;
    char q = a[0];
    const char *b = strchr(a + 1, q);
    if (!b || (size_t)(b - a - 1) >= n) return 0;
    size_t len = (size_t)(b - a - 1);
    memcpy(out, a + 1, len);
    out[len] = '\0';
    return 1;
}

/* MAC 统一成 AA:BB:CC:DD:EE:FF，方便 OUI 比较 */
static void mac_norm(const char *in, char *out, size_t n)
{
    char hex[13];
    int k = 0;
    for (; *in && k < 12; in++) {
        unsigned char c = (unsigned char)*in;
        if (isxdigit(c)) hex[k++] = (char)toupper(c);
    }
    size_t o = 0;
    for (int i = 0; i < k && o + 3 < n; i++) {
        if (i && !(i & 1)) out[o++] = ':';
        out[o++] = hex[i];
    }
    out[o] = '\0';
}

/* 常见 OUI（覆盖本网络现有设备）。未命中留空，前端显示「未知厂商」 */
static const struct { const char *p, *v; } OUI[] = {
    { "88:68:4B", "OPPO" },     { "F4:63:FC", "vivo" },
    { "34:1C:F0", "Xiaomi" },   { "F4:60:E2", "Xiaomi" },
    { "E8:4A:54", "Xiaomi" },   { "04:CF:8C", "Xiaomi" },
    { "40:31:3C", "Xiaomi" },   { "58:B6:23", "Xiaomi" },
    { "B0:4A:39", "Roborock" }, { "58:2D:34", "Qingping" },
    { "D8:D2:61", "Midea" },    { "B8:0B:DA", "Midea" },
    { "D0:BF:9C", "HPE" },      { "98:6E:E8", "UGREEN" },
    { "08:54:11", "Hikvision" },{ "FC:9F:FD", "Hikvision" },
    { "00:04:4B", "NVIDIA" },   { "3C:E4:41", "Amazon" },
    { "F0:4F:7C", "Amazon" },   { "54:99:63", "Apple" },
    { "24:A4:87", "HUAWEI" },   { "48:5F:08", "TP-LINK" },
    { "04:F9:F8", "TP-LINK" },  { "3C:6A:48", "TP-LINK" },
    { "FC:DB:B3", "Samsung" },  { "9C:AE:D3", "Epson" },
    { "28:80:88", "NETGEAR" },  { "4C:22:F3", "Yamaha" },
    { "E4:B0:05", "iQIYI" },    { "D0:BA:E4", "MXCHIP" },
    { "B0:F8:93", "Phicomm" },  { "54:EF:92", "EZpad" },
    { "AC:9E:17", "Intel" },    { "60:A4:4C", "Intel" },
    { "E0:3F:49", "Realtek" },  { "30:9C:23", "Realtek" },
    { "E4:B0:05", "iQIYI" }
};

/* 大小写不敏感的子串匹配（musl 下 strcasestr 不一定可用，自己实现） */
static int has_ci(const char *s, const char *sub)
{
    if (!s || !sub || !*sub) return 0;
    size_t n = strlen(sub);
    for (; *s; s++) {
        size_t i = 0;
        while (i < n && s[i] &&
               tolower((unsigned char)s[i]) == tolower((unsigned char)sub[i])) i++;
        if (i == n) return 1;
    }
    return 0;
}

/* 由名字/厂商猜设备类型，前端据此选图标。猜不中就归 other。 */
static const char *dev_kind(const char *name, const char *vendor)
{
    if (has_ci(name, "DS-2SC") || has_ci(name, "camera") ||
        has_ci(vendor, "Hikvision"))                        return "camera";
    if (has_ci(name, "DS-7808") || has_ci(name, "GEN8") ||
        has_ci(name, "UGOS") || has_ci(name, "NAS") ||
        has_ci(vendor, "HPE") || has_ci(vendor, "UGREEN")) return "nas";
    if (has_ci(name, "WTA301") || has_ci(name, "GS116E") ||
        has_ci(name, "TL-") || has_ci(vendor, "TP-LINK") ||
        has_ci(vendor, "NETGEAR"))                         return "router";
    if (has_ci(name, "ShieldTV") || has_ci(name, "Fire-Stick") ||
        has_ci(name, "IQIYI") || has_ci(name, "EZpad") ||
        has_ci(vendor, "NVIDIA"))                          return "tv";
    if (has_ci(name, "DESKTOP") || has_ci(name, "MacBook") ||
        has_ci(name, "iMac") || has_ci(vendor, "Intel"))   return "pc";
    if (has_ci(name, "MiAiSoundbox") || has_ci(name, "RX-V4A") ||
        has_ci(vendor, "Yamaha"))                          return "audio";
    if (has_ci(name, "EPSON") || has_ci(vendor, "Epson"))  return "printer";
    if (has_ci(name, "iPad") || has_ci(name, "Kindle") ||
        has_ci(name, "EZpad"))                             return "tablet";
    if (has_ci(name, "-ac") || has_ci(name, "light") ||
        has_ci(name, "roborock") || has_ci(name, "AirDetector") ||
        has_ci(name, "humidifier") || has_ci(name, "Qingping") ||
        has_ci(name, "mxchip") || has_ci(name, "Monitor") ||
        has_ci(name, "lamp") || has_ci(name, "sensor"))     return "iot";
    if (has_ci(name, "OPPO") || has_ci(name, "iQOO") ||
        has_ci(name, "MIX") || has_ci(name, "Galaxy") ||
        has_ci(name, "NOVA") || has_ci(name, "iPhone") ||
        has_ci(name, "M2006") || has_ci(vendor, "Xiaomi") ||
        has_ci(vendor, "HUAWEI") || has_ci(vendor, "vivo")) return "phone";
    return "other";
}

/* 生成厂商 + 类型 + 显示名。每次字段变动后都要重跑。 */
static void dev_decorate(devrec_t *d)
{
    const char *lastdot = strrchr(d->ip, '.');
    const char *lastcol = strrchr(d->ip, ':');
    const char *tail = lastdot ? lastdot + 1 : (lastcol ? lastcol + 1 : d->ip);
    size_t tn = strlen(tail);
    if (tn > 12) tail += tn - 12;   /* IPv6 末段太长时只留后 12 字符，别撑爆卡片 */

    /* vendor 要先算：没名字时的兜底名会用它 */
    d->vendor[0] = '\0';
    for (size_t i = 0; i < sizeof(OUI) / sizeof(OUI[0]); i++) {
        if (!strncmp(d->mac, OUI[i].p, 8)) {
            snprintf(d->vendor, sizeof(d->vendor), "%s", OUI[i].v);
            break;
        }
    }

    if (has_ci(d->name, "DS-2SC")) {
        /* 海康摄像头型号有 38 字符，全名会把卡片撑爆，按 IP 末段区分 */
        snprintf(d->display, sizeof(d->display), "监控摄像头 · %s", tail);
    } else if (strlen(d->name) > 30) {
        snprintf(d->display, sizeof(d->display), "%.*s…", 27, d->name);
    } else if (d->name[0]) {
        snprintf(d->display, sizeof(d->display), "%s", d->name);
    } else if (d->vendor[0]) {
        /* 查不到登记名，但 ARP 给了 MAC → 至少说得出厂商 */
        snprintf(d->display, sizeof(d->display), "%s 设备 · %s", d->vendor, tail);
    } else if (strchr(d->ip, ':')) {
        /* 隐私地址的末段是一串随机十六进制，当名字用只会让人困惑；
           前端会完整显示 IP 并标 IPv6，靠地址区分即可 */
        snprintf(d->display, sizeof(d->display), "未命名设备（IPv6）");
    } else {
        snprintf(d->display, sizeof(d->display), "未命名设备");
    }
    snprintf(d->kind, sizeof(d->kind), "%s", dev_kind(d->name, d->vendor));
}

static void dev_add(const char *ip, const char *name, const char *mac, int source)
{
    if (!ip || !ip[0] || g_ndev >= MAX_DEV) return;
    for (int i = 0; i < g_ndev; i++) {
        if (strcmp(g_dev[i].ip, ip)) continue;
        devrec_t *d = &g_dev[i];
        /* 静态分配的名字比租约更可信（用户手填 vs 客户端自报） */
        if (source < d->source) {
            if (name && name[0]) snprintf(d->name, sizeof(d->name), "%s", name);
            d->source = source;
        } else if (!d->name[0] && name && name[0]) {
            snprintf(d->name, sizeof(d->name), "%s", name);
        }
        if (!d->mac[0] && mac && mac[0]) {
            char m[20]; mac_norm(mac, m, sizeof(m));
            snprintf(d->mac, sizeof(d->mac), "%s", m);
        }
        dev_decorate(d);
        return;
    }
    devrec_t *d = &g_dev[g_ndev++];
    memset(d, 0, sizeof(*d));
    snprintf(d->ip, sizeof(d->ip), "%s", ip);
    snprintf(d->canon, sizeof(d->canon), "%s", ip);   /* 新条目默认自指 */
    if (name && name[0]) snprintf(d->name, sizeof(d->name), "%s", name);
    if (mac && mac[0]) mac_norm(mac, d->mac, sizeof(d->mac));
    d->source = source;
    dev_decorate(d);
}

static void load_static(void)
{
    FILE *f = fopen("/etc/config/dhcp", "r");
    if (!f) return;
    char line[512], ip[46] = "", name[96] = "", mac[20] = "";
    int in_host = 0;
    while (fgets(line, sizeof(line), f)) {
        char *t = line;
        while (*t == ' ' || *t == '\t') t++;
        if (!strncmp(t, "config ", 7)) {
            if (in_host && ip[0]) dev_add(ip, name, mac, DEV_STATIC);
            in_host = !strncmp(t + 7, "host", 4);
            ip[0] = name[0] = mac[0] = '\0';
            continue;
        }
        if (!in_host) continue;
        char v[256];
        int is_name = !strncmp(t, "option name", 11);
        int is_ip   = !strncmp(t, "option ip", 9);
        int is_mac  = !strncmp(t, "option mac", 10) || !strncmp(t, "list mac", 8);
        if (!(is_name || is_ip || is_mac)) continue;
        if (!uci_val(t, v, sizeof(v))) continue;
        if      (is_name) snprintf(name, sizeof(name), "%.95s", v);
        else if (is_ip)   snprintf(ip,   sizeof(ip),   "%.45s", v);
        else { char m[20]; mac_norm(v, m, sizeof(m));
               snprintf(mac, sizeof(mac), "%s", m); }
    }
    if (in_host && ip[0]) dev_add(ip, name, mac, DEV_STATIC);
    fclose(f);
}

static void load_leases(void)
{
    FILE *f = fopen("/tmp/dhcp.leases", "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char mac[32] = "", ip[46] = "", name[96] = "", cid[96] = "";
        unsigned long long exp = 0;
        int got = sscanf(line, "%llu %31s %45s %95s %95s",
                         &exp, mac, ip, name, cid);
        if (got < 3 || !ip[0]) continue;
        /* '*' 表示客户端没上报主机名 */
        if (got < 4 || !name[0] || name[0] == '*') name[0] = '\0';
        dev_add(ip, name, mac, DEV_LEASE);
    }
    fclose(f);
}

/*
 * 第四层线索：/proc/net/arp。
 *
 * 光靠静态分配 + 租约表，有几台设备始终没名字。ARP 表能给出 IP -> MAC，
 * 于是多两件有用的事：
 *   1) MAC 精确匹配上静态分配里的某台设备 → 说明它换了 IP（DHCP 重分配、
 *      或手机同时开了另一个地址），直接沿用用户填的名字；
 *      实测 192.168.1.104 的 MAC 就是静态分配里的 Phone-5。
 *   2) 匹配不上也能用 OUI 猜厂商，兜底名从「设备 · 163」变成「HUAWEI 设备 · 163」。
 *
 * ARP 只用来补身份，不主动往目录里塞没有 DNS 活动的设备，避免客户端列表膨胀。
 */
#define MAX_ARP 256
typedef struct { char ip[46]; char mac[20]; } arp_t;
static arp_t g_arp[MAX_ARP];
static int   g_narp;

static void load_arp(void)
{
    FILE *f = fopen("/proc/net/arp", "r");
    if (!f) return;
    char line[256];
    (void)fgets(line, sizeof(line), f);          /* 表头 */
    while (g_narp < MAX_ARP && fgets(line, sizeof(line), f)) {
        char ip[46] = "", type[32] = "", flag[32] = "",
             hw[32] = "", mask[32] = "", dev[32] = "";
        if (sscanf(line, "%45s %31s %31s %31s %31s %31s",
                   ip, type, flag, hw, mask, dev) < 6) continue;
        if (strcmp(dev, "br-lan")) continue;      /* podman0 / wan 不算客户端 */
        /* flag=0x0 只是 ARP 尚未确认，MAC 本身通常是真实的，仍可用于 OUI 反查；
           只排除全零地址，不要把这些条目整个丢掉 */
        (void)flag;
        if (!strcmp(hw, "00:00:00:00:00:00")) continue;
        snprintf(g_arp[g_narp].ip, sizeof(g_arp[0].ip), "%s", ip);
        mac_norm(hw, g_arp[g_narp].mac, sizeof(g_arp[0].mac));
        g_narp++;
    }
    fclose(f);
}

static const char *arp_mac(const char *ip)
{
    for (int i = 0; i < g_narp; i++)
        if (!strcmp(g_arp[i].ip, ip)) return g_arp[i].mac;
    return NULL;
}

/* 按 MAC 找已登记（静态/租约）设备的名字 */
static const char *name_by_mac(const char *mac)
{
    if (!mac || !mac[0]) return NULL;
    for (int i = 0; i < g_ndev; i++)
        if (g_dev[i].source <= DEV_LEASE && g_dev[i].name[0] &&
            !strcmp(g_dev[i].mac, mac)) return g_dev[i].name;
    return NULL;
}

/*
 * IPv6 邻居表：手机等设备的 IPv6 地址不在 DHCP 租约里，
 * 直接拿 IP 尾段当名字会出现「设备 · 92e9」这种没意义的标识。
 * `ip -6 neigh` 能给出 IPv6 -> MAC，再用 MAC 反查静态分配里的真实名字。
 * 只在目录首次加载时 fork 一次，之后常驻内存。
 */
#define MAX_NEIGH6 256
typedef struct { char ip[46]; char mac[20]; } neigh6_t;
static neigh6_t g_n6[MAX_NEIGH6];
static int      g_nn6;

static void load_neigh6(void)
{
    FILE *f = popen("ip -6 neigh show 2>/dev/null", "r");
    if (!f) return;
    char line[512];
    while (g_nn6 < MAX_NEIGH6 && fgets(line, sizeof(line), f)) {
        char ip[64] = "", mac[32] = "";
        if (sscanf(line, "%63s", ip) != 1 || !strchr(ip, ':')) continue;
        char *p = strstr(line, "lladdr");
        if (!p || sscanf(p + 6, "%31s", mac) != 1) continue;   /* FAILED 项没有 MAC */
        snprintf(g_n6[g_nn6].ip, sizeof(g_n6[0].ip), "%s", ip);
        mac_norm(mac, g_n6[g_nn6].mac, sizeof(g_n6[0].mac));
        g_nn6++;
    }
    pclose(f);
}

static const char *neigh6_mac(const char *ip)
{
    for (int i = 0; i < g_nn6; i++)
        if (!strcmp(g_n6[i].ip, ip)) return g_n6[i].mac;
    return NULL;
}

static void load_catalog(void);

/* 同 MAC 视为同一台设备；用户在 LuCI 里给一台设备配了两条同名静态
   分配（如双网口）时也视为同一台。规范地址优先取 source 更小的
   （静态分配 > 租约 > MAC 反查 > 自动识别）；source 相同取数组下标
   更大的，因为配置文件里后出现的通常是新加的那条。 */
/* 只按 MAC 合并：那是「同一块网卡换了 IP」（时间先后），旧数据应归并。
   同名但 MAC 不同是「同一台设备两块网卡并存」（空间并列），不能合并，
   否则两个接口的活动会混成一条；交给 mark_multi_nic() 改名区分。 */
static int alias_same(const devrec_t *a, const devrec_t *b)
{
    return (a->mac[0] && b->mac[0] && !strcmp(a->mac, b->mac)) ? 1 : 0;
}

static int alias_better(int a, int b)
{
    if (g_dev[a].source != g_dev[b].source)
        return g_dev[a].source < g_dev[b].source ? a : b;
    return a > b ? a : b;
}

static int g_alias_ndev;   /* 上次归并时的设备数，用于检测运行期新增条目 */

static void build_aliases(void)
{
    int root[MAX_DEV];
    for (int i = 0; i < g_ndev; i++) root[i] = i;

    for (int i = 0; i < g_ndev; i++) {
        for (int j = i + 1; j < g_ndev; j++) {
            if (!alias_same(&g_dev[i], &g_dev[j])) continue;
            int a = root[i], b = root[j];
            if (a == b) continue;
            int keep = alias_better(a, b), drop = (keep == a ? b : a);
            for (int k = 0; k < g_ndev; k++)
                if (root[k] == drop) root[k] = keep;
        }
    }
    for (int i = 0; i < g_ndev; i++) {
        snprintf(g_dev[i].canon, sizeof(g_dev[i].canon), "%s",
                 g_dev[root[i]].ip);
        g_dev[i].aliases[0] = '\0';
        size_t used = 0;
        for (int j = 0; j < g_ndev; j++) {
            if (j == i || root[j] != root[i]) continue;
            size_t left = sizeof(g_dev[i].aliases) - used;
            if (left < 4) break;
            int w = snprintf(g_dev[i].aliases + used, left, "%s%s",
                             used ? "," : "", g_dev[j].ip);
            if (w < 0 || (size_t)w >= left) break;
            used += (size_t)w;
        }
    }
    /* 同名不同 MAC：给显示名加网卡标识，避免列表里两行看起来一模一样。
       用 MAC 末段而不是猜「有线/无线」，因为配置里没有接口类型信息。 */
    for (int i = 0; i < g_ndev; i++) {
        g_dev[i].multi_nic = 0;
        g_dev[i].peers[0] = '\0';
    }
    for (int i = 0; i < g_ndev; i++) {
        if (!g_dev[i].name[0]) continue;
        int hit = 0;
        for (int j = 0; j < g_ndev; j++) {
            if (j == i || !g_dev[j].name[0]) continue;
            if (strcmp(g_dev[i].name, g_dev[j].name)) continue;
            if (g_dev[i].mac[0] && g_dev[j].mac[0] &&
                !strcmp(g_dev[i].mac, g_dev[j].mac)) continue;
            hit = 1;
            size_t used = strlen(g_dev[i].peers);
            size_t left = sizeof(g_dev[i].peers) - used;
            if (left > 4) {
                snprintf(g_dev[i].peers + used, left, "%s%s",
                         used ? "," : "", g_dev[j].ip);
            }
        }
        g_dev[i].multi_nic = hit;
        dev_decorate(&g_dev[i]);          /* 先还原成不带后缀的原始显示名 */
        if (hit && g_dev[i].mac[0]) {
            const char *colon = strrchr(g_dev[i].mac, ':');
            const char *tail = colon ? colon + 1 : g_dev[i].mac;
            size_t used = strlen(g_dev[i].display);
            size_t left = sizeof(g_dev[i].display) - used;
            if (left > 8)
                snprintf(g_dev[i].display + used, left, " · %s", tail);
        }
    }
    g_alias_ndev = g_ndev;
}

/* dev_find() 会在运行期把历史 IP 补进目录，这时要重算归并关系 */
static void ensure_aliases(void)
{
    load_catalog();
    if (g_alias_ndev != g_ndev) build_aliases();
}

/* 任意 IP -> 规范 IP。目录外的 IP 原样返回。 */
static const char *canon_ip(const char *ip)
{
    if (!ip || !*ip) return ip;
    ensure_aliases();
    for (int i = 0; i < g_ndev; i++)
        if (!strcmp(g_dev[i].ip, ip))
            /* dev_find() 可能动态补进新条目，它的 canon 还没算过，
               这时必须回退到自身 IP，否则空键会让 row_for 每行都新建 */
            return g_dev[i].canon[0] ? g_dev[i].canon : g_dev[i].ip;
    return ip;
}

static void load_catalog(void)
{
    if (g_dev_loaded) return;
    g_dev_loaded = 1;
    load_static();
    load_leases();
    load_arp();
    load_neigh6();
    /* 静态分配里个别条目没写 MAC，用 ARP 补上（前端详情要显示 MAC / 厂商） */
    for (int i = 0; i < g_ndev; i++) {
        if (g_dev[i].mac[0]) continue;
        const char *m = arp_mac(g_dev[i].ip);
        if (m) {
            snprintf(g_dev[i].mac, sizeof(g_dev[i].mac), "%s", m);
            dev_decorate(&g_dev[i]);
        }
    }
    build_aliases();
}

static devrec_t *dev_find(const char *ip)
{
    load_catalog();
    if (!ip || !ip[0]) return NULL;
    for (int i = 0; i < g_ndev; i++)
        if (!strcmp(g_dev[i].ip, ip)) return &g_dev[i];

    const char *am = arp_mac(ip);
    if (!am && strchr(ip, ':')) am = neigh6_mac(ip);
    const char *known = name_by_mac(am);
    int before = g_ndev;
    dev_add(ip, known, am, known ? DEV_MAC : DEV_AUTO);
    return g_ndev > before ? &g_dev[g_ndev - 1] : NULL;
}

/* detail.c 用它给连接明细里的源 IP 补设备名 */
static const char *detail_name_cb(const char *ip, char *buf, size_t n)
{
    const char *cp = canon_ip(ip);
    devrec_t *d = dev_find(cp);
    snprintf(buf, n, "%s", (d && d->display[0]) ? d->display : (cp ? cp : ""));
    return buf;
}

static void dev_json(const devrec_t *d, char *out, size_t n)
{
    char a[128], b[256], c[256], e[64], f[64], g[64];
    if (!d) { snprintf(out, n, "null"); return; }
    json_esc(d->ip, a, sizeof(a));       json_esc(d->name, b, sizeof(b));
    json_esc(d->display, c, sizeof(c));  json_esc(d->mac, e, sizeof(e));
    json_esc(d->vendor, f, sizeof(f));   json_esc(d->kind, g, sizeof(g));
    char h[360], hp[200];
    json_esc(d->aliases, h, sizeof(h));
    json_esc(d->peers, hp, sizeof(hp));
    snprintf(out, n,
        "{\"ip\":\"%s\",\"name\":\"%s\",\"display\":\"%s\",\"mac\":\"%s\","
        "\"vendor\":\"%s\",\"kind\":\"%s\",\"source\":%d,"
        "\"aliases\":\"%s\",\"peers\":\"%s\",\"multi_nic\":%d}",
        a, b, c, e, f, g, d->source, h, hp, d->multi_nic);
}

/* 设备统计行：dashboard 里把 DNS / conns 两路数据按 IP 汇总到一起 */
typedef struct {
    char ip[46];
    long long n, hosts, replied, nx, ref;
    double avg_ms;
    long long first_ms, last_ms;
    long long conns;
    double bo, br;
    long long cls_n[4];      /* 按设备拆分的 OK/BLOCKED/LOCAL/UNRESOLVED */
    char top_q[256];
    /* 多 IP 归并用：规范地址自身的查询数、最近活动 IP、耗时加权 */
    long long canon_n;
    char last_ip[46];
    double ms_sum;
    long long ms_n;
} devstat_t;

static devstat_t g_rows[MAX_DEV];
static int       g_nrows;

static int catalog_has(const char *ip)
{
    for (int i = 0; i < g_ndev; i++)
        if (!strcmp(g_dev[i].ip, ip)) return 1;
    return 0;
}

static devstat_t *row_for(const char *ip)
{
    if (!ip || !ip[0] || g_nrows >= MAX_DEV) return NULL;
    /* 旧 DHCP 地址往往不在目录里，是 SQL 循环时才发现的。先补进目录
       （dev_find 会按 ARP 查到 MAC），再重算归并，才能归到新静态地址。 */
    ensure_aliases();
    if (!catalog_has(ip)) {
        dev_find(ip);
        ensure_aliases();
    }
    const char *cip = canon_ip(ip);
    for (int i = 0; i < g_nrows; i++)
        if (!strcmp(g_rows[i].ip, cip)) return &g_rows[i];
    devstat_t *r = &g_rows[g_nrows++];
    memset(r, 0, sizeof(*r));
    snprintf(r->ip, sizeof(r->ip), "%s", cip);
    return r;
}

/* ---------------- devices ---------------- */

/* 命令行版设备排行：字段比 dashboard 少，但同样带完整身份信息 */
static int cmd_devices(uint64_t since_ms)
{
    uint64_t from = now_ms() - since_ms;
    load_catalog();

    char sql[1024];
    snprintf(sql, sizeof(sql),
        "SELECT client_ip, COUNT(*), COUNT(DISTINCT qname),"
        "       SUM(CASE WHEN ms IS NOT NULL THEN 1 ELSE 0 END),"
        "       SUM(CASE WHEN rcode=3 THEN 1 ELSE 0 END),"
        "       SUM(CASE WHEN rcode=5 THEN 1 ELSE 0 END),"
        "       MIN(ts_ms), MAX(ts_ms)"
        "  FROM dnsq"
        " WHERE ts_ms >= %llu AND client_ip IS NOT NULL AND client_ip<>''"
        " GROUP BY client_ip ORDER BY COUNT(*) DESC;",
        (unsigned long long)from);
    sqlite3_stmt *st = prep(sql);

    if (g_json) printf("{\"catalog\":%d,\"devices\":[", g_ndev);
    else {
        printf("\nDNS 查询设备排行（最近 %.1f 小时，已登记 %d 台）\n",
               since_ms / 3600000.0, g_ndev);
        printf("%-24s %-17s %8s %7s %8s %6s %6s\n",
               "设备", "IP", "查询", "域名数", "应答率", "NX", "REF");
        printf("--------------------------------------------------------------------------\n");
    }

    int row = 0;
    while (sqlite3_step(st) == SQLITE_ROW) {
        const char *ip = (const char *)sqlite3_column_text(st, 0);
        long long n      = sqlite3_column_int64(st, 1);
        long long hosts  = sqlite3_column_int64(st, 2);
        long long rp     = sqlite3_column_int64(st, 3);
        long long nx     = sqlite3_column_int64(st, 4);
        long long ref    = sqlite3_column_int64(st, 5);
        double rate      = n ? (double)rp / (double)n : 0.0;
        devrec_t *d         = dev_find(ip);

        char dj[1536];
        dev_json(d, dj, sizeof(dj));

        if (g_json) {
            printf("%s{\"device\":%s,\"n\":%lld,\"hosts\":%lld,"
                   "\"ok_rate\":%.4f,\"nx\":%lld,\"ref\":%lld,"
                   "\"first_ms\":%lld,\"last_ms\":%lld}",
                   row ? "," : "", dj, n, hosts, rate, nx, ref,
                   sqlite3_column_int64(st, 6), sqlite3_column_int64(st, 7));
        } else {
            printf("%-24s %-17s %8lld %7lld %7.0f%% %6lld %6lld\n",
                   d ? d->display : "?", ip, n, hosts, rate * 100.0, nx, ref);
        }
        row++;
    }
    sqlite3_finalize(st);

    if (g_json) printf("]}\n");
    else if (!row) printf("（该时段没有可归属到设备的 DNS 记录）\n");
    else printf("--------------------------------------------------------------------------\n");
    return 0;
}

/* ---------------- client（单台设备详情） ---------------- */

/*
 * 前端点开设备时调用。始终输出 JSON（和 dashboard 一样），
 * 命令行用 `netmon-report client --ip 192.168.1.101 -j` 也能直接看。
 *
 * IP 是外部输入，一律走 sqlite3_bind_text，绝不拼进 SQL；
 * 另外做字符集白名单，挡掉引号/分号等一切元字符。
 */
static int valid_ip(const char *ip)
{
    if (!ip || !*ip || strlen(ip) > 45) return 0;
    int sep = 0;
    for (const char *p = ip; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (!(isdigit(c) || c == '.' || c == ':' ||
              (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return 0;
        /* CGI 会把非法字符删掉，可能剩下一串纯 hex 垃圾；
           真正的 IP 一定含 '.' 或 ':'，据此挡掉无意义查询 */
        if (c == '.' || c == ':') sep = 1;
    }
    return sep;
}

static int cmd_client(uint64_t since_ms, const char *ip)
{
    if (!valid_ip(ip)) {
        printf("{\"error\":\"invalid ip\"}\n");
        return 2;
    }
    const char *cip = canon_ip(ip);
    /* 建临时表存这台设备的全部地址，下面所有 SQL 用 IN 一次查全，
       这样换 IP 之前的历史查询/连接也归到同一台设备。 */
    sqlite3_exec(g_db, "DROP TABLE IF EXISTS _self_ips;"
                       "CREATE TEMP TABLE _self_ips(ip TEXT PRIMARY KEY);",
                 NULL, NULL, NULL);
    {
        sqlite3_stmt *ins = NULL;
        if (sqlite3_prepare_v2(g_db,
                "INSERT OR IGNORE INTO _self_ips VALUES(?);",
                -1, &ins, NULL) == SQLITE_OK && ins) {
            sqlite3_bind_text(ins, 1, cip, -1, SQLITE_TRANSIENT);
            sqlite3_step(ins); sqlite3_reset(ins);
            const devrec_t *cd = dev_find(cip);
            if (cd && cd->aliases[0]) {
                char al[320], *save = NULL;
                snprintf(al, sizeof(al), "%s", cd->aliases);
                for (char *p = strtok_r(al, ",", &save); p;
                     p = strtok_r(NULL, ",", &save)) {
                    sqlite3_bind_text(ins, 1, p, -1, SQLITE_TRANSIENT);
                    sqlite3_step(ins); sqlite3_reset(ins);
                }
            }
            sqlite3_finalize(ins);
        }
    }
    uint64_t from = now_ms() - since_ms;
    const devrec_t *d = dev_find(cip);
    char sql[1200], dj[1024];
    dev_json(d, dj, sizeof(dj));

    printf("{\n  \"device\": %s,\n", dj);
    printf("  \"since_ms\": %llu,\n", (unsigned long long)from);

    /* --- 概况：DNS + 连接流量 --- */
    printf("  \"summary\": {");
    {
        snprintf(sql, sizeof(sql),
            "SELECT COUNT(*), COUNT(DISTINCT qname),"
            " SUM(CASE WHEN ms IS NOT NULL THEN 1 ELSE 0 END),"
            " SUM(CASE WHEN rcode=0 THEN 1 ELSE 0 END),"
            " SUM(CASE WHEN rcode=3 THEN 1 ELSE 0 END),"
            " SUM(CASE WHEN rcode=5 THEN 1 ELSE 0 END),"
            " AVG(ms), MAX(ms), MIN(ts_ms), MAX(ts_ms)"
            " FROM dnsq WHERE ts_ms >= %llu AND client_ip IN (SELECT ip FROM _self_ips);",
            (unsigned long long)from);
        sqlite3_stmt *st = prep(sql);
        long long n = 0, hosts = 0, rp = 0, ne = 0, nx = 0, rf = 0;
        long long t1 = 0, t2 = 0;
        char ja[32] = "null", jm[32] = "null";
        if (sqlite3_step(st) == SQLITE_ROW) {
            n     = sqlite3_column_int64(st, 0);
            hosts = sqlite3_column_int64(st, 1);
            rp    = sqlite3_column_int64(st, 2);
            ne    = sqlite3_column_int64(st, 3);
            nx    = sqlite3_column_int64(st, 4);
            rf    = sqlite3_column_int64(st, 5);
            if (sqlite3_column_type(st, 6) != SQLITE_NULL)
                snprintf(ja, sizeof(ja), "%.2f", sqlite3_column_double(st, 6));
            if (sqlite3_column_type(st, 7) != SQLITE_NULL)
                snprintf(jm, sizeof(jm), "%.2f", sqlite3_column_double(st, 7));
            t1 = sqlite3_column_int64(st, 8);
            t2 = sqlite3_column_int64(st, 9);
        }
        sqlite3_finalize(st);

        /* conns.src 是连接发起方；LAN 设备发起时 byt_o=上行 byt_r=下行。
           IPv6 源地址与 IPv4 设备 IP 不同，这里只统计 IPv4 部分。 */
        long long conns = 0;
        double bo = 0, br = 0;
        snprintf(sql, sizeof(sql),
            "SELECT COUNT(*), SUM(byt_o), SUM(byt_r) FROM conns"
            " WHERE first_ms >= %llu AND src IN (SELECT ip FROM _self_ips);", (unsigned long long)from);
        st = prep(sql);
        if (sqlite3_step(st) == SQLITE_ROW) {
            conns = sqlite3_column_int64(st, 0);
            bo = sqlite3_column_double(st, 1);
            br = sqlite3_column_double(st, 2);
        }
        sqlite3_finalize(st);

        printf("\"n\":%lld,\"hosts\":%lld,\"replied\":%lld,\"noerror\":%lld,"
               "\"nx\":%lld,\"ref\":%lld,\"reply_rate\":%.4f,"
               "\"avg_ms\":%s,\"max_ms\":%s,\"first_ms\":%lld,\"last_ms\":%lld,"
               "\"conns\":%lld,\"bytes_out\":%.0f,\"bytes_in\":%.0f},\n",
               n, hosts, rp, ne, nx, rf, n ? (double)rp / (double)n : 0.0,
               ja, jm, t1, t2, conns, bo, br);
    }

    /* --- 小时活动曲线 --- */
    printf("  \"timeline\": [");
    {
        snprintf(sql, sizeof(sql),
            "SELECT (ts_ms/3600000)*3600000, COUNT(*),"
            " SUM(CASE WHEN rcode=3 THEN 1 ELSE 0 END),"
            " SUM(CASE WHEN rcode=5 THEN 1 ELSE 0 END), AVG(ms)"
            " FROM dnsq WHERE ts_ms >= %llu AND client_ip IN (SELECT ip FROM _self_ips)"
            " GROUP BY 1 ORDER BY 1;", (unsigned long long)from);
        sqlite3_stmt *st = prep(sql);
        int row = 0;
        while (sqlite3_step(st) == SQLITE_ROW) {
            char jm[32] = "null";
            if (sqlite3_column_type(st, 4) != SQLITE_NULL)
                snprintf(jm, sizeof(jm), "%.2f", sqlite3_column_double(st, 4));
            printf("%s{\"t\":%lld,\"n\":%lld,\"nx\":%lld,\"ref\":%lld,\"ms\":%s}",
                   row ? "," : "",
                   sqlite3_column_int64(st, 0),
                   sqlite3_column_int64(st, 1),
                   sqlite3_column_int64(st, 2),
                   sqlite3_column_int64(st, 3), jm);
            row++;
        }
        sqlite3_finalize(st);
    }
    printf("],\n");


    /* --- 该设备按小时的上下行流量：详情页的流量趋势图 ---
       与 DNS 活动曲线并列，回答「这台设备什么时候在下载/上传」。
       复用 _self_ips（含归并后的旧地址），换 IP 之前的历史不会丢。 */
    printf("  \"traffic_timeline\": [");
    {
        snprintf(sql, sizeof(sql),
            "SELECT (first_ms/3600000)*3600000, SUM(byt_o), SUM(byt_r), COUNT(*)"
            "  FROM conns WHERE first_ms >= %llu"
            "    AND src IN (SELECT ip FROM _self_ips)"
            "  GROUP BY 1 ORDER BY 1;", (unsigned long long)from);
        sqlite3_stmt *st = prep(sql);
        int row = 0;
        while (st && sqlite3_step(st) == SQLITE_ROW) {
            printf("%s{\"t\":%lld,\"out\":%.0f,\"in\":%.0f,\"conns\":%lld}",
                   row ? "," : "", sqlite3_column_int64(st, 0),
                   sqlite3_column_double(st, 1), sqlite3_column_double(st, 2),
                   sqlite3_column_int64(st, 3));
            row++;
        }
        if (st) sqlite3_finalize(st);
    }
    printf("],\n");
    /* --- Top 域名 --- */
    printf("  \"top_domains\": [");
    {
        snprintf(sql, sizeof(sql),
            "SELECT qname, COUNT(*),"
            " SUM(CASE WHEN rcode=3 THEN 1 ELSE 0 END),"
            " SUM(CASE WHEN rcode=5 THEN 1 ELSE 0 END),"
            " AVG(ms), MAX(ts_ms)"
            " FROM dnsq WHERE ts_ms >= %llu AND client_ip IN (SELECT ip FROM _self_ips)"
            " GROUP BY qname ORDER BY 2 DESC LIMIT 60;", (unsigned long long)from);
        sqlite3_stmt *st = prep(sql);
        int row = 0;
        while (sqlite3_step(st) == SQLITE_ROW) {
            const unsigned char *q = sqlite3_column_text(st, 0);
            char eq[512], jm[32] = "null";
            json_esc(q ? (const char *)q : "?", eq, sizeof(eq));
            if (sqlite3_column_type(st, 4) != SQLITE_NULL)
                snprintf(jm, sizeof(jm), "%.2f", sqlite3_column_double(st, 4));
            printf("%s{\"q\":\"%s\",\"n\":%lld,\"nx\":%lld,\"ref\":%lld,"
                   "\"ms\":%s,\"last_ms\":%lld}",
                   row ? "," : "", eq,
                   sqlite3_column_int64(st, 1),
                   sqlite3_column_int64(st, 2),
                   sqlite3_column_int64(st, 3), jm,
                   sqlite3_column_int64(st, 5));
            row++;
        }
        sqlite3_finalize(st);
    }
    printf("],\n");

    /* --- 查询类型分布 --- */
    printf("  \"qtypes\": [");
    {
        snprintf(sql, sizeof(sql),
            "SELECT qtype, COUNT(*) FROM dnsq"
            " WHERE ts_ms >= %llu AND client_ip IN (SELECT ip FROM _self_ips) GROUP BY qtype ORDER BY 2 DESC;",
            (unsigned long long)from);
        sqlite3_stmt *st = prep(sql);
        int row = 0;
        while (sqlite3_step(st) == SQLITE_ROW) {
            printf("%s{\"t\":%d,\"n\":%lld}", row ? "," : "",
                   sqlite3_column_int(st, 0), sqlite3_column_int64(st, 1));
            row++;
        }
        sqlite3_finalize(st);
    }
    printf("],\n");

    /* --- 应答码分布 --- */
    printf("  \"rcodes\": [");
    {
        snprintf(sql, sizeof(sql),
            "SELECT rcode, COUNT(*) FROM dnsq"
            " WHERE ts_ms >= %llu AND client_ip IN (SELECT ip FROM _self_ips) GROUP BY rcode ORDER BY 2 DESC;",
            (unsigned long long)from);
        sqlite3_stmt *st = prep(sql);
        int row = 0;
        while (sqlite3_step(st) == SQLITE_ROW) {
            printf("%s{\"r\":%d,\"n\":%lld}", row ? "," : "",
                   sqlite3_column_int(st, 0), sqlite3_column_int64(st, 1));
            row++;
        }
        sqlite3_finalize(st);
    }
    printf("],\n");

    /* --- 最近查询日志 --- */
    printf("  \"recent\": [");
    {
        snprintf(sql, sizeof(sql),
            "SELECT ts_ms, qname, qtype, rcode, ms, answers"
            " FROM dnsq WHERE ts_ms >= %llu AND client_ip IN (SELECT ip FROM _self_ips)"
            " ORDER BY ts_ms DESC LIMIT 300;", (unsigned long long)from);
        sqlite3_stmt *st = prep(sql);
        int row = 0;
        while (sqlite3_step(st) == SQLITE_ROW) {
            const unsigned char *q = sqlite3_column_text(st, 1);
            const unsigned char *a = sqlite3_column_text(st, 5);
            char eq[512], ea[256] = "", jm[32] = "null", cj[256];
            json_esc(q ? (const char *)q : "?", eq, sizeof(eq));
            json_esc(a ? (const char *)a : "", ea, sizeof(ea));
            if (sqlite3_column_type(st, 4) != SQLITE_NULL)
                snprintf(jm, sizeof(jm), "%.2f", sqlite3_column_double(st, 4));
            cls_json(q ? (const char *)q : "?", sqlite3_column_int(st, 3),
                     cj, sizeof(cj));
            printf("%s{%s\"ts\":%lld,\"q\":\"%s\",\"qtype\":%d,\"rcode\":%d,"
                   "\"ms\":%s,\"answers\":\"%s\"}",
                   row ? "," : "", cj, sqlite3_column_int64(st, 0), eq,
                   sqlite3_column_int(st, 2), sqlite3_column_int(st, 3),
                   jm, ea);
            row++;
        }
        sqlite3_finalize(st);
    }
    printf("],\n");

    /* --- 该设备访问量最大的目的地（conns.src 精确匹配 IPv4） --- */
    /* --- 该设备的 DNS 异常归因 --- */
    {
        long long cb = 0, cl = 0, cu = 0;
        struct { char q[256]; long long n; } top[12];
        int nt = 0;
        char s3[1024];
        snprintf(s3, sizeof(s3),
            "SELECT qname, rcode, COUNT(*) FROM dnsq"
            " WHERE ts_ms >= %llu AND client_ip IN (SELECT ip FROM _self_ips) AND rcode IN (2,3,5)"
            " GROUP BY qname, rcode ORDER BY COUNT(*) DESC;",
            (unsigned long long)from);
        sqlite3_stmt *s4 = prep(s3);
        while (sqlite3_step(s4) == SQLITE_ROW) {
            const unsigned char *q = sqlite3_column_text(s4, 0);
            int rc = sqlite3_column_int(s4, 1);
            long long n = sqlite3_column_int64(s4, 2);
            int cls = hprules_classify(q ? (const char *)q : "?", rc, NULL, 0);
            if (cls == HP_BLOCKED) cb += n;
            else if (cls == HP_LOCAL) cl += n;
            else if (cls == HP_UNRESOLVED) cu += n;
            if (nt < 12 && cls == HP_UNRESOLVED) {
                snprintf(top[nt].q, sizeof(top[0].q), "%s", q ? (const char *)q : "?");
                top[nt].n = n;
                nt++;
            }
        }
        sqlite3_finalize(s4);
        printf("  \"block_stats\": {\"blocked\":%lld,\"local\":%lld,"
               "\"unresolved\":%lld,\"top\":[", cb, cl, cu);
        for (int i = 0; i < nt; i++) {
            char eq[512];
            json_esc(top[i].q, eq, sizeof(eq));
            printf("%s{\"q\":\"%s\",\"n\":%lld}", i ? "," : "", eq, top[i].n);
        }
        printf("]},\n");
    }

    printf("  \"traffic\": [");
    {
        snprintf(sql, sizeof(sql),
            "SELECT COALESCE(host, dst), COUNT(*), SUM(byt_o), SUM(byt_r)"
            " FROM conns WHERE first_ms >= %llu AND src IN (SELECT ip FROM _self_ips)"
            " GROUP BY 1 ORDER BY SUM(byt_o+byt_r) DESC LIMIT 60;",
            (unsigned long long)from);
        sqlite3_stmt *st = prep(sql);
        int row = 0;
        while (sqlite3_step(st) == SQLITE_ROW) {
            const unsigned char *k = sqlite3_column_text(st, 0);
            char ek[512];
            json_esc(k ? (const char *)k : "?", ek, sizeof(ek));
            printf("%s{\"k\":\"%s\",\"conns\":%lld,\"out\":%.0f,\"in\":%.0f}",
                   row ? "," : "", ek,
                   sqlite3_column_int64(st, 1),
                   sqlite3_column_double(st, 2),
                   sqlite3_column_double(st, 3));
            row++;
        }
        sqlite3_finalize(st);
    }
    printf("]\n}\n");
    return 0;
}

/* ---------------- prune ---------------- */

/*
 * 删除指定目标的探测历史，或删除早于某个时间的探测记录。
 *
 * 用途：换掉不可达的探测目标后，把它的历史失败记录清掉，
 * 否则报表上永远挂着一行"100% 丢包"，看着像线路坏了。
 *
 * 只删 probe / dnsq，不碰 conns（那是真实流量记录，不能动）。
 */
static int cmd_prune(const char *target, uint64_t before_ms, int dry_run)
{
    if (!target && !before_ms) {
        fprintf(stderr, "prune 需要 --target 或 --before，至少一个\n");
        return 2;
    }

    /* 先数一下要删多少 */
    char sql[512];
    if (target)
        snprintf(sql, sizeof(sql),
            "SELECT COUNT(*) FROM probe WHERE target='%s'%s;",
            target, before_ms ? " AND ts_ms < " : "");
    else
        snprintf(sql, sizeof(sql),
            "SELECT COUNT(*) FROM probe WHERE ts_ms < %llu;",
            (unsigned long long)before_ms);

    /* target + before 组合需要拼上时间，重新构造一次 */
    if (target && before_ms)
        snprintf(sql, sizeof(sql),
            "SELECT COUNT(*) FROM probe WHERE target='%s' AND ts_ms < %llu;",
            target, (unsigned long long)before_ms);

    sqlite3_stmt *st = prep(sql);
    long long n = 0;
    if (sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);

    if (dry_run) {
        if (g_json)
            printf("{\"dry_run\":true,\"would_delete\":%lld}\n", n);
        else
            printf("将要删除 %lld 条探测记录（dry-run，未执行）\n", n);
        return 0;
    }

    if (n == 0) {
        if (g_json) printf("{\"deleted\":0}\n");
        else printf("没有匹配的探测记录，无需清理\n");
        return 0;
    }

    /* 删除 */
    if (target && before_ms)
        snprintf(sql, sizeof(sql),
            "DELETE FROM probe WHERE target='%s' AND ts_ms < %llu;",
            target, (unsigned long long)before_ms);
    else if (target)
        snprintf(sql, sizeof(sql),
            "DELETE FROM probe WHERE target='%s';", target);
    else
        snprintf(sql, sizeof(sql),
            "DELETE FROM probe WHERE ts_ms < %llu;",
            (unsigned long long)before_ms);

    char *err = NULL;
    if (sqlite3_exec(g_db, sql, NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "netmon-report: 删除失败: %s\n", err ? err : "?");
        sqlite3_free(err);
        return 2;
    }

    /* 回收空间，否则库文件不会变小 */
    sqlite3_exec(g_db, "PRAGMA incremental_vacuum;", NULL, NULL, NULL);

    if (g_json) printf("{\"deleted\":%lld}\n", n);
    else printf("已删除 %lld 条探测记录\n", n);
    return 0;
}

/* ---------------- DNS 异常归因（dashboard / client 共用） ---------------- */

/*
 * 24h 内 DNS 异常只有百来个唯一域名，先按 (qname,rcode) 聚合再逐个分类，
 * 比逐行扫 11 万条记录便宜得多。分类结果 g_cls_n 直接决定总览 KPI。
 */
typedef struct {
    char q[256];
    int  rcode;
    long long n;
    int  cls;
    char rule[64];
} badrow_t;

static badrow_t  g_bad[4096];
static int       g_nbad;
static long long g_cls_n[4];

static void collect_bad(uint64_t from)
{
    g_nbad = 0;
    memset(g_cls_n, 0, sizeof(g_cls_n));
    char sql[640];
    snprintf(sql, sizeof(sql),
        "SELECT qname, COUNT(*), MAX(rcode) FROM dnsq"
        " WHERE ts_ms >= %llu AND rcode IN (2,3,5)"
        " GROUP BY qname, rcode;", (unsigned long long)from);
    sqlite3_stmt *st = prep(sql);
    while (g_nbad < 4096 && sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char *q = sqlite3_column_text(st, 0);
        badrow_t *b = &g_bad[g_nbad++];
        snprintf(b->q, sizeof(b->q), "%s", q ? (const char *)q : "?");
        b->n     = sqlite3_column_int64(st, 1);
        b->rcode = sqlite3_column_int(st, 2);
        b->cls   = hprules_classify(b->q, b->rcode, b->rule, sizeof(b->rule));
        g_cls_n[b->cls] += b->n;
    }
    sqlite3_finalize(st);
}

/* netmon 尚未重启建表时，报表不能因为表不存在就整页报错 */
static int table_exists(const char *name)
{
    char sql[256];
    snprintf(sql, sizeof(sql),
        "SELECT 1 FROM sqlite_master WHERE type='table' AND name='%s';", name);
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(g_db, sql, -1, &st, NULL) != SQLITE_OK) return 0;
    int ok = (sqlite3_step(st) == SQLITE_ROW);
    sqlite3_finalize(st);
    return ok;
}

/* 输出 "cls":N,"rule":"...", —— 供 DNS 明细行复用 */
static void cls_json(const char *q, int rcode, char *out, size_t n)
{
    char rule[64], er[192];
    int cls = hprules_classify(q, rcode, rule, sizeof(rule));
    json_esc(rule, er, sizeof(er));
    snprintf(out, n, "\"cls\":%d,\"rule\":\"%s\",", cls, er);
}

/* 按次数取某类的 Top N（选择法，数据量小，不值得排序整个数组） */
static void bad_emit_top(int cls, int limit)
{
    char used[4096];
    memset(used, 0, sizeof(used));
    int row = 0;
    for (int k = 0; k < limit; k++) {
        int best = -1;
        for (int i = 0; i < g_nbad; i++) {
            if (used[i] || g_bad[i].cls != cls) continue;
            if (best < 0 || g_bad[i].n > g_bad[best].n) best = i;
        }
        if (best < 0) break;
        used[best] = 1;
        char eq[512], er[192];
        json_esc(g_bad[best].q, eq, sizeof(eq));
        json_esc(g_bad[best].rule, er, sizeof(er));
        printf("%s{\"q\":\"%s\",\"n\":%lld,\"rule\":\"%s\"}",
               row ? "," : "", eq, g_bad[best].n, er);
        row++;
    }
}

/* 按命中的规则聚合，回答「这些拦截分别来自哪条规则」 */
static void bad_emit_rules(void)
{
    typedef struct { char r[64]; long long n; } agg_t;
    static agg_t a[64];
    int na = 0;
    for (int i = 0; i < g_nbad; i++) {
        if (g_bad[i].cls != HP_BLOCKED) continue;
        const char *r = g_bad[i].rule[0] ? g_bad[i].rule : "未分类";
        int j;
        for (j = 0; j < na; j++) if (!strcmp(a[j].r, r)) break;
        if (j == na && na < 64) { snprintf(a[na].r, sizeof(a[0].r), "%s", r); a[na].n = 0; na++; }
        if (j < na) a[j].n += g_bad[i].n;
    }
    for (int i = 0; i < na; i++) {
        char er[192];
        json_esc(a[i].r, er, sizeof(er));
        printf("%s{\"rule\":\"%s\",\"n\":%lld}", i ? "," : "", er, a[i].n);
    }
}


/* ---------------- 系统状态（只读 /proc，零侵入） ----------------
 * 对标 pfSense / OPNsense 的 Dashboard widgets：System Information、
 * Interface Statistics、States Summary、Traffic Totals。
 * 全部数据来自 /proc 与已有聚合表，不抓包、不改防火墙、不产生额外流量。
 */
static void emit_sysinfo(uint64_t from)
{
    char line[1024];
    FILE *f;

    /* 运行时间与负载 */
    double up = 0;
    float l1 = 0, l5 = 0, l15 = 0;
    f = fopen("/proc/uptime", "r");
    if (f) { if (fscanf(f, "%lf", &up) != 1) up = 0; fclose(f); }
    f = fopen("/proc/loadavg", "r");
    if (f) { if (fscanf(f, "%f %f %f", &l1, &l5, &l15) != 3) { l1 = l5 = l15 = 0; } fclose(f); }

    /* 内存 */
    long long mem_total = 0, mem_avail = 0;
    f = fopen("/proc/meminfo", "r");
    if (f) {
        while (fgets(line, sizeof(line), f)) {
            if (!strncmp(line, "MemTotal:", 9))      sscanf(line + 9, "%lld", &mem_total);
            else if (!strncmp(line, "MemAvailable:", 13)) sscanf(line + 13, "%lld", &mem_avail);
        }
        fclose(f);
    }

    /* 数据盘容量 */
    long long disk_total = 0, disk_free = 0;
    struct statvfs sv;
    if (statvfs("/opt/netmon/data", &sv) == 0) {
        disk_total = (long long)sv.f_blocks * sv.f_frsize / 1048576;
        disk_free  = (long long)sv.f_bavail * sv.f_frsize / 1048576;
    }

    /* conntrack 状态摘要：只数与分类，不改任何连接 */
    long long ct_total = 0, ct_tcp = 0, ct_udp = 0, ct_est = 0;
    f = fopen("/proc/net/nf_conntrack", "r");
    if (f) {
        while (fgets(line, sizeof(line), f)) {
            ct_total++;
            if (strstr(line, "tcp")) ct_tcp++;
            else if (strstr(line, "udp")) ct_udp++;
            if (strstr(line, "ESTABLISHED")) ct_est++;
        }
        fclose(f);
    }

    printf("  \"sysinfo\": {\"uptime_s\":%.0f,\"load\":[%.2f,%.2f,%.2f],"
           "\"mem_total_kb\":%lld,\"mem_avail_kb\":%lld,"
           "\"disk_total_mb\":%lld,\"disk_free_mb\":%lld,"
           "\"ct_total\":%lld,\"ct_tcp\":%lld,\"ct_udp\":%lld,"
           "\"ct_established\":%lld,\"interfaces\":[",
           up, l1, l5, l15, mem_total, mem_avail,
           disk_total, disk_free, ct_total, ct_tcp, ct_udp, ct_est);
    f = fopen("/proc/net/dev", "r");
    int row = 0;
    if (f) {
        while (fgets(line, sizeof(line), f)) {
            char *colon = strchr(line, ':');
            if (!colon) continue;
            *colon = '\0';
            char name[64];
            if (sscanf(line, "%63s", name) != 1) continue;
            if (!strcmp(name, "lo")) continue;      /* 回环口对排障没意义 */
            long long rb, rp, re, rd, tb, tp, te, td;
            if (sscanf(colon + 1,
                       "%lld %lld %lld %lld %*s %*s %*s %*s "
                       "%lld %lld %lld %lld",
                       &rb, &rp, &re, &rd, &tb, &tp, &te, &td) != 8) continue;
            char en[80];
            json_esc(name, en, sizeof(en));
            printf("%s{\"name\":\"%s\",\"rx_bytes\":%lld,\"rx_pkts\":%lld,"
                   "\"rx_err\":%lld,\"rx_drop\":%lld,\"tx_bytes\":%lld,"
                   "\"tx_pkts\":%lld,\"tx_err\":%lld,\"tx_drop\":%lld}",
                   row ? "," : "", en, rb, rp, re, rd, tb, tp, te, td);
            row++;
        }
        fclose(f);
    }
    printf("]},\n");

    /* Traffic Totals：按天的全屋上下行总量，复用已有 agg_day 聚合表，
       不扫描明细，几乎零成本。 */
    printf("  \"daily\": [");
    {
        sqlite3_stmt *st = prep(
            "SELECT day_ms, SUM(bytes_o), SUM(bytes_r), SUM(conns)"
            " FROM agg_day GROUP BY day_ms ORDER BY day_ms DESC LIMIT 30;");
        row = 0;
        while (st && sqlite3_step(st) == SQLITE_ROW) {
            printf("%s{\"t\":%lld,\"out\":%.0f,\"in\":%.0f,\"conns\":%lld}",
                   row ? "," : "", sqlite3_column_int64(st, 0),
                   sqlite3_column_double(st, 1), sqlite3_column_double(st, 2),
                   sqlite3_column_int64(st, 3));
            row++;
        }
        if (st) sqlite3_finalize(st);
    }
    printf("],\n");
    (void)from;
}

/* ---------------- 流量排行 / 服务识别 / 隐私观察 ---------------- */

/*
 * 按「域名或目的 IP」聚合窗口内连接，供流量排行的目标榜使用。
 * 只查 conns 一张表，40 行结果，CGI 每 10 秒一次也毫无压力。
 */
/* 按「域名或目的 IP」聚合窗口内连接，供流量排行的目标榜使用。
   服务类别识别已停用：只靠域名关键词/端口推测准确率低，用户反馈价值不大。 */
/* 一周 7×24 活动热力图：固定看最近 7 天，与顶部所选时间范围无关，
 * 否则切到「1 小时」时热力图只剩一格，失去意义。
 *
 * 性能：ts_ms 是 idx_dnsq_tc 的首列，这里只做整数除法分组与计数，
 * 全程覆盖索引不回表；输出最多 200 个 {h,n} 点（7 天 = 169 小时）。
 * 星期与小时的定位交给浏览器用本地时区算，避免服务端硬编码 UTC+8。 */
static void emit_heatmap(uint64_t now)
{
    uint64_t from = now - 7ULL * 86400000ULL;
    char sql[320];
    snprintf(sql, sizeof(sql),
        "SELECT (ts_ms/3600000)*3600000 AS h, COUNT(*) FROM dnsq"
        " WHERE ts_ms >= %llu GROUP BY h ORDER BY h;",
        (unsigned long long)from);
    printf("  \"heatmap\": [");
    sqlite3_stmt *st = prep(sql);
    int row = 0;
    while (st && sqlite3_step(st) == SQLITE_ROW && row < 200) {
        printf("%s{\"h\":%lld,\"n\":%lld}", row ? "," : "",
               sqlite3_column_int64(st, 0), sqlite3_column_int64(st, 1));
        row++;
    }
    if (st) sqlite3_finalize(st);
    printf("],\n");
}

static void emit_traffic(uint64_t from)
{
    char sql[768];
    snprintf(sql, sizeof(sql),
        "SELECT COALESCE(host,dst), COUNT(*), SUM(byt_o), SUM(byt_r)"
        "  FROM conns WHERE first_ms >= %llu"
        " GROUP BY 1 ORDER BY SUM(byt_o+byt_r) DESC LIMIT 40;",
        (unsigned long long)from);
    sqlite3_stmt *st = prep(sql);
    printf("  \"traffic_hosts\": [");
    int row = 0;
    while (st && sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char *k = sqlite3_column_text(st, 0);
        char ek[512];
        json_esc(k ? (const char *)k : "?", ek, sizeof(ek));
        double bo = sqlite3_column_double(st, 2);
        double br = sqlite3_column_double(st, 3);
        printf("%s{\"k\":\"%s\",\"conns\":%lld,\"out\":%.0f,\"in\":%.0f,"
               "\"bytes\":%.0f}",
               row ? "," : "", ek, sqlite3_column_int64(st, 1),
               bo, br, bo + br);
        row++;
    }
    if (st) sqlite3_finalize(st);
    printf("],\n");
}

/* ---------------- dashboard（给前端用） ---------------- */

/*
 * 一次性输出前端需要的全部数据，单个 JSON。
 *
 * 为什么要合并成一个请求：
 *   路由器上每个 HTTP 请求都要 fork 一次 CGI 进程，多次请求既慢又浪费。
 *   一次拿完所有数据，前端只发一个 XHR。
 */
static int cmd_dashboard(uint64_t since_ms)
{
    uint64_t from = now_ms() - since_ms;
    uint64_t now  = now_ms();

    /* 每次请求都检查 homeproxy 规则指纹：配置或规则集一变，本次响应
       就用新规则重新归因。采集进程另有 30 秒轮询负责落变更事件。 */
    hprules_refresh(NULL);
    collect_bad(from);

    printf("{\n");

    /* --- 概况（一次拿全，前端顶部 KPI 全靠它） --- */
    {
        long long conns = 0, probes = 0, dnsn = 0;
        sqlite3_stmt *st = prep("SELECT COUNT(*) FROM conns;");
        if (sqlite3_step(st) == SQLITE_ROW) conns = sqlite3_column_int64(st, 0);
        sqlite3_finalize(st);

        st = prep("SELECT COUNT(*) FROM probe;");
        if (sqlite3_step(st) == SQLITE_ROW) probes = sqlite3_column_int64(st, 0);
        sqlite3_finalize(st);

        st = prep("SELECT COUNT(*) FROM dnsq;");
        if (sqlite3_step(st) == SQLITE_ROW) dnsn = sqlite3_column_int64(st, 0);
        sqlite3_finalize(st);

        st  = prep("PRAGMA page_count;");
        sqlite3_stmt *st2 = prep("PRAGMA page_size;");
        long long pages = 0, psz = 0;
        if (sqlite3_step(st) == SQLITE_ROW)  pages = sqlite3_column_int64(st, 0);
        if (sqlite3_step(st2) == SQLITE_ROW) psz   = sqlite3_column_int64(st2, 0);
        sqlite3_finalize(st); sqlite3_finalize(st2);

        /* 时间窗内的 DNS 全貌：查询数、活跃客户端、异常、平均耗时 */
        long long q = 0, rp = 0, nx = 0, rf = 0, clients = 0;
        double avgms = 0;
        char sql[768];
        snprintf(sql, sizeof(sql),
            "SELECT COUNT(*), SUM(CASE WHEN ms IS NOT NULL THEN 1 ELSE 0 END),"
            " SUM(CASE WHEN rcode=3 THEN 1 ELSE 0 END),"
            " SUM(CASE WHEN rcode=5 THEN 1 ELSE 0 END),"
            " AVG(ms), COUNT(DISTINCT client_ip)"
            " FROM dnsq WHERE ts_ms >= %llu;", (unsigned long long)from);
        st = prep(sql);
        if (sqlite3_step(st) == SQLITE_ROW) {
            q       = sqlite3_column_int64(st, 0);
            rp      = sqlite3_column_int64(st, 1);
            nx      = sqlite3_column_int64(st, 2);
            rf      = sqlite3_column_int64(st, 3);
            avgms   = sqlite3_column_double(st, 4);
            clients = sqlite3_column_int64(st, 5);
        }
        sqlite3_finalize(st);

        /* 时间窗内连接与流量 */
        long long cn = 0;
        double bo = 0, br = 0;
        snprintf(sql, sizeof(sql),
            "SELECT COUNT(*), SUM(byt_o), SUM(byt_r) FROM conns"
            " WHERE first_ms >= %llu;", (unsigned long long)from);
        st = prep(sql);
        if (sqlite3_step(st) == SQLITE_ROW) {
            cn = sqlite3_column_int64(st, 0);
            bo = sqlite3_column_double(st, 1);
            br = sqlite3_column_double(st, 2);
        }
        sqlite3_finalize(st);

        load_catalog();   /* catalog 总数也要给前端 */

        printf("  \"now_ms\": %llu,\n", (unsigned long long)now);
        printf("  \"since_ms\": %llu,\n", (unsigned long long)from);
        printf("  \"summary\": {\"conns\":%lld,\"probes\":%lld,\"dns\":%lld,"
               "\"db_bytes\":%lld,\"queries\":%lld,\"clients\":%lld,"
               "\"catalog\":%d,\"replied\":%lld,\"nx\":%lld,\"ref\":%lld,"
               "\"blocked\":%lld,\"reply_rate\":%.4f,\"avg_ms\":%.2f,"
               "\"win_conns\":%lld,\"bytes_out\":%.0f,\"bytes_in\":%.0f},\n",
               conns, probes, dnsn, pages * psz, q, clients, g_ndev,
               rp, nx, rf, nx + rf,
               q ? (double)rp / (double)q : 0.0, avgms, cn, bo, br);
    }

    /* --- 流量 Top（按网段） --- */
    printf("  \"top_cidr\": [");
    {
        char sql[512];
        snprintf(sql, sizeof(sql),
            "SELECT cidr, COUNT(*), SUM(byt_o), SUM(byt_r)"
            "  FROM conns WHERE first_ms >= %llu"
            " GROUP BY cidr ORDER BY SUM(byt_o+byt_r) DESC LIMIT 10;",
            (unsigned long long)from);
        sqlite3_stmt *st = prep(sql);
        int row = 0;
        while (sqlite3_step(st) == SQLITE_ROW) {
            const unsigned char *k = sqlite3_column_text(st, 0);
            char ek[128]; json_esc(k ? (const char *)k : "?", ek, sizeof(ek));
            printf("%s{\"k\":\"%s\",\"conns\":%lld,\"out\":%.0f,\"in\":%.0f}",
                   row ? "," : "", ek,
                   sqlite3_column_int64(st, 1),
                   sqlite3_column_double(st, 2),
                   sqlite3_column_double(st, 3));
            row++;
        }
        sqlite3_finalize(st);
    }
    printf("],\n");

    /* --- 流量 Top（按域名，反查命中的才有） --- */
    printf("  \"top_host\": [");
    {
        char sql[512];
        snprintf(sql, sizeof(sql),
            "SELECT host, COUNT(*), SUM(byt_o+byt_r)"
            "  FROM conns WHERE first_ms >= %llu AND host IS NOT NULL"
            " GROUP BY host ORDER BY SUM(byt_o+byt_r) DESC LIMIT 12;",
            (unsigned long long)from);
        sqlite3_stmt *st = prep(sql);
        int row = 0;
        while (sqlite3_step(st) == SQLITE_ROW) {
            const unsigned char *k = sqlite3_column_text(st, 0);
            char ek[256]; json_esc(k ? (const char *)k : "?", ek, sizeof(ek));
            printf("%s{\"k\":\"%s\",\"conns\":%lld,\"bytes\":%.0f}",
                   row ? "," : "", ek,
                   sqlite3_column_int64(st, 1),
                   sqlite3_column_double(st, 2));
            row++;
        }
        sqlite3_finalize(st);
    }
    printf("],\n");

    /* --- 时间趋势（小时桶，最近 24 个桶） --- */
    printf("  \"trend\": [");
    {
        char sql[512];
        snprintf(sql, sizeof(sql),
            "SELECT (first_ms/3600000)*3600000 AS b, COUNT(*),"
            "       SUM(byt_o), SUM(byt_r)"
            "  FROM conns WHERE first_ms >= %llu"
            " GROUP BY b ORDER BY b ASC;",
            (unsigned long long)from);
        sqlite3_stmt *st = prep(sql);
        int row = 0;
        while (sqlite3_step(st) == SQLITE_ROW) {
            printf("%s{\"t\":%lld,\"conns\":%lld,\"out\":%.0f,\"in\":%.0f}",
                   row ? "," : "",
                   sqlite3_column_int64(st, 0),
                   sqlite3_column_int64(st, 1),
                   sqlite3_column_double(st, 2),
                   sqlite3_column_double(st, 3));
            row++;
        }
        sqlite3_finalize(st);
    }
    printf("],\n");

    /* --- DNS 质量 Top ---
     * 字段语义与 cmd_dns 保持一致：replied 衡量链路，noerror/nxdomain/refused
     * 拆开看协议结果。前端用 replied/count 画健康度，用 rcode 分布做诊断。 */
    printf("  \"dns\": [");
    {
        char sql[640];
        snprintf(sql, sizeof(sql),
            "SELECT qname, COUNT(*),"
            "       SUM(CASE WHEN ms IS NOT NULL THEN 1 ELSE 0 END),"
            "       SUM(CASE WHEN rcode=0 THEN 1 ELSE 0 END),"
            "       SUM(CASE WHEN rcode=3 THEN 1 ELSE 0 END),"
            "       SUM(CASE WHEN rcode=5 THEN 1 ELSE 0 END),"
            "       AVG(ms), MAX(ms)"
            "  FROM dnsq WHERE ts_ms >= %llu"
            " GROUP BY qname ORDER BY COUNT(*) DESC LIMIT 15;",
            (unsigned long long)from);
        sqlite3_stmt *st = prep(sql);
        int row = 0;
        while (sqlite3_step(st) == SQLITE_ROW) {
            const unsigned char *q = sqlite3_column_text(st, 0);
            char eq[256]; json_esc(q ? (const char *)q : "?", eq, sizeof(eq));
            char ja[32] = "null", jm[32] = "null";
            if (sqlite3_column_type(st, 6) != SQLITE_NULL)
                snprintf(ja, sizeof(ja), "%.2f", sqlite3_column_double(st, 6));
            if (sqlite3_column_type(st, 7) != SQLITE_NULL)
                snprintf(jm, sizeof(jm), "%.2f", sqlite3_column_double(st, 7));
            /* 聚合行没有单一 rcode，用「是否出现过 REFUSED/NXDOMAIN」
               推定主要应答类型，让归因与明细行一致 */
            long long nxv  = sqlite3_column_int64(st, 4);
            long long refv = sqlite3_column_int64(st, 5);
            int mainrc = refv > 0 ? 5 : (nxv > 0 ? 3 : 0);
            char cj[256];
            cls_json(q ? (const char *)q : "?", mainrc, cj, sizeof(cj));
            printf("%s{%s\"q\":\"%s\",\"n\":%lld,\"replied\":%lld,"
                   "\"noerror\":%lld,\"nx\":%lld,\"ref\":%lld,"
                   "\"ms\":%s,\"ms_max\":%s}",
                   row ? "," : "", cj, eq,
                   sqlite3_column_int64(st, 1),
                   sqlite3_column_int64(st, 2),
                   sqlite3_column_int64(st, 3),
                   sqlite3_column_int64(st, 4),
                   sqlite3_column_int64(st, 5), ja, jm);
            row++;
        }
        sqlite3_finalize(st);
    }
    printf("],\n");

    /*
     * --- 按设备的 DNS 归属 ---
     * 前端用它画"哪台设备在查什么"。
     * 这里只看有 client_ip 的记录（本机自身查询没有设备归属）。
     */
    /*
     * --- 设备全量 ---
     * 与旧版的三点不同：
     *   1) 输出「已登记但本时段无活动」的设备，前端才能列出完整客户端清单；
     *   2) 身份来自统一目录（静态分配 + 租约 + 兜底），每台都有名字；
     *   3) DNS / conns 两路数据各用一条 GROUP BY 扫完，避免 N+1 子查询。
     */
    printf("  \"devices\": [");
    {
        load_catalog();
        g_nrows = 0;

        /* 先把目录里的设备都建出行，这样无活动的也会出现在结果里 */
        for (int i = 0; i < g_ndev; i++) row_for(g_dev[i].ip);

        char sql[1200];
        snprintf(sql, sizeof(sql),
            "SELECT client_ip, COUNT(*), COUNT(DISTINCT qname),"
            " SUM(CASE WHEN ms IS NOT NULL THEN 1 ELSE 0 END),"
            " SUM(CASE WHEN rcode=3 THEN 1 ELSE 0 END),"
            " SUM(CASE WHEN rcode=5 THEN 1 ELSE 0 END),"
            " AVG(ms), MIN(ts_ms), MAX(ts_ms)"
            " FROM dnsq WHERE ts_ms >= %llu"
            "   AND client_ip IS NOT NULL AND client_ip<>''"
            " GROUP BY client_ip;", (unsigned long long)from);
        sqlite3_stmt *st = prep(sql);
        while (sqlite3_step(st) == SQLITE_ROW) {
            const unsigned char *t = sqlite3_column_text(st, 0);
            const char *raw = t ? (const char *)t : "";
            devstat_t *r = row_for(raw);
            if (!r) continue;
            long long n     = sqlite3_column_int64(st, 1);
            long long first = sqlite3_column_int64(st, 7);
            long long last  = sqlite3_column_int64(st, 8);
            double    av    = sqlite3_column_double(st, 6);
            /* 归并后同一个规范地址会收到多行，必须累加而不是覆盖 */
            r->n       += n;
            r->hosts   += sqlite3_column_int64(st, 2);
            r->replied += sqlite3_column_int64(st, 3);
            r->nx      += sqlite3_column_int64(st, 4);
            r->ref     += sqlite3_column_int64(st, 5);
            r->ms_sum  += av * n;
            r->ms_n    += n;
            if (first && (!r->first_ms || first < r->first_ms)) r->first_ms = first;
            if (last > r->last_ms) {
                r->last_ms = last;
                snprintf(r->last_ip, sizeof(r->last_ip), "%s", raw);
            }
            if (!strcmp(raw, canon_ip(raw))) r->canon_n += n;
        }
        sqlite3_finalize(st);

        /* 按设备拆分的异常归因：哪些失败是广告拦截，哪些是真的要关注 */
        snprintf(sql, sizeof(sql),
            "SELECT client_ip, qname, rcode, COUNT(*) FROM dnsq"
            " WHERE ts_ms >= %llu AND rcode IN (2,3,5)"
            "   AND client_ip IS NOT NULL AND client_ip<>''"
            " GROUP BY client_ip, qname, rcode;", (unsigned long long)from);
        st = prep(sql);
        while (sqlite3_step(st) == SQLITE_ROW) {
            const unsigned char *ci = sqlite3_column_text(st, 0);
            const unsigned char *q  = sqlite3_column_text(st, 1);
            if (!ci || !q) continue;
            devstat_t *r = row_for((const char *)ci);
            if (!r) continue;
            int cls = hprules_classify((const char *)q,
                                       sqlite3_column_int(st, 2), NULL, 0);
            r->cls_n[cls] += sqlite3_column_int64(st, 3);
        }
        sqlite3_finalize(st);

        /* 每台设备最高频域名：窗口函数一次扫完，不再为每台跑一次子查询 */
        snprintf(sql, sizeof(sql),
            "SELECT client_ip, qname FROM ("
            "  SELECT client_ip, qname, COUNT(*) c,"
            "         ROW_NUMBER() OVER (PARTITION BY client_ip ORDER BY COUNT(*) DESC) rn"
            "    FROM dnsq WHERE ts_ms >= %llu"
            "     AND client_ip IS NOT NULL AND client_ip<>''"
            "   GROUP BY client_ip, qname"
            ") WHERE rn = 1;", (unsigned long long)from);
        st = prep(sql);
        while (sqlite3_step(st) == SQLITE_ROW) {
            const unsigned char *t = sqlite3_column_text(st, 0);
            const unsigned char *q = sqlite3_column_text(st, 1);
            devstat_t *r = row_for(t ? (const char *)t : "");
            if (r && q) json_esc((const char *)q, r->top_q, sizeof(r->top_q));
        }
        sqlite3_finalize(st);

        /* 连接数 / 上下行：只按 IPv4 源地址归属 */
        snprintf(sql, sizeof(sql),
            "SELECT src, COUNT(*), SUM(byt_o), SUM(byt_r) FROM conns"
            " WHERE first_ms >= %llu GROUP BY src;", (unsigned long long)from);
        st = prep(sql);
        while (sqlite3_step(st) == SQLITE_ROW) {
            const unsigned char *t = sqlite3_column_text(st, 0);
            if (!t) continue;
            /* 只累加到已存在的设备行：conns.src 还包含大量 WAN 侧公网地址，
               用 row_for 会把它们当成新设备、瞬间撑满 MAX_DEV。 */
            const char *key = canon_ip((const char *)t);
            for (int i = 0; i < g_nrows; i++) {
                if (strcmp(g_rows[i].ip, key)) continue;
                g_rows[i].conns += sqlite3_column_int64(st, 1);
                g_rows[i].bo    += sqlite3_column_double(st, 2);
                g_rows[i].br    += sqlite3_column_double(st, 3);
                break;
            }
        }
        sqlite3_finalize(st);

        /* 按查询数降序输出；同为零的按 IP 排，保证顺序稳定 */
        int order[MAX_DEV];
        for (int i = 0; i < g_nrows; i++) order[i] = i;
        for (int i = 1; i < g_nrows; i++) {
            int key = order[i], j = i - 1;
            while (j >= 0 && g_rows[order[j]].n < g_rows[key].n) {
                order[j + 1] = order[j]; j--;
            }
            order[j + 1] = key;
        }

        for (int k = 0; k < g_nrows; k++) {
            devstat_t *r = &g_rows[order[k]];
            devrec_t *d = dev_find(r->ip);
            char dj[1536], et[512], ea[360], eact[64];
            dev_json(d, dj, sizeof(dj));
            json_esc(r->top_q, et, sizeof(et));
            const char *act = r->last_ip[0] ? r->last_ip : r->ip;
            json_esc(act, eact, sizeof(eact));
            json_esc(d ? d->aliases : "", ea, sizeof(ea));
            /* 配了静态分配但该地址窗口内没有任何查询、旧地址却有活动：
               说明设备还没重连拿到新 IP，明确提示而不是静默合并。 */
            int stale = (d && d->source == DEV_STATIC &&
                         r->canon_n == 0 && r->n > 0) ? 1 : 0;
            printf("%s{\"device\":%s,\"n\":%lld,\"hosts\":%lld,"
                   "\"ok_rate\":%.4f,\"nx\":%lld,\"ref\":%lld,"
                   "\"avg_ms\":%.2f,\"first_ms\":%lld,\"last_ms\":%lld,"
                   "\"conns\":%lld,\"bytes_out\":%.0f,\"bytes_in\":%.0f,"
                   "\"top_q\":\"%s\",\"active\":%d,"
                   "\"aliases\":\"%s\",\"active_ip\":\"%s\",\"stale_static\":%d,"
                   "\"multi_nic\":%d,"
                   "\"blocked\":%lld,\"local\":%lld,\"unresolved\":%lld}",
                   k ? "," : "", dj, r->n, r->hosts,
                   r->n ? (double)r->replied / (double)r->n : 0.0,
                   r->nx, r->ref,
                   r->ms_n ? r->ms_sum / (double)r->ms_n : 0.0,
                   r->first_ms, r->last_ms,
                   r->conns, r->bo, r->br, et, r->n > 0 ? 1 : 0,
                   ea, eact, stale, d ? d->multi_nic : 0,
                   r->cls_n[HP_BLOCKED], r->cls_n[HP_LOCAL],
                   r->cls_n[HP_UNRESOLVED]);
        }
    }
    printf("],\n");

    /* --- 全局 DNS 活动曲线（小时桶） --- */
    printf("  \"dns_timeline\": [");
    {
        char sql[768];
        snprintf(sql, sizeof(sql),
            "SELECT (ts_ms/3600000)*3600000, COUNT(*),"
            " SUM(CASE WHEN rcode IN (3,5) THEN 1 ELSE 0 END), AVG(ms)"
            " FROM dnsq WHERE ts_ms >= %llu GROUP BY 1 ORDER BY 1;",
            (unsigned long long)from);
        sqlite3_stmt *st = prep(sql);
        int row = 0;
        while (sqlite3_step(st) == SQLITE_ROW) {
            char jm[32] = "null";
            if (sqlite3_column_type(st, 3) != SQLITE_NULL)
                snprintf(jm, sizeof(jm), "%.2f", sqlite3_column_double(st, 3));
            printf("%s{\"t\":%lld,\"n\":%lld,\"blocked\":%lld,\"ms\":%s}",
                   row ? "," : "",
                   sqlite3_column_int64(st, 0),
                   sqlite3_column_int64(st, 1),
                   sqlite3_column_int64(st, 2), jm);
            row++;
        }
        sqlite3_finalize(st);
    }
    printf("],\n");

    /* --- 最近查询日志（全局，带设备名） --- */
    printf("  \"dns_recent\": [");
    {
        char sql[768];
        snprintf(sql, sizeof(sql),
            "SELECT ts_ms, qname, qtype, rcode, ms, answers, client_ip"
            " FROM dnsq WHERE ts_ms >= %llu ORDER BY ts_ms DESC LIMIT 200;",
            (unsigned long long)from);
        sqlite3_stmt *st = prep(sql);
        int row = 0;
        while (sqlite3_step(st) == SQLITE_ROW) {
            const unsigned char *q  = sqlite3_column_text(st, 1);
            const unsigned char *a  = sqlite3_column_text(st, 5);
            const unsigned char *ci = sqlite3_column_text(st, 6);
            char eq[512], ea[256] = "", eip[64] = "", enm[256] = "";
            json_esc(q ? (const char *)q : "?", eq, sizeof(eq));
            json_esc(a ? (const char *)a : "", ea, sizeof(ea));
            const char *raw_ip = ci ? (const char *)ci : "";
            const char *linked = canon_ip(raw_ip);
            json_esc(linked, eip, sizeof(eip));
            if (linked[0]) {
                devrec_t *d = dev_find(linked);
                if (d) json_esc(d->display, enm, sizeof(enm));
            }
            char jm[32] = "null", cj[256];
            if (sqlite3_column_type(st, 4) != SQLITE_NULL)
                snprintf(jm, sizeof(jm), "%.2f", sqlite3_column_double(st, 4));
            cls_json(q ? (const char *)q : "?", sqlite3_column_int(st, 3),
                     cj, sizeof(cj));
            printf("%s{%s\"ts\":%lld,\"q\":\"%s\",\"qtype\":%d,\"rcode\":%d,"
                   "\"ms\":%s,\"answers\":\"%s\",\"ip\":\"%s\",\"name\":\"%s\"}",
                   row ? "," : "", cj, sqlite3_column_int64(st, 0), eq,
                   sqlite3_column_int(st, 2), sqlite3_column_int(st, 3),
                   jm, ea, eip, enm);
            row++;
        }
        sqlite3_finalize(st);
    }
    printf("],\n");

    emit_sysinfo(from);
    emit_traffic(from);

    /* --- 拦截归因总览 --- */
    {
        long long tot = g_cls_n[HP_BLOCKED] + g_cls_n[HP_LOCAL] +
                        g_cls_n[HP_UNRESOLVED];
        printf("  \"block_stats\": {\"total\":%lld,\"blocked\":%lld,"
               "\"local\":%lld,\"unresolved\":%lld,\"blocked_pct\":%.2f,"
               "\"top_blocked\":[",
               tot, g_cls_n[HP_BLOCKED], g_cls_n[HP_LOCAL],
               g_cls_n[HP_UNRESOLVED],
               tot ? (double)g_cls_n[HP_BLOCKED] / (double)tot * 100.0 : 0.0);
        bad_emit_top(HP_BLOCKED, 60);
        printf("],\"top_unresolved\":[");
        bad_emit_top(HP_UNRESOLVED, 60);
        printf("],\"by_rule\":[");
        bad_emit_rules();
        printf("]},\n");
    }

    /* --- 一周活动热力图（固定 7 天窗口）--- */
    emit_heatmap(now);

    /* --- homeproxy 规则版本与变更历史 --- */
    printf("  \"hp\": {");
    {
        const hp_version_t *hv = hprules_version();
        char efp[128], ed[512], emj[8192];
        json_esc(hv->fp, efp, sizeof(efp));
        json_esc(hv->detail, ed, sizeof(ed));
        char ers[256];
        json_esc(hprules_reason(), ers, sizeof(ers));
        printf("\"available\":%d,\"reason\":\"%s\","
               "\"fp\":\"%s\",\"changed_ms\":%llu,\"rules\":%d,"
               "\"patterns\":%d,\"detail\":\"%s\",\"rules_list\":[",
               hprules_available(), ers,
               efp, (unsigned long long)hv->changed_ms,
               hv->rules, hv->patterns, ed);
        hprules_meta_json(emj, sizeof(emj));
        printf("%s],\"events\":[", emj);
        if (table_exists("hp_event")) {
            sqlite3_stmt *ev = prep(
                "SELECT ts_ms,fp,detail,rules,patterns FROM hp_event"
                " ORDER BY ts_ms DESC LIMIT 12;");
            int r = 0;
            while (sqlite3_step(ev) == SQLITE_ROW) {
                const unsigned char *f = sqlite3_column_text(ev, 1);
                const unsigned char *d = sqlite3_column_text(ev, 2);
                char e1[128], e2[512];
                json_esc(f ? (const char *)f : "", e1, sizeof(e1));
                json_esc(d ? (const char *)d : "", e2, sizeof(e2));
                printf("%s{\"ts\":%lld,\"fp\":\"%s\",\"detail\":\"%s\","
                       "\"rules\":%d,\"patterns\":%d}", r ? "," : "",
                       sqlite3_column_int64(ev, 0), e1, e2,
                       sqlite3_column_int(ev, 3), sqlite3_column_int(ev, 4));
                r++;
            }
            sqlite3_finalize(ev);
        }
        printf("]}\n");
    }

    printf("}\n");
    return 0;
}

/* ---------------- status ---------------- */

static int cmd_status(const char *path)
{
    struct { const char *name; const char *sql; } q[] = {
        { "conns",    "SELECT COUNT(*) FROM conns;" },
        { "agg_hour", "SELECT COUNT(*) FROM agg_hour;" },
        { "agg_day",  "SELECT COUNT(*) FROM agg_day;" },
        { "probe",    "SELECT COUNT(*) FROM probe;" },
        { "dnsq",     "SELECT COUNT(*) FROM dnsq;" },
    };

    long long counts[5] = {0};
    for (int i = 0; i < 5; i++) {
        sqlite3_stmt *st = prep(q[i].sql);
        if (sqlite3_step(st) == SQLITE_ROW)
            counts[i] = sqlite3_column_int64(st, 0);
        sqlite3_finalize(st);
    }

    /* 时间覆盖范围 */
    sqlite3_stmt *st = prep("SELECT MIN(first_ms), MAX(last_ms) FROM conns;");
    uint64_t tmin = 0, tmax = 0;
    if (sqlite3_step(st) == SQLITE_ROW) {
        tmin = (uint64_t)sqlite3_column_int64(st, 0);
        tmax = (uint64_t)sqlite3_column_int64(st, 1);
    }
    sqlite3_finalize(st);

    /* 库文件体积 */
    sqlite3_stmt *p1 = prep("PRAGMA page_count;");
    sqlite3_stmt *p2 = prep("PRAGMA page_size;");
    long long pages = 0, psize = 0;
    if (sqlite3_step(p1) == SQLITE_ROW) pages = sqlite3_column_int64(p1, 0);
    if (sqlite3_step(p2) == SQLITE_ROW) psize = sqlite3_column_int64(p2, 0);
    sqlite3_finalize(p1);
    sqlite3_finalize(p2);
    double dbb = (double)pages * (double)psize;

    if (g_json) {
        printf("{\"db\":\"%s\",\"conns\":%lld,\"agg_hour\":%lld,\"agg_day\":%lld,"
               "\"probe\":%lld,\"dnsq\":%lld,\"db_bytes\":%.0f,"
               "\"first_ms\":%llu,\"last_ms\":%llu}\n",
               path, counts[0], counts[1], counts[2], counts[3], counts[4],
               dbb, (unsigned long long)tmin, (unsigned long long)tmax);
        return 0;
    }

    char hb[24], ts1[32], ts2[32];
    human_bytes(dbb, hb, sizeof(hb));

    printf("\n数据库: %s\n", path);
    printf("  体积      : %s\n", hb);
    printf("  连接明细  : %lld 行\n", counts[0]);
    printf("  小时聚合  : %lld 行\n", counts[1]);
    printf("  天聚合    : %lld 行\n", counts[2]);
    printf("  探测记录  : %lld 行\n", counts[3]);
    printf("  DNS 记录  : %lld 行\n", counts[4]);

    if (tmin) {
        ts_str(tmin, ts1, sizeof(ts1));
        ts_str(tmax, ts2, sizeof(ts2));
        printf("  数据范围  : %s ~ %s", ts1, ts2);
        double days = (double)(tmax - tmin) / 86400000.0;
        if (days >= 0.01) printf("（%.1f 天）", days);
        printf("\n");
    } else {
        printf("  数据范围  : （空库）\n");
    }
    printf("\n");
    return 0;
}

/* ---------------- main ---------------- */

int main(int argc, char **argv)
{
    const char *dbp = "/opt/netmon/data/netmon.db";
    if (getenv("NETMON_DB")) dbp = getenv("NETMON_DB");

    if (argc < 2) { usage(); return 0; }

    const char *cmd = argv[1];
    if (!strcmp(cmd, "-h") || !strcmp(cmd, "--help") || !strcmp(cmd, "help")) {
        usage();
        return 0;
    }

    uint64_t since = 24ULL * 3600 * 1000;   /* 默认 24 小时 */
    int n = 20;
    const char *by = "cidr";
    const char *unit = "hour";
    const char *only_target = NULL;
    const char *client_ip = NULL;
    const char *detail_type = NULL;
    const char *detail_key  = NULL;
    uint64_t before_ms = 0;
    int dry_run = 0;

    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "-j") || !strcmp(argv[i], "--json")) {
            g_json = 1;
        } else if ((!strcmp(argv[i], "-d") || !strcmp(argv[i], "--db")) && i + 1 < argc) {
            dbp = argv[++i];
        } else if (!strcmp(argv[i], "--since") && i + 1 < argc) {
            since = parse_span(argv[++i]);
        } else if ((!strcmp(argv[i], "-n") || !strcmp(argv[i], "--limit")) && i + 1 < argc) {
            n = atoi(argv[++i]);
            if (n <= 0) n = 20;
        } else if (!strcmp(argv[i], "--by") && i + 1 < argc) {
            by = argv[++i];
        } else if (!strcmp(argv[i], "--unit") && i + 1 < argc) {
            unit = argv[++i];
        } else if (!strcmp(argv[i], "--target") && i + 1 < argc) {
            only_target = argv[++i];
        } else if (!strcmp(argv[i], "--ip") && i + 1 < argc) {
            client_ip = argv[++i];
        } else if (!strcmp(argv[i], "--type") && i + 1 < argc) {
            detail_type = argv[++i];
        } else if (!strcmp(argv[i], "--key") && i + 1 < argc) {
            detail_key = argv[++i];
        } else if (!strcmp(argv[i], "--before") && i + 1 < argc) {
            /* --before 24h 表示"删除 24 小时之前的" */
            before_ms = now_ms() - parse_span(argv[++i]);
        } else if (!strcmp(argv[i], "--dry-run")) {
            dry_run = 1;
        } else {
            fprintf(stderr, "未知参数: %s\n", argv[i]);
            usage();
            return 2;
        }
    }

    /* prune 需要写权限，其他命令一律只读 */
    g_rw = !strcmp(cmd, "prune");
    /* client 依赖窗口函数与 bind，打开库前先挡掉非法 IP，省一次无谓 IO */
    if (!strcmp(cmd, "client") && !client_ip) {
        fprintf(stderr, "client 需要 --ip IP\n");
        return 2;
    }

    /*
     * status 也要先 open，因为它要读 PRAGMA；
     * 但库文件不存在时给出更明确的提示而不是 SQLite 的含糊报错。
     */
    if (open_db(dbp, !g_rw) != 0) {
        fprintf(stderr, "提示: 数据库可能还没生成。确认 netmon 服务在跑："
                        "/etc/init.d/netmon status\n");
        return 2;
    }

    /* 规则同步只做一次：hprules_init 会强制全量加载，
       之后各命令用 hprules_refresh() 按指纹增量更新 */
    hprules_init("/var/run/homeproxy/cache.db",
                 "/etc/config/homeproxy", "/opt/netmon/data");

    int rc = 0;
    if      (!strcmp(cmd, "top"))    rc = cmd_top(since, n, by);
    else if (!strcmp(cmd, "trend"))  rc = cmd_trend(since, unit);
    else if (!strcmp(cmd, "probe"))  rc = cmd_probe(since, only_target);
    else if (!strcmp(cmd, "dns"))    rc = cmd_dns(since);
    else if (!strcmp(cmd, "devices")) rc = cmd_devices(since);
    else if (!strcmp(cmd, "client"))   rc = cmd_client(since, client_ip);
    else if (!strcmp(cmd, "detail")) {
        if (!detail_valid(detail_type, detail_key)) {
            printf("{\"error\":\"invalid detail request\"}\n");
            rc = 2;
        } else {
            rc = detail_emit(g_db, detail_type, detail_key, since, detail_name_cb);
        }
    }
    else if (!strcmp(cmd, "status")) rc = cmd_status(dbp);
    else if (!strcmp(cmd, "prune"))  rc = cmd_prune(only_target, before_ms, dry_run);
    else if (!strcmp(cmd, "dashboard")) rc = cmd_dashboard(since);
    else {
        fprintf(stderr, "未知命令: %s\n\n", cmd);
        usage();
        rc = 2;
    }

    sqlite3_close(g_db);
    return rc;
}
