/*
 * bench_cts.c - 采样引擎基准测试
 *
 * 目标：证明每轮采样在真实数据量下足够快（不阻塞路由器转发）。
 * 直接读真实的 /proc/net/nf_conntrack 跑 N 轮，统计耗时分布。
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>
#include "ctsample.h"

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000;
}

static uint64_t g_bytes_out = 0, g_bytes_in = 0;

static void on_seen(const ct_key_t *k, const ct_rec_t *cur,
                    uint64_t d_po, uint64_t d_bo, uint64_t d_pr, uint64_t d_br,
                    void *ud)
{
    (void)k; (void)cur; (void)ud;
    g_bytes_out += d_bo;
    g_bytes_in  += d_br;
}

static void on_gone(const ct_key_t *k, uint64_t s, uint64_t l,
                    uint64_t po, uint64_t bo, uint64_t pr, uint64_t br,
                    uint8_t preexisting, void *ud)
{
    (void)k; (void)s; (void)l; (void)po; (void)pr; (void)ud;
    (void)preexisting; (void)bo; (void)br;
    g_bytes_out += 0;
    g_bytes_in  += 0;
}

static int cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}

int main(int argc, char **argv)
{
    int rounds = argc > 1 ? atoi(argv[1]) : 100;
    if (rounds < 5) rounds = 5;

    printf("=== 采样引擎基准测试 (%d 轮) ===\n\n", rounds);

    uint32_t *cost = malloc(sizeof(uint32_t) * rounds);
    cts_stats_t st;

    /* 第一轮作为预热（建表），不计入统计 */
    cts_sample(now_ms(), on_seen, on_gone, NULL, &st);
    printf("预热轮: 解析 %zu 行, 存活 %zu, 消亡 %zu, 表容量 %zu\n\n",
           st.parsed, st.alive, st.gone, st.cap);

    uint64_t t_start = now_ms();
    for (int i = 0; i < rounds; i++) {
        cts_sample(now_ms(), on_seen, on_gone, NULL, &st);
        cost[i] = st.cost_us;
        if (i == 0)
            printf("样本轮: 解析 %zu 行, 存活 %zu, 表容量 %zu\n\n",
                   st.parsed, st.alive, st.cap);
    }
    uint64_t t_total = now_ms() - t_start;

    uint32_t *sorted = malloc(sizeof(uint32_t) * rounds);
    memcpy(sorted, cost, sizeof(uint32_t) * rounds);
    qsort(sorted, rounds, sizeof(uint32_t), cmp_u32);

    uint64_t sum = 0;
    uint32_t mx = 0;
    for (int i = 0; i < rounds; i++) {
        sum += cost[i];
        if (cost[i] > mx) mx = cost[i];
    }

    printf("--- 单轮耗时（微秒）---\n");
    printf("  最小 : %u\n", sorted[0]);
    printf("  中位 : %u\n", sorted[rounds / 2]);
    printf("  p95  : %u\n", sorted[(int)(rounds * 0.95)]);
    printf("  最大 : %u\n", mx);
    printf("  平均 : %.1f\n", (double)sum / rounds);
    printf("\n--- 总览 ---\n");
    printf("  %d 轮总耗时 : %llu ms\n", rounds, (unsigned long long)t_total);
    printf("  数据量      : %.1f KB 上行 / %.1f KB 下行\n",
           g_bytes_out / 1024.0, g_bytes_in / 1024.0);

    /*
     * 关键指标：CPU 占用率估算。
     * 若采样间隔 5s，单轮平均 X ms，则 CPU 占用 = X/5000。
     */
    double avg_ms = (double)sum / rounds / 1000.0;
    printf("  按 5s 间隔采样，CPU 占用约 %.2f%%\n", avg_ms / 5000.0 * 100.0);
    printf("  按 5s 间隔采样，CPU 占用约 %.3f%% （p95=%ums 时）\n",
           sorted[(int)(rounds * 0.95)] / 1000.0 / 5000.0 * 100.0,
           sorted[(int)(rounds * 0.95)] / 1000);

    free(cost);
    free(sorted);
    return 0;
}
