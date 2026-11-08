/*
 * reach.h - 代理可达性探测（旁路）
 *
 * 触发条件：某个域名/IP 出现解析失败或无应答连接，且已排除
 * homeproxy 广告拦截与本地搜索域。分别测一次直连与走代理，
 * 把「直连不通、代理通」的目标记下来，作为完善 Proxy 规则的依据。
 *
 * 严格约束：低频（默认 15 分钟一轮）、限量（默认 6 个目标）、
 * 冷却 24 小时、单目标双超时 6+10 秒；只写自己的 reach 表，
 * 绝不自动修改 homeproxy 配置或防火墙。
 */
#ifndef NETMON_REACH_H
#define NETMON_REACH_H

#include <stdint.h>
#include "db.h"

typedef void (*reach_log_fn)(const char *fmt, ...);

/*
 * 跑一轮探测。
 *   db       : 已打开的可写数据库
 *   proxy    : homeproxy mixed 端口，如 http://127.0.0.1:5330
 *   limit    : 本轮最多测几个目标
 *   since_ms : 候选时间窗
 * 返回实际探测的目标数。
 */
int reach_run_round(db_t *db, const char *proxy, int limit,
                    uint64_t since_ms, reach_log_fn logfn);

#endif /* NETMON_REACH_H */
