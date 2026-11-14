/*
 * hprules.c - homeproxy 拦截规则同步与 DNS 异常归因（实现）
 *
 * 规则来源（全部现场读取，不固化副本）：
 *   /etc/config/homeproxy            reject 规则（label / scope / 显式域名 / rule_set）
 *   /var/run/homeproxy/cache.db      sing-box 缓存的各 rule_set 二进制(.srs)
 *
 * 同步机制：
 *   指纹 = UCI 全文 FNV  +  各被引用 rule_set 的 SRS 段 FNV
 *   任一变化 -> 重新反编译变化的 SRS -> 重建匹配表 -> 记录一次变更事件。
 *   只看内容不看 mtime，避免 sing-box 定期写 cache.db 造成无谓重编译。
 *   采集进程每 30s、报表进程每次请求都会检查，因此规则改动最迟 30s 生效。
 *
 * 为什么用 sing-box rule-set decompile 而不是自己解 SRS：
 *   SRS v2 解压后是压缩的 domain trie + 位图，不是明文。官方 decompile
 *   输出标准 source JSON，既准确又不会随 sing-box 版本变化而失效。
 *   这个外部命令只在规则真的变化时 fork 一次，不在任何热路径上。
 */
#include "hprules.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>   /* strcasecmp / strncasecmp：POSIX 规定在这里，别只靠 glibc 的 string.h 兜底 */
#include <stdint.h>
#include <ctype.h>
#include <regex.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>

#define MAX_PATTERN  8192
#define MAX_REGEX    512
#define MAX_META     32
#define MAX_RULESET  16
#define PAT_LEN      160
#define RX_LEN       512

typedef struct {
    char pat[PAT_LEN];
    char label[64];
} suf_t;

typedef struct {
    regex_t rx;
    char    pat[RX_LEN];
    char    label[64];
} rx_t;

static char g_cache[256];
static char g_uci[256];
static char g_work[256];

static suf_t  g_suf[MAX_PATTERN];
static int    g_nsuf;
static rx_t   g_rx[MAX_REGEX];
static int    g_nrx;
static hp_rule_meta_t g_meta[MAX_META];
static int    g_nmeta;
static hp_version_t   g_ver;
static char   g_uci_fp[20], g_rs_fp[20];
static int    g_ready;
static int    g_available;      /* homeproxy 是否可用（可选依赖） */
static char   g_reason[160];    /* 不可用原因，展示在面板上 */
static char   g_ads_label[64];

/* ---------------- 通用小工具 ---------------- */

static uint64_t fnv1a(const void *p, size_t n, uint64_t h)
{
    const unsigned char *b = (const unsigned char *)p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ULL; }
    return h;
}
static void hex16(uint64_t h, char out[20])
{
    snprintf(out, 20, "%016llx", (unsigned long long)h);
}

static unsigned char *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long n = ftell(f);
    if (n < 0 || n > 32 * 1024 * 1024) { fclose(f); return NULL; }
    rewind(f);
    unsigned char *b = (unsigned char *)malloc((size_t)n + 1);
    if (!b) { fclose(f); return NULL; }
    size_t got = fread(b, 1, (size_t)n, f);
    fclose(f);
    b[got] = 0;
    if (len) *len = got;
    return b;
}

/* memmem 在部分 libc 里没有，自己实现（cache.db 只有 128KB，朴素搜索足够） */
static size_t mem_find(const unsigned char *hay, size_t n,
                       const void *needle, size_t m)
{
    if (!m || m > n) return (size_t)-1;
    const unsigned char *p = (const unsigned char *)needle;
    for (size_t i = 0; i + m <= n; i++) {
        if (hay[i] == p[0] && !memcmp(hay + i, p, m)) return i;
    }
    return (size_t)-1;
}

static int exists_nonempty(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && st.st_size > 0;
}

static void trim_quote(char *s)
{
    if (!s) return;
    while (*s == ' ' || *s == '\t') s++;
    size_t n = strlen(s);
    while (n && (s[n-1] == '\n' || s[n-1] == '\r' || s[n-1] == ' ')) s[--n] = 0;
    if (n >= 2 && ((s[0] == '\'' && s[n-1] == '\'') ||
                   (s[0] == '"'  && s[n-1] == '"'))) {
        s[n-1] = 0;
        memmove(s, s + 1, n - 1);
    }
}

static uint64_t wall_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000);
}

/* ---------------- 匹配表构建 ---------------- */

static void add_suffix(const char *s, const char *label)
{
    if (!s || !*s || g_nsuf >= MAX_PATTERN) return;
    size_t n = strlen(s);
    if (n >= PAT_LEN) return;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (!(isalnum(c) || c == '.' || c == '-')) return;   /* 不是域名 */
    }
    for (int i = 0; i < g_nsuf; i++)
        if (!strcmp(g_suf[i].pat, s)) return;                 /* 去重 */
    snprintf(g_suf[g_nsuf].pat, PAT_LEN, "%s", s);
    snprintf(g_suf[g_nsuf].label, sizeof(g_suf[0].label), "%s", label ? label : "");
    g_nsuf++;
}

static void add_regex(const char *s, const char *label)
{
    if (!s || !*s || g_nrx >= MAX_REGEX) return;
    if (strlen(s) >= RX_LEN) return;
    /* Go regex 里的 (?:...) / (?i) 之类 POSIX ERE 不支持，编译失败就跳过，
       不影响其它规则；显式规则仍可通过后缀命中。 */
    if (regcomp(&g_rx[g_nrx].rx, s, REG_EXTENDED | REG_NOSUB | REG_ICASE) != 0)
        return;
    snprintf(g_rx[g_nrx].pat, RX_LEN, "%s", s);
    snprintf(g_rx[g_nrx].label, sizeof(g_rx[0].label), "%s", label ? label : "");
    g_nrx++;
}

static void json_esc_local(const char *s, char *out, size_t n);

/* 规则集展开出来的模式按 label 回填到对应 reject 规则，
   否则前端会看到「BlockAD 贡献 0 条模式」这种自相矛盾的数字 */
static void recount_meta_patterns(void)
{
    for (int i = 0; i < g_nmeta; i++) g_meta[i].patterns = 0;
    for (int i = 0; i < g_nsuf; i++)
        for (int j = 0; j < g_nmeta; j++)
            if (!strcmp(g_meta[j].label, g_suf[i].label)) { g_meta[j].patterns++; break; }
    for (int i = 0; i < g_nrx; i++)
        for (int j = 0; j < g_nmeta; j++)
            if (!strcmp(g_meta[j].label, g_rx[i].label)) { g_meta[j].patterns++; break; }
}

static void reset_table(void)
{
    for (int i = 0; i < g_nrx; i++) regfree(&g_rx[i].rx);
    g_nrx = 0;
    g_nsuf = 0;
    g_nmeta = 0;
    g_ads_label[0] = 0;
}

/* 扫描 source JSON 里所有 "..." 字符串。
   decompile 输出的结构固定：键名不含 '.'，值就是域名或正则，
   因此无需完整 JSON 解析器，按字符串特征分类即可。 */
static void scan_rule_json(const unsigned char *buf, size_t n, const char *label)
{
    char tmp[1024];
    size_t tl = 0;
    int in_str = 0, bad = 0;

    for (size_t i = 0; i < n; i++) {
        unsigned char c = buf[i];
        if (!in_str) {
            if (c == '"') { in_str = 1; tl = 0; bad = 0; }
            continue;
        }
        if (c == '\\') {
            if (i + 1 >= n) break;
            unsigned char e = buf[++i];
            switch (e) {
            case 'n': e = '\n'; break;
            case 't': e = '\t'; break;
            case 'r': e = '\r'; break;
            case 'u': bad = 1; break;     /* 广告规则里没有 unicode 转义 */
            default: break;               /* \\ \" \/ 原样保留字符 */
            }
            if (tl + 1 < sizeof(tmp)) tmp[tl++] = (char)e;
            continue;
        }
        if (c == '"') {
            tmp[tl] = 0;
            if (!bad && tl) {
                int is_rx = strpbrk(tmp, "^$[](){}|\\*+?") != NULL;
                if (is_rx) add_regex(tmp, label);
                else if (strchr(tmp, '.')) add_suffix(tmp, label);
            }
            in_str = 0;
            continue;
        }
        if (tl + 1 < sizeof(tmp)) tmp[tl++] = (char)c;
    }
}

/* ---------------- UCI 配置解析 ---------------- */

typedef struct {
    char label[64];
    char scope[8];
    int  enabled;
    char action[32];
    char rulesets[MAX_RULESET][64];
    int  nruleset;
    char pats[64][512];
    int  npats;
} uci_sec_t;

static void sec_flush(uci_sec_t *s)
{
    if (!s || strcmp(s->action, "reject") != 0 || !s->label[0]) return;
    if (g_nmeta >= MAX_META) return;

    hp_rule_meta_t *m = &g_meta[g_nmeta++];
    memset(m, 0, sizeof(*m));
    snprintf(m->label, sizeof(m->label), "%s", s->label);
    snprintf(m->scope, sizeof(m->scope), "%s", s->scope);
    m->enabled = s->enabled;
    m->rulesets[0] = 0;
    for (int i = 0; i < s->nruleset; i++) {
        if (i) strncat(m->rulesets, ",", sizeof(m->rulesets) - strlen(m->rulesets) - 1);
        strncat(m->rulesets, s->rulesets[i],
                sizeof(m->rulesets) - strlen(m->rulesets) - 1);
        if (strncasecmp(s->rulesets[i], "ads", 3) == 0)
            snprintf(g_ads_label, sizeof(g_ads_label), "%s", s->label);
    }

    int before = g_nsuf + g_nrx;
    for (int i = 0; i < s->npats; i++) {
        if (strpbrk(s->pats[i], "^$[](){}|\\*+?")) add_regex(s->pats[i], s->label);
        else add_suffix(s->pats[i], s->label);
    }
    m->patterns = (g_nsuf + g_nrx) - before;
}

static void parse_uci_buf(const unsigned char *buf, size_t n)
{
    uci_sec_t sec;
    memset(&sec, 0, sizeof(sec));
    int in_sec = 0;
    size_t start = 0;

    for (size_t i = 0; i <= n; i++) {
        if (i < n && buf[i] != '\n') continue;
        size_t len = i - start;
        if (len && len < 1024) {
            char line[1024];
            memcpy(line, buf + start, len);
            line[len] = 0;
            char *t = line;
            while (*t == ' ' || *t == '\t') t++;

            if (!strncmp(t, "config ", 7)) {
                if (in_sec) sec_flush(&sec);
                memset(&sec, 0, sizeof(sec));
                sec.enabled = 1;
                in_sec = strstr(t, "dns_rule") || strstr(t, "routing_rule");
                if (in_sec)
                    snprintf(sec.scope, sizeof(sec.scope), "%s",
                             strstr(t, "dns_rule") ? "dns" : "route");
                /* section 名可作为 label 兜底 */
                char *q = strchr(t + 7, '\'');
                if (q) {
                    char *e = strchr(q + 1, '\'');
                    if (e) { *e = 0; snprintf(sec.label, sizeof(sec.label), "%s", q + 1); }
                }
            } else if (in_sec && (*t == 'o' || *t == 'l')) {
                char kw[16] = "", key[64] = "", val[600] = "";
                if (sscanf(t, "%15s %63s %599[^\n]", kw, key, val) >= 2) {
                    trim_quote(val);
                    if (!strcmp(key, "label"))       snprintf(sec.label, sizeof(sec.label), "%s", val);
                    else if (!strcmp(key, "action")) snprintf(sec.action, sizeof(sec.action), "%s", val);
                    else if (!strcmp(key, "enabled")) sec.enabled = !(val[0] == '0');
                    else if (!strcmp(key, "rule_set") && sec.nruleset < MAX_RULESET)
                        snprintf(sec.rulesets[sec.nruleset++], 64, "%s", val);
                    else if ((!strcmp(key, "domain") || !strcmp(key, "domain_suffix") ||
                              !strcmp(key, "domain_keyword") || !strcmp(key, "domain_regex"))
                             && sec.npats < 64)
                        snprintf(sec.pats[sec.npats++], 512, "%s", val);
                }
            }
        }
        start = i + 1;
    }
    if (in_sec) sec_flush(&sec);
}

/* ---------------- 规则集缓存 -> 明文 ---------------- */

/* 找 rule_set 名对应的 SRS 段：cache.db 里有明文键 cfg-<name>-rule，
   其后紧跟该规则集的二进制流（SRS\x02 + zlib）。 */
static int find_srs(const unsigned char *buf, size_t n, const char *rsname,
                    size_t *off, size_t *len)
{
    char key[128];
    snprintf(key, sizeof(key), "cfg-%s-rule", rsname);
    size_t k = mem_find(buf, n, key, strlen(key));
    if (k == (size_t)-1) return -1;
    size_t s = mem_find(buf + k, n - k, "SRS\x02", 4);
    if (s == (size_t)-1) return -1;
    s += k;
    size_t e = mem_find(buf + s + 4, n - s - 4, "SRS\x02", 4);
    e = (e == (size_t)-1) ? n : e + s + 4;
    if (e <= s) return -1;
    *off = s;
    *len = e - s;
    return 0;
}

/* 把一个 rule_set 反编译成 source JSON；成功返回 0。
   用 .tmp + rename 保证其它进程永远读到完整文件。 */
static int decompile(const unsigned char *cache, size_t cache_n, const char *rsname)
{
    size_t off = 0, len = 0;
    if (strpbrk(rsname, ";&|`$<>()'\"\\ \t\n") != NULL) return -1;
    if (find_srs(cache, cache_n, rsname, &off, &len) != 0) return -1;

    char srs[512], tmp[512], dst[512], cmd[1400];
    snprintf(srs, sizeof(srs), "%s/rs-%s.srs", g_work, rsname);
    /* 采集进程和 CGI 进程可能同时检测到规则变化；tmp 带上 PID，
       避免两个 decompile 互相 unlink 掉对方的输出 */
    snprintf(tmp, sizeof(tmp), "%s/rs-%s.%d.json.tmp", g_work, rsname, (int)getpid());
    snprintf(dst, sizeof(dst), "%s/rs-%s.json", g_work, rsname);

    FILE *f = fopen(srs, "wb");
    if (!f) return -1;
    size_t w = fwrite(cache + off, 1, len, f);
    fclose(f);
    if (w != len) return -1;

    unlink(tmp);
    /* 路径由常量 + 已过滤的规则名拼成，不经过任何用户输入 */
    snprintf(cmd, sizeof(cmd),
             "sing-box rule-set decompile -o '%s' '%s' >/dev/null 2>&1", tmp, srs);
    if (system(cmd) != 0 || !exists_nonempty(tmp)) return -1;
    unlink(dst);
    if (rename(tmp, dst) != 0) return -1;
    return 0;
}

/* ---------------- 指纹 ---------------- */

/*
 * 从 UCI 原文里挑出所有 action=reject 段引用的 rule_set。
 *
 * 关键：必须读文件本身，不能用内存里的 g_meta。
 * 否则用户新增一条引用新规则集的 reject 规则时，本轮指纹还是按旧清单算的，
 * 新规则集要等下一轮才被发现 —— 规则同步会整整晚一个周期。
 */
static int uci_reject_rulesets(const unsigned char *ub, size_t un,
                               char seen[][64], int max)
{
    int n = 0, in_reject = 0;
    size_t start = 0;
    for (size_t i = 0; i <= un; i++) {
        if (i < un && ub[i] != '\n') continue;
        size_t len = i - start;
        if (len && len < 512) {
            char line[512];
            memcpy(line, ub + start, len);
            line[len] = 0;
            char *t = line;
            while (*t == ' ' || *t == '\t') t++;
            if (!strncmp(t, "config ", 7)) {
                in_reject = 0;
            } else if (*t == 'o' || *t == 'l') {
                char kw[16] = "", key[64] = "", val[128] = "";
                if (sscanf(t, "%15s %63s %127[^\n]", kw, key, val) >= 2) {
                    trim_quote(val);
                    if (!strcmp(key, "action")) in_reject = !strcmp(val, "reject");
                    else if (in_reject && !strcmp(key, "rule_set") && val[0] && n < max) {
                        int dup = 0;
                        for (int j = 0; j < n; j++)
                            if (!strcmp(seen[j], val)) { dup = 1; break; }
                        if (!dup) snprintf(seen[n++], 64, "%s", val);
                    }
                }
            }
        }
        start = i + 1;
    }
    return n;
}

static int compute_fp(char uci_fp[20], char rs_fp[20],
                      unsigned char **cache_out, size_t *cache_n)
{
    size_t un = 0;
    unsigned char *ub = read_file(g_uci, &un);
    if (!ub) return -1;
    uint64_t hu = fnv1a(ub, un, 1469598103934665603ULL);
    char seen[MAX_RULESET][64];
    int nseen = uci_reject_rulesets(ub, un, seen, MAX_RULESET);
    free(ub);
    hex16(hu, uci_fp);

    size_t cn = 0;
    unsigned char *cb = read_file(g_cache, &cn);
    uint64_t hr = fnv1a("rs", 2, 1469598103934665603ULL);
    if (cb) {
        for (int i = 0; i < nseen; i++) {
            size_t off = 0, len = 0;
            if (find_srs(cb, cn, seen[i], &off, &len) == 0)
                hr = fnv1a(cb + off, len, hr);
            hr = fnv1a(seen[i], strlen(seen[i]), hr);
        }
        *cache_out = cb;
        *cache_n = cn;
    } else {
        *cache_out = NULL;
        *cache_n = 0;
    }
    hex16(hr, rs_fp);
    return 0;
}

/* ---------------- 对外接口 ---------------- */

/* 只做文件存在性判断，不依赖 homeproxy 的任何二进制或 uci 命令 */
static int probe_files(void)
{
    struct stat st;
    if (stat(g_uci, &st) != 0) {
        snprintf(g_reason, sizeof(g_reason),
                 "未检测到 homeproxy（%.100s 不存在）", g_uci);
        return 0;
    }
    if (stat(g_cache, &st) != 0) {
        snprintf(g_reason, sizeof(g_reason),
                 "homeproxy 规则缓存尚未生成（%.100s）", g_cache);
        return 0;
    }
    return 1;
}

int hprules_available(void) { return g_available; }
const char *hprules_reason(void) { return g_reason[0] ? g_reason : "ok"; }

int hprules_init(const char *cache_db, const char *uci_conf, const char *work_dir)
{
    snprintf(g_cache, sizeof(g_cache), "%s", cache_db);
    snprintf(g_uci,   sizeof(g_uci),   "%s", uci_conf);
    snprintf(g_work,  sizeof(g_work),  "%s", work_dir);
    memset(&g_ver, 0, sizeof(g_ver));
    g_uci_fp[0] = g_rs_fp[0] = 0;
    g_ready = 0; g_available = 0; g_reason[0] = 0;

    if (!probe_files()) {
        /* 没装 homeproxy 属正常情况，不是错误：清空规则表安静降级 */
        g_nmeta = g_nsuf = g_nrx = 0;
        snprintf(g_ver.detail, sizeof(g_ver.detail), "%s", g_reason);
        return 0;
    }
    int changed = 0;
    if (hprules_refresh(&changed) != 0) {
        g_available = 0;
        snprintf(g_reason, sizeof(g_reason),
                 "规则同步失败，拦截归因已禁用（其余功能不受影响）");
        return 0;                 /* 不让可选依赖拖垮主流程 */
    }
    return 0;
}

int hprules_refresh(int *changed)
{
    if (changed) *changed = 0;
    if (!g_available) {
        /* 支持运行中才安装 homeproxy：每次刷新重探一次，装上就自动启用 */
        if (!probe_files()) return 0;
        g_ready = 0;             /* 强制全量重建 */
    }

    unsigned char *cache = NULL;
    size_t cache_n = 0;
    char ufp[20], rfp[20];
    if (compute_fp(ufp, rfp, &cache, &cache_n) != 0) {
        free(cache);
        return -1;
    }

    int uci_changed = !g_ready || strcmp(ufp, g_uci_fp) != 0;
    int rs_changed  = !g_ready || strcmp(rfp, g_rs_fp) != 0;
    if (!uci_changed && !rs_changed) { free(cache); return 0; }

    int old_rules = g_ver.rules, old_pat = g_ver.patterns;

    /* 1) 规则集内容变了才重新反编译；只改 UCI 时复用已生成的 JSON */
    if (rs_changed && cache) {
        for (int i = 0; i < g_nmeta; i++) {
            char buf[160];
            snprintf(buf, sizeof(buf), "%s", g_meta[i].rulesets);
            char *p = strtok(buf, ",");
            while (p) {
                decompile(cache, cache_n, p);   /* 单个失败不影响其它 */
                p = strtok(NULL, ",");
            }
        }
    }

    /* 2) 所有可能失败的 IO 都做完了，再一次性重建内存表 */
    size_t un = 0;
    unsigned char *ub = read_file(g_uci, &un);
    if (!ub) { free(cache); return -1; }
    reset_table();
    parse_uci_buf(ub, un);
    free(ub);

    if (cache) {
        for (int i = 0; i < g_nmeta; i++) {
            char buf[160];
            snprintf(buf, sizeof(buf), "%s", g_meta[i].rulesets);
            char *p = strtok(buf, ",");
            while (p) {
                char path[512];
                snprintf(path, sizeof(path), "%s/rs-%s.json", g_work, p);
                size_t jn = 0;
                unsigned char *jb = read_file(path, &jn);
                if (jb) {
                    /* 该 rule_set 归属哪条 reject 规则；没找到就用规则集名 */
                    const char *lb = g_ads_label[0] ? g_ads_label : p;
                    scan_rule_json(jb, jn, lb);
                    free(jb);
                }
                p = strtok(NULL, ",");
            }
        }
    }
    free(cache);

    recount_meta_patterns();

    /* 3) 版本与变更描述 */
    snprintf(g_ver.fp, sizeof(g_ver.fp), "%.16s%.16s", ufp, rfp);
    g_ver.changed_ms = wall_ms();
    g_ver.rules = g_nmeta;
    g_ver.patterns = g_nsuf + g_nrx;
    if (!g_ready) {
        snprintf(g_ver.detail, sizeof(g_ver.detail),
                 "初始加载：%d 条 reject 规则 / %d 条模式", g_ver.rules, g_ver.patterns);
    } else {
        const char *what = uci_changed && rs_changed ? "配置与规则集均变更"
                         : uci_changed ? "配置变更" : "规则集更新";
        snprintf(g_ver.detail, sizeof(g_ver.detail),
                 "%s：规则 %d→%d，模式 %d→%d", what,
                 old_rules, g_ver.rules, old_pat, g_ver.patterns);
    }
    snprintf(g_uci_fp, sizeof(g_uci_fp), "%s", ufp);
    snprintf(g_rs_fp,  sizeof(g_rs_fp),  "%s", rfp);
    g_ready = 1;
    g_available = 1;
    g_reason[0] = 0;
    if (changed) *changed = 1;
    return 0;
}

int hprules_match(const char *qname, char *rule_out, size_t n)
{
    if (!qname || !*qname) return 0;
    size_t qn = strlen(qname);
    char q[512];
    if (qn >= sizeof(q)) return 0;
    for (size_t i = 0; i < qn; i++) q[i] = (char)tolower((unsigned char)qname[i]);
    q[qn] = 0;
    while (qn && q[qn-1] == '.') q[--qn] = 0;

    for (int i = 0; i < g_nsuf; i++) {
        const char *s = g_suf[i].pat;
        size_t sn = strlen(s);
        int exact   = (qn == sn && !strcmp(q, s));
        int subdom  = (qn > sn && q[qn - sn - 1] == '.' && !strcmp(q + qn - sn, s));
        if (exact || subdom) {
            if (rule_out && n)
                snprintf(rule_out, n, "%s",
                         g_suf[i].label[0] ? g_suf[i].label : "rule_set");
            return 1;
        }
    }
    for (int i = 0; i < g_nrx; i++) {
        if (regexec(&g_rx[i].rx, q, 0, NULL, 0) == 0) {
            if (rule_out && n)
                snprintf(rule_out, n, "%s", g_rx[i].label[0] ? g_rx[i].label : "regex");
            return 1;
        }
    }
    return 0;
}

static const char *LOCAL_SUF[] = { "lan", "local", "localdomain",
                                   "arpa", "localhost", "invalid", NULL };

int hprules_is_local(const char *qname)
{
    if (!qname || !*qname) return 0;
    size_t n = strlen(qname);
    for (int i = 0; LOCAL_SUF[i]; i++) {
        size_t sn = strlen(LOCAL_SUF[i]);
        if (n > sn && qname[n - sn - 1] == '.' &&
            !strcasecmp(qname + n - sn, LOCAL_SUF[i])) return 1;
    }
    return 0;
}

int hprules_is_local_retry(const char *qname)
{
    if (!qname || !*qname) return 0;
    if (!hprules_is_local(qname)) return 0;

    size_t n = strlen(qname);
    while (n && qname[n - 1] == '.') n--;      /* 容忍 FQDN 的尾点 */

    size_t i = n;
    while (i > 0 && qname[i - 1] != '.') i--;  /* i = 最后一级标签起点 */
    if (i < 2) return 0;                       /* 没有点 → 单标签名 */

    /*
     * 只有搜索域才会被客户端自动追加。.arpa 是标准反查后缀
     * （1.0.2.192.in-addr.arpa 本身就是完整名字），.invalid 是保留域，
     * 把它们的层级结构当成「补后缀」会误判 —— 反查是真查询，不能折叠。
     */
    const char *suf = qname + i;
    if (strcasecmp(suf, "lan") && strcasecmp(suf, "local") &&
        strcasecmp(suf, "localdomain"))
        return 0;

    /* 剩余部分 qname[0 .. i-2] 里若还有 '.'，说明是把完整域名拼上了本地后缀 */
    for (size_t k = 0; k + 1 < i - 1; k++)
        if (qname[k] == '.') return 1;
    return 0;
}

int hprules_classify(const char *qname, int rcode, char *rule_out, size_t n)
{
    if (rule_out && n) rule_out[0] = 0;
    /* 没有规则表时绝不臆测「广告拦截」：本地域仍可判，
       其余异常一律归为解析失败，避免把未知原因说成策略拦截。 */
    if (!g_available) {
        if (hprules_is_local(qname)) {
            if (rule_out && n) snprintf(rule_out, n, "本地搜索域");
            return HP_LOCAL;
        }
        return rcode == 0 ? HP_OK : HP_UNRESOLVED;
    }
    if (hprules_is_local(qname)) {
        if (rule_out && n) snprintf(rule_out, n, "本地搜索域");
        return HP_LOCAL;
    }
    if (hprules_match(qname, rule_out, n)) return HP_BLOCKED;
    /* sing-box 的 DNS reject 默认就是 REFUSED；即便规则集暂时读不到，
       这条也能把广告拦截兜住，不会把它误报成网络故障 */
    if (rcode == 5) {
        if (rule_out && n) snprintf(rule_out, n, "reject(rcode=5)");
        return HP_BLOCKED;
    }
    if (rcode != 0) return HP_UNRESOLVED;
    return HP_OK;
}

int hprules_meta_count(void) { return g_nmeta; }
const hp_rule_meta_t *hprules_meta(int i)
{
    return (i >= 0 && i < g_nmeta) ? &g_meta[i] : NULL;
}
int hprules_pattern_count(void) { return g_nsuf + g_nrx; }
const hp_version_t *hprules_version(void) { return &g_ver; }

void hprules_meta_json(char *out, size_t n)
{
    if (!out || !n) return;
    size_t o = 0;
    out[0] = 0;
    for (int i = 0; i < g_nmeta; i++) {
        char el[256], es[256];
        json_esc_local(g_meta[i].label, el, sizeof(el));
        json_esc_local(g_meta[i].rulesets, es, sizeof(es));
        int w = snprintf(out + o, n - o,
            "%s{\"label\":\"%s\",\"scope\":\"%s\",\"enabled\":%d,"
            "\"rulesets\":\"%s\",\"patterns\":%d}",
            i ? "," : "", el, g_meta[i].scope, g_meta[i].enabled,
            es, g_meta[i].patterns);
        if (w < 0 || (size_t)w >= n - o) break;
        o += (size_t)w;
    }
}

/* 本模块独立编译，不依赖 netmon-report.c 的静态函数 */
static void json_esc_local(const char *s, char *out, size_t n)
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
