/*
 * detail.h - 通用详情下钻
 *
 * 面板里几乎所有榜单条目都应该能点开。与其给每个榜写一套接口，
 * 不如统一成「实体类型 + 键」的查询：
 *   host    域名或目的 IP（流量榜、网段、代理探测结果）
 *   qname   DNS 查询域名（DNS 榜、查询日志）
 *   app     服务大类（服务分布）
 *   verdict 代理探测结论（如 need_proxy，列出到底是哪些目标）
 *
 * 只读，键一律 sqlite3_bind，不拼 SQL。
 */
#ifndef NETMON_DETAIL_H
#define NETMON_DETAIL_H

#include <stdint.h>
#include <stddef.h>
#include <sqlite3.h>

/* 由报表提供设备名解析，避免本模块依赖设备目录的内部实现 */
typedef const char *(*detail_name_fn)(const char *ip, char *buf, size_t n);

/* 输出完整 JSON 到 stdout。返回 0 成功，非 0 输出 {"error":...}。
   since_ms 为时间窗长度（不是起始时间）。 */
int detail_emit(sqlite3 *db, const char *type, const char *key,
                uint64_t since_ms, detail_name_fn name_fn);

/* 参数合法性：type 白名单 + key 字符集，防止任何拼注入 */
int detail_valid(const char *type, const char *key);

#endif /* NETMON_DETAIL_H */
