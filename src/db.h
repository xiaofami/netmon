/*
 * db.h - netmon 持久化层
 *
 * 设计取舍（针对 30 天全留明细 + 性能优先 + UPS 不断电）：
 *
 * 1) 明细粒度 = 连接，不是采样点。
 *    conntrack 每 5 秒采一次，如果每次都落库，30 天数据量是
 *    "连接数 × 采样次数"，直接把 SSD 写穿。所以每条连接在整个生命周期
 *    只写一行，消亡时提交，字段是累计值 + 起止时间。
 *
 * 2) 写放大靠大事务摊薄。
 *    实测 fdatasync 平均 5.8ms。单条连接一提交 = 每条连接 5.8ms 磁盘等待。
 *    改成攒够 db_flush_interval_ms 或 db_flush_rows 条再提交一次，
 *    几千条连接共享一次 fsync，写放大降到可忽略。
 *
 * 3) synchronous=NORMAL + WAL。
 *    UPS 供电，断电风险低；最坏只丢最后一个未提交事务。
 *    换来的是提交路径不需要每次 fsync WAL，只在新 checkpoint 时同步。
 */

#ifndef NETMON_DB_H
#define NETMON_DB_H

#include <stdint.h>
#include <stddef.h>
#include "ctparse.h"

typedef struct db db_t;

/* 打开/创建数据库。
 * path        : db 文件路径
 * cache_kb    : page cache 大小（KB），建议 524288（512MB）
 * mmap_kb     : memory-map 大小（KB），建议 524288（512MB）
 * 返回 NULL 表示失败（原因写到 errbuf） */
db_t *db_open(const char *path, int cache_kb, int mmap_kb,
              char *errbuf, size_t errlen);
void  db_close(db_t *d);

/* 建表/建索引（幂等） */
int   db_init_schema(db_t *d, char *errbuf, size_t errlen);

/* 记录一条已消亡的连接。内部走预编译语句 + 大事务，不立即提交。
 *
 * host 可为 NULL：表示 DNS 反查没命中（很常见，比如 IP 直连、
 * 或 dnsmasq 日志没开）。为空时不写这一列。 */
int   db_add_conn(db_t *d, const ct_key_t *k,
                  uint64_t first_ms, uint64_t last_ms,
                  uint64_t pkt_o, uint64_t byt_o,
                  uint64_t pkt_r, uint64_t byt_r,
                  uint8_t proto_hint, uint32_t zone, uint32_t mark,
                  const char *host);

/*
 * 带「LAN 侧地址 + 方向」的扩展写入（1.1.0）。
 *
 * client_ip : LAN 侧地址文本（设备归属）；NULL 表示旧行/旧调用，由报表
 *             COALESCE(client_ip, src) 兜底。
 * dir       : 0 = LAN→WAN（client_ip 即 src），1 = WAN→LAN（client_ip 即 dst，
 *             且调用方应已把 orig/repl 互换，保证 byt_o 恒为上传）。
 *
 * 旧 db_add_conn() 原型保持不变，内部以 client_ip=NULL、dir=0 调用本函数，
 * 使旧调用（含单测）写出的行与历史口径完全一致。
 */
int   db_add_conn_ex(db_t *d, const ct_key_t *k,
                     uint64_t first_ms, uint64_t last_ms,
                     uint64_t pkt_o, uint64_t byt_o,
                     uint64_t pkt_r, uint64_t byt_r,
                     uint8_t proto_hint, uint32_t zone, uint32_t mark,
                     const char *host, const char *client_ip, uint8_t dir);

/* 显式提交当前事务。返回本次提交的行数，失败返回 -1。 */
long  db_flush(db_t *d);

/* 需要提交吗？调用方按时间/行数判断。 */
int   db_should_flush(db_t *d, uint64_t now_ms);

/* 待提交行数 */
long  db_pending(db_t *d);

/* 清理早于 cutoff_ms 的明细；顺带把汇总滚进小时聚合表。
 * 返回删除行数，失败 -1。 */
long  db_gc(db_t *d, uint64_t cutoff_ms);

/* 把明细滚成小时聚合（幂等，可重复调）。返回处理行数。 */
long  db_rollup_hourly(db_t *d, uint64_t from_ms, uint64_t to_ms);

/* 统计信息，用于日志与状态页 */
typedef struct {
    long long rows_conns;
    long long rows_agg;
    long long db_bytes;
    long long wal_bytes;
    long     commits;
    long     rows_written;
    long     last_flush_ms;
} db_stats_t;

void db_get_stats(db_t *d, db_stats_t *out);

/* 便于人读的 IP 字符串（v4/v6 都行），失败返回 "?" */
void db_ip_str(uint8_t family, const uint8_t ip[16], char *out, size_t n);

/* ---------------- 探测结果 ---------------- */

/*
 * 一次探测的完整结果，字段留空用 NAN 表示"未测"。
 * kind: "ping" / "http" / "dns" / "bw"
 * tag : "oversea" / "domestic" 等分组标签
 */
typedef struct {
    uint64_t ts_ms;
    const char *kind;
    const char *target;
    const char *tag;
    int    ok;
    double rtt_ms;       /* ping: 平均 RTT；http: total */
    double jitter_ms;
    double loss;         /* 0.0 ~ 1.0 */
    double bw_kbps;
    int    code;         /* HTTP 状态码 */
    const char *extra;   /* 附加信息，可为 NULL */
} db_probe_t;

int db_add_probe(db_t *d, const db_probe_t *p);

/* DNS 查询质量 */
typedef struct {
    uint64_t ts_ms;
    const char *qname;
    int      qtype;
    int      rcode;
    double   ms;
    int      answers;
    const char *first_ip;
    /*
     * 发起查询的客户端 IP（点分十进制文本）。
     * 用于回答"哪台设备在查什么" —— 这是 DNS 报文里唯一能拿到的
     * 客户端身份，没有 MAC 也没有主机名（要主机名得配 DHCP 租约表）。
     * 本机（路由器自己）的查询没有 br-lan 以太帧，此字段为 NULL。
     */
    const char *client_ip;

    /* ---- 1.1.0 新增（旧字段语义不变，只增不改） ----
     * 这些字段把「查询发给谁、应答是什么」补全 —— 全部来自报文本身，
     * 不做任何推断。缓存命中率不在这里：被动旁路读不到解析器的缓存表，
     * 靠时延/转发/TTL 猜出来的数字误差太大，1.1.0 起不再提供该结论。 */
    uint8_t     client_fam;      /* 客户端地址协议族 4/6 */
    const char *server_ip;       /* 查询发往的 DNS 服务器（v4/v6 文本） */
    uint8_t     server_fam;      /* 服务器地址协议族 4/6 */
    int         server_port;     /* 服务端端口（识别非标端口/代理 DNS） */
    int         ttl;             /* 首个 A/AAAA 应答 TTL（秒），-1 = 未知，<=0 落库写 NULL */
    int         answer_cnt;      /* A/AAAA 应答条数 */
    const char *answer_ips;     /* 逗号分隔的应答 IP（v4/v6 混排），空则写 NULL */

    /*
     * 归因结论（HP_* 见 hprules.h），写入时由 hprules_classify() 算好。
     *
     * 为什么要把结论存下来而不是查询时现算：规则集会变，事后用新规则
     * 去解释旧行，等于篡改历史。存当时的事实，才能回答"那天到底拦没拦"。
     * -1 表示未归因（老行 / homeproxy 不可用），报表按 rcode + 后缀兜底。
     */
    int         hp;
} db_dnsq_t;

int db_add_dnsq(db_t *d, const db_dnsq_t *q);

/* ---------------- 代理可达性探测 ---------------- */

/*
 * 一次「直连 vs 走代理」双路径探测的结果。
 * 目的不是测速，而是回答：这个直连失败的域名，加进 Proxy 规则有没有用。
 * 结论只做建议，程序绝不自动改 homeproxy 配置。
 */
typedef struct {
    uint64_t    ts_ms;
    const char *host;
    const char *reason;      /* dns_fail / no_reply */
    long long   attempts;    /* 观测到的失败次数，用于排序 */
    int         direct_code;
    double      direct_ms;
    int         direct_ok;
    int         proxy_code;
    double      proxy_ms;
    int         proxy_ok;
    const char *verdict;     /* need_proxy / both_ok / direct_only / both_fail */
    const char *note;
} db_reach_t;

int db_add_reach(db_t *d, const db_reach_t *r);

/* homeproxy 规则变更事件（指纹变化时才写一条） */
typedef struct {
    uint64_t    ts_ms;
    const char *fp;
    const char *detail;
    int         rules;
    int         patterns;
} db_hpevent_t;

int db_add_hpevent(db_t *d, const db_hpevent_t *e);

/*
 * 通用只读查询。回调签名与 sqlite3_exec 一致。
 * 让 reach / 报表模块能查库而不必暴露 sqlite3 句柄或链接 sqlite3.h。
 */
int db_foreach(db_t *d, const char *sql,
               int (*cb)(void *ud, int argc, char **argv, char **col),
               void *ud);

#endif /* NETMON_DB_H */
