#ifndef CTPARSE_H
#define CTPARSE_H

#include <stdint.h>
#include <stddef.h>

/* 连接的五元组 + mark + zone，用作哈希表 key */
typedef struct {
    uint8_t  family;     /* 4 或 6 */
    uint8_t  proto;      /* IPPROTO_* ，0 表示其它 */
    uint16_t zone;
    uint32_t mark;
    uint16_t sport, dport;
    uint8_t  src[16];
    uint8_t  dst[16];
} ct_key_t;

/* 单行解析结果 */
typedef struct {
    ct_key_t key;
    uint64_t pkt_orig, byt_orig;   /* 原始方向累计 */
    uint64_t pkt_repl, byt_repl;   /* 应答方向累计 */
    uint8_t  have_reply;
    uint8_t  unreplied;
} ct_rec_t;

int  ct_parse_line(char *buf, ct_rec_t *out);
void ct_key_str(const ct_key_t *k, char *out, size_t n);

#endif
