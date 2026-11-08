#ifndef CTSAMPLE_H
#define CTSAMPLE_H

#include <stdint.h>
#include <stddef.h>
#include "ctparse.h"

typedef struct {
    size_t   parsed;      /* 本轮解析成功的行数 */
    size_t   unknown;     /* 解析失败跳过的行数 */
    size_t   alive;       /* 当前存活连接数 */
    size_t   gone;        /* 本轮消亡的连接数 */
    size_t   cap;         /* 哈希表容量 */
    uint32_t cost_us;     /* 本轮耗时（微秒） */
    uint8_t  error;
} cts_stats_t;

typedef void (*seen_cb_t)(const ct_key_t *k, const ct_rec_t *cur,
                          uint64_t d_pkt_o, uint64_t d_byt_o,
                          uint64_t d_pkt_r, uint64_t d_byt_r,
                          void *ud);

/*
 * 连接消亡回调。
 *
 * preexisting=1 表示这条连接在 netmon 启动前就已存在，
 * 所以 seen_ms 只是"程序启动时刻"，不是真实建立时间，dur 不可信。
 * 它的累计字节数仍然是 conntrack 里的真实全量，可以正常统计流量。
 * 上层应据此决定：丢弃该行，或只统计流量不统计时长。
 */
typedef void (*gone_cb_t)(const ct_key_t *k, uint64_t seen_ms,
                          uint64_t last_ms, uint64_t pkt_o, uint64_t byt_o,
                          uint64_t pkt_r, uint64_t byt_r,
                          uint8_t preexisting, void *ud);

void   cts_sample(uint64_t now_ms, seen_cb_t on_seen, gone_cb_t on_gone,
                  void *ud, cts_stats_t *st);
size_t cts_alive_count(void);

#endif
