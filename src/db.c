/*
 * db.c - netmon 持久化层实现
 *
 * 写入路径是整条链路的热点，所有取舍都围绕它：
 *
 *   conntrack 消亡 -> db_add_conn() -> bind 到预编译语句 -> step
 *   -> 攒够行数/时间 -> COMMIT（一次 fsync）
 *
 * 关键：sqlite3_step 在事务内只是写 page cache，不碰磁盘。
 * 磁盘 io 只在 COMMIT 时发生，所以事务越大、每条记录的摊销成本越低。
 */

#define _GNU_SOURCE
#include "db.h"

/*
 * 优先用官方 sqlite3.h（/opt/netmon/include 下），
 * 没有就退回自带的最小声明。
 *
 * 为什么两者都留着：
 *   - 路由器上编译时用官方头（有完整的 sqlite3_int64 等类型定义）
 *   - 某些环境（如只装了运行库没装 dev 包）用最小声明也能编过
 *   两者的 ABI 完全一致，混用安全。
 */
#if defined(__has_include)
#  if __has_include(<sqlite3.h>)
#    define HAVE_REAL_SQLITE_H 1
#  endif
#endif

#ifdef HAVE_REAL_SQLITE_H
#  include <sqlite3.h>
#else
#  include "sqlite3_min.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <arpa/inet.h>

/* 一个事务最多攒这么多行就强制提交，防止内存里 padding 太多 */
#define MAX_PENDING_ROWS  200000

struct db {
    sqlite3 *h;
    sqlite3_stmt *ins_conn;

    long     pending;          /* 当前事务里已 step 的行数 */
    long     commits;
    long     rows_written;
    uint64_t last_flush_ms;
    char     path[512];
};

/* ---------------- 小工具 ---------------- */

/* v6-mapped 前缀 ::ffff: */
static const uint8_t V4MAP[12] = {0,0,0,0,0,0,0,0,0,0,0xff,0xff};

/*
 * 取出 v4 地址的 4 个真实字节。
 *
 * ctparse 解析 conntrack 得到的 v4 地址是 ::ffff:a.b.c.d 形态，
 * 真实地址在后 4 字节；而从 sockaddr_in 直接拷的则在前 4 字节。
 * 所有需要读 v4 字节的地方都必须经过这里，否则会静默读到 0.0.0.0。
 */
static inline const uint8_t *v4bytes(const uint8_t ip[16])
{
    return (memcmp(ip, V4MAP, 12) == 0) ? (ip + 12) : ip;
}

static uint64_t mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

void db_ip_str(uint8_t family, const uint8_t ip[16], char *out, size_t n)
{
    if (!out || n == 0) return;
    out[0] = '\0';

    if (family == 6) {
        if (inet_ntop(AF_INET6, ip, out, (socklen_t)n) != NULL) return;
        snprintf(out, n, "?");
        return;
    }

    /*
     * v4 的处理要小心：ip 缓冲有两种可能的存放方式
     *   - 裸 v4 在前 4 字节（从 sockaddr_in 直接拷）
     *   - v6-mapped 在后 4 字节（ctparse 解析 conntrack 得到的就是这种）
     *
     * 之前直接 inet_ntop(AF_INET, ip) 在第二种情况下会读到前 4 字节的全零，
     * 得到 "0.0.0.0" 而且不报错，于是兜底分支永远走不到。
     * 这里改为：先判断是不是 v6-mapped（前缀 ::ffff:），
     * 是就取后 4 字节，否则按裸 v4 处理。
     */
    if (inet_ntop(AF_INET, v4bytes(ip), out, (socklen_t)n) != NULL)
        return;

    snprintf(out, n, "?");
}

/* 把 v4 地址规整成 "a.b.c.d"，供 LIKE 前缀查询用 */
static void ip_text(uint8_t family, const uint8_t ip[16], char *out, size_t n)
{
    char tmp[64];
    db_ip_str(family, ip, tmp, sizeof(tmp));
    snprintf(out, n, "%s", tmp);
}

static int exec_sql(sqlite3 *h, const char *sql, char *errbuf, size_t errlen)
{
    char *err = NULL;
    int rc = sqlite3_exec(h, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        snprintf(errbuf, errlen, "%s", err ? err : sqlite3_errmsg(h));
        sqlite3_free(err);
        return -1;
    }
    return 0;
}

/* ---------------- schema ---------------- */

/*
 * conns：明细表，一行 = 一条连接的一生。
 *
 * 为什么不存每个采样点：
 *   5 秒一采，一条长连接活 1 小时就是 720 行。家用路由器日均几万连接，
 *   30 天下来是十亿级行数，SQLite 撑不住也不必要——分析要的是
 *   "这条连接是谁、去了哪、传了多少、什么时候"，不是逐秒曲线。
 *
 * 字段取舍：
 *   first_ms/last_ms  连接起止（毫秒时间戳，UTC）
 *   pkt_o/byt_o       原始方向（内网->外网）包数与字节
 *   pkt_r/byt_r       应答方向
 *   proto             6=TCP 17=UDP 1=ICMP 其它=hint
 *   dport             目的端口，用于识别服务
 *   host              反查出的域名（可为空）
 *   cidr              目的 IP 的 /24 前缀，用于聚合查询走索引
 *
 * 索引策略：
 *   只建两个。明细表写入量大，每多一个索引就多一份写放大。
 *   - (first_ms)          时间范围扫、GC 删除
 *   - (cidr, first_ms)    按网段查历史
 *   域名查询靠 host 不建索引，因为它是低频分析操作，
 *   全表扫一次几百万行也就一两秒，不值得为它付写入代价。
 */
static const char *SCHEMA_SQL =
    "PRAGMA journal_mode=WAL;"
    "PRAGMA synchronous=NORMAL;"
    "PRAGMA temp_store=MEMORY;"
    "PRAGMA foreign_keys=OFF;"
    "PRAGMA auto_vacuum=INCREMENTAL;"

    "CREATE TABLE IF NOT EXISTS conns("
    "  id       INTEGER PRIMARY KEY,"
    "  first_ms INTEGER NOT NULL,"
    "  last_ms  INTEGER NOT NULL,"
    "  dur_ms   INTEGER NOT NULL,"
    "  fam      INTEGER NOT NULL,"          /* 4 或 6 */
    "  proto    INTEGER NOT NULL,"          /* 6/17/1 */
    "  sport    INTEGER NOT NULL,"
    "  dport    INTEGER NOT NULL,"
    "  src      TEXT    NOT NULL,"
    "  dst      TEXT    NOT NULL,"
    "  sip      INTEGER NOT NULL DEFAULT 0," /* dst 的数值形式，便于排序/范围 */
    "  cidr     TEXT    NOT NULL DEFAULT '',"/* v4: a.b.c.0/24  v6: /48 */
    "  host     TEXT,"
    "  zone     INTEGER NOT NULL DEFAULT 0,"
    "  mark     INTEGER NOT NULL DEFAULT 0,"
    "  pkt_o    INTEGER NOT NULL,"
    "  byt_o    INTEGER NOT NULL,"
    "  pkt_r    INTEGER NOT NULL,"
    "  byt_r    INTEGER NOT NULL,"
    "  unreplied INTEGER NOT NULL DEFAULT 0"
    ");"

    "CREATE INDEX IF NOT EXISTS idx_conns_t   ON conns(first_ms);"
    "CREATE INDEX IF NOT EXISTS idx_conns_cidr ON conns(cidr, first_ms);"

    /*
     * agg_hour：小时聚合，永久保留。
     *
     * 粒度 = 小时 × 目标网段 × 协议 × 目的端口。
     * 这个粒度能回答你关心的问题：
     *   - 某个时段出国流量多大、走哪些端口
     *   - 某个 CDN 网段的流量随时间怎么变
     * 再细就没意义了，30 天明细已经覆盖了细查需求。
     */
    "CREATE TABLE IF NOT EXISTS agg_hour("
    "  hour_ms  INTEGER NOT NULL,"
    "  cidr     TEXT    NOT NULL,"
    "  proto    INTEGER NOT NULL,"
    "  dport    INTEGER NOT NULL,"
    "  conns    INTEGER NOT NULL,"
    "  bytes_o  INTEGER NOT NULL,"
    "  bytes_r  INTEGER NOT NULL,"
    "  pkts     INTEGER NOT NULL,"
    "  unrepl   INTEGER NOT NULL,"
    "  PRIMARY KEY(hour_ms, cidr, proto, dport)"
    ") WITHOUT ROWID;"

    /*
     * agg_day：天聚合，永久保留。从 agg_hour 再滚一层，
     * 让"过去一年每天出国流量"这种查询不用碰明细表。
     */
    "CREATE TABLE IF NOT EXISTS agg_day("
    "  day_ms   INTEGER NOT NULL,"
    "  cidr     TEXT    NOT NULL,"
    "  conns    INTEGER NOT NULL,"
    "  bytes_o  INTEGER NOT NULL,"
    "  bytes_r  INTEGER NOT NULL,"
    "  PRIMARY KEY(day_ms, cidr)"
    ") WITHOUT ROWID;"

    /* 元信息：记录已经滚到哪个时间点，避免重复计算 */
    "CREATE TABLE IF NOT EXISTS meta("
    "  k TEXT PRIMARY KEY,"
    "  v TEXT"
    ") WITHOUT ROWID;"

    /* 探测结果：时延/带宽/丢包/站点可用性，和连接明细分开存 */
    "CREATE TABLE IF NOT EXISTS probe("
    "  ts_ms    INTEGER NOT NULL,"
    "  kind     TEXT    NOT NULL,"   /* ping / dns / http / speed */
    "  target   TEXT    NOT NULL,"
    "  tag      TEXT,"               /* 运营商 / 地区 / 线路标签 */
    "  ok       INTEGER NOT NULL,"
    "  rtt_ms   REAL,"
    "  jitter_ms REAL,"
    "  loss     REAL,"
    "  bw_kbps  REAL,"
    "  code     INTEGER,"
    "  extra    TEXT"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_probe_k ON probe(kind, target, ts_ms);"

    /* DNS 质量：解析耗时、应答 IP、发往哪个服务器 */
    "CREATE TABLE IF NOT EXISTS dnsq("
    "  ts_ms   INTEGER NOT NULL,"
    "  qname   TEXT    NOT NULL,"
    "  qtype   INTEGER NOT NULL,"
    "  rcode   INTEGER NOT NULL,"
    "  ms      REAL,"
    "  answers TEXT,"
    "  upstream TEXT,"
    "  client_ip TEXT"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_dnsq_t ON dnsq(ts_ms);"
    /* dashboard 的三条重头查询都按 (时间, 维度) 分组：
       单列 ts_ms 索引只能缩小范围，仍要回表取 client_ip/qname/rcode。
       复合覆盖索引让 GROUP BY 直接走索引，避免几十万次回表，
       这是把 dashboard 从秒级压到百毫秒级的关键。 */
    "CREATE INDEX IF NOT EXISTS idx_dnsq_tc ON dnsq(ts_ms, client_ip);"
    "CREATE INDEX IF NOT EXISTS idx_dnsq_tq ON dnsq(ts_ms, qname);"
    "CREATE INDEX IF NOT EXISTS idx_dnsq_tr ON dnsq(ts_ms, rcode, qname);"
    "CREATE INDEX IF NOT EXISTS idx_conns_ts ON conns(first_ms, src);"

    /* 代理可达性探测：排除广告拦截后，直连失败的目标测一次走代理能否通。
       verdict=need_proxy 的就是「建议加进 Proxy 规则」的候选。 */
    "CREATE TABLE IF NOT EXISTS reach("
    "  ts_ms       INTEGER NOT NULL,"
    "  host        TEXT    NOT NULL,"
    "  reason      TEXT,"
    "  attempts    INTEGER DEFAULT 0,"
    "  direct_code INTEGER DEFAULT 0,"
    "  direct_ms   REAL,"
    "  direct_ok   INTEGER DEFAULT 0,"
    "  proxy_code  INTEGER DEFAULT 0,"
    "  proxy_ms    REAL,"
    "  proxy_ok    INTEGER DEFAULT 0,"
    "  verdict     TEXT,"
    "  note        TEXT"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_reach_host ON reach(host, ts_ms);"

    /* homeproxy 规则指纹变化记录：让前端能显示「规则什么时候改过、改了什么」 */
    "CREATE TABLE IF NOT EXISTS hp_event("
    "  ts_ms    INTEGER NOT NULL,"
    "  fp       TEXT,"
    "  detail   TEXT,"
    "  rules    INTEGER,"
    "  patterns INTEGER"
    ");"
    ;

/*
 * 1.1.0 新增的复合索引（§5.2.1）：按 DNS 服务器聚合查询量。
 *
 * 为什么不放进上面的 SCHEMA_SQL 一起执行：
 *   索引引用 server_ip，而这一列是 db_init_schema() 在 CREATE TABLE 之后
 *   用 ensure_column() 幂等 ALTER 出来的（CREATE TABLE 里刻意不加，保证
 *   老库能就地升级）。SCHEMA_SQL 是一次性 sqlite3_exec，若把索引放进去，
 *   执行到 CREATE INDEX 时列还不存在 → "no such column"，整个 exec 失败，
 *   新库和老库都打不开。
 *
 * 所以拆成「先建表/旧索引 → ensure_column 加列 → 再建新索引」三步。
 * 语句本身仍是 CREATE INDEX IF NOT EXISTS，重复执行无副作用。
 */
static const char *SCHEMA_POST_MIGRATE_SQL =
    "CREATE INDEX IF NOT EXISTS idx_dnsq_server ON dnsq(ts_ms, server_ip);";

/* sqlite3_free 已在 sqlite3_min.h 声明，无需额外补声明 */

/*
 * 老库就地迁移。
 *
 * 为什么需要：CREATE TABLE IF NOT EXISTS 对已存在的表什么都不做，
 * 所以给 dnsq 加 client_ip 这种新列，老库不会自动长出来，
 * 一插入就报 "no such column"。
 *
 * 做法：读 PRAGMA table_info 看列在不在，不在就 ALTER TABLE ADD COLUMN。
 * ALTER 加列是 O(1) 元数据操作，不重写表，几百万行也是瞬间完成。
 * 幂等：重复执行无副作用。
 */
static int ensure_column(sqlite3 *h, const char *table, const char *col,
                         const char *decl)
{
    char sql[256];
    snprintf(sql, sizeof(sql), "PRAGMA table_info(%s);", table);

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(h, sql, -1, &st, NULL) != SQLITE_OK) return -1;

    int found = 0;
    while (sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char *name = sqlite3_column_text(st, 1);
        if (name && strcmp((const char *)name, col) == 0) { found = 1; break; }
    }
    sqlite3_finalize(st);

    if (found) return 0;

    snprintf(sql, sizeof(sql), "ALTER TABLE %s ADD COLUMN %s %s;",
             table, col, decl);
    char *err = NULL;
    if (sqlite3_exec(h, sql, NULL, NULL, &err) != SQLITE_OK) {
        sqlite3_free(err);
        return -1;
    }
    return 0;
}

int db_init_schema(db_t *d, char *errbuf, size_t errlen)
{
    if (!d || !d->h) return -1;
    if (exec_sql(d->h, SCHEMA_SQL, errbuf, errlen) != 0) return -1;

    /* 迁移：dnsq.client_ip（2026-11 加入，用于按设备看 DNS 行为）。保留不动。 */
    if (ensure_column(d->h, "dnsq", "client_ip", "TEXT") != 0) {
        if (errbuf) snprintf(errbuf, errlen, "迁移 dnsq.client_ip 失败");
        return -1;
    }

    /*
     * 1.1.0：dnsq 新列一律走 ensure_column 幂等 ALTER。
     * 顺序与 §5.2.1 表一致；不在 CREATE TABLE 里加，老库就地升级。
     */
    static const struct { const char *col; const char *decl; } dnsq_cols[] = {
        { "server_ip",   "TEXT"                  },
        { "server_fam",  "INTEGER DEFAULT 4"     },
        { "server_port", "INTEGER DEFAULT 53"    },
        { "client_fam",  "INTEGER DEFAULT 4"     },
        { "ttl",         "INTEGER"               },
        { "answer_cnt",  "INTEGER DEFAULT 0"     },
        { "answer_ips",  "TEXT"                  },
        /*
         * 归因列（HP_*）。老行留 NULL，报表按 rcode + 本地域后缀兜底，
         * 不需要回填 —— 回填反而会把"当时没归因"伪装成"当时判过"。
         */
        { "hp",          "INTEGER"               },
    };
    for (size_t i = 0; i < sizeof(dnsq_cols)/sizeof(dnsq_cols[0]); i++) {
        if (ensure_column(d->h, "dnsq", dnsq_cols[i].col, dnsq_cols[i].decl) != 0) {
            if (errbuf) snprintf(errbuf, errlen, "迁移 dnsq.%s 失败",
                                  dnsq_cols[i].col);
            return -1;
        }
    }

    /* conns：LAN 侧地址 + 方向。旧行 client_ip 为 NULL，报表 COALESCE 兜底。 */
    if (ensure_column(d->h, "conns", "client_ip", "TEXT") != 0) {
        if (errbuf) snprintf(errbuf, errlen, "迁移 conns.client_ip 失败");
        return -1;
    }
    if (ensure_column(d->h, "conns", "dir", "INTEGER DEFAULT 0") != 0) {
        if (errbuf) snprintf(errbuf, errlen, "迁移 conns.dir 失败");
        return -1;
    }

    /* 上面这些列都就位之后，才能建引用它们的复合索引（见 SCHEMA_POST_MIGRATE_SQL 注释） */
    if (exec_sql(d->h, SCHEMA_POST_MIGRATE_SQL, errbuf, errlen) != 0) return -1;

    return 0;
}

/* ---------------- 生命周期 ---------------- */

db_t *db_open(const char *path, int cache_kb, int mmap_kb,
              char *errbuf, size_t errlen)
{
    if (errbuf && errlen) errbuf[0] = '\0';

    db_t *d = calloc(1, sizeof(*d));
    if (!d) { snprintf(errbuf, errlen, "oom"); return NULL; }

    snprintf(d->path, sizeof(d->path), "%s", path);

    int rc = sqlite3_open_v2(path, &d->h,
                             SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL);
    if (rc != SQLITE_OK) {
        snprintf(errbuf, errlen, "open %s: %s", path,
                 d->h ? sqlite3_errmsg(d->h) : "?");
        if (d->h) sqlite3_close_v2(d->h);
        free(d);
        return NULL;
    }

    sqlite3_extended_result_codes(d->h, 1);

    /* 连接级 PRAGMA。cache_size 单位是 page（默认 4KB），负数表示 KB */
    char pragma[256];
    snprintf(pragma, sizeof(pragma),
             "PRAGMA cache_size=-%d;"
             "PRAGMA mmap_size=%d;"
             "PRAGMA busy_timeout=5000;",
             cache_kb > 0 ? cache_kb : 65536,
             mmap_kb > 0 ? mmap_kb * 1024 : 0);
    if (exec_sql(d->h, pragma, errbuf, errlen) != 0) {
        db_close(d);
        return NULL;
    }

    d->last_flush_ms = mono_ms();
    return d;
}

void db_close(db_t *d)
{
    if (!d) return;
    if (d->ins_conn) {
        db_flush(d);
        sqlite3_finalize(d->ins_conn);
        d->ins_conn = NULL;
    }
    if (d->h) {
        sqlite3_exec(d->h, "PRAGMA wal_checkpoint(TRUNCATE);", NULL, NULL, NULL);
        sqlite3_close_v2(d->h);
    }
    free(d);
}

/* ---------------- 写入 ---------------- */

static int ensure_insert_stmt(db_t *d, char *errbuf, size_t errlen)
{
    if (d->ins_conn) return 0;

    const char *sql =
        "INSERT INTO conns"
        "(first_ms,last_ms,dur_ms,fam,proto,sport,dport,src,dst,sip,cidr,"
        " host,zone,mark,pkt_o,byt_o,pkt_r,byt_r,unreplied,client_ip,dir)"
        " VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)";

    int rc = sqlite3_prepare_v2(d->h, sql, -1, &d->ins_conn, NULL);
    if (rc != SQLITE_OK) {
        snprintf(errbuf, errlen, "prepare insert: %s", sqlite3_errmsg(d->h));
        d->ins_conn = NULL;
        return -1;
    }
    return 0;
}

/* 计算 /24 或 /48 前缀文本 */
static void make_cidr(uint8_t family, const uint8_t ip[16], char *out, size_t n)
{
    if (family == 4) {
        /*
         * 注意：ctparse 给的 v4 是 v6-mapped 形态（::ffff:a.b.c.d），
         * 真实地址在后 4 字节。直接取 ip[0..2] 会得到 0.0.0.0/24。
         */
        const uint8_t *p = v4bytes(ip);
        snprintf(out, n, "%u.%u.%u.0/24", p[0], p[1], p[2]);
    } else {
        /* v6 用 /48，取前 6 字节，压缩写法 */
        char tmp[64];
        uint8_t p[16];
        memset(p, 0, sizeof(p));
        memcpy(p, ip, 6);
        if (inet_ntop(AF_INET6, p, tmp, sizeof(tmp)) == NULL)
            snprintf(tmp, sizeof(tmp), "::");
        snprintf(out, n, "%s/48", tmp);
    }
}

int db_add_conn_ex(db_t *d, const ct_key_t *k,
                   uint64_t first_ms, uint64_t last_ms,
                   uint64_t pkt_o, uint64_t byt_o,
                   uint64_t pkt_r, uint64_t byt_r,
                   uint8_t proto_hint, uint32_t zone, uint32_t mark,
                   const char *host, const char *client_ip, uint8_t dir)
{
    if (!d || !d->h || !k) return -1;

    char errb[256];
    if (ensure_insert_stmt(d, errb, sizeof(errb)) != 0) return -1;

    /* 事务是按需开启的：pending==0 时开，提交后清零 */
    if (d->pending == 0) {
        if (sqlite3_exec(d->h, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK)
            return -1;
    }

    char src[64], dst[64], cidr[80];
    ip_text(k->family, k->src, src, sizeof(src));
    ip_text(k->family, k->dst, dst, sizeof(dst));
    make_cidr(k->family, k->dst, cidr, sizeof(cidr));

    /* dst 的数值形式：只对 v4 有意义，用于范围排序。
     * 同样要处理 v6-mapped 形态，真实地址在后 4 字节。 */
    long long sip = 0;
    if (k->family == 4) {
        const uint8_t *d4 = v4bytes(k->dst);
        sip = ((long long)d4[0] << 24) | ((long long)d4[1] << 16)
            | ((long long)d4[2] << 8) | (long long)d4[3];
    }

    uint64_t dur = (last_ms > first_ms) ? (last_ms - first_ms) : 0;
    uint8_t unreplied = (pkt_r == 0) ? 1 : 0;

    sqlite3_stmt *s = d->ins_conn;
    sqlite3_bind_int64(s,  1, (long long)first_ms);
    sqlite3_bind_int64(s,  2, (long long)last_ms);
    sqlite3_bind_int64(s,  3, (long long)dur);
    sqlite3_bind_int  (s,  4, k->family);
    sqlite3_bind_int  (s,  5, proto_hint ? proto_hint : k->proto);
    sqlite3_bind_int  (s,  6, k->sport);
    sqlite3_bind_int  (s,  7, k->dport);
    sqlite3_bind_text (s,  8, src, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (s,  9, dst, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(s, 10, sip);
    sqlite3_bind_text (s, 11, cidr, -1, SQLITE_TRANSIENT);

    /* host 为空时绑 NULL，而不是空串——NULL 在 SQL 里能区分"反查失败" */
    if (host && host[0])
        sqlite3_bind_text (s, 12, host, -1, SQLITE_TRANSIENT);
    else
        sqlite3_bind_null (s, 12);

    sqlite3_bind_int64(s, 13, (long long)zone);
    sqlite3_bind_int64(s, 14, (long long)mark);
    sqlite3_bind_int64(s, 15, (long long)pkt_o);
    sqlite3_bind_int64(s, 16, (long long)byt_o);
    sqlite3_bind_int64(s, 17, (long long)pkt_r);
    sqlite3_bind_int64(s, 18, (long long)byt_r);
    sqlite3_bind_int  (s, 19, unreplied);

    /* LAN 侧地址：NULL = 旧行/旧调用（报表 COALESCE(client_ip, src) 兜底） */
    if (client_ip && client_ip[0])
        sqlite3_bind_text(s, 20, client_ip, -1, SQLITE_TRANSIENT);
    else
        sqlite3_bind_null(s, 20);
    sqlite3_bind_int(s, 21, dir);

    int rc = sqlite3_step(s);
    sqlite3_reset(s);
    sqlite3_clear_bindings(s);

    if (rc != SQLITE_DONE) return -1;

    d->pending++;
    if (d->pending >= MAX_PENDING_ROWS)
        db_flush(d);
    return 0;
}

/* 旧原型保持不变：旧调用写出的行 client_ip=NULL、dir=0，口径不变。 */
int db_add_conn(db_t *d, const ct_key_t *k,
                uint64_t first_ms, uint64_t last_ms,
                uint64_t pkt_o, uint64_t byt_o,
                uint64_t pkt_r, uint64_t byt_r,
                uint8_t proto_hint, uint32_t zone, uint32_t mark,
                const char *host)
{
    return db_add_conn_ex(d, k, first_ms, last_ms,
                          pkt_o, byt_o, pkt_r, byt_r,
                          proto_hint, zone, mark, host, NULL, 0);
}

long db_flush(db_t *d)
{
    if (!d || !d->h) return -1;
    if (d->pending == 0) return 0;

    if (sqlite3_exec(d->h, "COMMIT", NULL, NULL, NULL) != SQLITE_OK) {
        sqlite3_exec(d->h, "ROLLBACK", NULL, NULL, NULL);
        d->pending = 0;
        return -1;
    }

    long n = d->pending;
    d->pending = 0;
    d->commits++;
    d->rows_written += n;
    d->last_flush_ms = mono_ms();
    return n;
}

long db_pending(db_t *d) { return d ? d->pending : 0; }

int db_should_flush(db_t *d, uint64_t now_ms)
{
    if (!d || d->pending == 0) return 0;
    /* 攒够 2000 行或超过 5 秒就提交 */
    if (d->pending >= 2000) return 1;
    if (now_ms > d->last_flush_ms + 5000) return 1;
    return 0;
}

/* ---------------- 聚合与 GC ---------------- */

static const char *ROLLUP_SQL =
    "INSERT INTO agg_hour(hour_ms,cidr,proto,dport,conns,bytes_o,bytes_r,pkts,unrepl)"
    " SELECT (first_ms/3600000)*3600000 AS h, cidr, proto, dport,"
    "        COUNT(*), SUM(byt_o), SUM(byt_r), SUM(pkt_o+pkt_r), SUM(unreplied)"
    "   FROM conns"
    "  WHERE first_ms >= ?1 AND first_ms < ?2"
    "  GROUP BY h, cidr, proto, dport"
    " ON CONFLICT(hour_ms,cidr,proto,dport) DO UPDATE SET"
    "   conns=conns+excluded.conns,"
    "   bytes_o=bytes_o+excluded.bytes_o,"
    "   bytes_r=bytes_r+excluded.bytes_r,"
    "   pkts=pkts+excluded.pkts,"
    "   unrepl=unrepl+excluded.unrepl";

long db_rollup_hourly(db_t *d, uint64_t from_ms, uint64_t to_ms)
{
    if (!d || !d->h) return -1;

    sqlite3_stmt *s = NULL;
    int rc = sqlite3_prepare_v2(d->h, ROLLUP_SQL, -1, &s, NULL);
    if (rc != SQLITE_OK) return -1;

    sqlite3_bind_int64(s, 1, (long long)from_ms);
    sqlite3_bind_int64(s, 2, (long long)to_ms);
    rc = sqlite3_step(s);
    sqlite3_finalize(s);

    if (rc != SQLITE_DONE && rc != SQLITE_ROW) return -1;
    return sqlite3_changes(d->h);
}

static const char *DAY_SQL =
    "INSERT INTO agg_day(day_ms,cidr,conns,bytes_o,bytes_r)"
    " SELECT (hour_ms/86400000)*86400000 AS dy, cidr,"
    "        SUM(conns), SUM(bytes_o), SUM(bytes_r)"
    "   FROM agg_hour"
    "  WHERE hour_ms >= ?1 AND hour_ms < ?2"
    "  GROUP BY dy, cidr"
    " ON CONFLICT(day_ms,cidr) DO UPDATE SET"
    "   conns=excluded.conns, bytes_o=excluded.bytes_o, bytes_r=excluded.bytes_r";

long db_gc(db_t *d, uint64_t cutoff_ms)
{
    if (!d || !d->h) return -1;

    /* 先滚聚合，再删明细：顺序不能反，否则删掉的数据没进聚合就永久丢了 */
    db_rollup_hourly(d, 0, cutoff_ms);

    /* 天聚合也刷一次最近两天的，保证跨天时数据正确 */
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(d->h, DAY_SQL, -1, &s, NULL) == SQLITE_OK) {
        uint64_t now = (uint64_t)time(NULL) * 1000u;
        uint64_t from = (now > 3u * 86400000u) ? (now - 3u * 86400000u) : 0;
        sqlite3_bind_int64(s, 1, (long long)from);
        sqlite3_bind_int64(s, 2, (long long)(now + 86400000u));
        sqlite3_step(s);
        sqlite3_finalize(s);
    }

    sqlite3_stmt *del = NULL;
    if (sqlite3_prepare_v2(d->h, "DELETE FROM conns WHERE first_ms < ?1", -1,
                           &del, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_int64(del, 1, (long long)cutoff_ms);
    int rc = sqlite3_step(del);
    sqlite3_finalize(del);
    if (rc != SQLITE_DONE) return -1;

    long n = sqlite3_changes(d->h);

    /* 删除后回收空闲页，避免数据库文件只增不减 */
    sqlite3_exec(d->h, "PRAGMA incremental_vacuum(2000);", NULL, NULL, NULL);

    return n;
}

/* ---------------- 探测结果 ---------------- */

/*
 * 探测结果属于低频写入（每 5 分钟几十条），
 * 不需要走连接表那套大事务缓冲，直接独立提交即可。
 *
 * 但要注意：如果此时连接表的事务还开着（pending>0），
 * 在同一个连接上执行其他写入会被卷进那个事务。
 * 所以这里先 flush 一次，保证两套数据不纠缠。
 */
int db_add_probe(db_t *d, const db_probe_t *p)
{
    if (!d || !d->h || !p || !p->kind || !p->target) return -1;

    if (d->pending > 0) db_flush(d);

    static const char *SQL =
        "INSERT INTO probe(ts_ms,kind,target,tag,ok,rtt_ms,jitter_ms,"
        "loss,bw_kbps,code,extra) VALUES(?,?,?,?,?,?,?,?,?,?,?)";

    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(d->h, SQL, -1, &s, NULL) != SQLITE_OK)
        return -1;

    sqlite3_bind_int64(s, 1, (long long)p->ts_ms);
    sqlite3_bind_text (s, 2, p->kind, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (s, 3, p->target, -1, SQLITE_TRANSIENT);

    if (p->tag && p->tag[0])
        sqlite3_bind_text(s, 4, p->tag, -1, SQLITE_TRANSIENT);
    else
        sqlite3_bind_null(s, 4);

    sqlite3_bind_int(s, 5, p->ok ? 1 : 0);

    /* NaN 表示"未测"，写 NULL 而不是写 0，避免污染平均值 */
    if (p->rtt_ms == p->rtt_ms)
        sqlite3_bind_double(s, 6, p->rtt_ms); else sqlite3_bind_null(s, 6);
    if (p->jitter_ms == p->jitter_ms)
        sqlite3_bind_double(s, 7, p->jitter_ms); else sqlite3_bind_null(s, 7);
    if (p->loss == p->loss)
        sqlite3_bind_double(s, 8, p->loss); else sqlite3_bind_null(s, 8);
    if (p->bw_kbps == p->bw_kbps)
        sqlite3_bind_double(s, 9, p->bw_kbps); else sqlite3_bind_null(s, 9);

    sqlite3_bind_int(s, 10, p->code);

    if (p->extra && p->extra[0])
        sqlite3_bind_text(s, 11, p->extra, -1, SQLITE_TRANSIENT);
    else
        sqlite3_bind_null(s, 11);

    int rc = sqlite3_step(s);
    sqlite3_finalize(s);

    return (rc == SQLITE_DONE) ? 0 : -1;
}

int db_add_dnsq(db_t *d, const db_dnsq_t *q)
{
    if (!d || !d->h || !q || !q->qname) return -1;

    if (d->pending > 0) db_flush(d);

    /*
     * 历史列处置（§5.2.1）：
     *   answers   —— 新行写 answer_ips（真实应答 IP），为空时退回旧计数字符串，
     *                修复「应答 IP 列显示 0/1」；旧行 "0"/"1" 由报表读时归一。
     *   upstream  —— 1.1.0 起不再写入（该列原先是「首个应答 IP」，语义已废）。
     *                留列不删是为了老库不用重建表、老报表仍能读历史行。
     *   first_ip  —— 旧字段保留但不再单独写列（应答 IP 已由 answer_ips 承载）。
     */
    static const char *SQL =
        "INSERT INTO dnsq(ts_ms,qname,qtype,rcode,ms,answers,client_ip,"
        "server_ip,server_fam,server_port,client_fam,ttl,answer_cnt,answer_ips,hp)"
        " VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)";

    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(d->h, SQL, -1, &s, NULL) != SQLITE_OK)
        return -1;

    sqlite3_bind_int64(s, 1, (long long)q->ts_ms);
    sqlite3_bind_text (s, 2, q->qname, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int  (s, 3, q->qtype);
    sqlite3_bind_int  (s, 4, q->rcode);

    if (q->ms == q->ms)
        sqlite3_bind_double(s, 5, q->ms); else sqlite3_bind_null(s, 5);

    /* answers：应答 IP 文本优先，空时退回旧计数（"0"/"1" 字符串） */
    if (q->answer_ips && q->answer_ips[0])
        sqlite3_bind_text(s, 6, q->answer_ips, -1, SQLITE_TRANSIENT);
    else {
        char cnt[16];
        snprintf(cnt, sizeof(cnt), "%d", q->answers);
        sqlite3_bind_text(s, 6, cnt, -1, SQLITE_TRANSIENT);
    }

    /* 客户端 IP：抓不到（本机自身查询）时写 NULL，不要写空串 */
    if (q->client_ip && q->client_ip[0])
        sqlite3_bind_text(s, 7, q->client_ip, -1, SQLITE_TRANSIENT);
    else
        sqlite3_bind_null(s, 7);

    /* ---- 1.1.0 新列 ---- */
    if (q->server_ip && q->server_ip[0])
        sqlite3_bind_text(s, 8, q->server_ip, -1, SQLITE_TRANSIENT);
    else
        sqlite3_bind_null(s, 8);
    sqlite3_bind_int(s, 9,  q->server_fam  ? q->server_fam  : 4);
    sqlite3_bind_int(s, 10, q->server_port ? q->server_port : 53);
    sqlite3_bind_int(s, 11, q->client_fam ? q->client_fam : 4);

    /* ttl<=0（含 -1 未知）写 NULL，别用 0 污染平均值 */
    if (q->ttl > 0) sqlite3_bind_int(s, 12, q->ttl);
    else            sqlite3_bind_null(s, 12);

    sqlite3_bind_int(s, 13, q->answer_cnt);

    if (q->answer_ips && q->answer_ips[0])
        sqlite3_bind_text(s, 14, q->answer_ips, -1, SQLITE_TRANSIENT);
    else
        sqlite3_bind_null(s, 14);

    /* 归因：hp < 0 表示调用方没算（不该发生），写 NULL 而不是冒充"正常" */
    if (q->hp >= 0) sqlite3_bind_int(s, 15, q->hp);
    else            sqlite3_bind_null(s, 15);

    int rc = sqlite3_step(s);
    sqlite3_finalize(s);

    return (rc == SQLITE_DONE) ? 0 : -1;
}

/* ---------------- 代理可达性 / 规则变更 ---------------- */

int db_add_reach(db_t *d, const db_reach_t *r)
{
    if (!d || !d->h || !r || !r->host) return -1;
    if (d->pending > 0) db_flush(d);

    static const char *SQL =
        "INSERT INTO reach(ts_ms,host,reason,attempts,"
        "direct_code,direct_ms,direct_ok,proxy_code,proxy_ms,proxy_ok,verdict,note)"
        " VALUES(?,?,?,?,?,?,?,?,?,?,?,?)";
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(d->h, SQL, -1, &s, NULL) != SQLITE_OK) return -1;

    sqlite3_bind_int64 (s, 1, (long long)r->ts_ms);
    sqlite3_bind_text  (s, 2, r->host, -1, SQLITE_TRANSIENT);
    if (r->reason) sqlite3_bind_text(s, 3, r->reason, -1, SQLITE_TRANSIENT);
    else           sqlite3_bind_null(s, 3);
    sqlite3_bind_int64 (s, 4, r->attempts);
    sqlite3_bind_int   (s, 5, r->direct_code);
    sqlite3_bind_double(s, 6, r->direct_ms);
    sqlite3_bind_int   (s, 7, r->direct_ok ? 1 : 0);
    sqlite3_bind_int   (s, 8, r->proxy_code);
    sqlite3_bind_double(s, 9, r->proxy_ms);
    sqlite3_bind_int   (s, 10, r->proxy_ok ? 1 : 0);
    if (r->verdict) sqlite3_bind_text(s, 11, r->verdict, -1, SQLITE_TRANSIENT);
    else            sqlite3_bind_null(s, 11);
    if (r->note)    sqlite3_bind_text(s, 12, r->note, -1, SQLITE_TRANSIENT);
    else            sqlite3_bind_null(s, 12);

    int rc = sqlite3_step(s);
    sqlite3_finalize(s);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

int db_add_hpevent(db_t *d, const db_hpevent_t *e)
{
    if (!d || !d->h || !e) return -1;
    if (d->pending > 0) db_flush(d);

    static const char *SQL =
        "INSERT INTO hp_event(ts_ms,fp,detail,rules,patterns) VALUES(?,?,?,?,?)";
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(d->h, SQL, -1, &s, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int64(s, 1, (long long)e->ts_ms);
    sqlite3_bind_text (s, 2, e->fp ? e->fp : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (s, 3, e->detail ? e->detail : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_int  (s, 4, e->rules);
    sqlite3_bind_int  (s, 5, e->patterns);
    int rc = sqlite3_step(s);
    sqlite3_finalize(s);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

int db_foreach(db_t *d, const char *sql,
               int (*cb)(void *ud, int argc, char **argv, char **col), void *ud)
{
    if (!d || !d->h || !sql || !cb) return -1;
    char *err = NULL;
    int rc = sqlite3_exec(d->h, sql, cb, ud, &err);
    if (err) sqlite3_free(err);
    return (rc == SQLITE_OK) ? 0 : -1;
}

/* ---------------- 统计 ---------------- */

static long long file_size(const char *p)
{
    struct stat st;
    return (stat(p, &st) == 0) ? (long long)st.st_size : 0;
}

static long long query_ll(sqlite3 *h, const char *sql)
{
    sqlite3_stmt *s = NULL;
    long long v = 0;
    if (sqlite3_prepare_v2(h, sql, -1, &s, NULL) == SQLITE_OK) {
        if (sqlite3_step(s) == SQLITE_ROW) v = sqlite3_column_int64(s, 0);
        sqlite3_finalize(s);
    }
    return v;
}

void db_get_stats(db_t *d, db_stats_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!d || !d->h) return;

    out->rows_conns = query_ll(d->h, "SELECT COUNT(*) FROM conns");
    out->rows_agg   = query_ll(d->h, "SELECT COUNT(*) FROM agg_hour");
    out->db_bytes   = file_size(d->path);
    out->commits    = d->commits;
    out->rows_written = d->rows_written;
    out->last_flush_ms = (long)(mono_ms() - d->last_flush_ms);

    char wal[600];
    snprintf(wal, sizeof(wal), "%s-wal", d->path);
    out->wal_bytes = file_size(wal);
}
