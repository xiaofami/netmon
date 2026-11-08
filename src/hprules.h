/*
 * hprules.h - homeproxy 拦截规则同步与 DNS 异常归因
 *
 * 设计要点：
 *   1) 不固化规则副本。规则集来自 sing-box 当前的 cache.db，配置来自
 *      /etc/config/homeproxy，任一变化都会被指纹检测发现并重新同步。
 *   2) 纯 C。高频的匹配/归因路径不 fork、不调外部命令；只有规则真的变了
 *      才 fork 一次 `sing-box rule-set decompile`（几十毫秒）。
 *   3) 只读，不碰 homeproxy 配置、不改防火墙、不参与 DNS 转发。
 */
#ifndef NETMON_HPRULES_H
#define NETMON_HPRULES_H

#include <stdint.h>
#include <stddef.h>

/* DNS 异常归因分类 */
enum {
    HP_OK         = 0,   /* 正常应答 */
    HP_BLOCKED    = 1,   /* 命中 homeproxy reject 规则 / rcode=5 */
    HP_LOCAL      = 2,   /* .lan 等本地搜索域的伴生 NXDOMAIN，非故障 */
    HP_UNRESOLVED = 3    /* 排除以上之后的真实解析失败，代理探测候选 */
};

typedef struct {
    char label[64];       /* UCI 里的 label，如 BlockAD */
    char scope[8];        /* dns / route */
    int  enabled;
    int  patterns;        /* 该规则贡献的模式数 */
    char rulesets[160];   /* 引用的 rule_set，逗号分隔 */
} hp_rule_meta_t;

typedef struct {
    char     fp[33];      /* 当前规则指纹（十六进制） */
    uint64_t changed_ms;  /* 最近一次检测到变化的时间 */
    int      rules;       /* 当前 reject 规则数 */
    int      patterns;    /* 当前模式总数 */
    char     detail[256]; /* 最近一次变更的可读描述 */
} hp_version_t;

/*
 * 初始化路径。
 *   cache_db  : /var/run/homeproxy/cache.db
 *   uci_conf  : /etc/config/homeproxy
 *   work_dir  : 可写目录，存放反编译中间文件（/opt/netmon/data）
 */
int hprules_init(const char *cache_db, const char *uci_conf, const char *work_dir);

/* homeproxy 是可选依赖：没装时规则同步安静降级，
   面板其余功能（设备、流量、DNS、系统状态）完全可用。
   available=0 时 hprules_classify() 不再把任何查询判成「广告拦截」。 */
int hprules_available(void);
const char *hprules_reason(void);

/*
 * 重新计算指纹；发现变化时自动重新反编译 + 重建匹配表。
 * 采集进程每 30 秒调一次，报表进程每次 CGI 请求调一次。
 * *changed 置 1 表示本次检测到规则变化（调用方应记一条事件）。
 * 返回 0 成功；负数表示同步失败（仍保留上一次可用的规则表）。
 */
int hprules_refresh(int *changed);

/* 归因：返回 HP_* 之一；rule_out 回填命中的规则 label（可传 NULL） */
int hprules_classify(const char *qname, int rcode, char *rule_out, size_t n);

/* 只判断域名是否命中拦截规则（不含 rcode 兜底） */
int hprules_match(const char *qname, char *rule_out, size_t n);
int hprules_is_local(const char *qname);

int  hprules_meta_count(void);
const hp_rule_meta_t *hprules_meta(int i);
int  hprules_pattern_count(void);
const hp_version_t  *hprules_version(void);

/* 输出规则清单的 JSON 数组（不含外层 key），供 dashboard 直接拼 */
void hprules_meta_json(char *out, size_t n);

#endif /* NETMON_HPRULES_H */
