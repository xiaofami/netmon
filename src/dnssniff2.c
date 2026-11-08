/*
 * dnssniff2.c - AF_PACKET DNS 查询嗅探验证程序
 *
 * 目的：验证"不用 eBPF、不用改 dnsmasq，靠 AF_PACKET 抓 53 端口"这条路可行。
 * 这是 eBPF 方案的等效替代品，零依赖（不需要 clang / libbpf / 内核 BTF 程序）。
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/socket.h>
#include <linux/if_packet.h>
#include <linux/if_ether.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <time.h>

static int parse(const uint8_t *p, int len)
{
    if (len < 12) return 0;
    if (p[2] & 0x80) return 0;                  /* 跳过响应 */
    if (((p[4] << 8) | p[5]) < 1) return 0;     /* qdcount */

    const uint8_t *q = p + 12, *end = p + len;
    char name[256];
    int n = 0;

    /*
     * 逐个读 label（长度前缀 + 内容），用 '.' 拼接。
     * 循环退出时 q 指向终止的 0x00，必须跳过它才能读到 qtype。
     * —— 这里漏掉 q++ 会让 qtype 恒为 0（踩过一次）。
     */
    while (q < end && *q) {
        int l = *q++;
        if (l > 63 || q + l > end) return 0;
        if (n) name[n++] = '.';
        if (n + l < (int)sizeof(name)) { memcpy(name + n, q, l); n += l; }
        q += l;
    }
    if (q >= end) return 0;
    q++;                                        /* 跳过根域终止符 0x00 */
    if (q + 4 > end) return 0;
    name[n] = '\0';

    int qt = (q[0] << 8) | q[1];
    if (qt != 1 && qt != 28) return 0;          /* 只要 A / AAAA */

    printf("%s\n", name);
    fflush(stdout);
    return 1;
}

int main(int argc, char **argv)
{
    int fd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    if (fd < 0) { perror("socket"); return 1; }

    unsigned char b[2048];
    time_t st = time(NULL);
    int c = 0, got = 0;
    int secs = (argc > 1) ? atoi(argv[1]) : 8;

    while (time(NULL) - st < secs) {
        int n = recv(fd, b, sizeof(b), MSG_DONTWAIT);
        if (n < 42) continue;

        if (!(b[12] == 0x08 && b[13] == 0x00)) continue;   /* 只要 IPv4 */
        if (b[23] != 17) continue;                          /* 只要 UDP */

        int ihl = (b[14] & 0x0f) * 4;
        if (14 + ihl + 8 > n) continue;

        int sport = (b[14 + ihl] << 8) | b[14 + ihl + 1];
        int dport = (b[14 + ihl + 2] << 8) | b[14 + ihl + 3];
        /* 只要"发往 53 端口"的查询帧；sport=53 的是响应，跳过 */
        if (dport != 53 || sport == 53) continue;

        c++;
        got += parse(b + 14 + ihl + 8, n - 14 - ihl - 8);
    }

    printf("--- udp53 frames=%d parsed=%d ---\n", c, got);
    return 0;
}
