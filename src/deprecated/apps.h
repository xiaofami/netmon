/*
 * apps.h - 服务/应用识别（旁路、零抓包）
 *
 * 为什么不引入 nDPI / libprotoident：
 *   两者都要读取报文载荷（nDPI 看 TLS 证书/SNI，libprotoident 看载荷前 4 字节），
 *   而 netmon 只读 conntrack 的累计计数，AF_PACKET 嗅探器只放行 udp/53。
 *   为识别服务去抓全量报文，会推翻「不碰数据面、零额外开销」的架构。
 *
 * 这里用三层纯元数据特征，全部来自已有的 conns / dnsq 记录：
 *   1) 域名关键词（抖音/快手/B站/萤石/系统更新…）
 *   2) 目的端口（1935 RTMP、6881 BT、554 RTSP…）
 *   3) 上下行比例（由前端据 bytes_out/bytes_in 判断观看/上传）
 *
 * 结论是「推测」，不是 DPI 判定，界面上会如实标注。
 */
#ifndef NETMON_APPS_H
#define NETMON_APPS_H

#include <stddef.h>

/* 返回 1 表示识别到具体服务；name/cat 回填显示名与大类 */
int apps_classify(const char *host, int dport, char *name, size_t n,
                  const char **cat);

/* 是否命中已知遥测/上报类域名（用于隐私观察，启发式，需人工核实） */
int apps_is_telemetry(const char *qname, char *kind, size_t n);

#endif /* NETMON_APPS_H */
