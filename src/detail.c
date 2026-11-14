/*
 * detail.c - 通用详情下钻实现
 *
 * 设计要点：
 *   1) 所有用户输入都走 sqlite3_bind，SQL 里只有 ? 占位符；
 *   2) 四类实体共用同一套 JSON 结构，前端一个渲染函数就能画；
 *   3) 明细限 60 条、设备限 20 台，避免 CGI 一次拉出几万行；
 *   4) 只读，不写任何表。
 */
#include "detail.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <time.h>

#define MAX_DEV_ROWS   20
#define MAX_TIME_ROWS  256
#define MAX_CONN_ROWS  60
#define MAX_DNS_ROWS   60

/* server_label 展示映射由 netmon-report.c 提供（全局函数），
   避免把公共解析器静态表在 detail.c 里复制一份。 */
extern void dns_label(const char *ip, char *out, size_t n);

/* 老库列容错：detail 只读打开，1.0.6 老库没有 1.1.0 的新列。
   每次 detail_emit 用 PRAGMA table_info 探测一次并缓存；所有新列引用
   走下面的表达式宏，列不存在时退化成合法 SQL 字面量（NULL / 常量）。
   table / col 均为内部常量，不来自用户输入。 */
typedef struct {
    int probed;
    int d_server_ip, d_ttl, d_answer_ips;                  /* dnsq */
    int c_client_ip;                                       /* conns */
} dcaps_t;
static dcaps_t g_dcaps;

static int dc_has_column(sqlite3 *db, const char *table, const char *col)
{
    char sql[128];
    sqlite3_stmt *st = NULL;
    snprintf(sql, sizeof(sql), "PRAGMA table_info(%s);", table);
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) return 0;
    int ok = 0;
    while (sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char *nm = sqlite3_column_text(st, 1);
        if (nm && !strcmp((const char *)nm, col)) { ok = 1; break; }
    }
    sqlite3_finalize(st);
    return ok;
}

static void dc_probe(sqlite3 *db)
{
    if (g_dcaps.probed) return;
    g_dcaps.probed = 1;
    g_dcaps.d_server_ip   = dc_has_column(db, "dnsq", "server_ip");
    g_dcaps.d_ttl         = dc_has_column(db, "dnsq", "ttl");
    g_dcaps.d_answer_ips  = dc_has_column(db, "dnsq", "answer_ips");
    g_dcaps.c_client_ip   = dc_has_column(db, "conns", "client_ip");
}

#define DC_SERVER_IP   (g_dcaps.d_server_ip   ? "server_ip"   : "NULL")
#define DC_TTL         (g_dcaps.d_ttl         ? "ttl"         : "NULL")
#define DC_ANSWER_IPS  (g_dcaps.d_answer_ips  ? "answer_ips"  : "NULL")
#define DC_CLIENT_IP   (g_dcaps.c_client_ip   ? "client_ip"   : "NULL")

/* 旧行 answers 是 "0"/"1" 计数串 → 归一成空串；新行优先 answer_ips
   列（COALESCE），显示真实应答 IP。 */
static const char *dc_norm_answers(const char *a)
{
    if (!a || !*a) return "";
    if (!strcmp(a, "0") || !strcmp(a, "1")) return "";
    return a;
}

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000);
}

static void jesc(const char *s, char *out, size_t n)
{
    size_t o = 0;
    if (!s) { if (n) out[0] = 0; return; }
    for (; *s && o + 8 < n; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = (char)c; }
        else if (c < 0x20) o += (size_t)snprintf(out + o, n - o, "\\u%04x", c);
        else out[o++] = (char)c;
    }
    out[o] = 0;
}

int detail_valid(const char *type, const char *key)
{
    if (!type || !key || !*key || strlen(key) > 200) return 0;
    if (strcmp(type, "host") && strcmp(type, "qname") &&
        strcmp(type, "cidr"))
        return 0;
    /* 前端用 encodeURIComponent 传键，所以允许 %XX；
       >=0x80 是为了兼容未编码的 UTF-8（中文服务类别等）。
       所有键最终都走 sqlite3_bind，放宽字符集不带来注入风险。 */
    for (const char *p = key; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (isalnum(c) || c == '.' || c == '-' || c == '_' ||
            c == ':' || c == '%' || c >= 0x80) continue;
        return 0;
    }
    return 1;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void url_decode(const char *src, char *out, size_t n)
{
    size_t o = 0;
    for (; *src && o + 1 < n; src++) {
        if (*src == '%' && hexval(src[1]) >= 0 && hexval(src[2]) >= 0) {
            out[o++] = (char)(hexval(src[1]) * 16 + hexval(src[2]));
            src += 2;
        } else if (*src == '+') {
            out[o++] = ' ';
        } else {
            out[o++] = *src;
        }
    }
    out[o] = 0;
}

/* 解码后再挡一次：%XX 能把任意字符送进来。虽然所有键都走 sqlite3_bind、
   不存在注入，但把 SQL 片段原样回显到页面上既难看也容易引起误解。
   允许 UTF-8（中文服务类别）、域名、IPv6、CIDR 的 / 与类别名中的空格。 */
static int decoded_key_ok(const char *k)
{
    for (const unsigned char *p = (const unsigned char *)k; *p; p++) {
        unsigned char c = *p;
        if (c >= 0x80) continue;
        if (isalnum(c) || c == '.' || c == '-' || c == '_' || c == ':' ||
            c == '/' || c == ' ') continue;
        return 0;
    }
    return 1;
}

static sqlite3_stmt *prep(sqlite3 *db, const char *sql)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) return NULL;
    return st;
}

/* 三条匹配路径都绑同一个键：反查域名、目的 IP、目的网段 */
static void bind_entity(sqlite3_stmt *st, uint64_t from, const char *key)
{
    sqlite3_bind_int64(st, 1, (sqlite3_int64)from);
    sqlite3_bind_text (st, 2, key, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (st, 3, key, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (st, 4, key, -1, SQLITE_TRANSIENT);
}

#define ENT_COND "(host=? OR dst=? OR cidr=?)"

typedef struct {
    char     ip[64];
    char     name[256];
    long long conns, bo, br, last_ms;
} devrow_t;

typedef struct {
    long long t, conns, bo, br;
} timerow_t;

static void emit_header(const char *type, const char *key, uint64_t from,
                        const char *title)
{
    char ek[512], et[512];
    jesc(key, ek, sizeof(ek));
    jesc(title, et, sizeof(et));
    printf("{\"type\":\"%s\",\"key\":\"%s\",\"title\":\"%s\","
           "\"since_ms\":%llu,", type, ek, et, (unsigned long long)from);
}

/* ---------------- host / qname / cidr ---------------- */
static int emit_entity(sqlite3 *db, const char *type, const char *key,
                       uint64_t from, detail_name_fn name_fn)
{
    char title[512], app[64] = "", ek[512], sql[1024];
    const char *cat = "其他";
    int is_q = !strcmp(type, "qname");   /* DNS 域名走 dnsq 聚合，不靠 conns */
    int row = 0;                         /* 各明细段的输出行计数器，全函数复用 */
    dc_probe(db);                        /* 老库列容错：探测一次并缓存 */
    snprintf(title, sizeof(title), "%s · %s",
             !strcmp(type, "qname") ? "DNS 域名" :
             !strcmp(type, "cidr") ? "目的网段" : "域名 / IP", key);
    emit_header(type, key, from, title);

    /* --- 概况 --- */
    long long conns = 0, devs = 0;
    double bo = 0, br = 0;
    long long first = 0, last = 0;
    snprintf(sql, sizeof(sql),
        "SELECT COUNT(*),SUM(byt_o),SUM(byt_r),MIN(first_ms),MAX(last_ms),"
        "COUNT(DISTINCT COALESCE(%s, src)) FROM conns"
        " WHERE first_ms>=? AND " ENT_COND ";", DC_CLIENT_IP);
    sqlite3_stmt *st = prep(db, sql);
    if (st) {
        bind_entity(st, from, key);
        if (sqlite3_step(st) == SQLITE_ROW) {
            conns = sqlite3_column_int64(st, 0);
            bo = sqlite3_column_double(st, 1);
            br = sqlite3_column_double(st, 2);
            first = sqlite3_column_int64(st, 3);
            last = sqlite3_column_int64(st, 4);
            devs = sqlite3_column_int64(st, 5);
        }
        sqlite3_finalize(st);
    }
    long long dnsc = 0;
    st = prep(db, "SELECT COUNT(*) FROM dnsq WHERE ts_ms>=? AND qname=?;");
    if (st) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)from);
        sqlite3_bind_text(st, 2, key, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(st) == SQLITE_ROW) dnsc = sqlite3_column_int64(st, 0);
        sqlite3_finalize(st);
    }
    /* 域名的「关联设备 / 首末时间」只能从 dnsq 取；被拦截的域名根本没有连接 */
    if (is_q) {
        st = prep(db, "SELECT COUNT(*),COUNT(DISTINCT client_ip),"
                      "MIN(ts_ms),MAX(ts_ms) FROM dnsq"
                      " WHERE ts_ms>=? AND qname=?;");
        if (st) {
            sqlite3_bind_int64(st, 1, (sqlite3_int64)from);
            sqlite3_bind_text(st, 2, key, -1, SQLITE_TRANSIENT);
            if (sqlite3_step(st) == SQLITE_ROW) {
                dnsc = sqlite3_column_int64(st, 0);
                devs = sqlite3_column_int64(st, 1);
                first = sqlite3_column_int64(st, 2);
                last = sqlite3_column_int64(st, 3);
            }
            sqlite3_finalize(st);
        }
    }
    char ea[128], ec[96];
    jesc(app, ea, sizeof(ea)); jesc(cat, ec, sizeof(ec));
    printf("\"summary\":{\"conns\":%lld,\"bytes_out\":%.0f,\"bytes_in\":%.0f,"
           "\"bytes_total\":%.0f,\"devices\":%lld,\"dns\":%lld,"
           "\"first_ms\":%lld,\"last_ms\":%lld,\"app\":\"%s\",\"cat\":\"%s\"},",
           conns, bo, br, bo + br, devs, dnsc, first, last, ea, ec);

    /* --- 关联设备 --- */
    printf("\"devices\":[");
    if (is_q) {
        st = prep(db,
            "SELECT COALESCE(client_ip,''),COUNT(*),MAX(ts_ms) FROM dnsq"
            " WHERE ts_ms>=? AND qname=? AND client_ip IS NOT NULL"
            " GROUP BY 1 ORDER BY COUNT(*) DESC LIMIT 20;");
        if (st) {
            sqlite3_bind_int64(st, 1, (sqlite3_int64)from);
            sqlite3_bind_text(st, 2, key, -1, SQLITE_TRANSIENT);
            int r2 = 0;
            while (sqlite3_step(st) == SQLITE_ROW) {
                const unsigned char *ip = sqlite3_column_text(st, 0);
                const char *sip = ip ? (const char *)ip : "";
                if (!*sip) continue;
                char nb[256], e1[96], e2[512];
                name_fn(sip, nb, sizeof(nb));
                jesc(sip, e1, sizeof(e1)); jesc(nb, e2, sizeof(e2));
                printf("%s{\"ip\":\"%s\",\"name\":\"%s\",\"conns\":%lld,"
                       "\"out\":0,\"in\":0,\"last_ms\":%lld}",
                       r2 ? "," : "", e1, e2,
                       sqlite3_column_int64(st, 1),
                       sqlite3_column_int64(st, 2));
                r2++;
            }
            sqlite3_finalize(st);
        }
    } else {
        snprintf(sql, sizeof(sql),
            "SELECT COALESCE(%s, src),COUNT(*),SUM(byt_o),SUM(byt_r),MAX(last_ms)"
            " FROM conns WHERE first_ms>=? AND " ENT_COND
            " GROUP BY 1 ORDER BY SUM(byt_o+byt_r) DESC LIMIT 20;",
            DC_CLIENT_IP);
        st = prep(db, sql);
        row = 0;
        if (st) {
            bind_entity(st, from, key);
            while (sqlite3_step(st) == SQLITE_ROW) {
                const unsigned char *ip = sqlite3_column_text(st, 0);
                const char *sip = ip ? (const char *)ip : "";
                char nb[256], e1[96], e2[512];
                name_fn(sip, nb, sizeof(nb));
                jesc(sip, e1, sizeof(e1)); jesc(nb, e2, sizeof(e2));
                printf("%s{\"ip\":\"%s\",\"name\":\"%s\",\"conns\":%lld,"
                       "\"out\":%.0f,\"in\":%.0f,\"last_ms\":%lld}",
                       row ? "," : "", e1, e2,
                       sqlite3_column_int64(st, 1),
                       sqlite3_column_double(st, 2),
                       sqlite3_column_double(st, 3),
                       sqlite3_column_int64(st, 4));
                row++;
            }
            sqlite3_finalize(st);
        }
    }
    printf("],");

    /* --- 小时时间线 --- */
    printf("\"timeline\":[");
    if (is_q) {
        st = prep(db,
            "SELECT (ts_ms/3600000)*3600000,COUNT(*) FROM dnsq"
            " WHERE ts_ms>=? AND qname=? GROUP BY 1 ORDER BY 1;");
        if (st) {
            sqlite3_bind_int64(st, 1, (sqlite3_int64)from);
            sqlite3_bind_text(st, 2, key, -1, SQLITE_TRANSIENT);
            int r3 = 0;
            while (sqlite3_step(st) == SQLITE_ROW) {
                printf("%s{\"t\":%lld,\"conns\":%lld,\"out\":0,\"in\":0}",
                       r3 ? "," : "",
                       sqlite3_column_int64(st, 0),
                       sqlite3_column_int64(st, 1));
                r3++;
            }
            sqlite3_finalize(st);
        }
    } else {
        st = prep(db,
            "SELECT (first_ms/3600000)*3600000,COUNT(*),SUM(byt_o),SUM(byt_r)"
            " FROM conns WHERE first_ms>=? AND " ENT_COND
            " GROUP BY 1 ORDER BY 1;");
        row = 0;
        if (st) {
            bind_entity(st, from, key);
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
    }
    printf("],");

    /* --- 连接明细 --- */
    printf("\"conns\":[");
    snprintf(sql, sizeof(sql),
        "SELECT first_ms,last_ms,dur_ms,COALESCE(%s, src),dst,dport,"
        "COALESCE(host,''),byt_o,byt_r,fam FROM conns"
        " WHERE first_ms>=? AND " ENT_COND
        " ORDER BY first_ms DESC LIMIT 60;", DC_CLIENT_IP);
    st = prep(db, sql);
    row = 0;
    if (st) {
        bind_entity(st, from, key);
        while (sqlite3_step(st) == SQLITE_ROW) {
            /* 第 3 列是 LAN 侧地址 COALESCE(client_ip, src)，用它归属设备 */
            const unsigned char *src = sqlite3_column_text(st, 3);
            const unsigned char *dst = sqlite3_column_text(st, 4);
            const unsigned char *h   = sqlite3_column_text(st, 6);
            const char *sip = src ? (const char *)src : "";
            char nb[256], e1[96], e2[512], e3[128], e4[512];
            name_fn(sip, nb, sizeof(nb));
            jesc(sip, e1, sizeof(e1)); jesc(nb, e2, sizeof(e2));
            jesc(dst ? (const char *)dst : "", e3, sizeof(e3));
            jesc(h ? (const char *)h : "", e4, sizeof(e4));
            printf("%s{\"ts\":%lld,\"last_ms\":%lld,\"dur_ms\":%lld,"
                   "\"ip\":\"%s\",\"name\":\"%s\",\"dst\":\"%s\","
                   "\"dport\":%d,\"host\":\"%s\",\"out\":%.0f,\"in\":%.0f,"
                   "\"fam\":%d,\"client_ip\":\"%s\"}",
                   row ? "," : "",
                   sqlite3_column_int64(st, 0),
                   sqlite3_column_int64(st, 1),
                   sqlite3_column_int64(st, 2),
                   e1, e2, e3, sqlite3_column_int(st, 5), e4,
                   sqlite3_column_double(st, 7),
                   sqlite3_column_double(st, 8),
                   sqlite3_column_int(st, 9), e1);
            row++;
        }
        sqlite3_finalize(st);
    }
    printf("],");

    /* --- DNS 查询明细 --- */
    printf("\"dns\":[");
    snprintf(sql, sizeof(sql),
        "SELECT ts_ms,qname,qtype,rcode,ms,COALESCE(%s, answers),"
        "COALESCE(client_ip,''),%s,%s FROM dnsq"
        " WHERE ts_ms>=? AND qname=? ORDER BY ts_ms DESC LIMIT 60;",
        DC_ANSWER_IPS, DC_SERVER_IP, DC_TTL);
    st = prep(db, sql);
    row = 0;
    if (st) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)from);
        sqlite3_bind_text(st, 2, key, -1, SQLITE_TRANSIENT);
        while (sqlite3_step(st) == SQLITE_ROW) {
            const unsigned char *q  = sqlite3_column_text(st, 1);
            const unsigned char *a  = sqlite3_column_text(st, 5);
            const unsigned char *ci = sqlite3_column_text(st, 6);
            const unsigned char *sv = sqlite3_column_text(st, 7);
            const char *sip = ci ? (const char *)ci : "";
            char nb[256], e1[96], e2[512], eq[512], ea[256];
            char esv[128] = "", elabel[160] = "", label[128];
            char jm[32] = "null", jttl[32] = "null";
            name_fn(sip, nb, sizeof(nb));
            jesc(sip, e1, sizeof(e1)); jesc(nb, e2, sizeof(e2));
            jesc(q ? (const char *)q : "", eq, sizeof(eq));
            jesc(dc_norm_answers(a ? (const char *)a : ""), ea, sizeof(ea));
            jesc(sv ? (const char *)sv : "", esv, sizeof(esv));
            dns_label(sv ? (const char *)sv : "", label, sizeof(label));
            jesc(label, elabel, sizeof(elabel));
            if (sqlite3_column_type(st, 4) != SQLITE_NULL)
                snprintf(jm, sizeof(jm), "%.2f", sqlite3_column_double(st, 4));
            if (sqlite3_column_type(st, 8) != SQLITE_NULL)
                snprintf(jttl, sizeof(jttl), "%d", sqlite3_column_int(st, 8));
            printf("%s{\"ts\":%lld,\"q\":\"%s\",\"qtype\":%d,\"rcode\":%d,"
                   "\"ms\":%s,\"answers\":\"%s\",\"ip\":\"%s\",\"name\":\"%s\","
                   "\"server\":\"%s\",\"server_label\":\"%s\","
                   "\"ttl\":%s}",
                   row ? "," : "",
                   sqlite3_column_int64(st, 0), eq,
                   sqlite3_column_int(st, 2), sqlite3_column_int(st, 3),
                   jm, ea, e1, e2, esv, elabel, jttl);
            row++;
        }
        sqlite3_finalize(st);
    }
    printf("],\"items\":[],\"note\":\"%s\"}\n",
           is_q ? "本页按 DNS 查询日志聚合：次数与时间来自被动嗅探，"
                  "被拦截的域名不会产生连接记录，因此流量为 0 属正常。"
                : "连接明细按时间倒序，最多 60 条；上下行以连接发起方为基准。");
    (void)ek;
    return 0;
}


int detail_emit(sqlite3 *db, const char *type, const char *key,
                uint64_t since_ms, detail_name_fn name_fn)
{
    if (!db || !detail_valid(type, key)) {
        printf("{\"error\":\"invalid detail request\"}\n");
        return 2;
    }
    uint64_t from = now_ms() - since_ms;

    char dkey[256];
    url_decode(key, dkey, sizeof(dkey));
    if (!dkey[0] || !decoded_key_ok(dkey)) {
        printf("{\"error\":\"invalid detail key\"}\n");
        return 2;
    }
    key = dkey;

    return emit_entity(db, type, key, from, name_fn);
}
