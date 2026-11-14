/*
 * test_hprules.c - 规则归因里「纯字符串判定」部分的单元测试
 *
 * 只测 hprules_is_local / hprules_is_local_retry：这两个不依赖
 * homeproxy、cache.db 或 sing-box，跑在开发机上就能验证。
 * 规则集同步需要真机的 /var/run/homeproxy/cache.db，不在单测范围内
 * （仓库冒烟时用 hprules_init/hprules_refresh 现场验）。
 */
#include <stdio.h>
#include <string.h>

#include "hprules.h"

static int fail;

#define CHECK(cond, fmt, ...) do {                       \
    if (cond) printf("  ok  : " fmt "\n", __VA_ARGS__);   \
    else    { printf("  FAIL: " fmt "\n", __VA_ARGS__); fail++; } \
} while (0)

int main(void)
{
    /*
     * local = 是否局域网/特殊用途后缀；retry = 是否是「完整域名 + 搜索域」
     * 的伴生重试（会被前端默认折叠）。
     */
    static const struct { const char *q; int local, retry; } cases[] = {
        /* 真正的伴生重试：原域名失败后补 .lan 再问一次 */
        { "dig.bdurl.net.lan",        1, 1 },
        { "mail.example.com.lan",     1, 1 },
        { "oth.eve.mdt.qq.com.lan",   1, 1 },
        { "a.b.c.local",              1, 1 },
        /* 真正的内网主机名：不能折叠，否则「查不到 NAS」这类故障会看不见 */
        { "nas.lan",                  1, 0 },
        { "pages.lan",                1, 0 },
        { "printer.local",            1, 0 },
        { "x.localdomain",            1, 0 },
        /* 反向解析是完整名字，层级多但绝不能被当成重试 */
        { "1.0.2.192.in-addr.arpa",   1, 0 },
        { "b.a.9.8.7.6.5.0.1.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.ip6.arpa",
                                      1, 0 },
        { "host.invalid",             1, 0 },
        /* 普通公网域名 */
        { "www.baidu.com",            0, 0 },
        { "lan",                      0, 0 },   /* 裸后缀本身不是域内名字 */
        { "",                         0, 0 },
    };

    printf("=== 本地域判定（hprules_is_local / is_local_retry）===\n");
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        int l = hprules_is_local(cases[i].q);
        int r = hprules_is_local_retry(cases[i].q);
        CHECK(l == cases[i].local && r == cases[i].retry,
              "%-24s local=%d retry=%d（期望 %d/%d）",
              cases[i].q, l, r, cases[i].local, cases[i].retry);
    }

    if (fail) printf("\n=== %d 个失败 ===\n", fail);
    else      printf("\n=== ALL PASS ===\n");
    return fail ? 1 : 0;
}
