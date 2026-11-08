/*
 * stress_cts.c - 合成数据压力测试
 *
 * 真实 conntrack 只有几百条，测不出极限。这里直接生成 N 条合成行，
 * 喂给同一个解析+比对路径，测真实吞吐上限。
 *
 * 目的：回答"如果高峰期有 1 万 / 5 万条连接，会不会卡"。
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>
#include "ctparse.h"

#define MAXREC 120000

static char *g_lines[MAXREC];
static int   g_n = 0;

/* 生成一条逼真的 conntrack 行 */
static void gen(int i)
{
    char buf[512];
    int a = (i >> 16) & 0xff;
    int b = (i >> 8) & 0xff;
    int c = i & 0xff;

    int t = i % 4;
    if (t == 0) {
        snprintf(buf, sizeof(buf),
            "ipv4     2 tcp      6 7440 ESTABLISHED "
            "src=192.168.1.%d dst=104.16.%d.%d sport=%u dport=443 "
            "packets=100 bytes=50000 "
            "src=104.16.%d.%d dst=203.0.113.45 sport=443 dport=%u "
            "packets=200 bytes=900000 [ASSURED] mark=0 zone=0 use=2",
            (a % 250) + 2, b, c, 1024 + (i % 60000),
            b, c, 1024 + (i % 60000));
    } else if (t == 1) {
        snprintf(buf, sizeof(buf),
            "ipv4     2 udp      17 30 "
            "src=192.168.1.%d dst=8.8.8.8 sport=%u dport=53 "
            "packets=2 bytes=120 [UNREPLIED] mark=0 zone=0 use=1",
            (a % 250) + 2, 2000 + (i % 60000));
    } else if (t == 2) {
        snprintf(buf, sizeof(buf),
            "ipv6     10 tcp      6 56 TIME_WAIT "
            "src=2001:db8:1:2:70be:a391:%x:%x "
            "dst=2001:db8:3:4:0000:0000:0000:%x "
            "sport=%u dport=443 packets=17 bytes=7938 "
            "src=2001:db8:3:4::31 dst=2001:db8:1:2:70be:a391:%x:%x "
            "sport=443 dport=%u packets=12 bytes=5400 [ASSURED] mark=0 zone=0 use=2",
            a, b, c, 3000 + (i % 60000), a, b, 3000 + (i % 60000));
    } else {
        snprintf(buf, sizeof(buf),
            "ipv4     2 icmp     1 16 "
            "src=192.168.1.%d dst=1.1.1.1 type=8 code=0 id=%d "
            "packets=1 bytes=84 "
            "src=1.1.1.1 dst=203.0.113.45 type=0 code=0 id=%d "
            "packets=1 bytes=84 mark=0 zone=0 use=2",
            (a % 250) + 2, i % 65535, i % 65535);
    }
    g_lines[i] = strdup(buf);
}

int main(int argc, char **argv)
{
    int n = argc > 1 ? atoi(argv[1]) : 10000;
    int rounds = argc > 2 ? atoi(argv[2]) : 20;
    if (n > MAXREC) n = MAXREC;

    printf("=== 合成压力测试: %d 条连接 x %d 轮 ===\n\n", n, rounds);

    for (int i = 0; i < n; i++) gen(i);

    /* 每轮把每条 line 复制一份再解析（因为解析会破坏原串） */
    char *work = malloc(600);

    struct timespec t0, t1;
    uint64_t total_us = 0, min_us = UINT64_MAX, max_us = 0;
    size_t ok = 0, bad = 0;

    for (int r = 0; r < rounds; r++) {
        clock_gettime(CLOCK_MONOTONIC, &t0);
        ok = bad = 0;
        for (int i = 0; i < n; i++) {
            strcpy(work, g_lines[i]);
            ct_rec_t rec;
            if (ct_parse_line(work, &rec)) ok++; else bad++;
        }
        clock_gettime(CLOCK_MONOTONIC, &t1);

        uint64_t us = (t1.tv_sec - t0.tv_sec) * 1000000ULL
                    + (t1.tv_nsec - t0.tv_nsec) / 1000;
        total_us += us;
        if (us < min_us) min_us = us;
        if (us > max_us) max_us = us;
    }

    double avg = (double)total_us / rounds;
    printf("解析成功 %zu 条, 失败 %zu 条\n", ok, bad);
    printf("\n--- 单轮 %d 条 ---\n", n);
    printf("  最小 : %.2f ms\n", min_us / 1000.0);
    printf("  平均 : %.2f ms\n", avg / 1000.0);
    printf("  最大 : %.2f ms\n", max_us / 1000.0);
    printf("  单条 : %.3f us\n", avg / n);
    printf("  吞吐 : %.0f 条/秒\n", n / (avg / 1000000.0));

    printf("\n--- 若 5s 采样一次的 CPU 占用 ---\n");
    printf("  %d 条时 : %.3f%%\n", n, avg / 1000.0 / 5000.0 * 100.0);
    printf("  65536 条时（按线性外推）: %.3f%%\n",
           avg / n * 65536 / 1000.0 / 5000.0 * 100.0);

    free(work);
    for (int i = 0; i < n; i++) free(g_lines[i]);
    return 0;
}
