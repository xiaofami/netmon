#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
排查某个域名的流量归属。

只读打开数据库（mode=ro），不会影响 netmon 正在写入的数据。
输出 JSON，便于本地再加工。

用法：
    python3 tools/probe-host.py <域名> [数据库路径]
"""
import sqlite3, json, sys

DB = sys.argv[2] if len(sys.argv) > 2 else "/opt/netmon/data/netmon.db"
HOST = sys.argv[1] if len(sys.argv) > 1 else "example.com"

c = sqlite3.connect("file:%s?mode=ro" % DB, uri=True, timeout=10)
c.row_factory = sqlite3.Row


def q(sql, *a):
    try:
        return [dict(r) for r in c.execute(sql, a)]
    except Exception as e:
        return [{"error": str(e)}]


out = {"host": HOST}

# 1) 哪些设备（源 IP）在拉
out["by_src"] = q(
    "SELECT src, COUNT(*) n, SUM(byt_o) up, SUM(byt_r) down, "
    "MIN(first_ms) t0, MAX(last_ms) t1 "
    "FROM conns WHERE host=? GROUP BY src ORDER BY down DESC LIMIT 12", HOST)

# 2) 目的 IP + 端口 + 协议
out["by_dst"] = q(
    "SELECT dst, dport, proto, COUNT(*) n, SUM(byt_o) up, SUM(byt_r) down "
    "FROM conns WHERE host=? GROUP BY dst ORDER BY down DESC LIMIT 12", HOST)

# 3) 端口/协议分布
out["by_port"] = q(
    "SELECT dport, proto, COUNT(*) n, SUM(byt_r) down "
    "FROM conns WHERE host=? GROUP BY dport, proto ORDER BY n DESC", HOST)

# 4) 按小时看流量曲线（最近 24h）
out["by_hour"] = q(
    "SELECT strftime('%m-%d %H', first_ms/1000, 'unixepoch', 'localtime') h, "
    "COUNT(*) n, SUM(byt_o) up, SUM(byt_r) down "
    "FROM conns WHERE host=? AND first_ms > (strftime('%s','now','-24 hours')*1000) "
    "GROUP BY h ORDER BY h", HOST)

# 5) 同一批字节系域名的总量（判断是不是一整套 CDN 在跑）
out["byte_family"] = q(
    "SELECT host, COUNT(*) n, SUM(byt_o) up, SUM(byt_r) down "
    "FROM conns WHERE host IS NOT NULL AND ("
    " host LIKE '%volces.com' OR host LIKE '%zijieapi.com' "
    " OR host LIKE '%byteimg.com' OR host LIKE '%bytedance%' "
    " OR host LIKE '%douyin%' OR host LIKE '%feishu%') "
    "GROUP BY host ORDER BY down DESC LIMIT 18")

# 6) DNS 里谁在查这个域名
out["dns_clients"] = q(
    "SELECT client_ip, COUNT(*) n, SUM(CASE WHEN ms IS NOT NULL THEN 1 ELSE 0 END) replied "
    "FROM dnsq WHERE qname=? GROUP BY client_ip ORDER BY n DESC LIMIT 10", HOST)

# 7) 这个域名解析出的 IP（从 dnsq 的 upstream 字段拿第一条 A 记录）
out["resolved"] = q(
    "SELECT upstream, COUNT(*) n FROM dnsq "
    "WHERE qname=? AND upstream IS NOT NULL AND upstream<>'' "
    "GROUP BY upstream ORDER BY n DESC LIMIT 10", HOST)

# 8) 该源设备的总流量占比（看是不是它一个人吃掉全部带宽）
out["src_total_share"] = q(
    "SELECT src, SUM(byt_o+byt_r) tot FROM conns "
    "WHERE first_ms > (strftime('%s','now','-24 hours')*1000) "
    "GROUP BY src ORDER BY tot DESC LIMIT 8")

print(json.dumps(out, ensure_ascii=False, indent=1))
