/*
 * test_probe.c - probe 模块实测
 *
 * 不是纯单元测试，是真的打网络——因为探测模块的价值就在于
 * 把网络真实状况量出来，mock 掉就没意义了。
 */

#define _GNU_SOURCE
#include "probe.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static void show_ping(const char *label, const char *target)
{
    ping_result_t r;
    memset(&r, 0, sizeof(r));

    int rc = probe_ping(target, 4, 1500, &r);
    if (rc != 0) {
        printf("  %-12s %-18s  探测失败 errno=%d (%s)\n",
               label, target, r.err, strerror(r.err));
        return;
    }
    printf("  %-12s %-18s  %d/%d 收  loss=%.0f%%  "
           "rtt min/avg/max=%.1f/%.1f/%.1f ms  jitter=%.1f\n",
           label, target, r.ok, r.sent, r.loss * 100,
           r.rtt_min, r.rtt_avg, r.rtt_max, r.jitter);
}

static void show_http(const char *label, const char *url, long max_bytes)
{
    http_result_t r;
    memset(&r, 0, sizeof(r));

    int rc = probe_http(url, 15000, max_bytes, &r);
    if (rc != 0) {
        printf("  %-12s %-45s  失败 errno=%d\n", label, url, r.err);
        return;
    }
    printf("  %-12s code=%d ok=%d  conn=%.0f tls=%.0f ttfb=%.0f total=%.0f ms  "
           "%.1f KB  %.0f kbps\n",
           label, r.code, r.ok, r.connect_ms, r.tls_ms,
           r.ttfb_ms, r.total_ms, r.bytes / 1024.0, r.kbps);
}

static void show_dns(const char *label, const char *name)
{
    dns_result_t r;
    memset(&r, 0, sizeof(r));

    int rc = probe_dns(name, 3000, &r);
    if (rc != 0) {
        printf("  %-12s %-30s  调用失败\n", label, name);
        return;
    }
    printf("  %-12s %-30s  ok=%d rcode=%u %.1f ms  answers=%d  first=%s\n",
           label, name, r.ok, r.rcode, r.ms, r.answers, r.first_ip);
}

static void cb(const probe_target_t *t, const ping_result_t *r, void *ud)
{
    (void)ud;
    printf("  %-12s %-18s  %d/%d  loss=%.0f%%  avg=%.1f ms\n",
           t->tag ? t->tag : "-", t->host,
           r->ok, r->sent, r->loss * 100, r->rtt_avg);
}

int main(int argc, char **argv)
{
    /* 允许命令行传 WAN 源 IP 用于绕过代理：./test_probe 203.0.113.10 */
    if (argc > 1) {
        probe_set_bypass_iface(argv[1]);
        printf("绕过代理：interface=%s mark=%u\n\n", argv[1], probe_get_bypass_mark());
    }

    printf("=== 1. Ping 国内/国外目标 ===\n");
    show_ping("gw",        "192.168.1.1");
    show_ping("dns-mobile", "211.137.32.178");
    show_ping("cf-104.16",  "104.16.0.1");
    show_ping("cf-1.1.1.1", "1.1.1.1");
    show_ping("google",     "8.8.8.8");
    show_ping("baidu",      "180.101.50.242");

    printf("\n=== 2. HTTP 拨测 ===\n");
    show_http("cf-trace",  "https://www.cloudflare.com/cdn-cgi/trace", 1);
    show_http("baidu",     "https://www.baidu.com/", 1);
    show_http("github",    "https://github.com/", 1);

    printf("\n=== 3. 带宽测速（限 3MB）===\n");
    show_http("cf-speed",   "https://speed.cloudflare.com/__down?bytes=3000000", 3145728);

    printf("\n=== 4. DNS 解析质量 ===\n");
    show_dns("a-record",  "www.baidu.com");
    show_dns("cf",        "www.cloudflare.com");
    show_dns("nxdomain",  "this-does-not-exist-xyz123.com");
    show_dns("v6",        "ipv6.google.com");

    printf("\n=== 5. 批量线路质量 ===\n");
    probe_target_t tg[] = {
        { "gateway",  "192.168.1.1" },
        { "dns-mob",  "211.137.32.178" },
        { "cf-hkg",   "104.16.0.1" },
        { "cf-sin",   "172.64.0.1" },
    };
    probe_run_group(tg, sizeof(tg)/sizeof(tg[0]), 3, 1200, cb, NULL);

    printf("\n=== 6. 参数边界 ===\n");
    ping_result_t pr;
    int rc = probe_ping("not-an-ip", 1, 500, &pr);
    printf("  非IP字面量: rc=%d errno=%d (应为 -1/EINVAL)\n", rc, pr.err);

    http_result_t hr;
    rc = probe_http("https://example.com/;rm -rf /", 1000, 1, &hr);
    printf("  shell注入防御: rc=%d errno=%d (应为 -1/EINVAL)\n", rc, hr.err);

    rc = probe_http("http://this-host-does-not-exist-abc123.invalid/", 3000, 1, &hr);
    printf("  不存在的主机: rc=%d ok=%d (应为失败)\n", rc, hr.ok);

    
    return 0;
}
