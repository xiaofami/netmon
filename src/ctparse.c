/*
 * ctparse.c - conntrack 行解析
 *
 * 这一行是全部数据的入口，必须严格正确，所以单独成文件并配单元测试。
 *
 * 真实行样例：
 *   ipv4 2 tcp 6 97 TIME_WAIT src=203.0.113.10 dst=59.82.113.219 sport=34458
 *   dport=80 packets=5 bytes=972 src=59.82.113.219 dst=203.0.113.10 sport=80
 *   dport=34458 packets=5 bytes=1396 [ASSURED] mark=0 zone=0 use=2
 *
 * 关键点：
 *   1) 一行里出现两遍 src/dst/sport/dport/packets/bytes。
 *      第一遍是原始方向（发起方 -> 目标），第二遍是应答方向。
 *   2) 两遍的出现顺序固定为 dst 在前、src 在后（conntrack 打印惯例），
 *      所以不能靠"第几个 src"判断方向，要看这一组是否已完整。
 *   3) mark= 与 zone= 参与 key，因为 homeproxy 分流时同五元组可能并存。
 *   4) ICMP 没有 sport/dport，用 type/code/id 代替，这里统一记 0，
 *      但 id 塞进 sport 高位不划算 —— ICMP 单列，不参与端口匹配。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <arpa/inet.h>
#include <netinet/in.h>

#include "ctparse.h"

static int parse_ip(const char *s, int family, uint8_t out[16])
{
    if (family == 4) {
        struct in_addr a;
        if (inet_pton(AF_INET, s, &a) != 1) return -1;
        memset(out, 0, 16);
        memcpy(out, &a, 4);
        return 0;
    }
    struct in6_addr a;
    if (inet_pton(AF_INET6, s, &a) != 1) return -1;
    memcpy(out, &a, 16);
    return 0;
}

/*
 * 解析一行（会就地修改 buf，把空白换成 \0）。
 * 成功返回 1，跳过（不认识的格式）返回 0。
 */
int ct_parse_line(char *buf, ct_rec_t *out)
{
    memset(out, 0, sizeof(*out));
    out->key.proto = 0xff;

    /* --- 1. family --- */
    if (strncmp(buf, "ipv4", 4) == 0)      out->key.family = 4;
    else if (strncmp(buf, "ipv6", 4) == 0) out->key.family = 6;
    else return 0;

    char *save = NULL;
    char *tok  = strtok_r(buf, " \t\r\n", &save);   /* [0] family */

    for (int i = 1; i <= 4; i++) {                  /* [1]..[4] */
        tok = strtok_r(NULL, " \t\r\n", &save);
        if (!tok) return 0;
        if (i == 2) {                               /* [2] l4proto */
            if      (strncmp(tok, "tcp", 3) == 0)    out->key.proto = IPPROTO_TCP;
            else if (strncmp(tok, "udp", 3) == 0)    out->key.proto = IPPROTO_UDP;
            else if (strncmp(tok, "icmp", 4) == 0)   out->key.proto = IPPROTO_ICMP;
            else if (strncmp(tok, "icmpv6", 6) == 0) out->key.proto = IPPROTO_ICMPV6;
            else if (strncmp(tok, "sctp", 4) == 0)   out->key.proto = IPPROTO_SCTP;
            else                                     out->key.proto = 0;
        }
    }
    if (out->key.proto == 0xff) return 0;

    /* 取下一个 token：若仍不是 key=value，则是 state，跳过 */
    tok = strtok_r(NULL, " \t\r\n", &save);
    if (!tok) return 0;
    if (strchr(tok, '=') == NULL) {
        /* 这是 state（如 ESTABLISHED），继续取下一个 */
        tok = strtok_r(NULL, " \t\r\n", &save);
        if (!tok) return 0;
    }
    /* 此处 tok 一定是第一个 key=value，回填给它自己处理 */
    /* 用一个标记让下面的循环先处理 tok */
    int have_first = 1;

    /*
     * --- 3. 扫 key=value，按"组"填充方向 ---
     *
     * 一组的字段是 { src, dst, sport, dport, packets, bytes }，
     * 但 conntrack 打印顺序并不固定（实测 src 在前、dst 在后）。
     *
     * 切组判据：**上一组已经齐了（有 src 且有 dst 且有 bytes）**，
     * 此时再遇到 src= 或 dst= 就是新一组的开始。
     * 用"齐了"而不是"见到第二个 dst"作判据，才能兼容任意字段顺序。
     */
    int  grp = 0;          /* 0=原始方向 1=应答方向 */
    int  have_src = 0, have_dst = 0, have_bytes = 0;
    uint64_t pkts = 0, bytes = 0;
    uint16_t sport = 0, dport = 0;
    uint8_t  addr[16];

    /* 把当前组落账到 out，然后清空组内状态 */
    #define FLUSH_GROUP() do {                                              \
        if (grp == 0) {                                                     \
            out->pkt_orig = pkts; out->byt_orig = bytes;                    \
            out->key.sport = sport; out->key.dport = dport;                 \
        } else if (grp == 1) {                                              \
            out->pkt_repl = pkts; out->byt_repl = bytes;                    \
            out->have_reply = 1;                                            \
        }                                                                   \
        grp++;                                                              \
        pkts = bytes = 0; sport = dport = 0;                                \
        have_src = have_dst = have_bytes = 0;                               \
    } while (0)

    while (have_first || (tok = strtok_r(NULL, " \t\r\n", &save)) != NULL) {
        if (have_first) have_first = 0;

        int is_addr = (strncmp(tok, "src=", 4) == 0 ||
                       strncmp(tok, "dst=", 4) == 0);

        /* 组边界：上一组已完整，且这是一个地址字段 -> 开新组 */
        if (is_addr && have_src && have_dst && have_bytes && grp < 2)
            FLUSH_GROUP();

        if (grp > 1) {
            /* 第三组及以后（如 ICMP 的 type/code/id）不再关心 */
            if (strncmp(tok, "mark=", 5) == 0)
                out->key.mark = (uint32_t)strtoul(tok + 5, NULL, 10);
            else if (strncmp(tok, "zone=", 5) == 0)
                out->key.zone = (uint16_t)atoi(tok + 5);
            continue;
        }

        if (strncmp(tok, "src=", 4) == 0) {
            if (grp == 0 && parse_ip(tok + 4, out->key.family, out->key.src) == 0)
                have_src = 1;
            else if (grp == 1 && parse_ip(tok + 4, out->key.family, addr) == 0)
                have_src = 1;
            continue;
        }
        if (strncmp(tok, "dst=", 4) == 0) {
            if (grp == 0 && parse_ip(tok + 4, out->key.family, out->key.dst) == 0)
                have_dst = 1;
            else if (grp == 1 && parse_ip(tok + 4, out->key.family, addr) == 0)
                have_dst = 1;
            continue;
        }

        if (strncmp(tok, "sport=", 6) == 0)        sport = (uint16_t)atoi(tok + 6);
        else if (strncmp(tok, "dport=", 6) == 0)   dport = (uint16_t)atoi(tok + 6);
        else if (strncmp(tok, "packets=", 8) == 0) pkts  = strtoull(tok + 8, NULL, 10);
        else if (strncmp(tok, "bytes=", 6) == 0) { bytes = strtoull(tok + 6, NULL, 10); have_bytes = 1; }
        else if (strncmp(tok, "mark=", 5) == 0)
            out->key.mark = (uint32_t)strtoul(tok + 5, NULL, 10);
        else if (strncmp(tok, "zone=", 5) == 0)
            out->key.zone = (uint16_t)atoi(tok + 5);
        else if (strcmp(tok, "[UNREPLIED]") == 0) out->unreplied = 1;
    }

    /* 收尾：最后未落账的那一组 */
    int got_any = (have_src || have_dst);
    if (got_any && grp < 2) FLUSH_GROUP();

    #undef FLUSH_GROUP

    /* ICMP 没有端口，用 id 塞进 dport 以便区分不同 ping 会话 */
    if (out->key.proto == IPPROTO_ICMP || out->key.proto == IPPROTO_ICMPV6) {
        out->key.sport = 0;
        out->key.dport = 0;
    }

    return got_any ? 1 : 0;
}

/* 把 key 转成可读字符串，用于调试和日志 */
void ct_key_str(const ct_key_t *k, char *out, size_t n)
{
    char a[INET6_ADDRSTRLEN] = {0}, b[INET6_ADDRSTRLEN] = {0};
    inet_ntop(k->family == 4 ? AF_INET : AF_INET6, k->src, a, sizeof(a));
    inet_ntop(k->family == 4 ? AF_INET : AF_INET6, k->dst, b, sizeof(b));
    snprintf(out, n, "%s|%s|%u|%s|%u|%s|%u|%u",
             k->family == 4 ? "4" : "6",
             k->proto == IPPROTO_TCP ? "tcp" :
             k->proto == IPPROTO_UDP ? "udp" :
             k->proto == IPPROTO_ICMP ? "icmp" :
             k->proto == IPPROTO_ICMPV6 ? "icmpv6" : "oth",
             k->mark, a, k->sport, b, k->dport, k->zone);
}
