/*
 * dnsmap.h - IP -> 域名 反查表
 *
 * 为什么需要它：
 *   conntrack 只告诉我们 "连了 1.2.3.4:443"，分析时这是噪音。
 *   要回答"哪些站点占了带宽"、"某时段访问了什么"，必须把 IP 映射回域名。
 *
 * 数据来源：dnsmasq 的查询日志。
 *   dnsmasq 会打两类行：
 *     query[A] api.example.com from 192.168.1.100
 *     reply api.example.com is 93.184.216.34
 *   把 reply 行解析出来，就得到 qname -> IP 的映射。
 *
 * 实现取舍：
 *   1) 用哈希表把 (ip) -> 一组域名 存内存里，带 TTL 和 LRU 淘汰。
 *      CDN 场景下一个 IP 可能对应成百上千域名，所以一个 IP 挂一个小链表，
 *      链表长度封顶 8，超出就丢最旧的——分析时能认出主域名就够。
 *   2) 只认 A/AAAA 的 reply 行。CNAME 行（"is an alias for"）跳过。
 *   3) 内存上限可配。按 5 万条映射、平均域名 40 字节算，约 5MB，可以接受。
 */

#ifndef NETMON_DNSMAP_H
#define NETMON_DNSMAP_H

#include <stdint.h>
#include <stddef.h>

#define DNSMAP_NAME_MAX 64      /* 存进库的域名截断到 64 字节 */
#define DNSMAP_PER_IP   8       /* 一个 IP 最多记几个域名 */

typedef struct dnsmap dnsmap_t;

/* max_entries 为 0 时用默认 65536 */
dnsmap_t *dnsmap_new(size_t max_entries);
void      dnsmap_free(dnsmap_t *m);

/* 喂一行 dnsmasq 日志。返回 1 表示解析出了映射，0 表示这行无关。 */
int       dnsmap_feed(dnsmap_t *m, const char *line, uint64_t now_s);

/*
 * 直接添加一条 IP -> 域名 映射（DNS 嗅探用，避免绕道文本日志）。
 * ip 为 16 字节，v4 用 v4-mapped 形式（::ffff:a.b.c.d）。
 * 返回 1 成功，0 失败。
 */
int       dnsmap_add(dnsmap_t *m, uint8_t family, const uint8_t ip[16],
                     const char *name, uint64_t now_s);

/* 查 IP 对应的域名。命中返回域名指针（内部存储，不要 free），未命中返回 NULL。
 * 多个域名时返回最近命中的那个。 */
const char *dnsmap_lookup(dnsmap_t *m, uint8_t family,
                          const uint8_t ip[16]);

/* 统计 */
typedef struct {
    size_t ips;        /* 表里有多少个 IP */
    size_t names;      /* 一共记了多少条映射 */
    size_t hits;
    size_t misses;
    size_t evicted;    /* 因为表满被淘汰的 IP 数 */
} dnsmap_stats_t;

void dnsmap_get_stats(dnsmap_t *m, dnsmap_stats_t *out);

/* 条目过期时间（秒）。CDN 换 IP 频繁，默认 3600。 */
void dnsmap_set_ttl(dnsmap_t *m, uint32_t ttl_s);

#endif /* NETMON_DNSMAP_H */
