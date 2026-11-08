/*
 * dnsmap.c - IP -> 域名 反查表实现
 *
 * 数据结构：开放寻址哈希表，key = 16 字节 IP（v4 用后 4 字节，前面补零）。
 * 每个槽位挂一个定长数组（DNSMAP_PER_IP 个）存域名，环形覆盖。
 *
 * 为什么不做链表：
 *   定长数组避免了每个节点一次 malloc，缓存局部性好得多。
 *   域名数量不多（每条 ≤8），浪费可控。
 */

#define _GNU_SOURCE
#include "dnsmap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <arpa/inet.h>

typedef struct {
    char     name[DNSMAP_NAME_MAX];
    uint64_t seen_s;
    uint8_t  used;
} dm_name_t;

typedef struct {
    uint8_t   ip[16];
    uint8_t   family;
    uint8_t   used;
    uint8_t   next_slot;      /* 环形写入位置 */
    uint8_t   count;
    dm_name_t names[DNSMAP_PER_IP];
    uint64_t  last_s;
} dm_slot_t;

struct dnsmap {
    dm_slot_t *slots;
    size_t     cap;           /* 2 的幂 */
    size_t     used;          /* 已占用槽位 */
    size_t     max;
    uint32_t   ttl_s;

    size_t hits, misses, evicted;
};

/* FNV-1a，和 ctsample.c 用同一个，便于审阅时统一理解 */
static inline uint64_t fnv1a(const void *p, size_t n)
{
    const uint8_t *b = (const uint8_t *)p;
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < n; i++) {
        h ^= b[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static inline size_t mask(dnsmap_t *m, uint64_t h) { return (size_t)h & (m->cap - 1); }

/*
 * 把任意形态的 v4 地址归一化成哈希表内部统一的 key 形态
 * （v6-mapped：ip[10..11]=0xff, ip[12..15]=a.b.c.d），v6 原样拷贝。
 *
 * 为什么必须统一：本程序里 IPv4 有两种内存表示
 *   - DNS 嗅探（dnssniff）：v6-mapped，地址在后 4 字节
 *   - conntrack 解析（ctparse）：裸 v4，地址在前 4 字节
 * 写入和查询如果各用各的形态，哈希 key 永远对不上。
 * 早前就是因为没做这层归一化，导致所有 IPv4 查询都命中同一个全零槽位，
 * 把萤石 RTMP 的 1.2T 流量记到了广告域名 mssdk.volces.com 头上。
 *
 * 判据用 ip[10..11] 是否 == 0xffff：v6-mapped 一定满足，
 * 而裸 v4 的这两个字节恒为 0。
 */
static void norm_key(uint8_t family, const uint8_t ip[16], uint8_t out[16])
{
    memset(out, 0, 16);
    if (family == 4) {
        out[10] = 0xff;
        out[11] = 0xff;
        if (ip[10] == 0xff && ip[11] == 0xff)
            memcpy(out + 12, ip + 12, 4);   /* 已是 v6-mapped */
        else
            memcpy(out + 12, ip, 4);        /* 裸 v4 在前 4 字节 */
    } else {
        memcpy(out, ip, 16);
    }
}

dnsmap_t *dnsmap_new(size_t max_entries)
{
    dnsmap_t *m = calloc(1, sizeof(*m));
    if (!m) return NULL;

    m->max = max_entries ? max_entries : 65536;
    m->ttl_s = 3600;

    /* 容量取 >= max*2 的 2 的幂，保证负载因子 < 0.5，探测次数少 */
    size_t want = m->max * 2;
    size_t cap = 1024;
    while (cap < want) cap <<= 1;

    m->cap = cap;
    m->slots = calloc(cap, sizeof(dm_slot_t));
    if (!m->slots) { free(m); return NULL; }
    return m;
}

void dnsmap_free(dnsmap_t *m)
{
    if (!m) return;
    free(m->slots);
    free(m);
}

void dnsmap_set_ttl(dnsmap_t *m, uint32_t ttl_s)
{
    if (m) m->ttl_s = ttl_s ? ttl_s : 3600;
}

/* 把 "1.2.3.4" 或 "2001:db8::1" 解析进 16 字节缓冲，统一成 v6 形态 */
static int parse_ip(const char *s, uint8_t out[16], uint8_t *family)
{
    if (strchr(s, ':')) {
        struct in6_addr a6;
        if (inet_pton(AF_INET6, s, &a6) != 1) return -1;
        memcpy(out, &a6, 16);
        *family = 6;
        return 0;
    }

    struct in_addr a4;
    if (inet_pton(AF_INET, s, &a4) != 1) return -1;
    /* v4 加上 v6-mapped 前缀，这样同一个 IP 只有一种表示 */
    memset(out, 0, 10);
    out[10] = 0xff;
    out[11] = 0xff;
    memcpy(out + 12, &a4, 4);
    *family = 4;
    return 0;
}

static dm_slot_t *slot_find(dnsmap_t *m, const uint8_t ip[16], int create)
{
    uint64_t h = fnv1a(ip, 16);
    size_t i = mask(m, h);

    for (size_t probe = 0; probe < m->cap; probe++) {
        dm_slot_t *s = &m->slots[i];

        if (!s->used) {
            if (!create) return NULL;
            if (m->used >= m->max) {
                /* 表满：直接复用这个空槽，等于随机淘汰一条 */
                m->evicted++;
            }
            memset(s, 0, sizeof(*s));
            memcpy(s->ip, ip, 16);
            s->used = 1;
            m->used++;
            return s;
        }

        if (memcmp(s->ip, ip, 16) == 0)
            return s;

        i = (i + 1) & (m->cap - 1);
    }
    return NULL;
}

/*
 * 判断是不是"未指定地址"（0.0.0.0 / ::）。
 *
 * 为什么要拦：DNS 拦截（广告过滤、黑洞规则）常把域名解析成 0.0.0.0，
 * 这类"地址"没有任何连接意义，写进反查表只会制造污染 ——
 * 所有 v4 连接反查时都可能命中这个全零槽，拿到一个被拦截的域名。
 *
 * 兼容两种 v4 表示（前 4 字节 / v6-mapped），判断前先看形态。
 */
static int is_unspecified(uint8_t family, const uint8_t ip[16])
{
    if (family == 4) {
        const uint8_t *p = (ip[10] == 0xff && ip[11] == 0xff) ? ip + 12 : ip;
        return p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 0;
    }
    for (int i = 0; i < 16; i++)
        if (ip[i]) return 0;
    return 1;
}

/*
 * 解析 dnsmasq 日志行。
 *
 * 目标格式（dnsmasq --log-queries）：
 *   Nov  5 21:00:00 dnsmasq[1234]: reply api.example.com is 93.184.216.34
 *   也支持带 /etc/hosts 前缀和 "reply ... is <CNAME>" 的情况
 *
 * 只处理 "reply <name> is <ip>"。
 */
int dnsmap_feed(dnsmap_t *m, const char *line, uint64_t now_s)
{
    if (!m || !line) return 0;

    const char *p = strstr(line, "reply ");
    if (!p) return 0;
    p += 6;

    /* 域名到 " is " 为止 */
    const char *is = strstr(p, " is ");
    if (!is) return 0;

    size_t nlen = (size_t)(is - p);
    if (nlen == 0 || nlen >= DNSMAP_NAME_MAX) return 0;

    /* 跳过 "is an alias for" / "is <CNAME>" 这类非 IP 应答 */
    const char *ipstart = is + 4;
    if (!isdigit((unsigned char)ipstart[0]) &&
        ipstart[0] != ':' && ipstart[0] != 'f')
        return 0;

    /* 取 IP，到空白为止 */
    char ipbuf[64];
    size_t k = 0;
    while (ipstart[k] && !isspace((unsigned char)ipstart[k]) &&
           k + 1 < sizeof(ipbuf)) {
        ipbuf[k] = ipstart[k];
        k++;
    }
    ipbuf[k] = '\0';
    if (k == 0) return 0;

    /* dnsmasq 有时带 "NXDOMAIN" 之类，parse_ip 会自然失败 */
    uint8_t ip[16];
    uint8_t fam;
    if (parse_ip(ipbuf, ip, &fam) != 0) return 0;

    /* 0.0.0.0 / :: 是拦截结果，不是真实地址，不记 */
    if (is_unspecified(fam, ip)) return 0;

    /* 域名去掉结尾的点 */
    char name[DNSMAP_NAME_MAX];
    size_t nl = nlen < sizeof(name) - 1 ? nlen : sizeof(name) - 1;
    memcpy(name, p, nl);
    name[nl] = '\0';
    if (nl > 0 && name[nl - 1] == '.') name[--nl] = '\0';
    if (nl == 0) return 0;

    dm_slot_t *s = slot_find(m, ip, 1);
    if (!s) return 0;

    s->family = fam;
    s->last_s = now_s;

    /* 已存在同名就只更新时间 */
    for (int i = 0; i < DNSMAP_PER_IP; i++) {
        if (s->names[i].used && strcmp(s->names[i].name, name) == 0) {
            s->names[i].seen_s = now_s;
            return 1;
        }
    }

    uint8_t idx = s->next_slot % DNSMAP_PER_IP;
    snprintf(s->names[idx].name, DNSMAP_NAME_MAX, "%s", name);
    s->names[idx].seen_s = now_s;
    s->names[idx].used = 1;
    s->next_slot = (uint8_t)((idx + 1) % DNSMAP_PER_IP);
    if (s->count < DNSMAP_PER_IP) s->count++;

    return 1;
}

/*
 * 直接添加一条 IP -> 域名 映射。
 *
 * 这是给 DNS 嗅探（dnssniff）用的：嗅探器已经解析出 qname 和 A/AAAA 记录，
 * 不需要再绕一圈拼成 dnsmasq 日志文本再解析回来。
 *
 * family: 4 或 6；ip 必须是 16 字节（v4 用 v4-mapped 形式）。
 * 返回 1 表示写入成功，0 表示参数非法/表满。
 */
int dnsmap_add(dnsmap_t *m, uint8_t family, const uint8_t ip[16],
               const char *name, uint64_t now_s)
{
    if (!m || !ip || !name || name[0] == '\0') return 0;

    /* 0.0.0.0 / :: 是 DNS 拦截的返回值，写进反查表只会污染全零槽位 */
    if (is_unspecified(family, ip)) return 0;

    size_t nl = strlen(name);
    if (nl >= DNSMAP_NAME_MAX) nl = DNSMAP_NAME_MAX - 1;

    /* 无论调用方传裸 v4 还是 v6-mapped，统一成同一个 key 再入表 */
    uint8_t key[16];
    norm_key(family, ip, key);

    dm_slot_t *s = slot_find(m, key, 1);
    if (!s) return 0;

    s->family = family;
    s->last_s = now_s;

    /* 已存在同名就只更新时间 */
    for (int i = 0; i < DNSMAP_PER_IP; i++) {
        if (s->names[i].used && strncmp(s->names[i].name, name, nl) == 0
            && s->names[i].name[nl] == '\0') {
            s->names[i].seen_s = now_s;
            return 1;
        }
    }

    uint8_t idx = s->next_slot % DNSMAP_PER_IP;
    memcpy(s->names[idx].name, name, nl);
    s->names[idx].name[nl] = '\0';
    s->names[idx].seen_s = now_s;
    s->names[idx].used = 1;
    s->next_slot = (uint8_t)((idx + 1) % DNSMAP_PER_IP);
    if (s->count < DNSMAP_PER_IP) s->count++;

    return 1;
}

const char *dnsmap_lookup(dnsmap_t *m, uint8_t family, const uint8_t ip[16])
{
    if (!m) return NULL;

    /*
     * 统一成写入时用的 key 形态。
     *
     * ⚠️ 这里曾经有一个严重 bug，记录在此防止回退：
     *
     * 写入侧（dnsmap_add，来自 DNS 嗅探）把 v4 存成 v6-mapped：
     *     ip[10]=0xff ip[11]=0xff ip[12..15]=a.b.c.d
     * 而查询侧（netmon.c 传进来的 ctparse 结果）把 v4 存在**前 4 字节**：
     *     ip[0..3]=a.b.c.d  ip[12..15]=0
     *
     * 原来的代码不看形态，一律 memcpy(key+12, ip+12, 4)，
     * 于是查询侧取到的是 ip[12..15] —— 永远是 0。
     * 结果：**所有 IPv4 地址都归一化成 ::ffff:0.0.0.0**，命中同一个槽位。
     *
     * 而恰好所有被 DNS 拦截（返回 0.0.0.0）的域名都会写进这个全零槽，
     * 于是任意一条 IPv4 连接反查时，拿到的都是"最近被拦截的某个域名"。
     * 线上表现：同一个 IP（萤石 RTMP 服务器）被标成 mssdk.volces.com /
     * zijieapi.com / insiderservice.microsoft.com 等 7 种毫无关系的域名，
     * 流量排行完全失真，把一个 1.2T 的监控流量记到了一个广告域名头上。
     *
     * 现在按 ip[10..11] 是否为 0xffff 判断形态，两种输入都能正确归一化。
     */
    uint8_t key[16];
    norm_key(family, ip, key);

    dm_slot_t *s = slot_find(m, key, 0);
    if (!s) { m->misses++; return NULL; }

    /* 找最新的那条 */
    const dm_name_t *best = NULL;
    for (int i = 0; i < DNSMAP_PER_IP; i++) {
        if (!s->names[i].used) continue;
        if (!best || s->names[i].seen_s > best->seen_s)
            best = &s->names[i];
    }

    if (!best) { m->misses++; return NULL; }
    m->hits++;
    return best->name;
}

void dnsmap_get_stats(dnsmap_t *m, dnsmap_stats_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!m) return;

    out->ips = m->used;
    out->hits = m->hits;
    out->misses = m->misses;
    out->evicted = m->evicted;

    for (size_t i = 0; i < m->cap; i++)
        if (m->slots[i].used) out->names += m->slots[i].count;
}
